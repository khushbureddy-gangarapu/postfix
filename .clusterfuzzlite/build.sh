set -e
CC=clang  CXX=clang++ CFLAGS=-fsanitize=address
export CC CXX
set $LLVM_CONFIG=/usr/bin/llvm-config-10

cd postfix
make makefiles CCARGS="${CFLAGS}"
make
BASE=$PWD

git clone https://github.com/AFLplusplus/AFLplusplus

cd AFLplusplus
make all
make install
cp $SRC/.clusterfuzzlite/fuzz_mime.c .
cp $SRC/.clusterfuzzlite/fuzz_tok822.c .
./afl-cc -fsanitize=address,undefined -ggdb fuzz_mime.c -o fuzzer1
./afl-cc -fsanitize=address,undefined -ggdb fuzz_tok822.c -o fuzzer2
