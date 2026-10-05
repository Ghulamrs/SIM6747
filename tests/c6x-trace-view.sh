#!/bin/sh
# tests/c6x-trace-view.sh - --trace shows registers as TI's simulator does when halted after a step: with every
# result in flight already landed. After the step that issues LDW *B4,A6 (its word arrives in E5), A6 shows it.
#   MVK 1,A5; MVK 0x100,B4; MVKH 0x80000000,B4; STW A5,*B4; NOP 3; LDW *B4,A6   - six steps
VM=${VM:-./vm6747.exe}
printf 'c6x-trace-view: '
exec python3 "$(dirname "$0")/c6x-words.py" "$VM" 80001000 6 'A6=1' \
    028000a8 0200802a 0240006a 029002f4 00004000 031002e4 00006000
