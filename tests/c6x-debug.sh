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
# lines: the whole chain - cpp11 -g, ASM6x's .debug_line, LNK6x carrying it, rts6x.lib - built here when the four
# are at hand (CPP11, ASM6X, LNK6X, TIRTS name them; the defaults are the sibling checkouts), and skipped by name
# when not: the image depends on the runtime's build, so it is not kept.
CPP11=${CPP11:-$D/../../../Compilers/C++Optimize/cpp11.exe}; ASM6X=${ASM6X:-$D/../../ASM6x/build/asm6x.exe}
LNK6X=${LNK6X:-$D/../../LNK6x/build/lnk6x.exe}; TIRTS=${TIRTS:-$D/../../RTS6x/build}
cases="cl6x-g step"
if [ -x "$CPP11" ] && [ -x "$ASM6X" ] && [ -x "$LNK6X" ] && [ -f "$TIRTS/rts6x.lib" ] &&
   "$CPP11" -S -arch tms6747 -g -nologo lines.cpp -o "$OUT/lines.s" 2> "$OUT/lines.log" &&
   "$ASM6X" "$OUT/lines.s" -o "$OUT/lines.obj" 2>> "$OUT/lines.log" &&
   "$LNK6X" -mv6740 --abi=eabi -i "$TIRTS" link.cmd "$OUT/lines.obj" -l rts6x.lib -o "$OUT/lines.out" >> "$OUT/lines.log" 2>&1; then
    cases="$cases lines"
else echo "c6x-debug lines: skipped - cpp11, asm6x, lnk6x and rts6x.lib are not all here (CPP11, ASM6X, LNK6X, TIRTS name them)"; fi
for t in $cases; do
    if [ "$t" = cl6x-g ]; then "$SIM6747" --run --debug cl6x-g.out < cl6x-g.commands > "$OUT/$t.got" 2>&1; st=$?
    elif [ "$t" = lines ]; then ( cd "$OUT" && "$SIM6747" --run --debug lines.out < "$D/debug/lines.commands" ) > "$OUT/$t.got" 2>&1; st=$?
    else "$SIM6747" --debug step.s < step.commands > "$OUT/$t.got" 2>&1; st=$?; fi
    echo "exit $st" >> "$OUT/$t.got"
    if diff "$t.expected" "$OUT/$t.got" > "$OUT/$t.diff"; then echo "c6x-debug $t: ok"; else echo "c6x-debug $t: FAILED - $OUT/$t.diff"; head -12 "$OUT/$t.diff"; fail=1; fi
done
exit $fail
