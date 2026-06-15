"""The fixed UBIFS images test_flashfs.c and test_robust.c read.

fwtriage has no UBIFS writer: these come from mkfs.ubifs, the oracle for
UBIFS, on the tree make.py writes for the other formats. ubifs-journal.img
adds what mkfs.ubifs never writes, a journal, the way the kernel lays it
out: a reference node in the log naming a bud, and in the bud a new file, an
overwrite, a deletion, a rename, a truncation followed by a sparse
extension, and a last node cut short as a power loss leaves it.

    python3 tests/data/make_ubifs.py tests/data
"""

import os
import struct
import subprocess
import sys
import tempfile
import zlib

LEB = 15872  # the smallest UBIFS allows, on a 512-byte I/O unit: small images
GEOMETRY = ["-m", "512", "-e", str(LEB), "-c", "256", "-U"]
BIG = bytes((i * 7 + i // 300) & 0xFF for i in range(70_000))
PASSWD = b"root:x:0:0::/root:/bin/sh\n"

TREE = [
    ("/etc/passwd", PASSWD, 0o600),
    ("/bin/busybox", BIG, 0o755),
    ("/bin/sh", "busybox"),
    ("/lib/up", "../etc"),
    ("/loop1", "loop2"),
    ("/loop2", "loop1"),
    ("/tmp", 0o1777),
    ("/empty", b"", 0o644),
    ("/a/b/c/d.txt", b"deep\n" * 1000, 0o644),
]
SMALL = [n if n[0] != "/bin/busybox" else ("/bin/busybox", BIG[:5000], 0o755) for n in TREE]
C_FILE = bytes((i * 13) & 0xFF for i in range(10_000))
JOURNAL_BASE = [
    ("/a.txt", b"hello\n", 0o644),
    ("/gone", b"x", 0o644),
    ("/dir/b", b"bbbb\n" * 10, 0o644),
    ("/dir/c", C_FILE, 0o644),
]

def build(root, nodes):
    os.makedirs(root)
    for node in nodes:
        path = root + node[0]
        if len(node) == 2 and isinstance(node[1], int):
            os.makedirs(path, exist_ok=True)
            os.chmod(path, node[1])
            continue
        os.makedirs(os.path.dirname(path), exist_ok=True)
        if len(node) == 2:
            os.symlink(node[1], path)
        else:
            with open(path, "wb") as f:
                f.write(node[1])
            os.chmod(path, node[2])
    for dirpath, dirnames, _ in os.walk(root):
        for d in dirnames:
            if os.stat(os.path.join(dirpath, d)).st_mode & 0o7777 != 0o1777:
                os.chmod(os.path.join(dirpath, d), 0o755)
    os.chmod(root, 0o755)

def mkfs(nodes, compression, out):
    with tempfile.TemporaryDirectory() as tmp:
        build(os.path.join(tmp, "tree"), nodes)
        subprocess.run(["mkfs.ubifs", "-r", os.path.join(tmp, "tree"), "-o", out, "-x", compression] + GEOMETRY,
                       check=True)
    with open(out, "rb") as f:
        return bytearray(f.read())

def crc(data):
    return ~zlib.crc32(data) & 0xFFFFFFFF

def seal(node):
    """Fill in len and CRC of a node whose common header is in place."""
    struct.pack_into("<I", node, 16, len(node))
    struct.pack_into("<I", node, 4, crc(bytes(node[8:])))
    return node

def header(sqnum, node_type, length):
    node = bytearray(length)
    struct.pack_into("<IIQIBB", node, 0, 0x06101831, 0, sqnum, length, node_type, 0)
    return node

def r5(name):
    a = 0
    for c in name:
        c = c - 256 if c > 127 else c
        a = (a + (c << 4) + (c >> 4)) * 11 & 0xFFFFFFFF
    a &= 0x1FFFFFFF
    return a + 3 if a <= 2 else a

def key(inum, key_type, value):
    return struct.pack("<II", inum, key_type << 29 | value)

def dent(sqnum, parent, name, inum, itype):
    node = header(sqnum, 2, 56 + len(name) + 1)
    node[24:32] = key(parent, 2, r5(name))
    struct.pack_into("<QBBH", node, 40, inum, 0, itype, len(name))
    node[56:56 + len(name)] = name
    return seal(node)

def data(sqnum, inum, block, payload):
    node = header(sqnum, 1, 48 + len(payload))
    node[24:32] = key(inum, 1, block)
    struct.pack_into("<IHH", node, 40, len(payload), 0, 0)
    node[48:] = payload
    return seal(node)

def trun(sqnum, inum, old, new):
    node = header(sqnum, 4, 56)
    struct.pack_into("<I", node, 24, inum)
    struct.pack_into("<QQ", node, 40, old, new)
    return seal(node)

def inode(template, sqnum, inum, size, nlink):
    node = bytearray(template)
    struct.pack_into("<Q", node, 8, sqnum)
    node[24:32] = key(inum, 0, 0)
    struct.pack_into("<Q", node, 48, size)
    struct.pack_into("<I", node, 92, nlink)
    return seal(node)

def nodes_in(img):
    """Every node of the image, at 8-byte steps: (position, type, bytes)."""
    pos = 0
    while pos + 24 <= len(img):
        magic, _, _, length, node_type = struct.unpack_from("<IIQIB", img, pos)
        if magic == 0x06101831 and 24 <= length <= len(img) - pos:
            yield pos, node_type, bytes(img[pos:pos + length])
            pos += (length + 7) & ~7
        else:
            pos += 8

def survey(img):
    """Inode numbers by name, inode nodes by number, and the highest sequence number."""
    inums = {}
    inodes = {}
    top = 0
    for _, node_type, node in nodes_in(img):
        top = max(top, struct.unpack_from("<Q", node, 8)[0])
        if node_type == 2:
            name = node[56:56 + struct.unpack_from("<H", node, 50)[0]]
            inums[name] = struct.unpack_from("<Q", node, 40)[0]
        elif node_type == 0:
            inodes[struct.unpack_from("<I", node, 24)[0]] = node
    return inums, inodes, top

def journal_nodes(img):
    inums, inodes, top = survey(img)
    a, gone, b, c, d = (inums[n] for n in (b"a.txt", b"gone", b"b", b"c", b"dir"))
    new = max(inodes) + 1
    seq = iter(range(top + 1, top + 100))
    return [
        data(next(seq), new, 0, b"new content\n"),
        inode(inodes[a], next(seq), new, 12, 1),
        dent(next(seq), 1, b"new.txt", new, 0),
        data(next(seq), a, 0, b"changed\n"),
        inode(inodes[a], next(seq), a, 8, 1),
        dent(next(seq), 1, b"gone", 0, 0),
        inode(inodes[gone], next(seq), gone, 1, 0),
        dent(next(seq), d, b"b", 0, 0),
        dent(next(seq), 1, b"b2", b, 0),
        trun(next(seq), c, 10_000, 5_000),
        data(next(seq), c, 1, C_FILE[4096:5000]),
        inode(inodes[c], next(seq), c, 5_000, 1),
        inode(inodes[c], next(seq), c, 10_000, 1),  # grown again: what the truncation cut reads as zeros
        dent(next(seq), 1, b"cut-short", new, 0)[:40],  # the power went out while writing it
    ], top

def journal(img):
    sb = img[0:4096]
    leb_cnt = struct.unpack_from("<I", sb, 40)[0]
    bud = len(img) // LEB
    struct.pack_into("<I", sb, 40, max(leb_cnt, bud + 1))
    img[0:4096] = seal(bytearray(sb))
    nodes, top = journal_nodes(img)
    body = bytearray()
    for node in nodes:
        body += node + bytes(-len(node) % 8)
    img += body + b"\xff" * (LEB - len(body))
    log = 3 * LEB
    free = next(p for p in range(log, log + LEB, 512) if img[p:p + 4] == b"\xff\xff\xff\xff")
    ref = header(top + 50, 8, 64)
    struct.pack_into("<III", ref, 24, bud, 0, 1)
    img[free:free + 64] = seal(ref)
    return img

def main(out):
    images = {
        "ubifs-lzo.img": mkfs(TREE, "lzo", os.path.join(out, "ubifs-lzo.img")),
        "small-ubifs-zstd.img": mkfs(SMALL, "zstd", os.path.join(out, "small-ubifs-zstd.img")),
        "ubifs-journal.img": journal(mkfs(JOURNAL_BASE, "none", os.path.join(out, "ubifs-journal.img"))),
    }
    for name, img in images.items():
        with open(os.path.join(out, name), "wb") as f:
            f.write(img)

if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
