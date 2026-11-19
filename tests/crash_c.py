#!/usr/bin/env python3
"""crash_c.py IMAGE [DOUBLE] [--torn=MODE] [--sample=N] - the C tool (c/f2p_host)
cut off after every block write (or N sampled ones), then resumed by a fresh run,
then checked with f2p.verify against the plan made from the untouched image;
plus DOUBLE runs crashed twice. Runs the binary once per crash, on a copy."""
import os, random, shutil, subprocess, sys, tempfile, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..'))
from f2p.blockdev import BlockDev, Part
from f2p import rdb, plan
from f2p.ffs import FFS
from f2p.verify import verify

TOOL = os.path.join(HERE, '..', 'c', 'f2p_host')
BIN = os.path.join(HERE, '..', 'ref', 'pfs3aio')
BINARY = open(BIN, 'rb').read()
img = sys.argv[1]
double = int(sys.argv[2]) if len(sys.argv) > 2 and sys.argv[2].isdigit() else 100
TORN = next((a.split('=', 1)[1] for a in sys.argv if a.startswith('--torn=')), None)
SAMPLE = next((int(a.split('=')[1]) for a in sys.argv if a.startswith('--sample=')), 0)
work = os.path.join(tempfile.mkdtemp(), 'w.img')
d = BlockDev(img); p0 = rdb.partitions(d)[0]; part = Part(d, p0['start'], p0['blocks'])
ref, _ = plan.build(part, FFS(part).read())


def tool(crash=None):
    args = [TOOL, work, '--bin', BIN]
    if crash is not None:
        args += ['--crash-after', str(crash)] + (['--torn', TORN] if TORN else [])
    r = subprocess.run(args, capture_output=True, text=True)
    return r.returncode, r.stdout, r.stderr


def attempt(crashes):
    shutil.copyfile(img, work)
    for k in crashes:
        rc, out, err = tool(k)
        if rc == 0:
            break                                  # finished before the crash point
        if rc != 3:
            return "run crashed at %d returned %d: %s" % (k, rc, err.strip()[-200:])
    rc, out, err = tool()
    if rc:
        return "resume returned %d: %s" % (rc, err.strip()[-300:])
    return None


shutil.copyfile(img, work)
rc, out, err = tool()
W = int(out.strip().splitlines()[-1].split()[-1])
print("%s: %d block writes in a full run (C tool)%s" % (img, W, ", torn=" + TORN if TORN else ""))
points = sorted(random.Random(7).sample(range(W), min(SAMPLE, W))) if SAMPLE else range(W)
t0, fails = time.time(), 0
for k in points:
    e = attempt([k])
    errs = [e] if e else verify(BlockDev(work), p0['name'], ref, binary=BINARY, strict_boot1=not (TORN and k == W - 1))
    if errs:
        fails += 1
        if fails <= 10:
            print("  crash after write %d: %s" % (k, errs[:3]))
print("single crashes: %d points, %d failed (%.0f s)" % (len(points), fails, time.time() - t0))
rnd, f2 = random.Random(1), 0
for _ in range(double):
    ks = [rnd.randrange(W), rnd.randrange(W)]
    e = attempt(ks)
    errs = [e] if e else verify(BlockDev(work), p0['name'], ref, binary=BINARY, strict_boot1=not TORN)
    if errs:
        f2 += 1
        if f2 <= 10:
            print("  crashes %s: %s" % (ks, errs[:3]))
print("double crashes: %d tried, %d failed" % (double, f2))
