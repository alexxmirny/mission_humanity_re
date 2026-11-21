"""Convert strategic TLO files to indexed PNG atlases and back.

Export prints the tile count; supply that count when importing the atlas.
Example: python -m src.formats.mapkit.tlo_cli export in.tlo out.png
         python -m src.formats.mapkit.tlo_cli import out.png out.tlo --count 123
"""
from __future__ import annotations

import argparse
import io
import sys

from PIL import Image

from . import tlo


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="command", required=True)
    export = sub.add_parser("export", help="strategic TLO to indexed PNG")
    export.add_argument("source")
    export.add_argument("output")
    export.add_argument("--columns", type=int, default=32)
    export.add_argument("--force", action="store_true", help="overwrite output")
    imp = sub.add_parser("import", help="PNG atlas to strategic TLO")
    imp.add_argument("source")
    imp.add_argument("output")
    imp.add_argument("--count", type=int, required=True, help="number of tiles excluding padding")
    imp.add_argument("--quantize", action="store_true", help="explicit fixed RGB332 quantization")
    imp.add_argument("--force", action="store_true", help="overwrite output")
    args = ap.parse_args(argv)
    try:
        if args.command == "export":
            model = tlo.parse(args.source, "biome")
            img = tlo.to_png(model, args.columns)
            buf = io.BytesIO()
            img.save(buf, format="PNG")
            data = buf.getvalue()
        else:
            with Image.open(args.source) as img:
                if img.format != "PNG":
                    raise ValueError("input must be PNG")
                model = tlo.from_png(img, args.count, quantize=args.quantize)
            data = tlo.emit(model)
        with open(args.output, "wb" if args.force else "xb") as out:
            out.write(data)
    except (OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    print(f"{args.source} -> {args.output} ({model.count} tiles; import with --count {model.count})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
