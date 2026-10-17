#!/bin/sh
# SIMP-02: text + data of the stores' objects at most BUDGET bytes on a
# Cortex-M0+. SIMP-04: the deepest chain of stack frames under each public
# call at most 320 bytes, from GCC's call graphs (-fcallgraph-info=su) of
# all of them; the driver's own frames are the caller's to add.
# Usage: check_target.sh BUDGET obj...; each obj's call graph is beside it.
set -e
budget=$1
shift
size=$(arm-none-eabi-size "$@" | awk 'NR > 1 { s += $1 + $2 } END { print s }')
echo "stores: $size bytes (budget $budget)"
[ "$size" -le "$budget" ] || { echo "SIMP-02 fails"; exit 1; }
cis=$(for o in "$@"; do echo "${o%.o}.ci"; done)
python3 - $cis <<'PY'
import re, sys, functools
size, calls = {}, {}
for path in sys.argv[1:]:
    txt = open(path).read()
    for m in re.finditer(r'node: \{ title: "([^"]+)" label: "([^"]*)"', txt):
        s = re.search(r"\\n(\d+) bytes", m.group(2))
        name = m.group(1).split(":")[-1]
        if s or name not in size:
            size[name] = int(s.group(1)) if s else 0
    for m in re.finditer(r'edge: \{ sourcename: "([^"]+)" targetname: "([^"]+)"', txt):
        calls.setdefault(m.group(1).split(":")[-1], []).append(m.group(2).split(":")[-1])
@functools.lru_cache(None)
def depth(n):
    return size.get(n, 0) + max([depth(t) for t in calls.get(n, []) if t != n] or [0])
worst = sorted(((depth(n), n) for n in size if n.startswith("alfs_log_") or n.startswith("alfs_obj_")), reverse=True)
for d, n in worst[:5]:
    print(f"stack {d:4d}  {n}")
if worst[0][0] > 320:
    print("SIMP-04 fails"); sys.exit(1)
PY
