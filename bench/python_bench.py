"""G9: time fwtriage's Python readers on the images bench.c times.

Run in fwtriage's environment:
    cd ~/opensources/fwtriage && uv run python <lib>/bench/python_bench.py <image>... | -l <list>

The timed span is probe + unpack, which builds the whole tree with every
file's content: the same work as bench.c's open + list + read.
"""

import statistics
import sys
import time

from fwtriage.formats.cramfs import Cramfs
from fwtriage.formats.jffs2 import Jffs2
from fwtriage.formats.ubi import Ubi
from fwtriage.model.budget import Budget, Limits

READERS = (Ubi(), Cramfs(), Jffs2())
UNLIMITED = Limits(total_bytes=1 << 62, entry_bytes=1 << 62, depth=64, entries=1 << 40)

def corpus_limits(image_path: str) -> Limits:
    """The budget the corpus gave this image, from the .txt beside it, as test_parity applies it."""
    with open(image_path[: -len(".img")] + ".txt") as f:
        for line in f:
            if line.startswith("limits "):
                total, entry, entries = (int(v) for v in line.split()[1:])
                return Limits(total_bytes=total, entry_bytes=entry, entries=entries)
    return Limits()

def unpack(data: bytes, limits: Limits = UNLIMITED) -> tuple[float, bool]:
    view = memoryview(data)
    start = time.perf_counter()
    for reader in READERS:
        region = reader.probe(view, 0)
        if region is None:
            continue
        try:
            reader.unpack(view, region, Budget(limits))
            return time.perf_counter() - start, True
        except Exception:
            return time.perf_counter() - start, False
    return time.perf_counter() - start, False

def main(args: list[str]) -> None:
    if args[:1] == ["-l"]:
        total = 0.0
        refused = 0
        paths = [line.strip() for line in open(args[1]) if line.strip()]
        for path in paths:
            with open(path, "rb") as f:
                seconds, ok = unpack(f.read(), corpus_limits(path))
            total += seconds
            refused += not ok
        print(f"{len(paths)} images ({refused} refused), probe+unpack {total * 1e3:.1f} ms in total")
        return
    for path in args:
        with open(path, "rb") as f:
            data = f.read()
        runs = [unpack(data) for _ in range(3)]
        ok = all(r[1] for r in runs)
        print(f"{path}\n  probe+unpack {statistics.median(r[0] for r in runs) * 1e3:.1f} ms (median of 3)"
              + ("" if ok else " [refused]"))

if __name__ == "__main__":
    main(sys.argv[1:])
