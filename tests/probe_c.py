#!/usr/bin/env python3
"""probe_c.py IMAGE [--torn=MODE] [--sample=N] [--undo=N] - f2p_probe (c/f2p_host --probe),
which the GUI lists partitions with, after a conversion cut off at every block write
(or N sampled ones):

- the state goes only forward: none, stopped (step 3..6), finish, pfs3
- a stopped conversion is at the step it last logged, or the next one
- finish only in step 6, pfs3 only after the last write, never undo or bad
- the probe writes nothing

Then, for N (default 20) of the stopped points, a rollback cut off after its first and
last write and 40 sampled ones: stopped, then undo, then none, never backwards; and a
finished rollback is none."""
import os, random, re, shutil, subprocess, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, '..', 'c', 'f2p_host')
BIN = os.path.join(HERE, '..', 'ref', 'pfs3aio')
img = sys.argv[1]
opt = lambda k: next((a.split('=', 1)[1] for a in sys.argv if a.startswith('--' + k + '=')), None)
TORN, SAMPLE, UNDO = opt('torn'), int(opt('sample') or 0), int(opt('undo') or 20)
tmp = tempfile.mkdtemp()
work, stopped = os.path.join(tmp, 'w.img'), os.path.join(tmp, 's.img')
RANK = {'none': 0, 'stopped': 1, 'finish': 2, 'pfs3': 3}


def tool(path, *a, crash=None):
    args = [TOOL, path, '--bin', BIN] + list(a)
    if crash is not None:
        args += ['--crash-after', str(crash)] + (['--torn', TORN] if TORN else [])
    r = subprocess.run(args, capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


def probe(path):
    before = open(path, 'rb').read()
    r = subprocess.run([TOOL, path, '--probe'], capture_output=True, text=True)
    if open(path, 'rb').read() != before:
        return None, 'the probe wrote to the image'
    f = r.stdout.split('\n')[0].split()
    if r.returncode or len(f) < 3:
        return None, 'probe: %r' % r.stdout.strip()
    return (f[1], int(f[2])), None


def writes(out):
    return int(re.search(r'block writes: (\d+)', out)[1])


shutil.copy(img, work)
rc, out = tool(work)
assert rc == 0, out
W = writes(out)
st, e = probe(work)
assert st == ('pfs3', 0), (st, e)
shutil.copy(img, work)
st, e = probe(work)
assert st == ('none', 0), (st, e)
points = list(range(W))
if SAMPLE and SAMPLE < W:
    points = sorted(random.Random(1).sample(points, SAMPLE))
fails, last, stops = 0, (0, 0), []
for k in points:
    shutil.copy(img, work)
    rc, out = tool(work, crash=k)
    steps = [int(x) for x in re.findall(r'Step (\d)/6', out)]
    n = steps[-1] if steps else 0
    st, e = probe(work)
    if not e:
        s, step = st
        if s not in RANK:
            e = 'state %s' % s
        elif s == 'stopped' and not (max(n, 3) <= step <= max(n, 3) + 1 and step <= 6):
            e = 'stopped at step %d, the log says step %d' % (step, n)
        elif s == 'finish' and n != 6:
            e = 'finish in step %d' % n
        elif s == 'pfs3' and k != W - 1:
            e = 'pfs3 before the last write'
        elif (RANK[s], step) < last:
            e = '%s %d after %s' % (s, step, last)
        else:
            last = (RANK[s], step)
            if s == 'stopped':
                stops.append(k)
    if e:
        fails += 1
        print('crash after write %d: %s' % (k, e))
print('single crashes: %d points, %d failed' % (len(points), fails))

ufails, tried = 0, 0
for k in random.Random(2).sample(stops, min(UNDO, len(stops))):
    shutil.copy(img, stopped)
    tool(stopped, crash=k)
    shutil.copy(stopped, work)
    rc, out = tool(work, '--rollback')
    st, e = probe(work)
    if rc or st != ('none', 0):
        ufails += 1
        print('rollback from write %d: rc %d, then %s %s' % (k, rc, st, e or ''))
        continue
    R, last = writes(out), 0
    for j in sorted(set([0, R - 1] + random.Random(k).sample(range(R), min(40, R)))):
        shutil.copy(stopped, work)
        tool(work, '--rollback', crash=j)
        st, e = probe(work)
        tried += 1
        r = {'stopped': 0, 'undo': 1, 'none': 2}.get(st[0], -1) if st else -1
        if e or r < last:
            ufails += 1
            print('rollback from write %d cut off after write %d: %s %s' % (k, j, st, e or ''))
            break
        last = r
print('rollbacks cut off: %d points from %d stops, %d failed' % (tried, min(UNDO, len(stops)), ufails))
shutil.rmtree(tmp)
sys.exit(1 if fails or ufails else 0)
