"""M3: the Linux kernel as oracle.

Mounts the images the round trip left in <work> (run that first) and checks
that what the kernel shows matches what flashfs reads, entry by entry: path,
mode, owner, device, size, sha256 of every file, link target. Needs root and
the mtdram, mtdblock, nandsim, ubi and ubifs modules, so it runs in CI only:

    sudo python3 tests/kernel/oracle.py <dump binary> <round-trip work dir>

The kernel mounts JFFS2 and cramfs only in its own byte order, so on a
little-endian runner only the little-endian images are checked. UBI volumes
are compared as /dev/ubi0_N, whole, against the file flashfs shows for each;
the UBIFS in a UBI volume is mounted from it, as a device would.
"""

import contextlib
import glob
import hashlib
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "roundtrip"))
from roundtrip import actual, compare, expected  # noqa: E402

NAND2K = ["first_id_byte=0x20", "second_id_byte=0xaa", "third_id_byte=0x00", "fourth_id_byte=0x15"]

def sh(*cmd):
    subprocess.run(cmd, check=True)

def modprobe(module, *params):
    subprocess.run(["modprobe", "-r", module], check=False)
    sh("modprobe", module, *params)

def mounted(image, fstype, source, want_from_dump):
    with tempfile.TemporaryDirectory() as mnt:
        sh("mount", "-t", fstype, "-o", "ro", source, mnt)
        try:
            kernel = expected(mnt)
        finally:
            sh("umount", mnt)
    return compare(kernel, want_from_dump)

def check_jffs2(dump, image):
    got, err = actual(dump, image)
    if got is None:
        return [err]
    erase = int(image.rsplit("-", 1)[1].rstrip("k.img"))
    size_kib = -(-os.path.getsize(image) // 1024 // erase) * erase
    modprobe("mtdram", "total_size=%d" % size_kib, "erase_size=%d" % erase)
    modprobe("mtdblock")
    sh("dd", "if=" + image, "of=/dev/mtdblock0", "bs=64k", "status=none")
    try:
        return mounted(image, "jffs2", "mtd0", got)
    finally:
        subprocess.run(["modprobe", "-r", "mtdblock", "mtdram"], check=False)

def check_cramfs(dump, image):
    got, err = actual(dump, image)
    if got is None:
        return [err]
    loop = subprocess.run(["losetup", "-f", "--show", "-r", image], check=True, capture_output=True,
                          text=True).stdout.strip()
    try:
        return mounted(image, "cramfs", loop, got)
    finally:
        sh("losetup", "-d", loop)

@contextlib.contextmanager
def attached(image):
    """The UBI image written to a simulated 2 KiB-page NAND and attached as ubi0."""
    modprobe("nandsim", *NAND2K)
    try:
        sh("ubiformat", "-y", "-q", "-f", image, "/dev/mtd0")
        sh("ubiattach", "-m", "0", "-d", "0")
        yield
    finally:
        subprocess.run(["ubidetach", "-d", "0"], check=False)
        subprocess.run(["modprobe", "-r", "nandsim"], check=False)

def check_ubi(dump, image):
    got, err = actual(dump, image)
    if got is None:
        return [err]
    by_name = {bytes.fromhex(g[0]).decode(): g for g in got}
    problems = []
    with attached(image):
        for node in sorted(glob.glob("/sys/class/ubi/ubi0_*/name")):
            vol = open(node).read().strip()
            with open("/dev/" + os.path.basename(os.path.dirname(node)), "rb") as f:
                data = f.read()
            g = by_name.pop("/" + vol, None)
            if g is None or (g[5], g[6]) != (len(data), hashlib.sha256(data).hexdigest()):
                problems.append("volume %s: kernel %d bytes, flashfs %s" % (vol, len(data), g and g[5]))
    problems += ["extra volume " + p for p in by_name]
    return problems

def check_ubifs(dump, image):
    """The UBIFS in volume rootfs, read by flashfs through the UBI handle and mounted by the kernel."""
    got, err = actual(dump, image, "rootfs")
    if got is None:
        return [err]
    with attached(image):
        return mounted(image, "ubifs", "ubi0:rootfs", got)

def run(dump, work):
    native = "-l" if sys.byteorder == "little" else "-b"
    cases = [(p, check_jffs2) for p in sorted(glob.glob(os.path.join(work, "jffs2-*%s-*k.img" % native)))]
    cases += [(p, check_cramfs) for p in sorted(glob.glob(os.path.join(work, "cramfs-%s*.img" % sys.byteorder)))]
    cases += [(os.path.join(work, "ubi-nand2k.img"), check_ubi)]
    cases += [(os.path.join(work, "ubi-with-ubifs-lzo-nand2k.img"), check_ubifs)]
    failed = 0
    for path, check in cases:
        problems = check(dump, path)
        failed += bool(problems)
        print("%-36s %s" % (os.path.basename(path), "ok" if not problems else "FAIL"))
        for p in problems[:10]:
            print("  " + p)
    print("kernel oracle: %d images, %d failed" % (len(cases), failed))
    return 1 if failed or not cases else 0

if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    sys.exit(run(os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])))
