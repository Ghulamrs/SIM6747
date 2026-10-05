#!/bin/sh
# tests/c6x-reset.sh - the control registers' values at reset are the simulator's: CSR 0x44010100 (CPU ID 0x44,
# revision 1), DNUM 1, as the CCS 5.5 C6747 simulator reports them (review 2026-10-05).
VM=${VM:-./vm6747.exe}
printf 'c6x-reset: '
exec python3 "$(dirname "$0")/c6x-words.py" "$VM" 80001000 1 'CSR=44010100 DNUM=1' 00000000
