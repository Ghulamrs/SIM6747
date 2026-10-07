#!/usr/bin/env python3
"""compare-trace.py TI.trace VM.trace [--from=PC] [--max=N] [--ignore=A,B,...] [--window=N]

Holds sim6747's --trace to trace.js's on TI's simulator. The two step differently: TI's asmStep moves one
cycle (a NOP 5 is six lines at one PC), sim6747 one execute packet with its holds. So TI's trace is first
collapsed to the first line of each run of one PC, and the two are then walked together: at a PC both are at,
the registers both name are compared; where the PCs differ, the side that reaches the other's PC sooner
(within --window packets) is advanced - a granularity difference cannot show as a register difference, only
a real one can. Values are compared as 32-bit words (DSS may sign-extend a negative one to 16 digits).
--from starts both at the first visit of that PC (main, say). Exit status 0 when nothing disagrees and
neither path leaves the other.
Defaults ignore CYC and the control registers sim6747 does not yet model as the simulator does.
"""
import sys

DEFAULT_IGNORE = 'CYC CSR IER IFR ISTP DNUM TSR ITSR NTSR PCE1 ILC RILC'


def load(path):
    names, rows = None, []
    for line in open(path):
        if line.startswith('# REGS'):
            names = line.split()[2:]
        elif line.startswith('S '):
            t = line.split()
            rows.append((int(t[1]), [x[-8:].lower().zfill(8) if x.strip('0123456789abcdefABCDEF') == '' else x for x in t[2:]]))
    if names is None:
        sys.exit('%s: no "# REGS" line' % path)
    return names, rows


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    opts = dict((a[2:].split('=', 1) + ['1'])[:2] for a in sys.argv[1:] if a.startswith('--'))
    if len(args) != 2:
        sys.exit(__doc__)
    tn, tr = load(args[0])
    vn, vr = load(args[1])
    ti = []                                   # TI collapsed: the first line of each run of one PC
    for k, r in tr:
        if not ti or ti[-1][1][0] != r[0]:
            ti.append((k, r))
    vm = vr
    ignore = set(opts.get('ignore', DEFAULT_IGNORE).replace(',', ' ').split())
    cols = [c for c in tn if c in vn and c != 'PC' and c not in ignore]
    tix = {c: tn.index(c) for c in cols}
    vix = {c: vn.index(c) for c in cols}
    limit, window = int(opts.get('max', '10')), int(opts.get('window', '400'))
    i = j = 0
    if 'from' in opts:
        pc = opts['from'].lower().replace('0x', '').zfill(8)
        i = next((n for n, (k, r) in enumerate(vm) if r[0] == pc), len(vm))
        j = next((n for n, (k, r) in enumerate(ti) if r[0] == pc), len(ti))
    agree, shown, first, parted = 0, 0, None, False
    while i < len(vm) and j < len(ti):
        (kv, b), (kt, a) = vm[i], ti[j]
        if a[0] == b[0]:
            d = [(c, a[tix[c]], b[vix[c]]) for c in cols if int(a[tix[c]], 16) != int(b[vix[c]], 16)]
            if d:
                first = first or (kt, kv)
                if shown < limit:
                    print('TI step %d / vm step %d at %s: ' % (kt, kv, a[0]) + '  '.join('%s TI=%s vm=%s' % x for x in d[:10]))
                    shown += 1
            else:
                agree += 1
            i += 1; j += 1
            continue
        dv = next((n for n in range(1, window) if i + n < len(vm) and vm[i + n][1][0] == a[0]), None)
        dt = next((n for n in range(1, window) if j + n < len(ti) and ti[j + n][1][0] == b[0]), None)
        if dv is None and dt is None:
            print('paths part: TI step %d at %s, vm step %d at %s' % (kt, a[0], kv, b[0]))
            parted = True
            break
        if dt is None or (dv is not None and dv <= dt):
            i += dv
        else:
            j += dt
    print('%d packets agree at common PCs on %d registers; %s' % (agree, len(cols),
          'no disagreement' if first is None else 'first disagreement at TI/vm step %d/%d' % first))
    sys.exit(0 if first is None and not parted else 1)


main()
