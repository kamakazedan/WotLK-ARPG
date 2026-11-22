#!/usr/bin/env bash
# Assembles what a player installs into dist/WotLK-ARPG/, laid out like the
# game folder: the library and the setup scripts at the top, the addon under
# Interface/AddOns. Also writes dist/WotLK-ARPG-<version>.zip of the same.
#
# Builds the library first if client/build/WotLK-ARPG.dll is not there yet.
set -euo pipefail
cd "$(dirname "$0")/.."

addon_version=$(sed -n 's/^## Version: *//p' addon/WotLK-ARPG/WotLK-ARPG.toc | tr -d '\r')
library_version=$(sed -n 's/^#define ARPG_VERSION "\(.*\)"/\1/p' client/src/arpg.cpp)
if [ -z "$addon_version" ] || [ "$addon_version" != "$library_version" ]; then
    echo "version mismatch: the addon says '$addon_version', the library says '$library_version'" >&2
    exit 1
fi

[ -f client/build/WotLK-ARPG.dll ] || client/build.sh

out=dist/WotLK-ARPG
rm -rf dist
mkdir -p "$out/Interface/AddOns"
cp client/build/WotLK-ARPG.dll "$out/"
cp client/install/* "$out/"
cp -r addon/WotLK-ARPG "$out/Interface/AddOns/"

(cd "$out" && python3 -m zipfile -c "../WotLK-ARPG-$addon_version.zip" *)

echo "packaged version $addon_version:"
(cd dist && find WotLK-ARPG -type f | sort | sed 's/^/  /')
echo "  dist/WotLK-ARPG-$addon_version.zip"
if command -v sha256sum >/dev/null; then
    echo "sha256 of the library: $(sha256sum "$out/WotLK-ARPG.dll" | cut -d' ' -f1)"
fi
