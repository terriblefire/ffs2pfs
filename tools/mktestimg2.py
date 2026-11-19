#!/usr/bin/env python3
"""mktestimg2.py OUT MB FILL% SEED [ffs|ffs+intl]
A harder FFS test volume (one xdftool process, chained commands): nested and
big directories, empty files, comments, protection bits, then a quarter of the
files deleted and the holes refilled so later files are fragmented."""
import os, random, shutil, subprocess, sys, tempfile
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
from f2p.rdb import make_rdb_disk

def set_comments(path, comments):
    import struct
    from f2p.blockdev import BlockDev, Part
    from f2p.ffs import FFS
    dev = BlockDev(path, writable_file=True)
    part = Part(dev, 0, dev.nblocks)
    fs = FFS(part).read()
    by = {e['idx']: e for e in fs.entries}
    for e in fs.entries:
        names, x = [], e
        while x is not None:
            names.append(x['name']); x = by.get(x['parent'])
        c = comments.get('/'.join(reversed(names)))
        if c:
            b = bytearray(part.read(e['key']))
            b[328] = len(c); b[329:329 + 79] = c.encode().ljust(79, b'\0')
            struct.pack_into('>I', b, 20, 0)
            struct.pack_into('>I', b, 20, (-sum(struct.unpack('>128I', b))) & 0xffffffff)
            part.write(e['key'], bytes(b))
    dev.close()


out, mb, fill, seed = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
fstype = sys.argv[5] if len(sys.argv) > 5 else 'ffs'
rnd = random.Random(seed)
tmp = tempfile.mkdtemp()
xdf = os.path.join(os.path.dirname(sys.executable), 'xdftool')
try:
    part = os.path.join(tmp, 'part.hdf')
    cmds = [['create', 'chs=%d,1,32' % (mb * 64)], ['format', 'Test%d' % seed, fstype]]
    dirs = ['']
    for i in range(25):
        d = (rnd.choice(dirs) + '/' if dirs else '') + 'd%02d' % i
        d = d.lstrip('/')
        cmds.append(['makedir', d]); dirs.append(d)
    cmds.append(['makedir', 'big']); dirs.append('big')
    files, nfile, comments = [], 0, {}

    def newfile(budget_left):
        global nfile
        d = 'big' if nfile % 4 == 0 else rnd.choice(dirs)
        name = ('%s/' % d if d else '') + 'file%04d_%s' % (nfile, 'x' * rnd.randrange(0, 18))
        size = min(budget_left, rnd.choice((0, 1, 488, 512, 513, 1000, 4096)) if rnd.random() < 0.3
                   else int(rnd.expovariate(1 / 30000)))
        src = os.path.join(tmp, 'f%d' % nfile)
        with open(src, 'wb') as f:
            f.write(rnd.randbytes(size))
        cmds.append(['write', src, name])
        if rnd.random() < 0.2:        # set after xdftool (its comment command is broken)
            comments[name] = ''.join(rnd.choice('abc XYZ.-_09') for _ in range(rnd.randrange(1, 80)))
        if rnd.random() < 0.2:
            cmds.append(['protect', name, rnd.choice(('+rwed+s', '+rwed+p', '+rwed+a', '+rwe', '+red', '+rwd+s+a', '+rwed+h', '+wed'))])
        files.append(name)
        nfile += 1
        return size + 512 * (1 + size // 36000)

    budget = mb * 1048576 * fill // 100
    used = 0
    small = []
    if os.environ.get('FRAG'):            # many 1-3 block files, every other one deleted later
        for i in range(int(os.environ['FRAG'])):
            nm = (dirs[i % len(dirs)] + '/').lstrip('/') + 's%04d' % i
            src = os.path.join(tmp, 's%d' % i)
            with open(src, 'wb') as f:
                f.write(rnd.randbytes(rnd.randrange(1, 1500)))
            cmds.append(['write', src, nm]); files.append(nm); small.append(nm)
            used += 2048
        for nm in small[::2]:
            cmds.append(['delete', nm]); files.remove(nm)
            used -= 1024
    while used < budget * 0.8:
        used += newfile(budget - used)
    for name in rnd.sample(files, len(files) // 4):
        cmds.append(['delete', name])
        files.remove(name)
    while used < budget * 1.05:               # refill the holes: fragmented files
        used += newfile(10 ** 7)
    args = [xdf, '--', part]          # '--': protect strings like -d are not options
    for c in cmds:
        args += c + ['+']
    r = subprocess.run(args[:-1], capture_output=True, text=True)
    if r.returncode:
        print(r.stdout[-800:], r.stderr[-1500:]); sys.exit(1)
    set_comments(part, {k: v for k, v in comments.items() if k in files})
    fs_data = (open(os.environ['FS_BIN'], 'rb').read() if os.environ.get('FS_BIN')
               else os.urandom(20000) if os.environ.get('FS') else None)     # real or stand-in filesystem code
    info = make_rdb_disk(out, part, fs_data=fs_data)
    print("%s: %d files, %d dirs, partition %d blocks at %d" % (out, len(files), len(dirs) - 1, info['blocks'], info['start']))
finally:
    shutil.rmtree(tmp)
