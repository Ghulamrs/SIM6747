#!/bin/sh
# tests/c6x-count.sh - -c counts from main, as TI's simulator does once loadProgram has run the boot to main;
# entry= keeps the count from the entry point. MVK, MVK, ADD, NOP, NOP, NOP 4, NOP: counted from the third
# packet (--count-from, a raw binary having no symbols) to the stop after seven steps: five packets, eight cycles.
set -u
SIM6747=${SIM6747:-./sim6747.exe}
T=${TMPDIR:-/tmp}/c6x-count.$$
trap 'rm -f "$T".*' EXIT
python3 -c "
import struct,sys
w=[0x018002a8,0x020003a8,0x02906078,0,0,0x6000,0,0, 0,0,0,0,0,0,0,0]
open('$T.bin','wb').write(b''.join(struct.pack('<I',x) for x in w))"
got=$("$SIM6747" --run "$T.bin" --bin 80001000 --steps 7 --count-from 80001008 -c 2>&1 | grep '^CYCLES' | sed 's/ exit=.*//')
want='CYCLES count=8 packets=5 entry=10'
if [ "$got" = "$want" ]; then echo "c6x-count: ok"; exit 0; fi
echo "c6x-count: FAILED: want '$want', got '$got'"; exit 1
