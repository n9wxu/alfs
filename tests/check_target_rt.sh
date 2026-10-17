#!/bin/sh
# SIMP-02: text + data of alfs_rtos.o at most BUDGET bytes on a Cortex-M0+.
# SIMP-04: the deepest chain of stack frames under the flash task's
# function at most 512 bytes, through the stores' frames (every call graph
# given); the driver's and the kernel's own frames apart.
# Usage: check_target_rt.sh BUDGET obj ci store_ci...
set -e
budget=$1 obj=$2
shift 2
size=$(arm-none-eabi-size "$obj" | awk 'NR == 2 { print $1 + $2 }')
echo "task layer: $size bytes (budget $budget)"
[ "$size" -le "$budget" ] || { echo "SIMP-02 fails"; exit 1; }
python3 - "$@" <<'PY'
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
d = depth("task_fn")
print(f"stack {d:4d}  the flash task")
if d > 512:
    print("SIMP-04 fails"); sys.exit(1)
PY
