#!/usr/bin/env python3
"""convert_one.py IMAGE OUT: plan stats, convert (overlay), verify, write the result to OUT."""
import sys, time, shutil, os
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from f2p.blockdev import BlockDev, Part
from f2p import rdb, plan
from f2p.ffs import FFS
from f2p.convert import run
from f2p.verify import verify
img, out = sys.argv[1], sys.argv[2]
BINARY = open(os.path.expanduser('~/git/tfdemocd/pfs3aio'), 'rb').read()
dev = BlockDev(img); p = rdb.partitions(dev)[0]; part = Part(dev, p['start'], p['blocks'])
ffs = FFS(part).read(); body, _ = plan.build(part, ffs)
files = [e for e in body['entries'] if e['kind'] == 'file']
print("%s volume %r" % ('OFS' if ffs.ofs else 'FFS', ffs.volname))
frag = sum(1 for e in files if len(e['extents']) > 1)
print("%s: %d files (%d fragmented, %d empty, %d with comments, %d protect bits), %d dirs; %d of %d blocks free; "
      "reserved area %d..%d, %d blocks to move; %d PFS3 sectors" % (
      img, len(files), frag, sum(1 for e in files if e['size'] == 0), sum(1 for e in body['entries'] if e['comment']),
      sum(1 for e in body['entries'] if e['protect']), len(body['entries']) - len(files), len(ffs.free), part.n,
      body['layout']['first'], body['layout']['end'] - 1, len(body['moves']), body['reserved_used']))
ov = {}; d = BlockDev(img, ov); t = time.time(); r = run(d, binary=BINARY, log=print)
print("convert: %s, %d block writes (%.0f s); verify: %s" % (r, d.writes, time.time() - t,
      verify(BlockDev(img, ov), p['name'], body, binary=BINARY) or "ok"))
shutil.copy(img, out)
with open(out, 'r+b') as f:
    for k, b in ov.items():
        f.seek(k * 512); f.write(b)
