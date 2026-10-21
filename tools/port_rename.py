"""One-off (2026-10-21): turns the copy of peggle2-nx's app and tools into peggle1-nx's names.

Only the project's identifiers and file names change: peggle2 -> peggle1, Peggle2 -> Peggle1, PEGGLE2 -> PEGGLE1
(keys, macros, targets, files). Prose keeps "Peggle 2" and "peggle2-nx": the comments record what was found in
Peggle 2's port, and stay true as history. Paths that must name this project (the NRO's place on the SD card, the
delivery folder) are changed by hand where they are used.

  python -P tools/port_rename.py --from <peggle2-nx checkout> app tools [--skip file ...]

Each file of the given folders in the peggle2-nx checkout (git's tracked files) is written to its renamed place here,
unless it is listed in --skip (files this port has already changed by hand).
"""
import argparse
import os
import re
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEXT_EXT = {'.cpp', '.h', '.hpp', '.c', '.inc', '.in', '.txt', '.cmake', '.json', '.toml', '.py', '.ps1', '.sh', '.md',
            '.hlsl', '.glsl', '.frag', '.vert', '.comp', '.mjs', '.js', '.yml', '.bat', ''}
# peggle2 as part of an identifier or a file name, but not in peggle2-nx.
IDENT = [
    (re.compile(r'peggle2(?!-nx)'), 'peggle1'),
    (re.compile(r'Peggle2'), 'Peggle1'),
    (re.compile(r'PEGGLE2'), 'PEGGLE1'),
]


def rename_text(text):
    for rx, rep in IDENT:
        text = rx.sub(rep, text)
    return text


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--from', dest='src', required=True)
    ap.add_argument('folders', nargs='+')
    ap.add_argument('--skip', nargs='*', default=[])
    args = ap.parse_args()
    skip = {os.path.normpath(s) for s in args.skip}
    files = subprocess.run(['git', '-C', args.src, 'ls-files', '--', *args.folders], capture_output=True, text=True,
                           check=True).stdout.split('\n')
    written = skipped = binary = 0
    for rel in files:
        if not rel or '/generated/' in rel or '/thirdparty/' in rel:
            continue
        target = os.path.normpath(rename_text(rel))
        if target in skip:
            skipped += 1
            continue
        raw = open(os.path.join(args.src, rel), 'rb').read()
        ext = os.path.splitext(rel)[1].lower()
        out = raw
        if ext in TEXT_EXT and b'\0' not in raw[:4096]:
            try:
                out = rename_text(raw.decode('utf-8')).encode('utf-8')
            except UnicodeDecodeError:
                binary += 1
        os.makedirs(os.path.dirname(os.path.join(ROOT, target)), exist_ok=True)
        open(os.path.join(ROOT, target), 'wb').write(out)
        written += 1
    print(f'{written} files written, {skipped} skipped (changed by hand here), {binary} not UTF-8 (copied as they are)')


if __name__ == '__main__':
    main()
