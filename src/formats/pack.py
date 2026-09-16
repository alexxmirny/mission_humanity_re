import os
import sys
from typing import List

# repo root is three levels up (src/formats/pack.py); import machine_config for the game-data roots.
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
import machine_config as machine  # noqa: E402


class Entry():
    offset: int
    size: int
    final_size: int
    path: str

    def __init__(self, path: str, rsr_path: str):
        self.size = os.path.getsize(filename=path)
        # TODO read from LZW if there is a LZW header
        self.final_size = self.size
        self.path = path
        self.rsr_path = rsr_path

    def to_byte_array(self, offset):
        r = bytearray()
        r.extend(self.rsr_path.encode("ascii"))
        r.extend([0] * (47 - len(r)))
        r.extend("NONE\x00".encode("ascii"))
        r.extend(offset.to_bytes(length=4, byteorder='little'))
        r.extend(self.size.to_bytes(length=4, byteorder='little'))
        r.extend(self.final_size.to_bytes(length=4, byteorder='little'))
        assert len(r) == 64
        return r

    def get_file(self) -> bytes:
        with open(self.path, "rb") as in_f:
            r = in_f.read()
        return r


def pack(input: str, output: str, packs: List[str]):
    for pack in packs:
        out_files: List[Entry] = []
        pack_dir = os.path.join(input, pack)
        for path, dirs, files in os.walk(pack_dir):
            for name in files:
                full_path = os.path.join(path, name)
                rsr_path = os.path.relpath(full_path, pack_dir)
                out_files.append(Entry(full_path, rsr_path))

        with open(f"{output}/{pack}.rsr", "wb") as rsr_f:
            with open(f"{output}/{pack}.nam", "wb") as nam_f:
                offset = 0
                for entry in out_files:
                    nam_f.write(entry.to_byte_array(offset))
                    offset = offset + rsr_f.write(entry.get_file())


def main():
    # Main packing logic
    # --------------------------
    # input - directory with unpacked files. It contains subdirectories
    # each corresponding to a distinct rsr/nam pair
    # output - directory to write packed rsr/nam files
    # packs - list of subdirectory names to process
    # WARNING: existing files in output directory will be overwritten
    # --------------------------
    input = machine.RU_POLYGON + "/res_unpack"
    output = machine.RU_POLYGON
    packs = ['mh', 'mh_ex']
    pack(input, output, packs)


if __name__ == "__main__":
    main()
