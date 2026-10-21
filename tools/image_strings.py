"""Finds strings in the image dump (out/default_image.bin): ASCII and UTF-16BE, by regular expression.

  python -P tools/image_strings.py <regex> [<regex> ...] [--max 8]
Prints each match's address and the whole string around it.
"""
import argparse
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASE = 0x82000000


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('patterns', nargs='+')
    ap.add_argument('--image', default=os.path.join(ROOT, 'out', 'default_image.bin'))
    ap.add_argument('--max', type=int, default=8)
    args = ap.parse_args()
    img = open(args.image, 'rb').read()
    for pat in args.patterns:
        n = 0
        for m in re.finditer(pat.encode('latin-1'), img):
            s = m.start()
            a = img.rfind(b'\0', 0, s) + 1
            e = img.find(b'\0', s)
            print(f'{pat}: {BASE + a:08X} {img[a:e][:100]!r}')
            n += 1
            if n >= args.max:
                break
        u16 = b''.join(bytes([0, c]) for c in pat.encode('latin-1') if c not in b'\\.^$*+?()[]{}|')
        if u16:
            for m in list(re.finditer(re.escape(u16), img))[:args.max]:
                s = m.start()
                a = s
                while a >= 2 and img[a - 2:a] != b'\0\0':
                    a -= 2
                e = s
                while e + 2 <= len(img) and img[e:e + 2] != b'\0\0':
                    e += 2
                print(f'{pat} (UTF-16): {BASE + a:08X} {img[a:e].decode("utf-16-be", "replace")[:100]!r}')


if __name__ == '__main__':
    main()
