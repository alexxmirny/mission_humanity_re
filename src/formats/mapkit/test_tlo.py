"""Synthetic TLO/PNG authoring tests; no game data required."""
from __future__ import annotations

import contextlib
import io
import tempfile
import unittest
from pathlib import Path

from PIL import Image

from . import tlo, tlo_cli
from .common import rgb565_to_888, rgb888_to_565


def model(count=3):
    m = tlo.TloModel()
    m.geom, m.tile_w, m.tile_h, m.stride = "biome", 32, 32, 1024
    m.count = count
    m.palette = [(i * 257) & 65535 for i in range(256)]
    m.palette[1] = m.palette[0]  # Duplicate colors do not merge indices.
    m.palette[255] = 0xffff
    m.tiles = bytes((i * 37 + i // 1024) & 255 for i in range(count * 1024))
    return m


class TloPngTests(unittest.TestCase):
    def test_all_rgb565_colors_survive_expansion_and_packing(self):
        for value in range(65536):
            self.assertEqual(rgb888_to_565(*rgb565_to_888(value)), value)

    def test_indexed_roundtrip_through_png_file(self):
        original = model(5)
        atlas = tlo.to_png(original, columns=3)
        data = io.BytesIO()
        atlas.save(data, format="PNG")
        data.seek(0)
        with Image.open(data) as decoded:
            restored = tlo.from_png(decoded, 5)
        self.assertEqual(tlo.emit(restored), tlo.emit(original))
        self.assertEqual(restored.palette[1], restored.palette[0])
        self.assertEqual(restored.palette[255], 65535)
        self.assertIn(255, restored.tiles)

    def test_partial_row_requires_exact_count_and_zero_padding(self):
        atlas = tlo.to_png(model(4), columns=3)
        self.assertEqual(tlo.from_png(atlas, 4).count, 4)
        with self.assertRaises(ValueError):
            tlo.from_png(atlas, 3)  # An extra full row is not padding.
        atlas.putpixel((32, 32), 255)
        with self.assertRaises(ValueError):
            tlo.from_png(atlas, 4)

    def test_dimensions_count_and_model_bounds(self):
        for size, count in [((31, 32), 1), ((32, 33), 1), ((32, 32), 0),
                            ((32, 32), 8193), ((32, 32), 2), ((32, 64), 1)]:
            with self.subTest(size=size, count=count), self.assertRaises(ValueError):
                tlo.from_png(Image.new("P", size), count)
        bad = model()
        bad.tiles = bad.tiles[:-1]
        with self.assertRaises(ValueError):
            tlo.emit(bad)
        bad = model()
        bad.count = 8193
        with self.assertRaises(ValueError):
            tlo.emit(bad)
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "bad.tlo"
            for data in (b"", bytes(511), bytes(512), bytes(513),
                         bytes(512) + bytes(8193 * 1024)):
                path.write_bytes(data)
                with self.assertRaises(ValueError):
                    tlo.parse(str(path), "biome")

    def test_index_outside_palette_is_invalid(self):
        img = Image.new("P", (32, 32), 1)
        img.putpalette([0, 0, 0])
        with self.assertRaises(ValueError):
            tlo.from_png(img, 1)

    def test_nonopaque_alpha_rejected_without_index_255_convention(self):
        atlas = tlo.to_png(model(1), columns=1)
        atlas.info["transparency"] = 255
        with self.assertRaises(ValueError):
            tlo.from_png(atlas, 1)
        atlas.info["transparency"] = bytes([255] * 255 + [254])
        with self.assertRaises(ValueError):
            tlo.from_png(atlas, 1)
        atlas.info["transparency"] = bytes([255] * 256)
        self.assertEqual(tlo.from_png(atlas, 1).tiles, model(1).tiles)
        rgba = Image.new("RGBA", (32, 32), (1, 2, 3, 255))
        rgba.putpixel((2, 3), (1, 2, 3, 254))
        with self.assertRaises(ValueError):
            tlo.from_png(rgba, 1, quantize=True)

    def test_explicit_deterministic_quantization(self):
        img = Image.new("RGB", (32, 32))
        for y in range(32):
            for x in range(32):
                img.putpixel((x, y), (x * 8, y * 8, (x + y) * 4))
        with self.assertRaises(ValueError):
            tlo.from_png(img, 1)
        first = tlo.from_png(img, 1, quantize=True)
        second = tlo.from_png(img, 1, quantize=True)
        self.assertEqual(tlo.emit(first), tlo.emit(second))
        self.assertEqual(first.tiles[31], (248 & 0xe0) | (124 >> 6))
        self.assertEqual(first.palette[255], rgb888_to_565(255, 255, 255))
        padded = Image.new("RGB", (64, 32))
        padded.putpixel((32, 0), (1, 0, 0))
        with self.assertRaises(ValueError):
            tlo.from_png(padded, 1, quantize=True)

    def test_cli_refuses_invalid_without_touching_output_and_overwrite(self):
        with tempfile.TemporaryDirectory() as tmp, contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            root = Path(tmp)
            source, atlas, out = root / "in.tlo", root / "atlas.png", root / "out.tlo"
            source.write_bytes(tlo.emit(model(3)))
            self.assertEqual(tlo_cli.main(["export", str(source), str(atlas), "--columns", "2"]), 0)
            self.assertEqual(tlo_cli.main(["import", str(atlas), str(out), "--count", "3"]), 0)
            self.assertEqual(out.read_bytes(), source.read_bytes())
            self.assertEqual(tlo_cli.main(["import", str(atlas), str(out), "--count", "3"]), 1)
            self.assertEqual(tlo_cli.main(["import", str(atlas), str(out), "--count", "1", "--force"]), 1)
            self.assertEqual(out.read_bytes(), source.read_bytes())
            self.assertEqual(tlo_cli.main(["import", str(atlas), str(out), "--count", "3", "--force"]), 0)


if __name__ == "__main__":
    unittest.main()
