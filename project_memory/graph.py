#!/usr/bin/env python3
"""Rebuild, verify, or search the versioned project source inventory.

This is a lexical navigation aid, not a compiler, call graph, linker map or
proof that an include/feature is active. See README.md before using results.
"""
from __future__ import annotations

import argparse
from collections import defaultdict
import hashlib
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parent.parent
LIB = 'Clain_FileManager/FileManager_lib'
PROJECT = 'Clain_FileManager'
OUTPUT = 'project_memory/inventory.json'
INDEX = 'project_memory/MODULE_INDEX.md'
EXTENSIONS = {'.c', '.h', '.s', '.ld', '.ioc', '.json', '.py', '.sh', '.bat'}
SPECIAL = {'Makefile', 'CMakeLists.txt'}
SKIP_DIRS = {'.git', 'build', 'build-asan', 'dist', 'node_modules', '__pycache__', '.cache', '.venv'}
INCLUDE = re.compile(r'^\s*#\s*include\s*(["<])([^">]+)[">]', re.M)
# A deliberately conservative lexical hint, not an AST. Macros, function
# pointers, preprocessor branches and unusual declarators can be missed.
FUNCTION = re.compile(r'(?m)^[ \t]*(?:[A-Za-z_]\w*[ \t*]+){1,8}([A-Za-z_]\w*)[ \t]*\([^;{}]*\)[ \t\r\n]*\{')
NON_FUNCTIONS = {'if', 'for', 'while', 'switch', 'return', 'sizeof'}
SEARCH_DIRS = [f'{LIB}/include', f'{LIB}/src', f'{LIB}/backends', f'{LIB}/top_api',
               f'{LIB}/examples', f'{LIB}/board', f'{LIB}/ports',
               f'{PROJECT}/Core/Inc', f'{PROJECT}/FATFS/App', f'{PROJECT}/FATFS/Target',
               f'{PROJECT}/USB_DEVICE/App', f'{PROJECT}/USB_DEVICE/Target']


def selected(root: Path) -> list[Path]:
    base = root / PROJECT
    if not base.is_dir():
        raise ValueError(f'missing project directory: {base}')
    result = []
    for path in base.rglob('*'):
        if not path.is_file():
            continue
        rel = path.relative_to(root)
        if any(part in SKIP_DIRS for part in rel.parts):
            continue
        # Fixture contents, vendor internals and generated build output are not
        # indexed: their tree names are listed separately in the human graph.
        if 'third_party' in rel.parts or 'vectors' in rel.parts or 'halstub' in rel.parts or 'rtosstub' in rel.parts:
            continue
        if path.suffix.lower() in EXTENSIONS or path.name in SPECIAL:
            result.append(path)
    return sorted(result)


def snapshot(root: Path) -> dict:
    files = selected(root)
    names = {p.relative_to(root).as_posix() for p in files}
    by_basename = defaultdict(list)
    for name in names:
        by_basename[Path(name).name].append(name)
    records = []
    for path in files:
        name = path.relative_to(root).as_posix()
        data = path.read_bytes()
        text = data.decode('utf-8', errors='replace')
        includes = []
        if path.suffix.lower() in {'.c', '.h'}:
            for match in INCLUDE.finditer(text):
                bracket, target = match.groups()
                # Candidate paths are all lexical matches, not a build-system
                # include resolution; preserve ambiguity rather than guessing.
                candidates = set()
                relative = (path.parent / target).resolve()
                try:
                    candidate = relative.relative_to(root.resolve()).as_posix()
                    if candidate in names:
                        candidates.add(candidate)
                except ValueError:
                    pass
                for directory in SEARCH_DIRS:
                    candidate = f'{directory}/{target}'
                    if candidate in names:
                        candidates.add(candidate)
                # Keep basename candidates as hints only if there is no direct
                # path; they can include files not selected by any build.
                if not candidates and '/' not in target:
                    candidates.update(by_basename.get(target, []))
                includes.append({'line': text.count('\n', 0, match.start()) + 1,
                                 'spelling': target, 'kind': 'quoted' if bracket == '"' else 'angle',
                                 'candidates': sorted(candidates)})
        functions = []
        if path.suffix.lower() in {'.c', '.h'}:
            for match in FUNCTION.finditer(text):
                symbol = match.group(1)
                if symbol not in NON_FUNCTIONS:
                    functions.append({'name': symbol, 'line': text.count('\n', 0, match.start(1)) + 1})
        records.append({'path': name, 'sha256': hashlib.sha256(data).hexdigest(),
                        'bytes': len(data), 'includes': includes, 'definitions_hint': functions})
    vendor = root / LIB / 'third_party'
    vendors = sorted(p.name for p in vendor.iterdir() if p.is_dir()) if vendor.is_dir() else []
    return {'schema': 1, 'scope': 'first-party source/config scripts; excludes vendor internals, fixtures and build artifacts',
            'limitations': 'lexical hints only; includes can be conditional, unresolved or ambiguous; no call or memory-layout inference',
            'vendor_directories': vendors, 'files': records}


def inventory_json(data: dict) -> str:
    return json.dumps(data, ensure_ascii=False, indent=2, sort_keys=True) + '\n'


def module_index(data: dict) -> str:
    prefix = f'{LIB}/src/'
    names = {x['path'] for x in data['files']}
    lines = ['# Generated source module index', '',
             '> Generated by `python3 project_memory/graph.py update`. Do not edit by hand.',
             '> One implementation can cover several APIs; matching headers are naming hints, not proof of exclusivity.', '',
             '| Implementation | Matching public header | Lexical definitions |', '|---|---|---:|']
    for item in data['files']:
        path = item['path']
        if not path.startswith(prefix) or not path.endswith('.c'):
            continue
        header = f"{LIB}/include/{Path(path).stem}.h"
        if header not in names:
            header = f"{LIB}/src/{Path(path).stem}.h"
        hdr = f'[`{Path(header).name}`](../{header})' if header in names else '— / shared interface'
        lines.append(f'| [`{Path(path).name}`](../{path}) | {hdr} | {len(item["definitions_hint"])} |')
    lines += ['', f"First-party indexed files: **{len(data['files'])}**.",
              'Vendor directories (not indexed internally): ' + ', '.join(f'`{x}`' for x in data['vendor_directories']) + '.',
              '', 'For symbols and include candidates run `python3 project_memory/graph.py query <term>`.', '']
    return '\n'.join(lines)


def diff_snapshot(old: dict, new: dict) -> list[str]:
    before = {x['path']: x['sha256'] for x in old.get('files', [])}
    after = {x['path']: x['sha256'] for x in new['files']}
    changes = [f'added: {p}' for p in sorted(after.keys() - before.keys())]
    changes += [f'removed: {p}' for p in sorted(before.keys() - after.keys())]
    changes += [f'changed: {p}' for p in sorted(before.keys() & after.keys()) if before[p] != after[p]]
    if old.get('vendor_directories') != new['vendor_directories']:
        changes.append('vendor directory list changed')
    return changes


def link_errors(root: Path) -> list[str]:
    """Check local Markdown link paths (not fragment anchors or semantic claims)."""
    errors = []
    docs = list((root / 'project_memory').glob('*.md'))
    if (root / 'AGENTS.md').is_file():
        docs.append(root / 'AGENTS.md')
    for doc in sorted(docs):
        text = doc.read_text(encoding='utf-8')
        for match in re.finditer(r'(?<!!)\[[^\]]+\]\(([^)]+)\)', text):
            target = match.group(1).split('#', 1)[0]
            if not target or '://' in target or target.startswith('mailto:'):
                continue
            if not (doc.parent / target).exists():
                errors.append(f'{doc.relative_to(root)}: missing link {target}')
    return errors


def search(data: dict, term: str) -> int:
    q = term.lower()
    hits = 0
    for item in data['files']:
        matches = [s for s in item['definitions_hint'] if q in s['name'].lower()]
        inc = [i for i in item['includes'] if q in i['spelling'].lower()]
        if q not in item['path'].lower() and not matches and not inc:
            continue
        hits += 1
        print(item['path'])
        for symbol in matches:
            print(f"  definition hint L{symbol['line']}: {symbol['name']}")
        for edge in inc:
            candidates = ', '.join(edge['candidates']) or '(unresolved/external)'
            print(f"  include L{edge['line']}: {edge['spelling']} -> {candidates}")
    if not hits:
        print('No lexical matches. Search source directly: this index is incomplete.')
    return 0 if hits else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=['update', 'check', 'query'])
    parser.add_argument('term', nargs='?')
    args = parser.parse_args()
    output, index = ROOT / OUTPUT, ROOT / INDEX
    if args.command == 'query':
        if not args.term:
            parser.error('query requires a term')
        if not output.exists():
            print('Missing inventory; run update.', file=sys.stderr)
            return 2
        return search(json.loads(output.read_text(encoding='utf-8')), args.term)
    if args.term:
        parser.error('unexpected term')
    data = snapshot(ROOT)
    raw, md = inventory_json(data), module_index(data)
    if args.command == 'update':
        output.write_text(raw, encoding='utf-8')
        index.write_text(md, encoding='utf-8')
        print(f'Updated {len(data["files"])} first-party file entries.')
        return 0
    if not output.exists() or not index.exists():
        print('Missing generated graph files; run update.', file=sys.stderr)
        return 1
    if output.read_text(encoding='utf-8') != raw or index.read_text(encoding='utf-8') != md:
        old = json.loads(output.read_text(encoding='utf-8'))
        print('STALE: review architectural maps and run python3 project_memory/graph.py update')
        changes = diff_snapshot(old, data)
        for change in changes[:80]:
            print('  ' + change)
        if len(changes) > 80:
            print(f'  ... and {len(changes) - 80} more')
        if not changes:
            print('  generated format or include/symbol metadata changed')
        return 1
    errors = link_errors(ROOT)
    if errors:
        print('BROKEN LINKS:')
        for error in errors:
            print('  ' + error)
        return 1
    print(f'Graph inventory current: {len(data["files"])} first-party files; local link paths exist. Manual semantic review is still required.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
