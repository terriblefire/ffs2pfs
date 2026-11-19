#!/usr/bin/env python3
"""cmp_c.py IMAGE PYTHON_RESULT: convert a copy of IMAGE with the C tool (c/f2p_host),
then compare it with the Python model's result block for block: everything
outside the partition, the partition's boot blocks and reserved area, and every
data block PFS3 has in use. Only free blocks may differ (the plan and journal).
Then check the C result with f2p.verify."""
import os, shutil, subprocess, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..'))
from f2p.blockdev import BlockDev, Part
from f2p import rdb, plan
from f2p.ffs import FFS
from f2p.pfs3read import PFS3
from f2p.verify import verify

img, pyres = sys.argv[1], sys.argv[2]
BIN = os.path.join(HERE, '..', 'ref', 'pfs3aio')
work = tempfile.mkdtemp()
cimg = os.path.join(work, 'c.img')
shutil.copy(img, cimg)
r = subprocess.run([os.path.join(HERE, '..', 'c', 'f2p_host'), cimg, '--bin', BIN], capture_output=True, text=True)
print(r.stdout.strip().splitlines()[-1] if r.stdout.strip() else '', r.stderr.strip())
if r.returncode:
    print(r.stdout[-2000:]); sys.exit(1)
a, b = BlockDev(pyres), BlockDev(cimg)
p = rdb.partitions(a)[0]
fs = PFS3(Part(a, p['start'], p['blocks'])).read()
check = [k for k in range(a.nblocks) if not (p['start'] <= k < p['start'] + p['blocks'])]
check += [p['start'] + k for k in range(fs.root['lastreserved'] + 1)]
check += [p['start'] + k for k in sorted(fs.used_data)]
diff = [k for k in check if a.read(k) != b.read(k)]
print("compared %d blocks with the Python result: %d differ%s" % (len(check), len(diff),
      (" (e.g. %s)" % [k - p['start'] for k in diff[:8]]) if diff else ""))
d = BlockDev(img); pp = rdb.partitions(d)[0]; part = Part(d, pp['start'], pp['blocks'])
body, _ = plan.build(part, FFS(part).read())
errs = verify(BlockDev(cimg), pp['name'], body, binary=open(BIN, 'rb').read())
print("verify of the C result:", errs[:5] or "ok")
shutil.rmtree(work)
sys.exit(1 if diff or errs else 0)
