#!/bin/sh
# tests/c6x-rs-cmp1.sh - fphead RS leaves the 1-bit destination of the compact compares and logicals (L2c, Lx1c, Lx3c)
# alone: it is A0/A1 or B0/B1, the predicate registers. cl6x 8.2.2's matmul has, under RS=1, 1327h CMPLT.L2 0,B22,B0
# beside 0013h MVK.S2 0,B16; read as B16 the compare met the MVK - a write conflict that is not one (D7).
#   MVK 5,B22 ; NOPs | header-based fetch packet, RS=1: CMPLT.L2 0,B22,B0 (16-bit, 1327h) || NOP ; NOPs
SIM6747=${SIM6747:-./sim6747.exe}
printf 'c6x-rs-cmp1: '
exec python3 "$(dirname "$0")/c6x-words.py" "$SIM6747" 80001000 14 'B0=1 B16=0' \
    0b0002aa 0 0 0 0 0 0 0  0c6e1327 0 0 0 0 0 0 e0280000
