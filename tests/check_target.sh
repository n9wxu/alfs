#!/bin/sh
# SIMP-02: text + data of alfs.o at most 6144 bytes on a Cortex-M0+.
# SIMP-04: the deepest chain of stack frames under each public call at
# most 320 bytes, from GCC's call graph (-fcallgraph-info=su); the driver's
# own frames are the caller's to add.
set -e
obj=$1 ci=$2
size=$(arm-none-eabi-size "$obj" | awk 'NR == 2 { print $1 + $2 }')
echo "code: $size bytes (budget 6144)"
[ "$size" -le 6144 ] || { echo "SIMP-02 fails"; exit 1; }
python3 - "$ci" <<'PY'
import re, sys, functools
txt = open(sys.argv[1]).read()
size = {m.group(1): int(re.search(r"\\n(\d+) bytes", m.group(2)).group(1)) if re.search(r"\\n(\d+) bytes", m.group(2)) else 0
        for m in re.finditer(r'node: \{ title: "([^"]+)" label: "([^"]*)"', txt)}
calls = {}
for m in re.finditer(r'edge: \{ sourcename: "([^"]+)" targetname: "([^"]+)"', txt):
    calls.setdefault(m.group(1), []).append(m.group(2))
@functools.lru_cache(None)
def depth(n):
    return size.get(n, 0) + max([depth(t) for t in calls.get(n, []) if t != n] or [0])
worst = sorted(((depth(n), n) for n in size if n.startswith("alfs_")), reverse=True)
for d, n in worst[:5]:
    print(f"stack {d:4d}  {n}")
if worst[0][0] > 320:
    print("SIMP-04 fails"); sys.exit(1)
PY
