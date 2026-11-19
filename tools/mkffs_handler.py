#!/usr/bin/env python3
"""mkffs_handler.py OUT MB SPB SEED [ofs|ffs] - a test volume made entirely by the real
FFS 40.1 (AmiFUSE): RDB with SectorPerBlock=SPB, formatted by FFS, then directories,
files (random sizes), hard and soft links written through FFS."""
import os, random, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = [os.path.join(HERE, '..'), os.path.join(HERE, '..', 'check')]
from f2p.rdb import make_rdb_disk
from amibridge import HandlerBridge, Path, make_link
from amifuse.fuse_fs import format_volume

out, mb, spb, seed = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
kind = sys.argv[5] if len(sys.argv) > 5 else 'ffs'
fill = int(sys.argv[6]) if len(sys.argv) > 6 else 70
FFSBIN = os.path.join(HERE, '..', 'ref', 'FastFileSystem_40.1')
part = tempfile.mktemp()
open(part, 'wb').truncate(mb * 1048576)
make_rdb_disk(out, part, dostype=0x444F5301 if kind == 'ffs' else 0x444F5300, spb=spb)
os.unlink(part)
format_volume(Path(out), Path(FFSBIN), None, 'DH0', 'Test%d' % seed)
rnd = random.Random(seed)
b = HandlerBridge(Path(out), Path(FFSBIN), read_only=False)
root, _ = b.locate(0, '')
dirs = ['']
for i in range(12):
    parent = rnd.choice(dirs)
    plock, _ = b.locate(0, parent) if parent else (root, 0)
    name = 'd%02d' % i
    l, _ = b.create_dir(plock, name)
    if l:
        b.free_lock(l)
        dirs.append((parent + '/' + name).strip('/'))
    if parent:
        b.free_lock(plock)
files, used = [], 0
budget = mb * 1048576 * fill // 100
while used < budget:
    d = rnd.choice(dirs)
    path = '/' + (d + '/' if d else '') + 'f%03d' % len(files)
    size = min(budget - used, rnd.choice((0, 1, 511, 512, 513, 1023, 1024, 1025, 4096)) if rnd.random() < 0.3 else int(rnd.expovariate(1 / 20000)))
    h = b.open_file(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC)
    if not h:
        break
    fh, _ = h
    if size:
        b.write_handle(fh, rnd.randbytes(size))
    b.close_file(fh)
    files.append(path)
    used += size + 2048
for i in range(10):
    make_link(b, '/hl%02d' % i, rnd.choice(files), False)
for i in range(4):
    make_link(b, '/sl%02d' % i, rnd.choice(files).strip('/'), True)
make_link(b, '/dl0', rnd.choice(dirs[1:]), False)
b.flush_volume()
print("%s: %d MB, %d-byte blocks, %d files, %d dirs, links, made by FFS 40.1" % (out, mb, 512 * spb, len(files), len(dirs) - 1))
