#!/bin/sh
# tests/c6x-dis.sh - the machine-code decoder on words whose reading is worked out by hand from SPRUFE8's
# formats; the full check is oracle/compare-dis.py against TI's dis6x. Run from the repository root.
set -u
SIM6747=${SIM6747:-./sim6747.exe}
got=$("$SIM6747" --dis-words 0 00000000 00008000 018002a8 000c0362 02906078 01bc42e6 00000000 00000000 | sed 's/^[0-9a-f]* *[0-9a-f]*  *//')
want='NOP 1
NOP 5
MVK.S1 5,A3
B.S2 B3
ADD.L1 A3,A4,A5
LDW.D2T2 *+B15(8),B3
NOP 1
NOP 1'
if [ "$got" = "$want" ]; then echo "c6x-dis: 8/8"; exit 0; fi
echo "c6x-dis: FAILED"; echo "--- want"; echo "$want"; echo "--- got"; echo "$got"; exit 1
