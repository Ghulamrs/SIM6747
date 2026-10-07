#!/usr/bin/env python3
"""compare-dis.py DIS6X.dis VM.dis [--max N]

Holds sim6747 --dis to TI's dis6x on the same file, instruction by instruction, keyed by address. Both
listings are read loosely - an address, the instruction word (or half), the text - and the text compared
in a normal form: case and spacing dropped, every number read as its value, '||' kept apart. Prints the
first disagreements and a summary by mnemonic, so a wrong format shows as a block of one mnemonic.
"""
import collections
import re
import sys

NUM = re.compile(r'(?<![A-Za-z_$])(-?0x[0-9a-fA-F]+|-?[0-9a-fA-F]+h|-?\d+)(?![A-Za-z_])')
LINE = re.compile(r'^\s*([0-9a-fA-F]{8})[:\s]+([0-9a-fA-F]{4}|[0-9a-fA-F]{8})\s+(.*)$')


def num(tok):
    t = tok.lower()
    neg = t.startswith('-')
    t = t.lstrip('-')
    v = int(t[2:], 16) if t.startswith('0x') else int(t[:-1], 16) if t.endswith('h') else int(t)
    return str(-v if neg else v)


SIZE = {'B': 1, 'H': 2, 'W': 4, 'DW': 8}


def access_size(mn):
    m = re.match(r'^(?:LD|ST)N?(DW|BU|HU|B|H|W)$', mn)
    return SIZE.get(m.group(1).rstrip('U'), 1) if m else 1


def norm(text):
    t = text.split(';')[0].strip()
    par = t.startswith('||')
    t = t.lstrip('|').strip().lstrip('^').strip()       # dis6x marks an SPMASKed instruction ||^
    t = re.sub(r'\s+', ' ', t).upper()
    t = re.sub(r'\s*([,:()\[\]])\s*', r'\1', t)
    t = re.sub(r'\s*\.\s*', '.', t)                    # "ADD .L1" and "ADD.L1"
    # a branch target as dis6x writes it: $C$L25 (PC+160 = 0x80001234) is the address
    t = re.sub(r'[^\s,]*\s*\(PC[+-]\d+\s*=\s*(0X[0-9A-F]+)\)', r'\1', t)
    t = NUM.sub(lambda m: num(m.group(1)), t)
    t = t.replace('[ ', '[').replace(' ]', ']')
    head, _, ops = t.partition(' ')
    mn = re.sub(r'^\[!?[AB]\d+\]', '', head).split('.')[0]
    # memory operands in one form: *R is *+R(0), *R[n] is *+R[n], and a scaled constant offset is in bytes
    size = access_size(mn)
    ops = re.sub(r'\*([AB]\d+)(?=[,\s]|$)', r'*+\1(0)', ops)
    ops = re.sub(r'\*([AB]\d+)\[', r'*+\1[', ops)
    ops = re.sub(r'(\*[-+]{1,2}[AB]\d+|\*[AB]\d+[-+]{2})\[(\d+)\]', lambda m: '%s(%d)' % (m.group(1), int(m.group(2)) * size), ops)
    # the assembler's aliases, which dis6x prints and the table does not: MV is ADD/OR with 0, ZERO is SUB x,x,x
    o = ops.split(',')
    cond = head[:len(head) - len(head.lstrip('[!AB0123456789]'))] if head.startswith('[') else ''
    unit = head.split('.', 1)[1] if '.' in head else ''
    if mn in ('ADD', 'OR') and len(o) == 3 and '0' in o[:2]:
        mn, o = 'MV', [o[1] if o[0] == '0' else o[0], o[2]]
    elif mn == 'SUB' and len(o) == 3 and o[0] == o[1] == o[2]:
        mn, o = 'ZERO', [o[2]]
    elif mn == 'MVK' and len(o) == 2 and o[0] == '0':
        mn, o = 'ZERO', [o[1]]
    t = cond + mn + ('.' + unit if unit else '') + (' ' + ','.join(o) if ops else '')
    return par, t


def load(path):
    out = {}
    for line in open(path, errors='replace'):
        m = LINE.match(line)
        if not m:
            continue
        addr = int(m.group(1), 16)
        out[addr] = (m.group(2).lower(), norm(m.group(3)), m.group(3).strip())
    return out


def mnemonic(t):
    t = re.sub(r'^\[[^\]]*\]', '', t).strip()
    return t.split(' ')[0].split('.')[0]


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    limit = 20
    for a in sys.argv[1:]:
        if a.startswith('--max='):
            limit = int(a[6:])
    if len(args) != 2:
        sys.exit(__doc__)
    ti, vm = load(args[0]), load(args[1])
    common = sorted(set(ti) & set(vm))
    agree, bad, by = 0, 0, collections.Counter()
    for a in common:
        (_, (tp, tt), traw), (_, (vp, vt), vraw) = ti[a], vm[a]
        if tt == vt and tp == vp:
            agree += 1
            continue
        bad += 1
        by[mnemonic(tt) + ' / ' + mnemonic(vt)] += 1
        if bad <= limit:
            print('%08x  dis6x: %-40s vm: %s' % (a, traw, vraw))
    print('%d of %d instructions agree (dis6x listed %d, vm %d)' % (agree, len(common), len(ti), len(vm)))
    for k, n in by.most_common(25):
        print('  %5d  %s' % (n, k))
    sys.exit(0 if bad == 0 else 1)


main()
