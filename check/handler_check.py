#!/usr/bin/env python3
"""handler_check.py ORIG_FFS_IMAGE CONVERTED_IMAGE [DRIVER]
List and read every file of the converted image with the real pfs3aio handler
(AmiFUSE) and compare names, sizes and contents with the original FFS volume
(read by amidisk.ffs)."""
import json, os, subprocess, sys, zlib
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'tests'))
from amidisk.blockdev import BlockDev, Part
from amidisk import rdb
from amidisk.ffs import FFS
from amidisk.verify import ffs_paths

orig, conv = sys.argv[1], sys.argv[2]
drv = sys.argv[3] if len(sys.argv) > 3 else 'rdb'          # default: pfs3aio as installed in the RDB
BINARY = os.path.expanduser('~/git/tfdemocd/pfs3aio')
dev = BlockDev(orig); p = rdb.partitions(dev)[0]; part = Part(dev, p['start'], p['blocks'])
ffs = FFS(part).read()
want = {}
by_idx = {e['idx']: e for e in ffs.entries}
for path, e in ffs_paths(dict(entries=ffs.entries)).items():
    # read-protected files (R bit set = denied): the handler refuses to open them, compare the size only
    if e['kind'] == 'hardlink':
        t = by_idx[e['target']]
        want[path] = 'linkdir' if t['kind'] == 'dir' else [t['size'], ffs.file_crc(t) if not t['protect'] & 8 else None]
    elif e['kind'] == 'softlink':
        want[path] = ['soft', e['link']]
    else:
        want[path] = 'dir' if e['kind'] == 'dir' else [e['size'], ffs.file_crc(e) if not e['protect'] & 8 else None]
out = subprocess.run([sys.executable, os.path.join(HERE, 'pfsbridge.py'), conv, drv],
                     capture_output=True, text=True)
if out.returncode:
    print(out.stderr[-3000:]); sys.exit(1)
res = json.loads(out.stdout)
dcrc = res.pop('__driver_crc__')
if drv == 'rdb':
    print("handler loaded from the RDB: %s" % ("the pfs3aio 19.2 binary" if dcrc == zlib.crc32(open(BINARY, 'rb').read())
                                               else "NOT the pfs3aio binary (crc %08x)" % dcrc))
got = {k: (v if not isinstance(v, list) or v[0] == 'soft' else [v[0], v[1] if want.get(k, [0, 0])[1] is not None else None])
       for k, v in res.items()}
print("links: %d hard to files, %d hard to dirs, %d soft" % (
    sum(1 for e in ffs.entries if e['kind'] == 'hardlink' and by_idx[e['target']]['kind'] == 'file'),
    sum(1 for v in want.values() if v == 'linkdir'), sum(1 for v in want.values() if isinstance(v, list) and v[0] == 'soft')))
print("read-protected files (size compared only): %d" % sum(1 for v in want.values() if isinstance(v, list) and v[1] is None))
bad = [k for k in set(want) | set(got) if want.get(k) != got.get(k)]
# the handler shows names in their stored case; FFS keeps them too, so compare exactly
print("handler: %d entries (%d files), original: %d; mismatches: %d" % (
    len(got), sum(1 for v in got.values() if v != 'dir'), len(want), len(bad)))
for k in sorted(bad)[:10]:
    print("  %s: handler %s, ffs %s" % (k, got.get(k), want.get(k)))
sys.exit(1 if bad else 0)
