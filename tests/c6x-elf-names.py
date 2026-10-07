#!/usr/bin/env python3
"""c6x-elf-names.py VM - a fault or listing names a function by its own name: a minimal C6000 ELF whose one
function start also carries TI's __TI_exidx_linkto_scn_start_40 (first in the symbol table, as lnk6x writes
it) and a $C$L1 temporary; sim6747 --dis must label the address 'malloc'."""
import struct, subprocess, sys, tempfile, os

def elf(path):
    text = b'\x00' * 32
    strtab = b'\x00__TI_exidx_linkto_scn_start_40\x00$C$L1\x00malloc\x00'
    n1, n2, n3 = 1, strtab.index(b'$C$L1'), strtab.index(b'malloc')
    sym = b'\x00' * 16
    for name, typ in ((n1, 0), (n2, 0), (n3, 2)):      # NOTYPE, NOTYPE, FUNC; all global, in section 1
        sym += struct.pack('<IIIBBH', name, 0x80000000, 0, (1 << 4) | typ, 0, 1)
    shstr = b'\x00.text\x00.symtab\x00.strtab\x00.shstrtab\x00'
    off = 52
    body = text + sym + strtab + shstr
    o_text, o_sym = off, off + len(text)
    o_str, o_shs = o_sym + len(sym), o_sym + len(sym) + len(strtab)
    shoff = off + len(body)
    sh = b'\x00' * 40
    sh += struct.pack('<10I', shstr.index(b'.text'), 1, 6, 0x80000000, o_text, len(text), 0, 0, 32, 0)
    sh += struct.pack('<10I', shstr.index(b'.symtab'), 2, 0, 0, o_sym, len(sym), 3, 1, 4, 16)
    sh += struct.pack('<10I', shstr.index(b'.strtab'), 3, 0, 0, o_str, len(strtab), 0, 0, 1, 0)
    sh += struct.pack('<10I', shstr.index(b'.shstrtab'), 3, 0, 0, o_shs, len(shstr), 0, 0, 1, 0)
    hdr = b'\x7fELF\x01\x01\x01' + b'\x00' * 9
    hdr += struct.pack('<HHIIIIIHHHHHH', 2, 140, 1, 0x80000000, 0, shoff, 0, 52, 32, 0, 40, 5, 4)
    open(path, 'wb').write(hdr + body + sh)

d = tempfile.mkdtemp(); p = os.path.join(d, 'names.out'); elf(p)
out = subprocess.run([sys.argv[1], '--dis', p], capture_output=True, text=True).stdout
ok = '80000000 malloc:' in out
print('c6x-elf-names: ' + ('ok' if ok else 'FAILED\n' + out))
sys.exit(0 if ok else 1)
