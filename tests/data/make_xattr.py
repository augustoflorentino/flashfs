"""The fixed images with extended attributes test_flashfs.c reads.

fwtriage's writers keep no extended attributes, so these come from the
oracles of each format, which copy them from the source tree: mkfs.ubifs
always, mkfs.jffs2 with --with-xattr. Only user.* attributes can be set without privileges; the readers
treat every namespace alike.

    python3 tests/data/make_xattr.py tests/data

Each run writes new bytes (UUID and times); the tests read only names,
values and attributes.
"""

import os
import struct
import subprocess
import sys
import tempfile

BINARY = bytes(range(256)) * 2  # NULs included; mkfs.ubifs 2.3.1 fails near 1 KiB once a file has two attributes
ACL = struct.pack("<I", 2) + b"".join(struct.pack("<HHI", tag, perm, uid) for tag, perm, uid in [
    (0x01, 7, 0xFFFFFFFF), (0x02, 4, 1234), (0x04, 5, 0xFFFFFFFF), (0x10, 5, 0xFFFFFFFF), (0x20, 5, 0xFFFFFFFF)])
ATTRS = {
    "/etc/passwd": [("user.color", b"blue"), ("user.empty", b""), ("user.binary", BINARY)],
    "/etc": [("user.dir", b"d"), ("system.posix_acl_access", ACL), ("system.posix_acl_default", ACL)],
}

def build(root):
    os.makedirs(root + "/etc")
    with open(root + "/etc/passwd", "wb") as f:
        f.write(b"root:x:0:0::/root:/bin/sh\n")
    os.link(root + "/etc/passwd", root + "/etc/passwd-")  # a second name: the same attributes
    with open(root + "/plain", "wb") as f:
        f.write(b"no attributes\n")
    for path, attrs in ATTRS.items():
        for name, value in attrs:
            os.setxattr(root + path, name, value)
    for path in ("", "/etc"):
        os.chmod(root + path, 0o755)
    for path in ("/etc/passwd", "/plain"):
        os.chmod(root + path, 0o644)

def main(out):
    with tempfile.TemporaryDirectory() as tmp:
        tree = os.path.join(tmp, "tree")
        build(tree)
        subprocess.run(["mkfs.ubifs", "-r", tree, "-o", os.path.join(out, "ubifs-xattr.img"), "-x", "none",
                        "-m", "512", "-e", "15872", "-c", "64"], check=True)
        subprocess.run(["mkfs.jffs2", "-r", tree, "-o", os.path.join(out, "jffs2-xattr.img"), "--with-xattr",
                        "-e", "8KiB", "-n"], check=True)

if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
