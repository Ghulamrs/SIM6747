#!/bin/sh
# tests/c6x-base-update.sh - a load's or store's pre/post-increment of its base register lands at the end of E1
# (SPRUFE8 4.2.3), so the very next packet sees it - as in cl6x's epilogue LDW *++B15(8),B13; LDW *++B15(8),B11:B10.
#   MVK 0x100,B4; MVKH 0x80000000,B4; LDW *B4++[1],A6; ADD.L2 B4,B0,B5
VM=${VM:-./vm6747.exe}
printf 'c6x-base-update: '
exec python3 "$(dirname "$0")/c6x-words.py" "$VM" 80001000 4 'B4=80000104 B5=80000104' \
    0200802a 0240006a 031036e4 0280807a
