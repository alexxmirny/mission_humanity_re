from typing import List
import numpy as np
import struct
import tqdm


class Decoder:
    lzw_dict = bytes()
    lzw_mask = 0x1ff
    lzw_shift = 0
    lzw_dict_size = 0
    lzw_bit_count = 9

    def __init__(self):
        self.lzw_dict = np.zeros(8192, dtype=np.uint32)
        self.word_sizes = np.zeros(8192, dtype=np.uint32)

    def full_reset(self):
        self.lzw_shift = 0
        self.lzw_dict.fill(0)
        self.word_sizes.fill(0)
        self.reset()

    def reset(self):
        self.lzw_mask = 0x1ff
        self.lzw_dict_size = 0
        self.lzw_bit_count = 9

    def update_dict(self, lzw_current_word, lzw_current_word_2):
        index = lzw_current_word_2
        if lzw_current_word_2 == self.lzw_dict_size + 0x102:  # 258
            index = lzw_current_word
        while index > 0x101:  # 257
            index = self.lzw_dict[index * 2 - 516]
        self.lzw_dict[self.lzw_dict_size * 2] = lzw_current_word
        self.lzw_dict[self.lzw_dict_size * 2 + 1] = index
        # if lzw_current_word > 0x101:
        #     self.word_sizes[self.lzw_dict_size] = self.word_sizes[lzw_current_word - 258] + 1
        # else:
        #     self.word_sizes[self.lzw_dict_size] = 1
        self.lzw_dict_size += 1
        if self.lzw_dict_size not in [0xfe, 0x2fe, 0x6fe]:
            return
        self.lzw_bit_count += 1
        self.lzw_mask = self.lzw_mask << 1 | 1

    def fetch_word(self, input: bytearray):
        word = int.from_bytes(input, "little")
        word = word >> (self.lzw_shift & 0b00011111)
        word = word & self.lzw_mask
        self.lzw_shift += self.lzw_bit_count
        shift = 0
        while self.lzw_shift > 7:
            shift += 1
            self.lzw_shift -= 8

        return word, shift

    def handle_code(self, word: int) -> List[int]:
        if word < 0x102:  # 258
            return [0xFF & word]
        o = list()
        # o_word = word
        while 0x101 < word:
            o.append(0xFF & self.lzw_dict[word * 2 - 515])
            word = self.lzw_dict[word * 2 - 516]
        o.append(0xFF & word)
        # assert len(o) == self.word_sizes[o_word]
        return reversed(o)


class FastOutput:
    def __init__(self, size: int):
        self.data = np.zeros(size, dtype=np.uint8)
        self.size = 0
        self.pbar = tqdm.tqdm(total=size, desc="Uncompressing    ", position=-1, leave=False)

    def append(self, e):
        assert self.size < self.data.size
        self.data[self.size] = e
        self.size += 1
        self.pbar.update(1)

    def extend(self, e_list):
        for e in e_list:
            self.append(e)

    def to_bytes(self):
        self.pbar.close()
        return self.data.tobytes()


class Decompressor:
    decoder: Decoder

    def __init__(self):
        self.decoder = Decoder()
        pass

    def decompress(self, input: bytes) -> bytes:
        in_ptr: int = 0
        decoder = self.decoder
        max_depth = 0
        max_iter = 0

        header, unc_size, in_size = struct.unpack('<4sLL', input[0:12])
        header = input[0:4]
        if header != b'LZW ':
            return input

        assert in_size == len(input)
        output: FastOutput = FastOutput(unc_size)

        in_ptr = 12
        total_calls = 0

        def decompress_impl(depth):
            nonlocal in_ptr, max_depth, max_iter, total_calls
            total_calls += 1
            max_depth = max(depth, max_depth)
            word, shift = decoder.fetch_word(input[in_ptr:in_ptr+3])
            in_ptr = in_ptr + shift
            if word == 0x101:
                return None
            if word != 0x100:
                return word

            assert in_ptr < len(input)

            decoder.reset()
            word_L = decompress_impl(depth + 1)
            if word_L is None:
                return None
            output.extend(decoder.handle_code(word_L))
            iter = 0
            while True:
                iter += 1
                word_H = decompress_impl(depth + 1)
                if word_H is None:
                    max_iter = max(max_iter, iter)
                    return None
                decoder.update_dict(word_L, word_H)
                word_L = word_H
                output.extend(decoder.handle_code(word_L))

        decoder.full_reset()
        word_L = decompress_impl(1)
        if word_L is not None:
            raise Exception("invalid compressed data")
        output = output.to_bytes()
        # print(max_depth, max_iter, total_calls)
        assert unc_size == len(output)
        return output
