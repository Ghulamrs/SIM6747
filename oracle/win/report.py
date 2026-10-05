#!/usr/bin/env python3
"""report.py SHIP - oracle/ship/win-calib/out (vm6747sim on the Windows box) held to CCS 5.5: each image's output
against the oracle's, and its cycle count against cycle.CPU - fresh from the box when calibrate.sh ran with --ccs,
otherwise the stored count (res/ for the 49, img822/results*.txt for cl6x 8.2.2's)."""
import os, re, sys, statistics
ship = sys.argv[1]; out = os.path.join(ship, 'win-calib', 'out')
def rd(p):
    try: return open(p, 'rb').read().replace(b'\r\n', b'\n')
    except OSError: return None
stored = {}
for f in ('results.txt', 'results-ddr.txt'):
    p = os.path.join(ship, 'img822', f)
    if os.path.exists(p):
        for l in open(p):
            m = re.match(r'(\S+) sim .* ccs1 (\d+) ', l)
            if m: stored[m.group(1) + '.822-O2'] = int(m.group(2))
fresh, ms = {}, {}
for l in open(os.path.join(out, 'results.txt'), errors='replace'):
    t = l.split()
    if len(t) >= 3 and t[1] == 'sim_ms':
        ms[t[0]] = int(t[2])
        if len(t) >= 5 and t[3] == 'ccs1' and t[4].isdigit(): fresh[t[0]] = int(t[4])
rows, ratios, bad = [], [], 0
for n in sorted(ms):
    if n.endswith('.822-O2'): exp = rd(os.path.join(ship, 'img822', 'img', n + '.ccs1.cio'))
    else:
        exp = rd(os.path.join(ship, 'res', n + '.cpu.stdout'))
        p = os.path.join(ship, 'res', n + '.cpu.result')
        if os.path.exists(p):
            m = re.search(r'count=(\d+)', open(p).read())
            if m: stored[n] = int(m.group(1))
    got = rd(os.path.join(out, n + '.sim.txt')); err = (rd(os.path.join(out, n + '.sim.err')) or b'').decode(errors='replace')
    m = re.search(r'count=(\d+) .*exit=(\S+)', err); sc = int(m.group(1)) if m else None; ex = m.group(2) if m else 'none'
    ok = exp is not None and got == exp and ex == 'C$$EXIT'
    bad += not ok
    ref = fresh.get(n, stored.get(n))
    r = sc / ref if sc and ref else None
    if r: ratios.append(r)
    rows.append('%-42s %-5s %9s %9s %7s %6d ms' % (n, 'MATCH' if ok else 'DIFF', sc, ref, '%.4f' % r if r else '-', ms[n]))
print('%-42s %-5s %9s %9s %7s' % ('image', 'out', 'vm6747sim', 'cycle.CPU', 'ratio'))
print('\n'.join(rows))
src = 'fresh from the box' if fresh else 'stored'
print('\n%d of %d match CCS 5.5\'s output; cycles against %s cycle.CPU: median %.4f, range %.4f-%.4f' %
      (len(rows) - bad, len(rows), src, statistics.median(ratios), min(ratios), max(ratios)))
