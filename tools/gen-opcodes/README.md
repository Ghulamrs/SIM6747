# gen-opcodes

This regenerates `src/C6xOpcodes.inc`, the C674x formats and opcodes as data. You only need it if the
table has to change; the build never runs it.

The encodings are facts of TI's instruction set (SPRUFE8, appendices C–H). They are collected from the
opcode tables in GNU binutils (`include/opcode/tic6x-*.h`, GPL-3.0), which state those facts
field by field. The tables are read here and their contents written out in vm6747's own form. No
binutils code is compiled into vm6747. The result is held, word for word, to TI's `dis6x`
(`oracle/compare-dis.py`).

    # with a binutils source tree at $B (e.g. from Ubuntu's binutils-source package)
    gcc -w -I stub -I $B/include dump-binutils.c -o dump && ./dump > raw.txt
    python3 to-inc.py > ../../src/C6xOpcodes.inc       # reads raw.txt

`stub/bfd.h` needs only three lines:

    #include <stdbool.h>
    #include <stdint.h>
    typedef uint64_t bfd_vma;
