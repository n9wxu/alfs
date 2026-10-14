#!/bin/sh
# SIMP-02: text + data of alfs.o at most 4096 bytes on a Cortex-M0+.
# SIMP-04: no function's stack frame over 256 bytes. alfs has no recursion
# and no function pointers but the driver's, so the deepest path is a chain
# of frames; the frames are listed for review and each is bounded here.
set -e
obj=$1 su=$2
size=$(arm-none-eabi-size "$obj" | awk 'NR == 2 { print $1 + $2 }')
echo "code: $size bytes (budget 4096)"
[ "$size" -le 4096 ] || { echo "SIMP-02 fails"; exit 1; }
echo "stack frames:"
sort -t'	' -k2 -n -r "$su" | head -8
worst=$(awk -F'\t' '{ if ($2 > m) m = $2 } END { print m + 0 }' "$su")
[ "$worst" -le 256 ] || { echo "SIMP-04 fails: a frame of $worst"; exit 1; }
