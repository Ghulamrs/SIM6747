#!/bin/sh
# tests/oracle-compare-trace.sh - the kit's trace comparison on two small traces made to look like TI's and sim6747's:
# TI's steps a cycle at a time (its NOP 3 three lines at one PC) and writes a negative register as 16 digits;
# sim6747's an execute packet at a time. The two agree; then one register is made to differ, and must be found.
set -u
D=${TMPDIR:-/tmp}/oct.$$; mkdir -p "$D"; trap 'rm -rf "$D"' EXIT
H='# REGS PC A0 A1 CYC'
cat > "$D/ti" <<T
$H
S 0 80000000 00000000 00000000 0
S 1 80000004 ffffffff80000000 00000000 1
S 2 80000008 ffffffff80000000 00000001 2
S 3 80000008 ffffffff80000000 00000001 3
S 4 80000008 ffffffff80000000 00000001 4
S 5 8000000c ffffffff80000000 00000002 5
T
cat > "$D/vm" <<T
$H
S 0 80000000 00000000 00000000 0
S 1 80000004 80000000 00000000 1
S 2 80000008 80000000 00000001 2
S 3 8000000c 80000000 00000002 5
T
python3 "$(dirname "$0")/../oracle/compare-trace.py" "$D/ti" "$D/vm" > "$D/out1" 2>&1; r1=$?
sed 's/00000002 5$/00000003 5/' "$D/vm" > "$D/vm2"
python3 "$(dirname "$0")/../oracle/compare-trace.py" "$D/ti" "$D/vm2" > "$D/out2" 2>&1; r2=$?
if [ $r1 -eq 0 ] && [ $r2 -ne 0 ] && grep -q 'A1 TI=00000002 vm=00000003' "$D/out2"; then echo "oracle-compare-trace: ok"; exit 0; fi
echo "oracle-compare-trace: FAILED"; cat "$D/out1" "$D/out2"; exit 1
