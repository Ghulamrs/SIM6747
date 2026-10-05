#!/bin/sh
# tests/c6x-spint-nan.sh - SPINT of a NaN gives 8000 0000h, as SPRUFE8 gives it for SPINT and DPINT.
#   MVK 0,A3; MVKH 0x7fc00000,A3; SPINT A3,A4; NOP 4
VM=${VM:-./vm6747.exe}
printf 'c6x-spint-nan: '
exec python3 "$(dirname "$0")/c6x-words.py" "$VM" 80001000 4 'A3=7fc00000 A4=80000000' \
    01800028 01bfe068 020c0158 00006000
