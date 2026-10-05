#!/bin/sh
# tests/c6x-store-data.sh - a store reads its data register in E1, with its address (SPRUFE8 4.2.3); only the
# write to memory is in E3. cl6x's call idiom STW B3,*B15--(8) || CALLP f,B3 depends on it: the old B3 is pushed.
#   MVK 1,A5; MVK 0x100,B4; MVKH 0x80000000,B4; STW A5,*B4 || MVK 2,A5; LDW *B4,A6; NOP 4
# A6 must be 1, the value A5 had when the STW issued, not the 2 written beside it.
VM=${VM:-./vm6747.exe}
printf 'c6x-store-data: '
exec python3 "$(dirname "$0")/c6x-words.py" "$VM" 80001000 7 'A5=2 A6=1' \
    028000a8 0200802a 0240006a 029002f5 02800128 031002e4 00006000
