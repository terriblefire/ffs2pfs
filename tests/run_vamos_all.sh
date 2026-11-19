#!/bin/sh
# The rollback test (rollback_c.py) with the Amiga binary c/ffs2pfs under vamos,
# at every write point of every test image, with clean and torn (garbage)
# writes. N shards of each run at once (default 12), each with its own work
# image and vamos volume dir. Per-shard logs in tests/out/vamos/, the summary in
# tests/rollback_vamos_all.log. Needs vamos (amitools): on the PATH, in .venv,
# or named by VAMOS=. Run from the repo root: sh tests/run_vamos_all.sh
N=${N:-12}
PY=${PYTHON:-python3}
D=tests/out/vamos
OUT=tests/rollback_vamos_all.log
mkdir -p $D
: > $OUT
for img in t4t l8f h16 k8 l8o k8o o8; do
  case $img in k8|k8o) json=tests/${img}_ffs.json ;; *) json= ;; esac
  for torn in "" garbage; do
    tag=$img${torn:+_$torn}
    t0=$(date +%s)
    i=0
    while [ $i -lt $N ]; do
      $PY tests/rollback_c.py tests/$img.img $json --vamos --shard=$i/$N ${torn:+--torn=$torn} > $D/${tag}_$i.log 2>&1 &
      i=$((i+1))
    done
    wait
    $PY - "$D" "$tag" $N $(( $(date +%s) - t0 )) >> $OUT <<'PY'
import re, sys
d, tag, n, secs = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
tot, bad, head = {}, [], ''
for i in range(n):
    for l in open('%s/%s_%d.log' % (d, tag, i)):
        m = re.match(r'(crash|error|double|error2): (\d+) (?:points|tried), (\d+) failed', l)
        if m:
            t = tot.setdefault(m[1], [0, 0]); t[0] += int(m[2]); t[1] += int(m[3])
        elif l.startswith('  ') or 'Error' in l or 'assert' in l:
            bad.append('  shard %d:%s' % (i, l.rstrip()))
        elif 'block writes' in l:
            head = l.strip()
print(head or tag + ': no header (see %s)' % d)
for k in ('crash', 'error', 'double', 'error2'):
    a, f = tot.get(k, [0, 0])
    print('  %s: %d, %d failed' % (k, a, f))
print('  (%d s)' % secs)
print('\n'.join(bad[:20]))
PY
    rm -f tests/_vamos_rb*.img tests/_vamos_rb*.img.c
  done
done
echo "--- all done" >> $OUT
