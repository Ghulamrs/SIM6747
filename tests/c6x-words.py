#!/usr/bin/env python3
"""c6x-words.py VM BASE STEPS 'NAME=HEX ...' WORD... - the machine-code CPU's regression tests' common part.

The words are written as a raw binary at BASE, run for STEPS execute packets with --trace, and the
registers named are held to the values given as they stand after the last step. Prints one line;
exit status 0 when every register agrees.
"""
import os, struct, subprocess, sys, tempfile

vm, base, steps, want = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
words = [int(w, 16) for w in sys.argv[5:]]
words += [0] * (-len(words) % 8 + 8)                      # NOPs to the end of a fetch packet, and one more
d = tempfile.mkdtemp()
binp, tr = os.path.join(d, 't.bin'), os.path.join(d, 't.trace')
open(binp, 'wb').write(b''.join(struct.pack('<I', w) for w in words))
subprocess.run([vm, '--run', binp, '--bin', base, '--steps', steps, '--trace', tr], stderr=subprocess.DEVNULL)
names, last = None, None
for line in open(tr):
    if line.startswith('# REGS'): names = line.split()[2:]
    elif line.startswith('S '): last = line.split()[2:]
bad = []
for pair in want.split():
    n, v = pair.split('=')
    got = last[names.index(n)]
    if int(got, 16) != int(v, 16): bad.append('%s=%s (want %s)' % (n, got, v))
print('FAILED: ' + ', '.join(bad) if bad else 'ok')
sys.exit(1 if bad else 0)
