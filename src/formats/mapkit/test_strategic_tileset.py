import struct
import tempfile
import unittest
from pathlib import Path

from . import strategic, tiled_strat, tlo


class StrategicTilesetTest(unittest.TestCase):
    def fixture(self, root, name=b"diuna256.tlo", next_byte=0):
        header = bytearray(0x61)
        header[:5] = b"ver02"
        struct.pack_into("<II", header, 5, 4, 4)
        header[0x0d:0x19] = name.ljust(12, b"\0")
        header[0x19] = next_byte
        struct.pack_into("<II", header, 0x2d, 32, 16)
        struct.pack_into("<I", header, 0x3d, 1)
        data = bytes(header) + bytes(4 * 4 * 4 + 16 + 16 + 16)
        path = root / "test.mp"
        path.write_bytes(data)
        tile_path = root / "diuna256.tlo"
        tile_path.write_bytes(bytes(512 + 1024))
        return data, strategic.parse(str(path)), tlo.parse(str(tile_path))

    def test_full_length_roundtrip_and_editor_change(self):
        with tempfile.TemporaryDirectory() as d:
            data, model, art = self.fixture(Path(d))
            self.assertEqual(model.tlo_name, b"diuna256.tlo")
            self.assertEqual(strategic.emit(model), data)
            tmj, _ = tiled_strat.export(model, art, "ground.png")
            self.assertEqual(strategic.emit(tiled_strat.import_(tmj)), data)
            next(p for p in tmj["properties"] if p["name"] == "mapkit_tlo")["value"] = "marsh_01.tlo"
            result = tiled_strat.import_(tmj)
            self.assertEqual(result.tlo_name, b"marsh_01.tlo")
            changed = strategic.emit(result)
            self.assertEqual(changed[0x0d:0x1a], b"marsh_01.tlo\0")
            self.assertEqual(changed[0x1a:], data[0x1a:])

    def test_opaque_header_byte_preserved_unless_full_name_needs_nul(self):
        with tempfile.TemporaryDirectory() as d:
            data, model, art = self.fixture(Path(d), b"jungle.tlo", 0xa5)
            self.assertEqual(len(model.tlo_name), 12)
            self.assertEqual(strategic.emit(model), data)
            tmj, _ = tiled_strat.export(model, art, "ground.png")
            self.assertEqual(strategic.emit(tiled_strat.import_(tmj)), data)
            next(p for p in tmj["properties"] if p["name"] == "mapkit_tlo")["value"] = "marsh_01.tlo"
            changed = strategic.emit(tiled_strat.import_(tmj))
            self.assertEqual(changed[0x0d:0x1a], b"marsh_01.tlo\0")
            self.assertEqual(changed[0x1a:], data[0x1a:])

    def test_invalid_editor_names_are_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            _, model, art = self.fixture(Path(d))
            tmj, _ = tiled_strat.export(model, art, "ground.png")
            prop = next(p for p in tmj["properties"] if p["name"] == "mapkit_tlo")
            for name in ("", "abc/def.tlo", "012345678.tlo", "a.b.tlo", "caf\u00e9.tlo", "x.png"):
                prop["value"] = name
                with self.subTest(name=name), self.assertRaises(ValueError):
                    tiled_strat.import_(tmj)


if __name__ == "__main__":
    unittest.main()
