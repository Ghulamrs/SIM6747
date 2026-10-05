#!/usr/bin/env python3
"""compare-trace.py TI.trace VM.trace [--regs PC,A0,...] [--max N] [--no-cyc]

Holds vm6747's --trace to trace.js's on TI's simulator, step by step: both files name their columns in a
'# REGS' line, so only the columns both have are compared (or those --regs names). Prints the first steps
that differ, register by register, and a summary: how many steps agree, and where they first part.
Exit status 0 when every compared step agrees.
"""
import sys


def load(path):
    names, rows = None, []
    for line in open(path):
        if line.startswith('# REGS'):
            names = line.split()[2:]
        elif line.startswith('S '):
            t = line.split()
            rows.append((int(t[1]), t[2:]))
    if names is None:
        sys.exit('%s: no "# REGS" line' % path)
    return names, rows


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    opts = dict(a[2:].split('=', 1) if '=' in a else (a[2:], '1') for a in sys.argv[1:] if a.startswith('--'))
    if len(args) != 2:
        sys.exit(__doc__)
    tn, tr = load(args[0])
    vn, vr = load(args[1])
    cols = [c for c in tn if c in vn]
    if 'regs' in opts:
        cols = [c for c in opts['regs'].split(',') if c in tn and c in vn]
    if 'no-cyc' in opts and 'CYC' in cols:
        cols.remove('CYC')
    ti_ix = {c: tn.index(c) for c in cols}
    vm_ix = {c: vn.index(c) for c in cols}
    limit = int(opts.get('max', '10'))
    agree, shown, first = 0, 0, None
    for (k, a), (k2, b) in zip(tr, vr):
        diffs = [(c, a[ti_ix[c]], b[vm_ix[c]]) for c in cols
                 if a[ti_ix[c]].lower().lstrip('0x') != b[vm_ix[c]].lower().lstrip('0x')]
        if not diffs:
            agree += 1
            continue
        if first is None:
            first = k
        if shown < limit:
            print('step %d: ' % k + '  '.join('%s TI=%s vm=%s' % d for d in diffs[:12]))
            shown += 1
    n = min(len(tr), len(vr))
    print('%d of %d steps agree on %d columns (%s)%s' % (agree, n, len(cols), ','.join(cols[:6]) + (',...' if len(cols) > 6 else ''),
          '' if first is None else '; first difference at step %d' % first))
    if len(tr) != len(vr):
        print('lengths differ: TI %d steps, vm %d' % (len(tr), len(vr)))
    sys.exit(0 if first is None and len(tr) == len(vr) else 1)


main()
