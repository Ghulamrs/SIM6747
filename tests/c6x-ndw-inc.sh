#!/bin/sh
# tests/c6x-ndw-inc.sh - the compact STNDW/LDNDW post-increment (Dincdw) steps by doublewords, as *R++[n] does:
# cl6x 8.2.2's fillWords stores with 1d55h, STNDW.D2T2 B5:B4,*B6++[1], and needs B6 8 bytes on. Read as a byte
# step, every fill past fifteen words overlapped itself - fill-stores' silent wrong checksums from 16 elements (D8).
#   MVK 1100h,B6 ; MVKH 80000000h,B6 ; NOPs | header-based fetch packet, DSZ 100b: STNDW B5:B4,*B6++[1] (16-bit) ; NOPs
VM=${VM:-./vm6747.exe}
printf 'c6x-ndw-inc: '
exec python3 "$(dirname "$0")/c6x-words.py" "$VM" 80001000 16 'B6=80001108' \
    0308802a 0340006a 0 0 0 0 0 0  0c6e1d55 0 0 0 0 0 0 e0240000
