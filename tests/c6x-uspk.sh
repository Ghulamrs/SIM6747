#!/bin/sh
# tests/c6x-uspk.sh - the compact SPKERNEL's fstg/fcyc bits (Uspk: 9-7, 15-14, 0) as the 32-bit field's bits 0-5,
# read with the same bit-reversed fstg. The table had bit 0 as bit 0 of the field, which gave the 83 ii=1 loops'
# 1c67h "SPKERNEL 32,0" for an epilog of one cycle, and rts6740's divide loop (ii 12, 9c66h) a fetch delay of one
# stage instead of two: its MVKH B4 met the epilog's own B4 writes, divide-by-constant-signs.ti-O2's fault (D4).
set -u
VM=${VM:-./vm6747.exe}
k() { "$VM" --dis-words 80001000 "$1" 0 0 0 0 0 0 0 "0c6e$2" 0 0 0 0 0 0 e0200000 | grep SPKERNEL | sed 's/^[0-9a-f]* *[0-9a-f]*  *//'; }
got="$(k 05838000 9c66) / $(k 00038000 1c67) / $(k 00038000 1e67) / $(k 00038000 1c66)"
want='SPKERNEL 2,0 / SPKERNEL 1,0 / SPKERNEL 9,0 / SPKERNEL 0,0'
if [ "$got" = "$want" ]; then echo "c6x-uspk: ok"; exit 0; fi
echo "c6x-uspk: FAILED: got '$got', want '$want'"; exit 1
