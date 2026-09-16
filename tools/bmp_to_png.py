#!/usr/bin/env python3
"""bmp_to_png.py -- convert capture_*.bmp (from the gfx_capture UI harness) to PNG for viewing.

Usage: python tools/bmp_to_png.py <file-or-dir> [...]   (dirs are scanned for capture_*.bmp)
"""

import os
import sys

from PIL import Image


def convert(path):
    out = os.path.splitext(path)[0] + ".png"
    Image.open(path).convert("RGB").save(out)
    print(out)


def main(args):
    if not args:
        print(__doc__)
        return 1
    for a in args:
        if os.path.isdir(a):
            for name in sorted(os.listdir(a)):
                if name.lower().startswith("capture_") and name.lower().endswith(".bmp"):
                    convert(os.path.join(a, name))
        else:
            convert(a)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
