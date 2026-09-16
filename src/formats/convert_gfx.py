import io
import struct
from PIL import Image
import os

from tqdm import tqdm


def get_mask(size: int):
    return (1 << size) - 1


class PixelConverter:

    def __init__(self):
        self.c_sizes = [5, 6, 5]
        self.c_masks = [get_mask(c_size) for c_size in self.c_sizes]

    def convert(self, pixel: int):
        r = []
        for c_size, c_mask in zip(self.c_sizes, self.c_masks):
            c = pixel & c_mask
            c = 256 * c / float((1 << c_size) - 1)
            c = int(round(c))
            r.append(c)
            pixel = pixel >> c_size
        return (r[2], r[1], r[0])


pixel_converter = PixelConverter()


# Convert a menu.res GFX format file to PNG
def convert_gfx_to_png_bytes(data: bytes):
    if data[0:4] == b'dupa':
        print("dupa format")
        return None
    w = struct.unpack('<H', data[0:2])[0]
    h = struct.unpack('<H', data[2:4])[0]

    readlen = len(data) - 4
    if readlen != 2 * w * h:
        print("readlen != 2 *  w * h")
        return None
    pixel_ct = int(readlen / 2)
    struct_str = '<' + str(pixel_ct) + 'H'
    b = struct.unpack(struct_str, data[4:])
    image = Image.new('RGB', (w, h))
    pixels = image.load()
    for i in tqdm(range(0, pixel_ct), desc="Converting to PNG", position=-1, leave=False):
        pixels[int(i % w), int(i / w)] = pixel_converter.convert(b[i])
    img_byte_arr = io.BytesIO()
    image.save(img_byte_arr, format="PNG")
    return img_byte_arr.getvalue()


# Convert a menu.res GFX format file to PNG
def convert_gfx_to_png(filename: str):
    in_file = filename
    with open(in_file, "rb") as f:
        head = f.read(4)
        if head == b'dupa':
            print("dupa format")
            return
        f.seek(0)
        w = struct.unpack('<H', f.read(2))[0]
        h = struct.unpack('<H', f.read(2))[0]

        print("Converting " + filename + " to PNG")
        readlen = os.path.getsize(in_file) - 4
        if readlen != 2 * w * h:
            print("readlen != 2 *  w * h")
            return
        pixel_ct = int(readlen / 2)
        struct_str = '<' + str(pixel_ct) + 'H'
        b = struct.unpack(struct_str, f.read(readlen))
        image = Image.new('RGB', (w, h))
        pixels = image.load()
        for i in tqdm(range(0, pixel_ct)):
            pixels[int(i % w), int(i / w)] = pixel_converter.convert(b[i])
        image.tobytes()
        image.save(filename.lower().replace("gfx", "png"), "PNG")
