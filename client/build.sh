#!/usr/bin/env bash
# Builds WotLK-ARPG.dll (32-bit Windows) with clang + lld on any OS, plus
# mock-client.exe, the Windows test program that loads the DLL (see test/).
#
# Needs: clang++, lld-link and llvm-dlltool (Ubuntu: apt install clang lld llvm).
# No Windows SDK and no C runtime are used; both files import only kernel32.dll.
set -euo pipefail

cd "$(dirname "$0")"
out=build
mkdir -p "$out"

CXX=${CXX:-clang++}
LINK=${LINK:-lld-link}
DLLTOOL=${DLLTOOL:-llvm-dlltool}

flags=(
    --target=i686-pc-windows-msvc
    -std=c++17 -O2 -msse2
    -ffreestanding -fno-builtin
    -fno-exceptions -fno-rtti -fno-threadsafe-statics
    -fno-stack-protector -mno-stack-arg-probe
    -fno-asynchronous-unwind-tables -fno-strict-aliasing
    -Wall -Wextra -Werror
)

compile() {  # compile SOURCE -> prints the object's path
    local object="$out/$(basename "${1%.cpp}").obj"
    "$CXX" "${flags[@]}" -c "$1" -o "$object"
    echo "$object"
}

size() {
    stat -c %s "$1" 2>/dev/null || stat -f %z "$1"
}

# Import library for the kernel32 functions declared in src/win.h and test/.
"$DLLTOOL" -m i386 -k -d src/kernel32.def -l "$out/kernel32.lib"

# --- the library -----------------------------------------------------------

objects=()
for source in src/*.cpp; do
    objects+=("$(compile "$source")")
done

"$LINK" /dll /machine:x86 /nodefaultlib /entry:DllMain /subsystem:windows \
    /opt:ref /opt:icf /safeseh:no /Brepro \
    /out:"$out/WotLK-ARPG.dll" "${objects[@]}" "$out/kernel32.lib"
echo "built $out/WotLK-ARPG.dll ($(size "$out/WotLK-ARPG.dll") bytes)"

# --- the Windows test program ----------------------------------------------
# Linked where Wow.exe is linked (0x00400000, not relocatable) and stamped
# with Wow.exe's build timestamp (0x4C2452FE), because the library checks both
# before it touches anything. test/mock_platform_windows.cpp has the details.

test_objects=(
    "$(compile test/mock_client.cpp)"
    "$(compile test/mock_platform_windows.cpp)"
    "$out/crt.obj"
)

"$LINK" /machine:x86 /nodefaultlib /entry:Entry /subsystem:console \
    /base:0x400000 /fixed /dynamicbase:no /timestamp:1277448958 /safeseh:no \
    /out:"$out/mock-client.exe" "${test_objects[@]}" "$out/kernel32.lib"
echo "built $out/mock-client.exe ($(size "$out/mock-client.exe") bytes): run it on Windows next to the DLL"
