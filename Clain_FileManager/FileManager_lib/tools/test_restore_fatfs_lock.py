#!/usr/bin/env python3
"""Host regression test for CubeMX FatFS lock restoration."""
from pathlib import Path
import subprocess
import sys
import tempfile

script = Path(__file__).with_name('restore_fatfs_lock.py')
with tempfile.TemporaryDirectory() as d:
    p = Path(d) / 'ffconf.h'
    for key in ('_FS_LOCK', 'FF_FS_LOCK'):
        original = f'/* generated */\r\n#define {key}    2 /* slots */\r\n'.encode()
        expected = original.replace(b'    2 ', b'    16 ')
        p.write_bytes(original)
        subprocess.run([sys.executable, str(script), str(p)], check=True)
        assert p.read_bytes() == expected
        subprocess.run([sys.executable, str(script), str(p)], check=True)
        assert p.read_bytes() == expected
    subprocess.run([sys.executable, str(script), str(p), '--slots', '24'], check=True)
    assert b'    24 ' in p.read_bytes()
    for invalid in (b'/* #define _FS_LOCK 2 */\n',
                    b'#define _FS_LOCK 2\n#define FF_FS_LOCK 2\n',
                    b'#define _FS_LOCK VALUE\n'):
        p.write_bytes(invalid)
        result = subprocess.run([sys.executable, str(script), str(p)], capture_output=True)
        assert result.returncode != 0 and p.read_bytes() == invalid
print('FatFS lock regeneration helper: PASS')
