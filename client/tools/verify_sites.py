#!/usr/bin/env python3
"""Checks a Wow.exe against the hook sites declared in src/game.h.

Reads only: nothing is modified and nothing is executed. For every
`constexpr uintptr_t kName = 0x...;` that has a matching
`constexpr uint8_t kNameBytes[] = {...};` it compares the bytes in the file
with the expected ones, and it reports the build timestamp and whether the
start-up loader stub (the one awesome_wotlk and St0ny's patcher add) is there.

Usage: verify_sites.py path/to/Wow.exe
"""
import os
import re
import struct
import sys

GAME_H = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "game.h")

LOADER_STUB_VA = 0x004E5CB0
LOADER_STUB = bytes.fromhex("B801000000A374B4B60068E05C4E00")  # up to and including `push offset name`
LOADER_CALLS = {0xE8: "calls LoadLibraryA through the client", 0xFF: "calls LoadLibraryA through the import table"}
LOADER_NAME_VA = 0x004E5CE0
LOADER_NAME_SIZE = 20


def load_sections(data):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[:2] != b"MZ" or data[pe:pe + 4] != b"PE\0\0":
        raise SystemExit("not a PE executable")
    count = struct.unpack_from("<H", data, pe + 6)[0]
    timestamp = struct.unpack_from("<I", data, pe + 8)[0]
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    image_base = struct.unpack_from("<I", data, pe + 24 + 28)[0]
    table = pe + 24 + optional_size
    sections = []
    for i in range(count):
        vsize, rva, rsize, raw = struct.unpack_from("<IIII", data, table + 40 * i + 8)
        sections.append((image_base + rva, max(vsize, rsize), raw, rsize))
    return timestamp, image_base, sections


def read_va(data, sections, va, length):
    for start, size, raw, rsize in sections:
        if start <= va < start + size and va - start + length <= rsize:
            offset = raw + (va - start)
            return data[offset:offset + length]
    return None


def parse_game_h(path):
    text = open(path, encoding="utf-8").read()
    addresses = dict(re.findall(r"constexpr uintptr_t (k\w+) = (0x[0-9A-Fa-f]+);", text))
    arrays = dict(re.findall(r"constexpr uint8_t (k\w+)Bytes\[\] = \{([^}]*)\};", text))
    timestamp = re.search(r"kBuildTimestamp = (0x[0-9A-Fa-f]+)", text)
    sites = []
    for name, body in arrays.items():
        if name in addresses:
            expected = bytes(int(b, 16) for b in re.findall(r"0x[0-9A-Fa-f]+", body))
            sites.append((name, int(addresses[name], 16), expected))
    return int(timestamp.group(1), 16), sites


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    data = open(sys.argv[1], "rb").read()
    timestamp, image_base, sections = load_sections(data)
    expected_timestamp, sites = parse_game_h(GAME_H)

    ok = True
    print(f"image base {image_base:#010x}, build timestamp {timestamp:#010x} "
          f"({'3.3.5a build 12340' if timestamp == expected_timestamp else 'NOT the expected build'})")
    ok &= timestamp == expected_timestamp and image_base == 0x00400000

    for name, address, expected in sorted(sites, key=lambda s: s[1]):
        actual = read_va(data, sections, address, len(expected))
        match = actual == expected
        ok &= match
        shown = actual.hex(" ").upper() if actual is not None else "not in file"
        print(f"  {'ok      ' if match else 'MISMATCH'} {name:<26} {address:#010x}  {shown}"
              + ("" if match else f"   expected {expected.hex(' ').upper()}"))

    stub = read_va(data, sections, LOADER_STUB_VA, len(LOADER_STUB) + 1)
    if stub is not None and stub[:-1] == LOADER_STUB and stub[-1] in LOADER_CALLS:
        raw = read_va(data, sections, LOADER_NAME_VA, LOADER_NAME_SIZE)
        name = raw.split(b"\0")[0].decode("latin1")
        print(f"loader stub present ({LOADER_CALLS[stub[-1]]}), loads: {name}")
    else:
        print("loader stub NOT present (install/WotLK-ARPG-Setup.ps1 needs it; see the README)")
    print("all hook sites match" if ok else "one or more checks failed")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
