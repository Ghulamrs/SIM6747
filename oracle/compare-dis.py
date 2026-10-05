#!/usr/bin/env python3
"""compare-dis.py DIS6X.dis VM.dis [--max N]

Holds vm6747 --dis to TI's dis6x on the same file, instruction by instruction, keyed by address. Both
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


def norm(text):
    t = text.split(';')[0].strip()
    par = t.startswith('||')
    t = t.lstrip('|').strip()
    t = re.sub(r'\s+', ' ', t).upper()
    t = re.sub(r'\s*([,:()\[\]])\s*', r'\1', t)
    t = re.sub(r'\s*\.\s*', '.', t)                    # "ADD .L1" and "ADD.L1"
    t = NUM.sub(lambda m: num(m.group(1)), t)
    t = t.replace('[ ', '[').replace(' ]', ']')
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
