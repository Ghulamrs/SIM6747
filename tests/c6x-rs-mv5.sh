#!/bin/sh
# tests/c6x-rs-mv5.sh - fphead RS moves only a compact instruction's 3-bit register fields to A16-A31/B16-B31.
# MV's lsdmvfr form keeps its 5-bit destination as written: in rts6740's __c6xabi_divd, under RS=1, the
# 16-bit 3846h is MV.L1X B16,A17 (A17 = -1023, the "exponent is zero" mark). Decoded as MV B16,B1 it left
# A17 stale, and 0.0/0.0 came out +inf and 1.0/0.0 as 2^1023 - isnan and isinf answered 0 (D6).
#   MVK -1023,B16 ; NOPs | header-based fetch packet, RS=1: NOP ; NOP (16-bit) ; MV.L1X B16,A17 (16-bit, 3846h) ; NOPs
SIM6747=${SIM6747:-./sim6747.exe}
printf 'c6x-rs-mv5: '
exec python3 "$(dirname "$0")/c6x-words.py" "$SIM6747" 80001000 14 'A17=fffffc01 B1=0' \
    087e00aa 0 0 0 0 0 0 0  0 38460c6e 0 0 0 0 0 e0480000
