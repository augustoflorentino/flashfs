"""The zlib floor of a JFFS2 image: time to inflate its zlib nodes and nothing else.

Reading a zlib JFFS2 image can be no faster than this, in any language; it
shows how much of flashfs's read time (and Python's) is zlib's own.

    python3 bench/zlib_floor.py <little-endian jffs2 image>
"""

import struct
import sys
import time
import zlib

def zlib_nodes(data):
    pos = 0
    while pos + 12 <= len(data):
        magic, nodetype, totlen = struct.unpack_from("<HHI", data, pos)
        if magic != 0x1985 or totlen < 12 or pos + totlen > len(data):
            pos += 4
            continue
        if nodetype == 0xE002 and data[pos + 56] == 6:
            csize, dsize = struct.unpack_from("<II", data, pos + 48)
            yield data[pos + 68 : pos + 68 + csize], dsize
        pos += (totlen + 3) & ~3

def main(path):
    with open(path, "rb") as f:
        nodes = list(zlib_nodes(f.read()))
    best = float("inf")
    for _ in range(7):
        start = time.perf_counter()
        for payload, size in nodes:
            zlib.decompressobj().decompress(payload, size)
        best = min(best, time.perf_counter() - start)
    out = sum(size for _, size in nodes)
    print("  %d zlib nodes, %.1f MB out, inflate alone %.1f ms (best of 7)" % (len(nodes), out / 1e6, best * 1e3))

if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
