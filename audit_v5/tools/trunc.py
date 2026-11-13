#!/usr/bin/env python3
"""Truncation sweep: cut a file at every offset and require Audio_Open +
read to never crash and never over-read.  usage: trunc.py <file> <step>"""
import os, sys, subprocess
src = open(sys.argv[1], 'rb').read()
full = subprocess.run(['/home/user/aud5/audtool', 's16', os.path.abspath(sys.argv[1])],
                      capture_output=True, cwd='/').stdout
step = int(sys.argv[2]) if len(sys.argv) > 2 else 1
tmp = '/tmp/trunc.bin'
problems = 0
tried = 0
for cut in range(0, len(src) + 1, step):
    open(tmp, 'wb').write(src[:cut])
    p = subprocess.run(['/home/user/aud5/audtool', 's16', tmp],
                       capture_output=True, cwd='/')
    tried += 1
    if p.returncode not in (0, 1):
        problems += 1
        print("  crash at cut=%d rc=%d %s" % (cut, p.returncode, p.stderr[-200:]))
    elif len(p.stdout) > len(full):
        problems += 1
        print("  over-read at cut=%d: %d bytes vs %d whole" % (cut, len(p.stdout), len(full)))
print("trunc.py: %d cuts of %s, %d problems" % (tried, os.path.basename(sys.argv[1]), problems))
