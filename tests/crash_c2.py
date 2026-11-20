#!/usr/bin/env python3
"""crash_c2.py IMAGE EXPECTED.json [DOUBLE] [--torn=MODE] [--sample=N] [--name=P]
Like crash_c.py, but the result is checked against what the real FFS handler read
from the untouched volume (EXPECTED.json, from check/pfsbridge.py): every crashed
and resumed conversion must read back, through amidisk.pfs3read, as exactly that tree,
with no PFS3 consistency problems. Works for any FFS block size."""
import json, os, random, shutil, subprocess, sys, tempfile, time, zlib
HERE = os.path.dirname(os.path.abspath(__file__))
from amidisk.blockdev import BlockDev, Part
from amidisk import rdb, rdbfs
from amidisk.pfs3read import PFS3

TOOL = os.path.join(HERE, '..', 'c', 'f2p_host')
BIN = os.path.join(HERE, '..', 'ref', 'pfs3aio')
BINARY = open(BIN, 'rb').read()
img, expected = sys.argv[1], json.load(open(sys.argv[2]))
expected.pop('__driver_crc__', None)
double = int(sys.argv[3]) if len(sys.argv) > 3 and sys.argv[3].isdigit() else 100
opt = lambda k: next((a.split('=', 1)[1] for a in sys.argv if a.startswith('--' + k + '=')), None)
TORN, SAMPLE, NAME = opt('torn'), int(opt('sample') or 0), opt('name')
work = os.path.join(tempfile.mkdtemp(), 'w.img')


def tool(crash=None):
    args = [TOOL, work, '--bin', BIN] + (['--name', NAME] if NAME else [])
    if crash is not None:
        args += ['--crash-after', str(crash)] + (['--torn', TORN] if TORN else [])
    r = subprocess.run(args, capture_output=True, text=True)
    return r.returncode, r.stdout, r.stderr


def tree():
    d = BlockDev(work)
    p = [x for x in rdb.partitions(d) if not x['invalid'] and (NAME is None or x['name'] == NAME)][0]
    part = Part(d, p['start'], p['blocks'])
    fs = PFS3(part).read()
    errs = list(fs.problems[:5]) + rdbfs.check(d, BINARY)
    if part.read(0)[:4] != b'PFS\x01':
        errs.append("block 0 not PFS\\1")
    by = {e['path']: e for e in fs.entries}

    def data(e):
        c, left = 0, e['fsize']
        for cs, blk in e['chain']:
            for k in range(blk, blk + cs):
                c = zlib.crc32(part.read(k)[:min(512, left)], c)
                left -= 512
        return [e['fsize'], c]
    out = {}
    for e in fs.entries:
        t = e['type']
        out[e['path']] = ('dir' if t == 2 else 'linkdir' if t == 4 else ['soft', e.get('target')] if t == 3
                          else data(by[e['target_path']]) if t == -4 else data(e))
    return out, errs


def attempt(crashes):
    shutil.copyfile(img, work)
    for k in crashes:
        rc, out, err = tool(k)
        if rc == 0:
            break
        if rc != 3:
            return ["run crashed at %d returned %d: %s" % (k, rc, err.strip()[-200:])]
    rc, out, err = tool()
    if rc:
        return ["resume returned %d: %s" % (rc, err.strip()[-300:])]
    t, errs = tree()
    diff = [k for k in set(t) | set(expected) if t.get(k) != expected.get(k)]
    return errs + (["%d entries differ, e.g. %s: %s vs %s" % (len(diff), diff[0], t.get(diff[0]), expected.get(diff[0]))] if diff else [])


shutil.copyfile(img, work)
rc, out, err = tool()
W = int(out.strip().splitlines()[-1].split()[-1])
print("%s: %d block writes in a full run (C tool)%s" % (img, W, ", torn=" + TORN if TORN else ""))
e0 = attempt([])
print("uninterrupted run:", e0 or "matches the FFS handler's view")
points = sorted(random.Random(7).sample(range(W), min(SAMPLE, W))) if SAMPLE else range(W)
t0, fails = time.time(), 0
for k in points:
    e = attempt([k])
    if e:
        fails += 1
        if fails <= 8:
            print("  crash after write %d: %s" % (k, e[:2]))
print("single crashes: %d points, %d failed (%.0f s)" % (len(points), fails, time.time() - t0))
rnd, f2 = random.Random(1), 0
for _ in range(double):
    ks = [rnd.randrange(W), rnd.randrange(W)]
    e = attempt(ks)
    if e:
        f2 += 1
        if f2 <= 8:
            print("  crashes %s: %s" % (ks, e[:2]))
print("double crashes: %d tried, %d failed" % (double, f2))
