#!/usr/bin/env python3
"""crash.py IMAGE [DOUBLE] - convert with a crash after every block write (and
DOUBLE random crash pairs), resume each time, and verify the result against the
plan made from the untouched image. The image file itself is never written."""
import os, random, sys, time
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
from f2p.blockdev import BlockDev, Part, Crash
from f2p import rdb, plan
from f2p.ffs import FFS
from f2p.convert import Converter, run as f2prun
from f2p.verify import verify

BINARY = open(os.path.expanduser('~/git/tfdemocd/pfs3aio'), 'rb').read()
img = sys.argv[1]
double = int(sys.argv[2]) if len(sys.argv) > 2 and sys.argv[2].isdigit() else 200
dev = BlockDev(img)
p0 = rdb.partitions(dev)[0]
part = Part(dev, p0['start'], p0['blocks'])
ref, _ = plan.build(part, FFS(part).read())      # file list and CRCs to check against


TORN = None
for a in sys.argv:
    if a.startswith('--torn'):
        TORN = a.split('=', 1)[1] if '=' in a else True


def attempt(crashes):
    """Run with a crash after each count in crashes (relative to that run), then to the end."""
    overlay = {}
    for k in crashes:
        d = BlockDev(img, overlay, crash_after=k, torn=TORN)
        try:
            return overlay, f2prun(d, binary=BINARY)              # finished before the crash point
        except Crash:
            pass
    d = BlockDev(img, overlay)
    return overlay, f2prun(d, binary=BINARY)


d = BlockDev(img, {})
f2prun(d, binary=BINARY)
W = d.writes
print("%s: %d block writes in a full run%s" % (img, W, ", torn writes" if TORN else ""))
t0, fails = time.time(), 0
SAMPLE = next((int(a.split('=')[1]) for a in sys.argv if a.startswith('--sample=')), 0)
points = sorted(random.Random(7).sample(range(W), min(SAMPLE, W))) if SAMPLE else range(W)
for k in points:
    try:
        ov, r = attempt([k])
    except Exception as e:
        ov, r = {}, "raised %r" % e
    try:
        errs = verify(BlockDev(img, ov), p0['name'], ref, binary=BINARY, strict_boot1=not (TORN and k == W - 1))
    except Exception as e:
        errs = ["verify raised %r" % e]
    if errs or r != 'done':
        fails += 1
        if fails <= 10:
            print("  crash after write %d: %s %s" % (k, r, errs[:3]))
print("single crashes: %d points, %d failed (%.0f s)" % (len(points), fails, time.time() - t0))
rnd, f2 = random.Random(1), 0
for _ in range(double):
    ks = [rnd.randrange(W), rnd.randrange(W)]
    try:
        ov, r = attempt(ks)
        errs = verify(BlockDev(img, ov), p0['name'], ref, binary=BINARY, strict_boot1=not TORN)
    except Exception as e:
        r, errs = "raised %r" % e, []
    if errs or r != 'done':
        f2 += 1
        if f2 <= 10:
            print("  crashes %s: %s %s" % (ks, r, errs[:3]))
print("double crashes: %d tried, %d failed" % (double, f2))
