#!/usr/bin/env python3
"""mktestimg.py OUT [MB] [FILL%] [SEED] - an RDB disk image with one FFS (DOS\\1)
partition, filled with deterministic random files and directories by xdftool."""
import os, random, shutil, subprocess, sys, tempfile
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
from f2p.rdb import make_rdb_disk

out = sys.argv[1]
mb = int(sys.argv[2]) if len(sys.argv) > 2 else 4
fill = int(sys.argv[3]) if len(sys.argv) > 3 else 85
seed = int(sys.argv[4]) if len(sys.argv) > 4 else 1
rnd = random.Random(seed)
tmp = tempfile.mkdtemp()
try:
    tree = os.path.join(tmp, 'tree')
    os.makedirs(tree)
    budget = mb * 1048576 * fill // 100
    dirs = [tree]
    n = 0
    while budget > 0:
        if rnd.random() < 0.15 and len(dirs) < 60:
            d = os.path.join(rnd.choice(dirs), 'dir%03d' % len(dirs))
            os.makedirs(d); dirs.append(d); continue
        size = min(budget, int(rnd.expovariate(1 / 20000)) + rnd.choice((0, 1, 511, 512, 513, 0)))
        with open(os.path.join(rnd.choice(dirs), 'f%04d.bin' % n), 'wb') as f:
            f.write(bytes(rnd.getrandbits(8) for _ in range(size)))
        budget -= size + 1024
        n += 1
    part = os.path.join(tmp, 'part.hdf')
    xdf = os.path.join(os.path.dirname(sys.executable), 'xdftool')
    subprocess.run([xdf, part, 'create', 'size=%dM' % mb, '+', 'format', 'Test', 'ffs'], check=True,
                   stdout=subprocess.DEVNULL)
    for root, ds, fs in os.walk(tree):
        rel = os.path.relpath(root, tree)
        for d in ds:
            subprocess.run([xdf, part, 'makedir', (rel + '/' + d) if rel != '.' else d], check=True, stdout=subprocess.DEVNULL)
        for fn in fs:
            subprocess.run([xdf, part, 'write', os.path.join(root, fn), (rel + '/' + fn) if rel != '.' else fn],
                           check=True, stdout=subprocess.DEVNULL)
    fs_data = os.urandom(20000) if os.environ.get('FS') else None    # stand-in filesystem code
    info = make_rdb_disk(out, part, fs_data=fs_data, tight=bool(os.environ.get('TIGHT')))
    print("%s: %d files, %d dirs, partition %d blocks at %d" % (out, n, len(dirs) - 1, info['blocks'], info['start']))
finally:
    shutil.rmtree(tmp)
