#!/usr/bin/env python3
"""mklinkimg.py OUT MB SEED [ffs|ofs|ffs+intl] - a test volume with hard and soft
links, made by the real FFS 40.1 handler (AmiFUSE). Files and directories come
from mktestimg2.py; FFS (from the RDB) then adds the links."""
import os, random, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = [os.path.join(HERE, '..'), os.path.join(HERE, '..', 'check')]
from amibridge import HandlerBridge, make_link, Path

out, mb, seed = sys.argv[1], sys.argv[2], int(sys.argv[3])
fstype = sys.argv[4] if len(sys.argv) > 4 else 'ffs'
fill = sys.argv[5] if len(sys.argv) > 5 else '35'
ffs_bin = os.path.join(HERE, '..', 'ref', 'FastFileSystem_40.1')
env = dict(os.environ, FS_BIN=ffs_bin)
subprocess.run([sys.executable, os.path.join(HERE, 'mktestimg2.py'), out, mb, fill, str(seed), fstype], check=True, env=env)
rnd = random.Random(seed)
b = HandlerBridge(Path(out), Path(ffs_bin), read_only=False)
files, dirs, stack = [], [], ['/']
while stack:
    cur = stack.pop()
    for e in b.list_dir_path(cur):
        p = ('' if cur == '/' else cur) + '/' + e['name']
        (dirs if e['dir_type'] > 0 else files).append(p)
        if e['dir_type'] > 0:
            stack.append(p)
made = {'hard-file': 0, 'hard-dir': 0, 'soft': 0, 'failed': 0}
places = ['/'] + dirs
for i in range(40):                                  # hard links to files, some with several links
    t = rnd.choice(files[:15]) if i % 3 else rnd.choice(files)
    r = make_link(b, rnd.choice(places).rstrip('/') + '/hl%02d' % i, t, False)
    made['hard-file' if r[0] else 'failed'] += 1
for i in range(8):                                   # hard links to directories
    t = rnd.choice(dirs)
    r = make_link(b, '/dl%02d' % i, t, False)
    made['hard-dir' if r[0] else 'failed'] += 1
targets = [rnd.choice(files)[1:] for _ in range(6)] + ['Test%d:%s' % (seed, rnd.choice(dirs)[1:]), 'nowhere/missing',
           '/', 'x' * 200, rnd.choice(files).split('/')[-1]]
for i, t in enumerate(targets):                      # soft links: relative, absolute, dangling, long
    r = make_link(b, rnd.choice(places).rstrip('/') + '/sl%02d' % i, t, True)
    made['soft' if r[0] else 'failed'] += 1
b.flush_volume()
print("links made by FFS 40.1:", made)
