#!/bin/sh
# tests/c6x-epilog-bnop.sh - a BNOP issued in an SPLOOP epilog: when the buffer goes idle with the BNOP's NOP cycles
# still to run, its branch lands at their end, and the packet after the BNOP never issues. rts6740's
# process_unwind_pop_bitmask ends its loop so; issuing the next packet took its "[B0] B $C$L6" as well and handed
# regb_core_get a context of 1000h - the fault five of cpp11's throwing programs stopped on (D5).
#   MVK 2,B1 ; MVC B1,ILC ; NOP 4 ; SPLOOP 1 ; ADD 1,A3,A3 ; ADD 1,A4,A4 || SPKERNEL ; BNOP 80001024h,5
#   80001020: MVK 7,A5 (must not issue) ; 80001024: MVK 9,A6
VM=${VM:-./vm6747.exe}
printf 'c6x-epilog-bnop: '
exec python3 "$(dirname "$0")/c6x-words.py" "$VM" 80001000 30 'A3=2 A4=2 A5=0 A6=9' \
    0080012a 068403a2 00006000 00038000 018c2058 02102059 00034000 0009a120  028003a8 030004a8 0 0 0 0 0 0
