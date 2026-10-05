#!/bin/sh
# tests/c6x-mvc-ilc.sh - the compact MVC src,ILC (Sx1), which rts6740's writemsg begins with (SPLOOPD 1 || MVC B6,ILC):
# its destination is the ILC operand the decoder gives by name, not a control-register field.
#   MVK 5,B6 ... | header-based fetch packet: MVC B6,ILC (16-bit) ; NOP (16-bit) ; NOPs
VM=${VM:-./vm6747.exe}
printf 'c6x-mvc-ilc: '
exec python3 "$(dirname "$0")/c6x-words.py" "$VM" 80001000 20 'B6=5 ILC=5' \
    030002aa 0 0 0 0 0 0 0  0c6edb6f 0 0 0 0 0 0 e0200000
