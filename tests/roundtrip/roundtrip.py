"""M2: round trip through the official tools.

Builds a fixed tree (files from 0 B to 8 MiB, sparse files, links, devices,
UTF-8 and 255-byte names, 10 000 files in one directory, 20 levels), packs it
with mkfs.jffs2 (each compression, both byte orders, 3 erase sizes),
mkfs.cramfs (both byte orders, with explicit holes and with the 512-byte
pad) and ubinize, and checks that every file comes back from flashfs with the
same content, mode, uid, gid, device number and link target.

Runs under fakeroot so the tree can hold owners and devices:

    fakeroot -- python3 tests/roundtrip/roundtrip.py <dump binary> <work dir>
"""

import hashlib
import os
import shutil
import stat
import subprocess
import sys

KiB = 1024
MiB = 1024 * KiB

LONG_NAME_CRAMFS = "n" * 252
LONG_NAME_JFFS2 = "N" * 255

def pattern(n, seed):
    """Bytes that no compressor flattens and that differ per file."""
    out = bytearray()
    block = 0
    while len(out) < n:
        out += hashlib.sha256(b"%d:%d" % (seed, block)).digest()
        block += 1
    return bytes(out[:n])

def put(path, data, mode=0o644, uid=0, gid=0):
    with open(path, "wb") as f:
        f.write(data)
    os.chown(path, uid, gid)
    os.chmod(path, mode)

def common_tree(root):
    os.makedirs(root)
    sizes = [0, 1, 4095, 4096, 4097, 65536, 131073, 1 * MiB + 3, 8 * MiB]
    os.mkdir(os.path.join(root, "sizes"))
    for i, n in enumerate(sizes):
        put(os.path.join(root, "sizes", "s%d" % n), pattern(n, i), 0o640 + i % 8, uid=1000 + i, gid=100 + i)
    put(os.path.join(root, "sizes", "text"), b"hello, flash\n" * 5000, 0o600)
    put(os.path.join(root, "sizes", "zeros"), bytes(3 * MiB), 0o644)

    sparse = os.path.join(root, "sparse")
    with open(sparse, "wb") as f:
        f.write(b"head")
        f.seek(1 * MiB)
        f.write(b"middle")
        f.seek(3 * MiB - 4)
        f.write(b"tail")
    os.chmod(sparse, 0o644)

    links = os.path.join(root, "links")
    os.mkdir(links)
    os.symlink("../sizes/s1", os.path.join(links, "relative"))
    os.symlink("/etc/passwd", os.path.join(links, "absolute"))
    os.symlink("nowhere", os.path.join(links, "dangling"))
    os.symlink("x" * 1000, os.path.join(links, "long-target"))
    os.symlink("relative", os.path.join(links, "chain"))

    dev = os.path.join(root, "dev")
    os.mkdir(dev)
    os.mknod(os.path.join(dev, "tty0"), 0o620 | stat.S_IFCHR, os.makedev(4, 0))
    os.mknod(os.path.join(dev, "ttyS1"), 0o660 | stat.S_IFCHR, os.makedev(4, 65))
    os.mknod(os.path.join(dev, "sda1"), 0o660 | stat.S_IFBLK, os.makedev(8, 1))
    os.mknod(os.path.join(dev, "mtdblock255"), 0o600 | stat.S_IFBLK, os.makedev(31, 255))
    os.mkfifo(os.path.join(dev, "fifo"), 0o644)
    os.mknod(os.path.join(dev, "sock"), 0o755 | stat.S_IFSOCK)
    for p, uid, gid in (("tty0", 0, 5), ("ttyS1", 0, 20), ("sda1", 0, 6)):
        os.chown(os.path.join(dev, p), uid, gid)

    names = os.path.join(root, "names")
    os.mkdir(names)
    for name in ("ação", "日本語", "emoji-\U0001f600", "space name", "-dash", "a.b.c", LONG_NAME_CRAMFS):
        put(os.path.join(names, name), name.encode() + b"\n")

    many = os.path.join(root, "many")
    os.mkdir(many)
    for i in range(10000):
        put(os.path.join(many, "f%05d" % i), b"%d\n" % i, 0o644, uid=i % 65536, gid=i % 256)

    deep = root
    for level in range(20):
        deep = os.path.join(deep, "d%02d" % level)
        os.mkdir(deep)
        os.chmod(deep, 0o750 if level % 2 else 0o755)
    put(os.path.join(deep, "bottom"), b"twenty levels down\n")

    special = os.path.join(root, "modes")
    os.mkdir(special)
    put(os.path.join(special, "setuid"), b"s", 0o4755)
    put(os.path.join(special, "setgid"), b"g", 0o2755)
    put(os.path.join(special, "none"), b"0", 0o000)
    os.mkdir(os.path.join(special, "sticky"))
    os.chmod(os.path.join(special, "sticky"), 0o1777)
    os.mkdir(os.path.join(root, "empty"))
    os.chown(root, 0, 0)
    os.chmod(root, 0o755)

def jffs2_extras(root):
    """What JFFS2 holds and cramfs cannot: wide ids and 255-byte names.

    Not here, because mkfs.jffs2 2.3.1 cannot write them: device numbers past
    8 bits (it stores (major << 8) + minor in 16 bits) and link targets past
    254 bytes (it stores 254 and an isize of the full length). The 32-bit
    device encoding is covered by test_flashfs; the long target is checked
    against what the kernel reads, below.
    """
    extra = os.path.join(root, "jffs2")
    os.mkdir(extra)
    put(os.path.join(extra, LONG_NAME_JFFS2), b"255\n")
    put(os.path.join(extra, "wide-ids"), b"ids\n", 0o644, uid=65000, gid=60000)

def expected(root):
    """The listing dump prints, built from the tree on disk."""
    lines = []
    for dirpath, dirnames, filenames in os.walk(root):
        for name in sorted(dirnames + filenames):
            path = os.path.join(dirpath, name)
            st = os.lstat(path)
            rel = "/" + os.path.relpath(path, root)
            size = st.st_size
            digest = "-"
            target = "-"
            if stat.S_ISREG(st.st_mode):
                with open(path, "rb") as f:
                    digest = hashlib.sha256(f.read()).hexdigest()
            elif stat.S_ISLNK(st.st_mode):
                target = os.readlink(path).encode().hex()
            elif stat.S_ISDIR(st.st_mode):
                size = None
            else:
                size = 0
            major = os.major(st.st_rdev) if stat.S_ISCHR(st.st_mode) or stat.S_ISBLK(st.st_mode) else 0
            minor = os.minor(st.st_rdev) if stat.S_ISCHR(st.st_mode) or stat.S_ISBLK(st.st_mode) else 0
            lines.append((rel.encode().hex(), "%o" % st.st_mode, st.st_uid, st.st_gid, "%d,%d" % (major, minor),
                          size, digest, target))
    return sorted(lines)

def actual(dump, image):
    """The listing from memory; reading through the callback window must print the same."""
    out = subprocess.run([dump, image], capture_output=True, check=False)
    if out.returncode != 0 or out.stderr:
        return None, out.stderr.decode(errors="replace").strip()
    through_cb = subprocess.run([dump, "-c", image], capture_output=True, check=False)
    if (through_cb.returncode, through_cb.stdout, through_cb.stderr) != (0, out.stdout, b""):
        return None, "open_cb differs from open_mem: " + through_cb.stderr.decode(errors="replace").strip()
    lines = []
    for line in out.stdout.decode().splitlines():
        path, mode, uid, gid, dev, size, digest, target = line.split(" ")
        lines.append((path, mode, int(uid), int(gid), dev, int(size), digest, target))
    return sorted(lines), ""

def compare(want, got):
    """Directory sizes are the format's own (cramfs counts directory bytes): not compared."""
    got_by_path = {g[0]: g for g in got}
    problems = []
    for w in want:
        g = got_by_path.pop(w[0], None)
        if g is None:
            problems.append("missing " + bytes.fromhex(w[0]).decode(errors="replace"))
        elif (w[:5], w[6:]) != (g[:5], g[6:]) or (w[5] is not None and w[5] != g[5]):
            problems.append("differs %s\n  want %s\n  got  %s" % (bytes.fromhex(w[0]).decode(errors="replace"), w, g))
    problems += ["extra " + bytes.fromhex(p).decode(errors="replace") for p in got_by_path]
    return problems

def jffs2_images(tree, work):
    modes = {
        "none": ["-m", "none"],
        "zlib": ["-x", "rtime", "-x", "lzo"],
        "rtime": ["-x", "zlib", "-x", "lzo"],
        "lzo": ["-X", "lzo", "-x", "zlib", "-x", "rtime"],
    }
    for compr, flags in modes.items():
        for order in ("-l", "-b"):
            for erase in (16 * KiB, 64 * KiB, 256 * KiB):
                name = "jffs2-%s%s-%dk.img" % (compr, order, erase // KiB)
                out = os.path.join(work, name)
                subprocess.run(["mkfs.jffs2", "-r", tree, "-o", out, order, "-e", str(erase), "-p"] + flags,
                               check=True)
                yield name, out

def cramfs_images(tree, work):
    for flags in (["-N", "little"], ["-N", "big"], ["-N", "little", "-z"], ["-N", "big", "-z", "-p"]):
        name = "cramfs%s.img" % "".join(flags).replace("-N", "-")
        out = os.path.join(work, name)
        subprocess.run(["mkfs.cramfs"] + flags + [tree, out], check=True, stdout=subprocess.DEVNULL)
        yield name, out

def ubi_images(work, payloads):
    """Volumes hold the payloads; flashfs shows each as a file padded with 0xff to its reserved size."""
    geometries = {"nand2k": ["-p", "128KiB", "-m", "2048", "-s", "512"],
                  "nand4k": ["-p", "256KiB", "-m", "4096"],
                  "nor": ["-p", "64KiB", "-m", "1"]}
    ini = os.path.join(work, "ubi.ini")
    with open(ini, "w") as f:
        for i, (vol, path, vtype) in enumerate(payloads):
            f.write("[%s]\nmode=ubi\nimage=%s\nvol_id=%d\nvol_type=%s\nvol_name=%s\n" % (vol, path, i, vtype, vol))
            if vtype == "dynamic":
                f.write("vol_size=%d\n" % (os.path.getsize(path) + 2 * MiB))
    for geometry, flags in geometries.items():
        name = "ubi-%s.img" % geometry
        out = os.path.join(work, name)
        subprocess.run(["ubinize", "-o", out] + flags + [ini], check=True)
        yield name, out

def check_ubi(dump, image, payloads):
    got, err = actual(dump, image)
    if got is None:
        return [err]
    got_by_path = {bytes.fromhex(g[0]).decode(): g for g in got}
    problems = []
    for vol, path, vtype in payloads:
        g = got_by_path.get("/" + vol)
        with open(path, "rb") as f:
            data = f.read()
        if g is None:
            problems.append("missing volume " + vol)
            continue
        padded = data + b"\xff" * (g[5] - len(data))
        if g[5] < len(data) or hashlib.sha256(padded).hexdigest() != g[6]:
            problems.append("volume %s (%s): content differs" % (vol, vtype))
    return problems

def run(dump, work):
    if os.path.exists(work):
        shutil.rmtree(work)
    os.makedirs(work)
    common = os.path.join(work, "tree-common")
    common_tree(common)
    full = os.path.join(work, "tree-jffs2")
    common_tree(full)
    jffs2_extras(full)

    total = 0
    failed = 0
    want_full = [w if w[0] != "/links/long-target".encode().hex() else w[:5] + (1000, "-", w[7][:2 * 254])
                 for w in expected(full)]
    want_common = expected(common)
    images = [(n, p, want_full) for n, p in jffs2_images(full, work)]
    images += [(n, p, want_common) for n, p in cramfs_images(common, work)]
    for name, path, want in images:
        got, err = actual(dump, path)
        problems = [err] if got is None else compare(want, got)
        total += 1
        failed += bool(problems)
        files = sum(1 for w in want)
        print("%-28s %6d entries  %s" % (name, files, "ok" if not problems else "FAIL"))
        for p in problems[:10]:
            print("  " + p)

    payloads = [("rootfs", os.path.join(work, "jffs2-zlib-l-64k.img"), "dynamic"),
                ("kernel", os.path.join(work, "tree-common", "sizes", "s%d" % (8 * MiB)), "static"),
                ("tiny", os.path.join(work, "tree-common", "sizes", "s1"), "static")]
    for name, path in ubi_images(work, payloads):
        problems = check_ubi(dump, path, payloads)
        total += 1
        failed += bool(problems)
        print("%-28s %6d volumes  %s" % (name, len(payloads), "ok" if not problems else "FAIL"))
        for p in problems:
            print("  " + p)
    print("roundtrip: %d images, %d failed" % (total, failed))
    return 1 if failed else 0

if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    sys.exit(run(os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])))
