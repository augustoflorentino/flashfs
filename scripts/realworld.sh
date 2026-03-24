#!/bin/sh
set -eu
build=${1:-build/dev}
dump=$build/roundtrip_dump
cache=${FLASHFS_REALWORLD_CACHE:-$HOME/.cache/flashfs/realworld}
mkdir -p "$cache"

images="
ee245d0f31895ca6368ee79a9ca4c24d080576f464cc71b7e4cf778fcc860b66 https://archive.openwrt.org/attitude_adjustment/12.09/ar71xx/generic/openwrt-ar71xx-generic-root.jffs2-64k
d9f33de5592956cee23dea6351a2ba83fc3aaff6a5a94f1bf62e02b8d8ecd5cc https://archive.openwrt.org/attitude_adjustment/12.09/kirkwood/generic/openwrt-kirkwood--jffs2-128k.img
0badbc423d6ee3630235fdad6bcaba51d59ed1ece8382629c8490d6e70e23298 https://downloads.openwrt.org/releases/23.05.5/targets/ipq807x/generic/openwrt-23.05.5-ipq807x-generic-xiaomi_ax3600-squashfs-factory.ubi
"
failed=0
echo "$images" | while read -r sum url; do
    [ -n "$sum" ] || continue
    file=$cache/$sum
    if [ ! -f "$file" ]; then
        curl -fsSL --max-time 600 -o "$file.part" "$url"
        mv "$file.part" "$file"
    fi
    if [ "$(sha256sum "$file" | cut -d' ' -f1)" != "$sum" ]; then
        echo "FAIL $url: pinned image changed upstream"
        exit 1
    fi
    if ! mem=$("$dump" "$file" 2>&1) || ! cb=$("$dump" -c "$file" 2>&1) || [ "$mem" != "$cb" ] ||
        echo "$mem" | grep -q "^dump:"; then
        echo "FAIL $url"
        echo "$mem" | grep "^dump:" || true
        exit 1
    fi
    echo "ok   $(echo "$mem" | wc -l) entries  $(basename "$url")"
done || failed=1
exit $failed
