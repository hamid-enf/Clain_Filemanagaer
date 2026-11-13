#!/usr/bin/env python3
"""Restore the FileManager FatFS lock setting after CubeMX regeneration.

Usage: python3 tools/restore_fatfs_lock.py [path/to/ffconf.h] [--slots 16]
Run this after generation and before building. This repository does not contain
an .ioc/CubeIDE project, so it cannot install a CubeMX post-generation hook.
"""
import argparse
from pathlib import Path
import re

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('path', nargs='?', type=Path,
               default=Path(__file__).resolve().parents[2] / 'FATFS' / 'Target' / 'ffconf.h')
p.add_argument('--slots', type=int, default=16,
               help='lock table size (must be at least your configured open handles; default 16)')
a = p.parse_args()
if a.slots < 1:
    p.error('slots must be positive; set it at least to FM_MAX_OPEN_FILES + FM_MAX_OPEN_DIRS')
original = a.path.read_bytes()
# Match only an actual numeric definition, not examples embedded in comments.
pattern = re.compile(rb'(?m)^(\s*#\s*define\s+(?:_FS_LOCK|FF_FS_LOCK)\s+)([0-9]+)(?=\s|/|$)')
matches = list(pattern.finditer(original))
if len(matches) != 1:
    raise SystemExit(f'{a.path}: expected exactly one numeric _FS_LOCK or FF_FS_LOCK definition; found {len(matches)}; unchanged')
m = matches[0]
updated = original[:m.start(2)] + str(a.slots).encode('ascii') + original[m.end(2):]
if updated != original:
    a.path.write_bytes(updated)
    print(f'{a.path}: lock slots {m.group(2).decode()} -> {a.slots}')
else:
    print(f'{a.path}: lock slots already {a.slots}')
