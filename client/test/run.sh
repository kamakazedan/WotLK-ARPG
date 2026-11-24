#!/usr/bin/env bash
# Builds and runs the tests that can run on the build machine (Linux x86-64):
#
#   1. hook_test:   the code-patching engine on its own, including the cases
#                   the simulation cannot stage (unexpected bytes, another
#                   library's hook already in place).
#   2. simulation:  the whole library, compiled as 32-bit code together with a
#                   mock of the game client (mock_client.cpp), and run.
#
# The same mock client is also built as a Windows program that loads the real
# DLL (../build.sh makes build/mock-client.exe); that one needs Windows to run.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build
CXX=${CXX:-clang++}

echo "== hook engine =="
"$CXX" -std=c++17 -O1 -Wall -Wextra -no-pie -fno-pie test/hook_test.cpp -o build/hook_test
build/hook_test

echo
echo "== library against the mock client (32-bit simulation) =="
# Freestanding 32-bit Linux: no C library, so nothing 32-bit needs installing.
# The program is linked high up, out of the way of the block of memory that
# stands in for Wow.exe at 0x00400000.
"$CXX" --target=i686-linux-gnu -m32 -std=c++17 -O2 -msse2 \
    -ffreestanding -fno-builtin -fno-exceptions -fno-rtti -fno-threadsafe-statics \
    -fno-stack-protector -fno-pic -fno-asynchronous-unwind-tables -fno-strict-aliasing \
    -mstackrealign -mstack-alignment=4 -Wall -Wextra -Werror \
    '-DDLLIMPORT=extern "C"' \
    -nostdlib -static -fuse-ld=lld -Wl,--image-base=0x20000000 \
    src/*.cpp test/mock_client.cpp test/mock_platform_linux.cpp -o build/mock-client-linux
(cd build && rm -f WotLK-ARPG.log && ./mock-client-linux)
echo
echo "-- the library's log from that run --"
cat build/WotLK-ARPG.log
