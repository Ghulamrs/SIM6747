#!/bin/sh
# tests/oracle-compare-dis.sh - compare-dis.py reads dis6x's spellings as the same instructions as sim6747's:
# a branch target written $C$L25 (PC+160 = 0x...), a scaled offset *B7[4], *B4 for *+B4(0), MV and ZERO for
# the ADD/OR/SUB/MVK they stand for, and ||^ for an SPMASKed instruction. One real difference must still show.
set -u
D=${TMPDIR:-/tmp}/ocd.$$; mkdir -p "$D"; trap 'rm -rf "$D"' EXIT
cat > "$D/ti" <<'T'
80001000   00002120           BNOP.S1       $C$L25 (PC+160 = 0x800010a0),0
80001004   0200a264           LDW.D1T1      *A4[4],A4
80001008   01001264           LDW.D2T1      *B4,A6
8000100c   0280a078           MV.L1         A3,A5
80001010   028ca0f8           ZERO.L1       A5
80001014   02908078   ||^     ADD.L1        A4,A3,A5
80001018   02906078           ADD.L1        A3,A4,A5
T
cat > "$D/vm" <<'T'
80001000 00002120     BNOP.S1 0x800010a0,0
80001004 0200a264     LDW.D1T1 *+A4(16),A4
80001008 01001264     LDW.D2T1 *+B4(0),A6
8000100c 0280a078     ADD.L1 A3,0,A5
80001010 028ca0f8     SUB.L1 A5,A5,A5
80001014 02908078  || ADD.L1 A4,A3,A5
80001018 02906078     ADD.L1 A3,A4,A6
T
out=$(python3 "$(dirname "$0")/../oracle/compare-dis.py" "$D/ti" "$D/vm" 2>&1)
if echo "$out" | grep -q '^6 of 7 instructions agree' && echo "$out" | grep -q '80001018'; then echo "oracle-compare-dis: ok"; exit 0; fi
echo "oracle-compare-dis: FAILED"; echo "$out"; exit 1
