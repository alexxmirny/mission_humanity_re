"""Unpack Mission Humanity / Exterminacja `.rsr`/`.nam` resource packs.

Each `<pack>.nam` is a table of 64-byte entries (filename[47], type[5], offset u32,
size u32, final_size u32); `<pack>.rsr` holds the LZW-compressed payloads back-to-back.
For every entry we write three artifacts under <out>/data/:
  raw/<pack>/<file>          the compressed bytes as stored
  uncompressed/<pack>/<file> the LZW-decompressed original (this is the file the game reads)
  converted/<pack>/<file>    gfx -> png (only for `*gfx*` names; needs Pillow; skip with --no-convert)

CLI (was previously a double-click script with hardcoded CWD + cProfile + input()):
  python unpack.py --src <clean-install> --out <clean-install>/res_unpack
  python unpack.py --src . --out . --packs mh --no-convert
"""
import argparse
import os
import pathlib
import struct

from decompress import Decompressor

decompressor = Decompressor()


class Entry:
    struct_format = '<47s5s3I'

    def __init__(self, data: bytes):
        r = struct.unpack(self.struct_format, data)
        self.filename = r[0].decode("ascii").split("\x00")[0]
        self.type = r[1].decode("ascii").split("\x00")[0]
        self.offset = r[2]
        self.size = r[3]
        self.final_size = r[4]


def write_file(path, data: bytes):
    path = pathlib.Path(path)
    os.makedirs(path.parent, exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


def convert_file(filename: str, data: bytes):
    if "gfx" in filename.lower():
        from convert_gfx import convert_gfx_to_png_bytes  # lazy: only needs Pillow when converting
        r = convert_gfx_to_png_bytes(data)
        if r is not None:
            return r, filename.lower().replace("gfx", "png")
    return None, ""


def handle_entry(out_dir, pack, entry, file_data, convert):
    write_file(os.path.join(out_dir, "data", "raw", pack, entry.filename), file_data)
    decompressed = decompressor.decompress(file_data)   # asserts final_size == len internally
    write_file(os.path.join(out_dir, "data", "uncompressed", pack, entry.filename), decompressed)
    if convert:
        conv, new_name = convert_file(entry.filename, decompressed)
        if conv is not None:
            write_file(os.path.join(out_dir, "data", "converted", pack, new_name), conv)
    return len(decompressed)


def unpack_pack(src_dir, out_dir, pack, convert=True):
    nam_path = os.path.join(src_dir, f"{pack}.nam")
    rsr_path = os.path.join(src_dir, f"{pack}.rsr")
    n = 0
    with open(nam_path, "rb") as nf, open(rsr_path, "rb") as rf:
        entry_data = nf.read(64)
        while len(entry_data) == 64:
            entry = Entry(entry_data)
            file_data = rf.read(entry.size)
            handle_entry(out_dir, pack, entry, file_data, convert)
            n += 1
            if n % 100 == 0:
                print(f"  {pack}: {n} files...", flush=True)
            entry_data = nf.read(64)
    return n


def main(src_dir=".", out_dir=".", packs=("mh", "mh_ex"), convert=True):
    for pack in packs:
        print(f"unpacking {pack}: {src_dir} -> {out_dir}/data/*/{pack}/", flush=True)
        n = unpack_pack(src_dir, out_dir, pack, convert)
        print(f"  {pack}: {n} files done", flush=True)


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--src", default=".", help="dir containing <pack>.rsr/.nam (default: cwd)")
    ap.add_argument("--out", default=".", help="output dir (default: cwd)")
    ap.add_argument("--packs", nargs="+", default=["mh", "mh_ex"])
    ap.add_argument("--no-convert", action="store_true", help="skip gfx->png (faster, no Pillow)")
    a = ap.parse_args()
    main(a.src, a.out, a.packs, not a.no_convert)
