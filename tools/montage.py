"""Contact sheet of screenshots: montage.py <out.png> <columns> <thumbnail width> <png>...

Each thumbnail is labelled with its file name. Needs Pillow (the user's Python: `py -E tools/montage.py ...`; it isn't
in tools/venv).
"""
import os
import sys

from PIL import Image, ImageDraw


def main():
    out, cols, tw = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    ims = []
    for f in sys.argv[4:]:
        im = Image.open(f).convert('RGB')
        im = im.resize((tw, im.height * tw // im.width))
        ImageDraw.Draw(im).text((4, 4), os.path.basename(f), fill=(255, 255, 0))
        ims.append(im)
    w, h = ims[0].size
    rows = (len(ims) + cols - 1) // cols
    m = Image.new('RGB', (w * cols, h * rows))
    for i, im in enumerate(ims):
        m.paste(im, ((i % cols) * w, (i // cols) * h))
    m.save(out)
    print(out, m.size)


if __name__ == '__main__':
    main()
