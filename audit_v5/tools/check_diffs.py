#!/usr/bin/env python3
"""Verify every ```diff block of AUDIT_AUDIO_V5.md is byte-identical to the
delivered file in audit_v5/ and applies to a clean fc31b25 extract.

  python3 audit_v5/tools/check_diffs.py <report.md> <file.diff>...
"""
import re, subprocess, sys, tempfile, os, shutil
report, refs = sys.argv[1], sys.argv[2:]
blocks = re.findall(rb'```diff\n(.*?)```\n', open(report, 'rb').read(), re.S)
if len(blocks) != len(refs):
    sys.exit(f'{len(blocks)} diff blocks in the report, {len(refs)} files given')
ok = True
for i, (blk, ref) in enumerate(zip(blocks, refs)):
    data = open(ref, 'rb').read()
    same = blk == data
    d = tempfile.mkdtemp()
    subprocess.run('cd /home/user/Clain_Filemanagaer && git archive fc31b25 | tar xf - -C ' + d,
                   shell=True, check=True)
    lib = os.path.join(d, 'Clain_FileManager/FileManager_lib')
    open(os.path.join(d, 'x.diff'), 'wb').write(blk)
    r = subprocess.run(['git', 'apply', '--check', os.path.join(d, 'x.diff')], cwd=lib,
                       capture_output=True)
    print(f'{i}: {ref}: identical={same} applies={r.returncode == 0}'
          + ('' if r.returncode == 0 else ' ' + r.stderr.decode()[:120]))
    ok &= same and r.returncode == 0
    shutil.rmtree(d)
sys.exit(0 if ok else 1)
