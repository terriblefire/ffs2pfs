#!/usr/bin/env python3
"""handler_write.py CONVERTED_IMAGE [DRIVER] [SEED]
Exercise a converted volume with the real pfs3aio handler (AmiFUSE, read-write,
on a copy): delete some original files, make a directory, write new random
files until the disk is full, flush. Then check, read-only with the handler,
that every original file not deleted is unchanged and every new file reads
back, and run f2p.pfs3read's consistency check. A wrong data bitmap shows up
as an original file overwritten by a new one."""
import json, os, random, shutil, subprocess, sys, zlib
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..'))
exec(open(os.path.join(HERE, 'pfsbridge.py')).read().split("img, drv = sys.argv[1], sys.argv[2]")[0])
from f2p.blockdev import BlockDev, Part
from f2p import rdb
from f2p.pfs3read import PFS3

src = sys.argv[1]
drv = sys.argv[2] if len(sys.argv) > 2 else os.path.expanduser('~/git/tfdemocd/pfs3aio')
if drv == 'rdb':
    from amifuse.fuse_fs import extract_embedded_driver
    drv = str(extract_embedded_driver(Path(sys.argv[1]), None, None)[0])
seed = int(sys.argv[3]) if len(sys.argv) > 3 else 1
img = src + '.rw'
shutil.copy(src, img)
rnd = random.Random(seed)


def walk(b):
    out, stack = {}, ['/']
    while stack:
        cur = stack.pop()
        for e in b.list_dir_path(cur):
            p = '/' + e['name'] if cur == '/' else cur + '/' + e['name']
            if e['dir_type'] > 0:
                out[p] = 'dir'; stack.append(p)
            else:
                size = int(e.get('size', 0))
                data = b''.join(b.read_file(p, min(1 << 18, size - o), o) for o in range(0, size, 1 << 18))
                out[p] = [size, zlib.crc32(data)]
    return out


b = HandlerBridge(Path(img), Path(drv), read_only=False)
before = walk(b)
files = sorted(k for k, v in before.items() if v != 'dir')
root, _ = b.locate(0, "")
deleted = set()
for p in files:
    if p.count('/') == 1 and rnd.random() < 0.3:
        r1, r2 = b.delete_object(root, p[1:])
        if r1:
            deleted.add(p)
r1, r2 = b.create_dir(root, 'newdir')
if r1:
    b.free_lock(r1)                      # CreateDir hands back an exclusive lock
new, n, full = {}, 0, False
MAXNEW = int(os.environ.get("MAXNEW", "5000"))
while not full and n < MAXNEW:
    d = '/newdir' if n % 2 else ''
    path = '%s/new%04d.bin' % (d, n)
    data = bytes(rnd.getrandbits(8) for _ in range(rnd.choice((0, 1, 511, 512, 513, 4000, 20000, 65536))))
    h = b.open_file(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC)
    if not h:
        print("open failed:", path); full = True; break
    fh, _ = h
    w = b.write_handle(fh, data) if data else 0
    b.close_file(fh)
    if w != len(data):
        print("short write:", path, w, len(data))
        full = True                      # disk full: keep what was written
        new[path] = None
        break
    new[path] = [len(data), zlib.crc32(data)]
    n += 1
b.flush_volume()
del b

b = HandlerBridge(Path(img), Path(drv))
after = walk(b)
bad = []
for p, v in before.items():
    if p in deleted:
        if p in after:
            bad.append("%s: deleted but still there" % p)
    elif after.get(p) != v:
        bad.append("%s: original changed %s -> %s" % (p, v, after.get(p)))
for p, v in new.items():
    if v is not None and after.get(p) != v:
        bad.append("%s: new file reads back %s, wrote %s" % (p, after.get(p), v))
dev = BlockDev(img)
pi = rdb.partitions(dev)[0]
fs = PFS3(Part(dev, pi['start'], pi['blocks'])).read()
print("deleted %d originals, wrote %d new files (%s), %d originals checked; handler mismatches %d; pfs3read problems %d; free %d" % (
    len(deleted), n, "disk full" if full else "stopped", len(before) - len(deleted), len(bad), len(fs.problems), fs.root['blocksfree']))
for x in bad[:10] + fs.problems[:10]:
    print("  " + x)
sys.exit(1 if bad or fs.problems else 0)
