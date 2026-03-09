"""The fixed images test_flashfs.c reads, built with fwtriage's corpus writers (the oracle).

    cd ~/opensources/fwtriage && PYTHONPATH=. uv run python <flashfs>/tests/data/make.py <flashfs>/tests/data
"""

import sys
from pathlib import Path

from tests.corpus.writers import cramfs, jffs2
from tests.corpus.writers.tree import DirNode, FileNode, LinkNode
from tests.corpus.writers.ubi import ubi

BIG = bytes((i * 7 + i // 300) & 0xFF for i in range(70_000))
TREE = [
    FileNode("/etc/passwd", b"root:x:0:0::/root:/bin/sh\n", 0o600),
    FileNode("/bin/busybox", BIG, 0o755),
    LinkNode("/bin/sh", "busybox"),
    LinkNode("/lib/up", "../etc"),
    LinkNode("/loop1", "loop2"),
    LinkNode("/loop2", "loop1"),
    DirNode("/tmp", 0o1777),
    FileNode("/empty", b""),
    FileNode("/a/b/c/d.txt", b"deep\n" * 1000),
]

SMALL = [node if node.path != "/bin/busybox" else FileNode("/bin/busybox", BIG[:5000], 0o755) for node in TREE]

def main(out: Path) -> None:
    images = {
        "jffs2-le-zlib.img": jffs2.write(TREE, order="<", compression="zlib", pad_to=1 << 16),
        "jffs2-be-lzma.img": jffs2.write(TREE, order=">", compression="lzma"),
        "jffs2-le-rtime.img": jffs2.write(TREE, order="<", compression="rtime"),
        "jffs2-be-none.img": jffs2.write(TREE, order=">", compression="none"),
        "cramfs-le.img": cramfs.write(TREE, order="<"),
        "cramfs-be.img": cramfs.write(TREE, order=">"),
        "small-jffs2-le-zlib.img": jffs2.write(SMALL, order="<", compression="zlib"),
        "small-jffs2-be-lzma.img": jffs2.write(SMALL, order=">", compression="lzma"),
        "small-cramfs-le.img": cramfs.write(SMALL, order="<"),
        "small-cramfs-be.img": cramfs.write(SMALL, order=">"),
        "ubi.img": ubi([("kernel", b"K" * 150_000), ("rootfs", BIG)], stale=True),
    }
    for name, data in images.items():
        (out / name).write_bytes(data)

if __name__ == "__main__":
    main(Path(sys.argv[1]))
