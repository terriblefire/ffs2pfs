#!/bin/sh
# Every host test of the C tool (c/f2p_host) at every write point, on every test
# image, with clean and torn (garbage) writes: rollback (rollback_c.py), resume
# after a crash (crash_c.py, crash_c2.py with the expected file tree), and the
# state the window lists (probe_c.py).
# Runs JOBS at once (default 8); per-test logs in tests/out/, the summary in
# tests/host_all.log. Run from the repo root, e.g. in Docker:
#   docker run --rm -v $PWD:/w -w /w tftools-hosttest sh tests/run_host_all.sh
JOBS=${JOBS:-8}
mkdir -p tests/out
{
  for t in "" garbage; do
    for i in t4t l8o l8f h16 o8; do
      echo "rb_$i${t:+_$t}|python3 tests/rollback_c.py tests/$i.img${t:+ --torn=$t}"
      echo "fw_$i${t:+_$t}|python3 tests/crash_c.py tests/$i.img 100${t:+ --torn=$t}"
    done
    for i in k8 k8o; do
      echo "rb_$i${t:+_$t}|python3 tests/rollback_c.py tests/$i.img tests/${i}_ffs.json${t:+ --torn=$t}"
      echo "fw_$i${t:+_$t}|python3 tests/crash_c2.py tests/$i.img tests/${i}_ffs.json 100${t:+ --torn=$t}"
    done
    for i in t4t l8o l8f h16 o8 k8 k8o; do
      echo "pr_$i${t:+_$t}|python3 tests/probe_c.py tests/$i.img${t:+ --torn=$t}"
    done
  done
} > tests/out/jobs.txt
xargs -P "$JOBS" -d '\n' -n 1 sh -c 'eval "${1#*|}" > "tests/out/${1%%|*}.log" 2>&1' _ < tests/out/jobs.txt
for n in $(cut -d'|' -f1 tests/out/jobs.txt); do cat tests/out/$n.log; done > tests/host_all.log
grep -E 'failed|Error|Traceback' tests/host_all.log
