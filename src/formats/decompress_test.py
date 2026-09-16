import binascii
import os
import sys

from decompress import Decompressor

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
import machine_config as machine  # noqa: E402

path_to_file = machine.RU_POLYGON + "/res_unpack/mh_/init/INIT.CFG"
path_to_output = path_to_file + ".py_dec"

with open(path_to_file, "rb") as f:
    # header = f.read(4)
    # assert header == b'LZW '
    # uncompressed_size = int.from_bytes(f.read(4), "little")
    # compressed_size = int.from_bytes(f.read(4), "little")
    allbytes = bytearray(f.read())


result = Decompressor().decompress(allbytes)
assert binascii.crc32(result) == 917366519


# with open(path_to_output, "wb") as f:
#     f.write(result)
print("_ok")
