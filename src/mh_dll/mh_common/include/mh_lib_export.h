#pragma once

namespace mh::lzw {
int Decompress(char *input, char *output, size_t inputSize);
}

namespace mh::misc {
_declspec(align(1)) struct sight_line {
    char x   = 0x80;
    char y   = 0x80;
    char len = 0x80;
};
sight_line *GetSightAreaFromRadius(unsigned char r);
} // namespace mh::misc