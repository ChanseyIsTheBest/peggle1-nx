#!/usr/bin/env python3
"""peggle2-nx: the raw shots of peggle1_shots_every_s (shot_<s>s_<w>x<h>.rgba) as PNGs, and a contact sheet.

  python tools/shots_to_png.py <shots folder> [--sheet out.jpg] [--scale 0.25]
"""
import argparse
import os
import re
import sys

from PIL import Image


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('folder')
    ap.add_argument('--sheet')
    ap.add_argument('--scale', type=float, default=0.25)
    a = ap.parse_args()
    shots = []
    for name in os.listdir(a.folder):
        m = re.fullmatch(r'shot_(\d+)s_(\d+)x(\d+)\.rgba', name)
        if not m:
            continue
        s, w, h = (int(x) for x in m.groups())
        data = open(os.path.join(a.folder, name), 'rb').read()
        if len(data) < w * h * 4:
            continue
        img = Image.frombytes('RGBA', (w, h), data[:w * h * 4]).convert('RGB')
        png = os.path.join(a.folder, name[:-5] + '.png')
        img.save(png)
        shots.append((s, img))
    shots.sort(key=lambda t: t[0])
    print('shots_to_png: %d shots in %s' % (len(shots), a.folder))
    if a.sheet and shots:
        tw, th = int(shots[0][1].width * a.scale), int(shots[0][1].height * a.scale)
        cols = 4
        rows = (len(shots) + cols - 1) // cols
        sheet = Image.new('RGB', (cols * tw, rows * th))
        for i, (s, img) in enumerate(shots):
            sheet.paste(img.resize((tw, th)), ((i % cols) * tw, (i // cols) * th))
        sheet.save(a.sheet, quality=85)
        print('shots_to_png: sheet %s' % a.sheet)


if __name__ == '__main__':
    sys.exit(main())
