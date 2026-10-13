#!/bin/sh
# tests/c6x-debug.sh - the --debug session (src/Debug.h) on both paths: --run --debug over tests/debug/cl6x-g.out,
# TI's own cl6x 8.2.2 -g build of t.c (its .debug_line is the line table read; boot and rts6740 inside), and
# --debug over step.s, cpp11's -g assembly (the .dwpsn positions). Each transcript is gdb's shape of the answers.
set -u
D=$(cd "$(dirname "$0")" && pwd)
SIM6747=${SIM6747:-$D/../sim6747.exe}
OUT=$D/out-debug; mkdir -p "$OUT"
cd "$D/debug" || exit 2
fail=0
for t in cl6x-g step; do
    if [ "$t" = cl6x-g ]; then "$SIM6747" --run --debug cl6x-g.out < cl6x-g.commands > "$OUT/$t.got" 2>&1; st=$?
    else "$SIM6747" --debug step.s < step.commands > "$OUT/$t.got" 2>&1; st=$?; fi
    echo "exit $st" >> "$OUT/$t.got"
    if diff "$t.expected" "$OUT/$t.got" > "$OUT/$t.diff"; then echo "c6x-debug $t: ok"; else echo "c6x-debug $t: FAILED - $OUT/$t.diff"; head -12 "$OUT/$t.diff"; fail=1; fi
done
exit $fail
