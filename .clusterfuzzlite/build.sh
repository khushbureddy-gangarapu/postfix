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
make distrib
make install
./afl-cc -fsanitize=address,undefined -ggdb -00 fuzz_mime.c fuzz_tok822.c -o fuzzer
./afl-fuzz -i in -o out -m none -- ./fuzzer @@
