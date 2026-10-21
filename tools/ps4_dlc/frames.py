"""Saves chosen frames of a raw recording (NAME.rgba with NAME.txt = "width height ...") as PNGs, or a contact sheet.

  python tools/ps4_dlc/frames.py <video.rgba> <out folder> <tag> <frame,frame,...>
  python tools/ps4_dlc/frames.py <video.rgba> <out folder> <tag> sheet:<step>[:<columns>[:<first>[:<last>]]]
      one JPEG with every <step>-th frame, each labelled with its number (a quick look at a whole run)
"""
import os
import sys

from PIL import Image, ImageDraw


def main():
    raw, out, tag, what = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
    w, h = (int(x) for x in open(os.path.splitext(raw)[0] + '.txt').read().split()[:2])
    size = w * h * 4
    count = os.path.getsize(raw) // size
    os.makedirs(out, exist_ok=True)
    if count == 0:
        print('the recording is empty (the run ended before it started?)')
        return
    with open(raw, 'rb') as f:
        def frame(n):
            f.seek(n * size)
            return Image.frombytes('RGBA', (w, h), f.read(size)).convert('RGB')
        if what.startswith('sheet:'):
            p = what.split(':')
            step = int(p[1])
            cols = int(p[2]) if len(p) > 2 else 5
            first = int(p[3]) if len(p) > 3 else 0
            last = min(int(p[4]), count - 1) if len(p) > 4 else count - 1
            nums = list(range(first, last + 1, step))
            tw, th = w // 2, h // 2
            rows = (len(nums) + cols - 1) // cols
            sheet = Image.new('RGB', (cols * tw, rows * th))
            draw = ImageDraw.Draw(sheet)
            for i, n in enumerate(nums):
                x, y = (i % cols) * tw, (i // cols) * th
                sheet.paste(frame(n).resize((tw, th), Image.BILINEAR), (x, y))
                draw.rectangle([x, y, x + 40, y + 12], fill=(0, 0, 0))
                draw.text((x + 2, y + 1), str(n), fill=(255, 255, 0))
            path = os.path.join(out, f'{tag}_sheet.jpg')
            sheet.save(path, quality=82)
            print(path, f'{sheet.size[0]}x{sheet.size[1]}, frames {nums[0]}..{nums[-1]} step {step} of {count}')
            return
        for n in [int(x) for x in what.split(',')]:
            if n >= count:
                print(f'frame {n}: the recording has {count} frames')
                continue
            path = os.path.join(out, f'{tag}_{n:04d}.png')
            frame(n).save(path)
            print(path, f'{w}x{h}')


if __name__ == '__main__':
    main()
