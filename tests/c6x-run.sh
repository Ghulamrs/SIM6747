#!/bin/sh
# tests/c6x-run.sh - the machine-code CPU on a few packets whose timing SPRUFE8 chapter 4 fixes: single-cycle
# results seen by the next packet, a store, and a load whose word lands after its four delay slots.
# Run from the repository root. The full check is oracle/compare-trace.py against TI's simulator.
set -u
VM=${VM:-./vm6747.exe}
T=${TMPDIR:-/tmp}/c6x-run.$$
trap 'rm -f "$T".*' EXIT
# MVK 5,A3; MVK 7,A4; ADD A3,A4,A5; MVK 0x100,B4; MVKH 0x80000000,B4; STW A5,*B4; LDW *B4,A6; NOP 4; NOP
python3 - "$T.bin" <<'PY'
import struct, sys
w = [0x018002a8, 0x020003a8, 0x02906078, 0x0200802a, 0x0240006a, 0x029002f4, 0x031002e4, 0x00006000, 0, 0, 0, 0, 0, 0, 0, 0]
open(sys.argv[1], 'wb').write(b''.join(struct.pack('<I', x) for x in w))
PY
"$VM" --run "$T.bin" --bin 80001000 --steps 9 --trace "$T.trace" --trace-view=issue 2>/dev/null
# columns: S k PC A0..: A5 is field 9, A6 field 10; the cycle count is last
got=$(awk '$1=="S" {print $2, $3, $9, $10, $NF}' "$T.trace")
want='0 80001000 00000000 00000000 0
1 80001004 00000000 00000000 1
2 80001008 00000000 00000000 2
3 8000100c 0000000c 00000000 3
4 80001010 0000000c 00000000 4
5 80001014 0000000c 00000000 5
6 80001018 0000000c 00000000 6
7 8000101c 0000000c 00000000 7
8 80001020 0000000c 0000000c 11
9 80001024 0000000c 0000000c 12'
if [ "$got" = "$want" ]; then echo "c6x-run: 10/10"; exit 0; fi
echo "c6x-run: FAILED"; echo "--- want"; echo "$want"; echo "--- got"; echo "$got"; exit 1
