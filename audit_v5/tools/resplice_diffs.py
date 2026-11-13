#!/usr/bin/env python3
"""Put the delivered *.diff files back into the ```diff blocks of the report.

Markdown edits are made in text mode, which drops the CRs of the diff bodies;
this restores them byte for byte. Run after every edit of the report.
"""
import re, sys
report = '/home/user/Clain_Filemanagaer/AUDIT_AUDIO_V5.md'
srcs = ['AUD5-02.diff', 'AUD5-03.diff', 'AUD5-01-only.diff', 'AUD5-04-only.diff']
base = '/home/user/Clain_Filemanagaer/audit_v5/'
b = open(report, 'rb').read()
pat = re.compile(rb'```diff(?:[^\n]*)\n(.*?)```\n', re.S)
pos, out = 0, []
for src in srcs:
    m = pat.search(b, pos)
    if not m:
        sys.exit('fewer diff blocks than files')
    out.append(b[pos:m.start()])
    d = open(base + src, 'rb').read()
    if not d.endswith(b'\n'):
        d += b'\n'
    out.append(b'```diff\n' + d + b'```\n')
    pos = m.end()
out.append(b[pos:])
open(report, 'wb').write(b''.join(out))
print('spliced', len(srcs), 'diff blocks')
