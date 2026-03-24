#!/bin/sh
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
fwtriage=${1:-$HOME/opensources/fwtriage}
build=$root/build/release
work=$build/bench-work
cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Release -DFLASHFS_BUILD_BENCH=ON -DFLASHFS_BUILD_TESTS=OFF >/dev/null
cmake --build "$build" >/dev/null
rm -rf "$work"
mkdir -p "$work"
python3 - "$work" <<'PY'
import hashlib, os, random, sys
work = sys.argv[1]
rng = random.Random(20251124)
words = [bytes(rng.choice(b"abcdefghijklmnopqrstuvwxyz") for _ in range(rng.randint(2, 9))) for _ in range(800)]
tree = os.path.join(work, "tree")
for i in range(1500):
    d = os.path.join(tree, "d%02d" % (i % 30), "e%d" % (i % 7))
    os.makedirs(d, exist_ok=True)
    if i % 5 == 0:
        data = rng.randbytes(rng.randint(1, 16000))           # binaries: little to gain
    else:
        data = b" ".join(rng.choice(words) for _ in range(rng.randint(10, 6000)))  # text
    with open(os.path.join(d, "f%04d" % i), "wb") as f:
        f.write(data)
    if i % 50 == 0:
        os.symlink("f%04d" % i, os.path.join(d, "l%04d" % i))
with open(os.path.join(work, "volume.bin"), "wb") as f:
    f.write(rng.randbytes(124 * 1024 * 1024 - 64 * 1024))
PY
mkfs.jffs2 -r "$work/tree" -o "$work/jffs2-16m.img" -l -e 128KiB --pad=16777216 -x lzo
mkfs.cramfs "$work/tree" "$work/cramfs.img" >/dev/null
printf '[v]\nmode=ubi\nimage=%s\nvol_id=0\nvol_type=dynamic\nvol_name=data\nvol_flags=autoresize\n' \
    "$work/volume.bin" >"$work/ubi.ini"
ubinize -o "$work/ubi-128m.img" -p 128KiB -m 2048 "$work/ubi.ini"
ls "$root"/tests/parity/data/*.img >"$work/corpus.list"

echo "== flashfs"
"$build/bench" "$work/jffs2-16m.img" "$work/cramfs.img" "$work/ubi-128m.img"
"$build/bench" -l "$work/corpus.list"
echo "== fwtriage (Python)"
cd "$fwtriage"
uv run python "$root/bench/python_bench.py" "$work/jffs2-16m.img" "$work/cramfs.img" "$work/ubi-128m.img"
uv run python "$root/bench/python_bench.py" -l "$work/corpus.list"
