/*++
/* NAME
/*	alias 3
/* SUMMARY
/*	alias data base lookups
/* SYNOPSIS
/*	#include "local.h"
/*
/*	int	deliver_alias(state, usr_attr, statusp)
/*	LOCAL_STATE state;
/*	USER_ATTR usr_attr;
/*	int	*statusp;
/* DESCRIPTION
/*	deliver_alias() looks up the expansion of the recipient in
/*	the global alias database and delivers the message to the
/*	listed destinations. The result is zero when no alias was found
/*	or when the message should be delivered to the user instead.
/*
/*	deliver_alias() has wired-in knowledge about a few reserved
/*	recipient names.
/* .IP \(bu
/*	When no alias is found for the local \fIpostmaster\fR or
/*	\fImailer-daemon\fR a warning is issued and the message
/*	is discarded.
/* .IP \(bu
/*	When an alias exists for recipient \fIname\fR, and an alias
/*	exists for \fIowner-name\fR, the sender address is changed
/*	to \fIowner-name\fR, and the owner delivery attribute is
/*	set accordingly. This feature is disabled with
/*	"owner_request_special = no".
/* .PP
/*	Arguments:
/* .IP state
/*      Attributes that specify the message, recipient and more.
/*	Expansion type (alias, include, .forward).
/*      A table with the results from expanding aliases or lists.
/*      A table with delivered-to: addresses taken from the message.
/* .IP usr_attr
/*	User attributes (rights, environment).
/* .IP statusp
/*	Delivery status. See below.
/* DIAGNOSTICS
/*	Fatal errors: out of memory. The delivery status is non-zero
/*	when delivery should be tried again.
/* LICENSE
/* .ad
/* .fi
/*	The Secure Mailer license must be distributed with this software.
/* AUTHOR(S)
/*	Wietse Venema
/*	IBM T.J. Watson Research
/*	P.O. Box 704
/*	Yorktown Heights, NY 10598, USA
/*--*/

/* System library. */

#include <sys_defs.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>

#ifdef STRCASECMP_IN_STRINGS_H
#include <strings.h>
#endif

/* Utility library. */

#include <msg.h>
#include <htable.h>
#include <dict.h>
#include <argv.h>
#include <stringops.h>
#include <mymalloc.h>
#include <vstring.h>
#include <vstream.h>

/* Global library. */

#include <mail_params.h>
#include <mail_addr.h>
#include <sent.h>
#include <defer.h>
#include <maps.h>
#include <bounce.h>
#include <mypwd.h>
#include <canon_addr.h>

/* Application-specific. */

#include "local.h"

/* Application-specific. */

#define NO	0
#define YES	1

/* dict_owner - find out alias database owner */

static uid_t dict_owner(char *table)
{
    char   *myname = "dict_owner";
    DICT   *dict;
    struct stat st;

    /*
     * This code sits here for now, but we may want to move it to the library
     * some time.
     */
    if ((dict = dict_handle(table)) == 0)
	msg_panic("%s: can't find dictionary: %s", myname, table);
    if (dict->fd < 0)
	return (0);
    if (fstat(dict->fd, &st) < 0)
	msg_fatal("%s: fstat dictionary %s: %m", myname, table);
    return (st.st_uid);
}

/* deliver_alias - expand alias file entry */

int     deliver_alias(LOCAL_STATE state, USER_ATTR usr_attr, int *statusp)
{
    char   *myname = "deliver_alias";
    const char *alias_result;
    char   *expansion;
    char   *owner;
    static MAPS *maps;
    char  **cpp;
    uid_t   alias_uid;
    struct mypasswd *alias_pwd;
    VSTRING *canon_owner;

    /*
     * Make verbose logging easier to understand.
     */
    state.level++;
    if (msg_verbose)
	msg_info("%s[%d]: %s", myname, state.level, state.msg_attr.local);

    /*
     * Do this only once.
     */
    if (maps == 0)
	maps = maps_create("aliases", var_alias_maps);

    /*
     * DUPLICATE/LOOP ELIMINATION
     * 
     * We cannot do duplicate elimination here. Sendmail compatibility requires
     * that we allow multiple deliveries to the same alias, even recursively!
     * For example, we must deliver to mailbox any messags that are addressed
     * to the alias of a user that lists that same alias in her own .forward
     * file. Yuck! This is just an example of some really perverse semantics
     * that people will expect Postfix to implement just like sendmail.
     * 
     * We can recognize one special case: when an alias includes its own name,
     * deliver to the user instead, just like sendmail. Otherwise, we just
     * bail out when nesting reaches some unreasonable depth, and blame it on
     * a possible alias loop.
     */
    if (state.msg_attr.exp_from != 0
	&& strcasecmp(state.msg_attr.exp_from, state.msg_attr.local) == 0)
	return (NO);
    if (state.level > 100) {
	msg_warn("possible alias database loop for %s", state.msg_attr.local);
	*statusp = bounce_append(BOUNCE_FLAG_KEEP, BOUNCE_ATTR(state.msg_attr),
	       "possible alias database loop for %s", state.msg_attr.local);
	return (YES);
    }
    state.msg_attr.exp_from = state.msg_attr.local;

    /*
     * There are a bunch of roles that we're trying to keep track of.
     * 
     * First, there's the issue of whose rights should be used when delivering
     * to "|command" or to /file/name. With alias databases, the rights are
     * those of who owns the alias, i.e. the database owner. With aliases
     * owned by root, a default user is used instead. When an alias with
     * default rights references an include file owned by an ordinary user,
     * we must use the rights of the include file owner, otherwise the
     * include file owner could take control of the default account.
     * 
     * Secondly, there's the question of who to notify of delivery problems.
     * With aliases that have an owner- alias, the latter is used to set the
     * sender and owner attributes. Otherwise, the owner attribute is reset
     * (the alias is globally visible and could be sent to by anyone).
     */
    for (cpp = maps->argv->argv; *cpp; cpp++) {
	if ((alias_result = dict_lookup(*cpp, state.msg_attr.local)) != 0) {
	    if (msg_verbose)
		msg_info("%s: %s: %s = %s", myname, *cpp,
			 state.msg_attr.local, alias_result);

	    /*
	     * DELIVERY POLICY
	     * 
	     * Update the expansion type attribute, so we can decide if
	     * deliveries to |command and /file/name are allowed at all.
	     */
	    state.msg_attr.exp_type = EXPAND_TYPE_ALIAS;

	    /*
	     * DELIVERY RIGHTS
	     * 
	     * What rights to use for |command and /file/name deliveries? The
	     * command and file code will use default rights when the alias
	     * database is owned by root, otherwise it will use the rights of
	     * the alias database owner.
	     */
	    if ((alias_uid = dict_owner(*cpp)) == 0) {
		alias_pwd = 0;
		RESET_USER_ATTR(usr_attr, state.level);
	    } else {
		if ((alias_pwd = mypwuid(alias_uid)) == 0) {
		    msg_warn("cannot find alias database owner for %s", *cpp);
		    *statusp = defer_append(BOUNCE_FLAG_KEEP,
					    BOUNCE_ATTR(state.msg_attr),
					"cannot find alias database owner");
		    return (YES);
		}
		SET_USER_ATTR(usr_attr, alias_pwd, state.level);
	    }

	    /*
	     * WHERE TO REPORT DELIVERY PROBLEMS.
	     * 
	     * Use the owner- alias if one is specified, otherwise reset the
	     * owner attribute and use the include file ownership if we can.
	     * Save the dict_lookup() result before something clobbers it.
	     */
#define STR(x)	vstring_str(x)
#define OWNER_ASSIGN(own) \
	    (own = (var_ownreq_special == 0 ? 0 : \
	    concatenate("owner-", state.msg_attr.local, (char *) 0)))

	    expansion = mystrdup(alias_result);
	    if (OWNER_ASSIGN(owner) != 0 && maps_find(maps, owner)) {
		canon_owner = canon_addr_internal(vstring_alloc(10), owner);
		SET_OWNER_ATTR(state.msg_attr, STR(canon_owner), state.level);
	    } else {
		canon_owner = 0;
		RESET_OWNER_ATTR(state.msg_attr, state.level);
	    }

	    /*
	     * EXTERNAL LOOP CONTROL
	     * 
	     * Set the delivered message attribute to the recipient, so that
	     * this message will list the correct forwarding address.
	     */
	    state.msg_attr.delivered = state.msg_attr.recipient;

	    /*
	     * Deliver.
	     */
	    *statusp =
		(dict_errno ?
		 defer_append(BOUNCE_FLAG_KEEP, BOUNCE_ATTR(state.msg_attr),
			      "alias database unavailable") :
	       deliver_token_string(state, usr_attr, expansion, (int *) 0));
	    myfree(expansion);
	    if (owner)
		myfree(owner);
	    if (canon_owner)
		vstring_free(canon_owner);
	    if (alias_pwd)
		mypwfree(alias_pwd);
	    return (YES);
	}

	/*
	 * If the alias database was inaccessible for some reason, defer
	 * further delivery for the current top-level recipient.
	 */
	if (dict_errno != 0) {
	    *statusp = defer_append(BOUNCE_FLAG_KEEP,
				    BOUNCE_ATTR(state.msg_attr),
				    "alias database unavailable");
	    return (YES);
	} else {
	    if (msg_verbose)
		msg_info("%s: %s: %s not found", myname, *cpp,
			 state.msg_attr.local);
	}
    }

    /*
     * If no alias was found for a required reserved name, toss the message
     * into the bit bucket, and issue a warning instead.
     */
#define STREQ(x,y) (strcasecmp(x,y) == 0)

    if (STREQ(state.msg_attr.local, MAIL_ADDR_MAIL_DAEMON)
	|| STREQ(state.msg_attr.local, MAIL_ADDR_POSTMASTER)) {
	msg_warn("required alias not found: %s", state.msg_attr.local);
	*statusp = sent(SENT_ATTR(state.msg_attr), "discarded");
	return (YES);
    }

    /*
     * Try delivery to a local user instead.
     */
    return (NO);
}
