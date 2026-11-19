#!/usr/bin/env python3
"""rollback_c.py IMAGE [EXPECTED.json] [--torn=MODE] [--sample=N] [--double=N] [--name=P] [--vamos] [--shard=I/N]
The C tool (c/f2p_host, or with --vamos the Amiga binary c/ffs2pfs under vamos) must be able to put the volume back as it was after a
failure at any point before the point of no return:
  crash:  cut off after write k, then --rollback
  error:  write k fails (once): the tool rolls back by itself
  double: cut off after write k, the rollback cut off after write j, --rollback again
  error2: write k fails, a write of the rollback fails too, then --rollback
After each, the whole image must equal the original, block for block, except
blocks the FFS bitmap of the original volume marks free (read here, not by the C
code). With EXPECTED.json some rolled-back images are converted again and must
read as that tree (as crash_c2.py). Any FFS block size. --shard=I/N runs every Nth point (and
its share of the double faults) from the Ith, on its own work image, so N copies can run at once."""
import json, os, random, shutil, struct, subprocess, sys, tempfile, time, zlib
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..'))
from f2p.blockdev import BlockDev, Part
from f2p import rdb, rdbfs
from f2p.pfs3read import PFS3

TOOL = os.path.join(HERE, '..', 'c', 'f2p_host')
BIN = os.path.join(HERE, '..', 'ref', 'pfs3aio')
BINARY = open(BIN, 'rb').read()
args = [a for a in sys.argv[1:] if not a.startswith('--')]
img = args[0]
expected = json.load(open(args[1])) if len(args) > 1 else None
if expected:
    expected.pop('__driver_crc__', None)
opt = lambda k: next((a.split('=', 1)[1] for a in sys.argv if a.startswith('--' + k + '=')), None)
TORN, SAMPLE, NAME = opt('torn'), int(opt('sample') or 0), opt('name')
DOUBLE = int(opt('double') or 100)
VAMOS = '--vamos' in sys.argv
VENV_VAMOS = os.path.join(HERE, '..', '.venv', 'bin', 'vamos')
VAMOS_BIN = os.environ.get('VAMOS') or (VENV_VAMOS if os.path.exists(VENV_VAMOS) else 'vamos')
SHARD, NSHARD = map(int, (opt('shard') or '0/1').split('/'))
VOLS = os.path.join(HERE, 'out', 'vamos', 'vols%d' % SHARD) if VAMOS and NSHARD > 1 else None
if VOLS:
    os.makedirs(VOLS, exist_ok=True)
DOUBLE = -(-DOUBLE // NSHARD)
RC_ASWAS, RC_ERR = (10, 20) if VAMOS else (11, 10)
# vamos: a path relative to the current directory
work = 'tests/_vamos_rb%s.img' % ('' if NSHARD == 1 else SHARD) if VAMOS else os.path.join(tempfile.mkdtemp(), 'w.img')
ORIG = open(img, 'rb').read()


def u32(b, i):
    return struct.unpack_from('>I', b, 4 * i)[0]


def ffs_free_sectors():
    """absolute sectors the original volume's FFS bitmap marks free"""
    d = BlockDev(img)
    p = [x for x in rdb.partitions(d) if not x['invalid'] and (NAME is None or x['name'] == NAME)][0]
    pb = d.read(p['block'])
    spb = u32(pb, 32 + 4) or 1
    bs = 512 * spb
    N = bs // 4
    rd = lambda k: b''.join(d.read(p['start'] + k * spb + i) for i in range(spb))
    n, res = p['blocks'] // spb, p['reserved']
    root = rd((n - 1 + res) // 2)
    T = lambda b, k: u32(b, N - k)
    assert u32(root, 0) == 2 and T(root, 1) == 1, "no root block"
    assert T(root, 50) == 0xffffffff, "bitmap not valid"
    pages = [T(root, k) for k in range(49, 24, -1)]
    ext = T(root, 24)
    while ext:
        e = rd(ext)
        pages += [u32(e, i) for i in range(N - 1)]
        ext = u32(e, N - 1)
    free, blk = set(), res
    for pg in pages:
        if not pg:
            break
        b = rd(pg)
        for i in range(1, N):
            w = u32(b, i)
            for bit in range(32):
                if blk >= n:
                    break
                if w >> bit & 1:
                    free.update(range(p['start'] + blk * spb, p['start'] + blk * spb + spb))
                blk += 1
    return free, p, spb


FREE, P, SPB = ffs_free_sectors()
if SPB == 1:                                     # cross-check with the Python FFS reader
    from f2p.ffs import FFS
    f = FFS(Part(BlockDev(img), P['start'], P['blocks']), P['reserved'])
    f.read()
    assert {P['start'] + k for k in f.free} == FREE, "free maps disagree"


def tool(*extra):
    if VAMOS:                                    # the same options, as the Amiga template has them
        amiga = {'--rollback': 'ROLLBACK', '--crash-after': 'CRASHAFTER=', '--torn': 'TORN=', '--fail-at': 'FAILAT='}
        a, x = [VAMOS_BIN], list(extra)
        if NSHARD > 1:                           # vamos's temp RAM: volume, one per shard: they race otherwise
            a += ['--vols-base-dir', VOLS]
        a += ['c/ffs2pfs', P['name'], 'IMAGE=' + work]
        while x:
            o = amiga[x.pop(0)]
            a.append(o + x.pop(0) if o.endswith('=') else o)
    else:
        a = [TOOL, work, '--bin', BIN] + (['--name', NAME] if NAME else []) + list(extra)
    r = subprocess.run(a, capture_output=True, text=True)
    out = r.stdout.replace('\r', '\n')
    w = int(out.strip().splitlines()[-1].split()[-1]) if 'block writes' in out else -1
    return r.returncode, w, out + r.stderr


def crash(k):
    return ['--crash-after', str(k)] + (['--torn', TORN] if TORN else [])


def diff_vs_orig(allow_b1=False):
    now = open(work, 'rb').read()
    if now == ORIG:
        return []
    bad, C = [], 65536
    for off in range(0, len(ORIG), C):
        if now[off:off + C] == ORIG[off:off + C]:
            continue
        for s in range(off // 512, (off + C) // 512):
            if now[s * 512:s * 512 + 512] != ORIG[s * 512:s * 512 + 512] and s not in FREE:
                if allow_b1 and s == P['start'] + 1:
                    continue
                bad.append(s - P['start'])
    return ["%d blocks differ that FFS uses (partition-relative %s)" % (len(bad), bad[:8])] if bad else []


def tree():
    d = BlockDev(work)
    p = [x for x in rdb.partitions(d) if not x['invalid'] and (NAME is None or x['name'] == NAME)][0]
    part = Part(d, p['start'], p['blocks'])
    fs = PFS3(part).read()
    errs = list(fs.problems[:5]) + rdbfs.check(d, BINARY)
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
    diff = [k for k in set(out) | set(expected) if out.get(k) != expected.get(k)]
    return errs + (["%d entries differ after converting again" % len(diff)] if diff else [])


shutil.copyfile(img, work)
rc, W, out = tool()
assert rc == 0, out
NORET = W - 1          # the last write (block 1 := zeros) follows the point of no return (the PART := PFS\3)
PB = int(out.split('start: plan ')[1].split()[0])
LOCK1 = 2 * PB + 2     # anchor B into block 1: torn, nothing can find the plan any more (FFS ignores block 1)
print("%s: %d block writes in a full run%s; %d sectors free to FFS%s" % (img, W, " (Amiga binary, vamos)" if VAMOS else "",
                                                                     len(FREE), ", torn=" + TORN if TORN else ""))
points = sorted(random.Random(7).sample(range(W), min(SAMPLE, W))) if SAMPLE else list(range(W))
reconvert = set(points[::max(1, len(points) // 8)]) if expected else set()
points = points[SHARD::NSHARD]


def after_noreturn():
    """past the point of no return: a rollback must refuse, a plain run must finish"""
    rc, _, out = tool('--rollback')
    # a torn last write (block 1) leaves no done mark: the volume reads as finished PFS3
    if rc != RC_ERR or not ('point of no return' in out or 'nothing to roll back' in out):
        return ["rollback past the point of no return returned %d" % rc]
    rc, _, out = tool()
    return [] if rc == 0 else ["finishing returned %d: %s" % (rc, out[-200:])]


def check(errs, label, fails):
    if errs:
        fails[0] += 1
        if fails[0] <= 8:
            print("  %s: %s" % (label, errs[:2]))


for kind in ('crash', 'error'):
    t0, fails = time.time(), [0]
    for k in points:
        shutil.copyfile(img, work)
        if kind == 'crash':
            rc, _, out = tool(*crash(k))
            if rc != 3:
                check(["run cut off at %d returned %d" % (k, rc)], "crash %d" % k, fails)
                continue
            if k >= NORET:
                check(after_noreturn(), "crash %d" % k, fails)
                continue
            rc, _, out = tool('--rollback')
            e = [] if rc == 0 else ["rollback returned %d: %s" % (rc, out[-200:])]
        else:
            rc, _, out = tool('--fail-at', str(k))
            if k >= NORET:
                e = [] if rc == RC_ERR and 'complete' in out else ["error at %d (past no return) returned %d" % (k, rc)]
                check(e or after_noreturn(), "error %d" % k, fails)
                continue
            e = [] if rc == RC_ASWAS else ["error at %d returned %d, not rolled back: %s" % (k, rc, out[-200:])]
        e = e or diff_vs_orig(allow_b1=bool(TORN) and kind == 'crash' and k == LOCK1)
        if not e and k in reconvert:
            rc, _, out = tool()
            e = tree() if rc == 0 else ["converting again returned %d" % rc]
        check(e, "%s at write %d" % (kind, k), fails)
    print("%s: %d points, %d failed (%.0f s)" % (kind, len(points), fails[0], time.time() - t0))

rnd = random.Random(1 + SHARD)
for kind in ('double', 'error2'):
    t0, fails = time.time(), [0]
    for _ in range(DOUBLE):
        k = rnd.randrange(NORET)
        shutil.copyfile(img, work)
        if kind == 'double':
            tool(*crash(k))
            shutil.copyfile(work, work + '.c')
            _, R, _ = tool('--rollback')             # how many writes a whole rollback takes from here
            if R <= 0:
                check(diff_vs_orig(allow_b1=bool(TORN) and k == LOCK1), "crash %d (nothing to roll back)" % k, fails)
                continue
            shutil.copyfile(work + '.c', work)
            j = rnd.randrange(R)
            rc, _, out = tool('--rollback', *crash(j))
            label = "crash %d, rollback cut off at %d of %d" % (k, j, R)
            ok_rc = (3,)
        else:
            shutil.copyfile(work, work + '.c')
            _, R, _ = tool('--fail-at', str(k))      # the rollback's writes come after attempt k
            shutil.copyfile(work + '.c', work)
            R -= k
            j = rnd.randrange(max(R, 1))
            rc, _, out = tool('--fail-at', '%d,%d' % (k, k + 1 + j))
            label = "error %d, rollback error at its write %d of %d" % (k, j, R)
            ok_rc = (RC_ERR, RC_ASWAS)
        if rc not in ok_rc:
            check(["first runs returned %d: %s" % (rc, out[-200:])], label, fails)
            continue
        rc, _, out = tool('--rollback')
        e = [] if rc == 0 else ["rollback returned %d: %s" % (rc, out[-200:])]
        check(e or diff_vs_orig(allow_b1=bool(TORN) and kind == 'double' and j == R - 1), label, fails)
    print("%s: %d tried, %d failed (%.0f s)" % (kind, DOUBLE, fails[0], time.time() - t0))
