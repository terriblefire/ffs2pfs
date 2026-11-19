#!/usr/bin/env python3
"""handler_links.py CONVERTED_IMAGE [SEED] - link changes by the real pfs3aio
(from the RDB) on a copy of a converted volume: delete objects that have hard
links (pfs3aio promotes a link), delete links, add hard links to objects with
and without links, add soft links. Then walk read-only and compare with what
must be there, and run f2p.pfs3read's checks (link chains included)."""
import os, random, shutil, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = [HERE, os.path.join(HERE, '..')]
from amibridge import HandlerBridge, Path, make_link, rdb_driver
from pfsbridge import walk
from f2p.blockdev import BlockDev, Part
from f2p import rdb
from f2p.pfs3read import PFS3

src = sys.argv[1]
rnd = random.Random(int(sys.argv[2]) if len(sys.argv) > 2 else 1)
img = src + '.lnk'
shutil.copy(src, img)
dev = BlockDev(img); pi = rdb.partitions(dev)[0]
fs = PFS3(Part(dev, pi['start'], pi['blocks'])).read()
links = {}
for e in fs.entries:
    if e['type'] == -4:
        links.setdefault(e['target_path'], []).append(e['path'])
drv, _ = rdb_driver(img)
b = HandlerBridge(Path(img), Path(drv), read_only=False)
before = walk(b)
expect = dict(before)
prot = {e['path']: e['prot'] for e in fs.entries}
# pfs3aio gives a promoted link the link's own protection, not the deleted object's:
# delete only objects anyone may read, so reading through the links does not change
targets = sorted(t for t in links if not prot.get(t, 0) & 8)
rnd.shuffle(targets)


def delete(path):
    d, name = '/'.join(path.strip('/').split('/')[:-1]), path.split('/')[-1]
    lock, _ = b.locate(0, d)
    r = b.delete_object(lock, name)
    b.free_lock(lock)
    return r[0]


ops = []
for t in targets[:3]:                                   # objects with links: a link takes over
    if delete(t):
        expect.pop(t)
        ops.append("deleted %s (%d links)" % (t, len(links[t])))
for t in targets[3:6]:                                   # a link of another object
    l = links[t][0]
    if delete(l):
        expect.pop(l)
        ops.append("deleted link %s" % l)
files = [p for p, v in before.items() if isinstance(v, list) and v[0] != 'soft' and p not in [x for t in targets[:3] for x in [t]]]
for i, t in enumerate(targets[6:9] + rnd.sample(files, 3)):  # new hard links, onto converted chains and fresh ones
    if t not in expect:
        continue
    r = make_link(b, '/newhl%d' % i, t, False)
    if r[0]:
        expect['/newhl%d' % i] = expect[t] if expect[t] != 'dir' else 'linkdir'
        ops.append("linked /newhl%d -> %s" % (i, t))
for i, t in enumerate(('newtarget', 'Vol:x/y')):
    if make_link(b, '/newsl%d' % i, t, True)[0]:
        expect['/newsl%d' % i] = ['soft', t]
b.flush_volume()
del b
b = HandlerBridge(Path(img), Path(drv))
after = walk(b)
bad = [k for k in set(expect) | set(after) if expect.get(k) != after.get(k)]
dev = BlockDev(img)
fs = PFS3(Part(dev, pi['start'], pi['blocks'])).read()
print("; ".join(ops))
print("after the link changes: %d entries, mismatches %d, pfs3read problems %d" % (len(after), len(bad), len(fs.problems)))
for k in sorted(bad)[:10]:
    print("  %s: got %s, want %s" % (k, after.get(k), expect.get(k)))
for x in fs.problems[:10]:
    print("  " + x)
sys.exit(1 if bad or fs.problems else 0)
