#!/bin/sh
# tests/oracle-kit.sh - the kit's blind spots stay closed (review 2026-10-05, 1.6): the decoder corpus is assembled
# into a code section, the programs link the exception-handling runtime when the box has it, and a C++ program
# that throws is among them.
set -u
K=$(dirname "$0")/../oracle
bad=0
python3 "$K/gen-words.py" 8 1 | grep -qx '	\.text' || { echo "gen-words.py does not write .text"; bad=1; }
grep -q 'rts6740_elf_eh.lib' "$K/remote/probe.sh" || { echo "probe.sh does not link rts6740_elf_eh.lib"; bad=1; }
grep -q 'rts6740_elf_eh.lib' "$K/remote/probe.cmd" || { echo "probe.cmd does not link rts6740_elf_eh.lib"; bad=1; }
ls "$K"/programs/*.cpp >/dev/null 2>&1 || { echo "no C++ program in oracle/programs"; bad=1; }
grep -q 'programs/\*\.cpp' "$K/remote/probe.sh" || { echo "probe.sh does not build the C++ programs"; bad=1; }
grep -q "programs/\*.cpp" "$K/run.sh" || { echo "run.sh does not ship the C++ programs"; bad=1; }
sh -n "$K/remote/probe.sh" || bad=1
sh -n "$K/run.sh" || bad=1
[ $bad -eq 0 ] && echo "oracle-kit: ok" || { echo "oracle-kit: FAILED"; exit 1; }
