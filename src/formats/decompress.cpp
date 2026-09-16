#include <vector>
#include <cstdint>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <iostream>
#include <algorithm>
#include <functional>

class Decoder {
public:
    Decoder() {
        lzw_dict.assign(8192, 0);
        full_reset();
    }

    void full_reset() {
        lzw_shift = 0;
        std::fill(lzw_dict.begin(), lzw_dict.end(), 0);
        reset();
    }

    void reset() {
        lzw_mask = 0x1ff;
        lzw_dict_size = 0;
        lzw_bit_count = 9;
    }

    void update_dict(uint32_t lzw_current_word, uint32_t lzw_current_word_2) {
        uint32_t _word = lzw_current_word_2;
        uint32_t index = _word;
        while (index > 0x101) { // 257
            _word = lzw_current_word;
            if (index != lzw_dict_size + 0x102) { // 258
                _word = lzw_dict[index * 2 - 516];
            }
            index = _word;
        }
        lzw_dict[lzw_dict_size * 2] = lzw_current_word;
        lzw_dict[lzw_dict_size * 2 + 1] = index;
        lzw_dict_size += 1;
        if (lzw_dict_size == 0xFE || lzw_dict_size == 0x2FE || lzw_dict_size == 0x6FE) {
            lzw_bit_count += 1;
            lzw_mask = (lzw_mask << 1) | 1;
        }
    }

    // read up to 3 bytes from data pointer (caller may pass fewer bytes); returns (word, bytes_consumed)
    std::pair<uint32_t,int> fetch_word(const uint8_t* data, size_t available) {
        uint32_t word = 0;
        // read little-endian up to 3 bytes (missing bytes treated as 0)
        for (size_t i = 0; i < 3 && i < available; ++i) {
            word |= uint32_t(data[i]) << (8 * i);
        }
        word = word >> (lzw_shift & 0x1F);
        word = word & lzw_mask;
        lzw_shift += lzw_bit_count;
        int shift = 0;
        while (lzw_shift > 7) {
            ++shift;
            lzw_shift -= 8;
        }
        return {word, shift};
    }

    // returns decoded bytes in correct order
    std::vector<uint8_t> handle_code(uint32_t word) {
        if (word < 0x102) { // 258
            return std::vector<uint8_t>{ static_cast<uint8_t>(0xFF & word) };
        }
        std::vector<uint8_t> o;
        while (word > 0x101) {
            uint32_t idx = word * 2 - 515;
            uint8_t b = static_cast<uint8_t>(0xFF & lzw_dict[idx]);
            o.push_back(b);
            word = lzw_dict[word * 2 - 516];
        }
        o.push_back(static_cast<uint8_t>(0xFF & word));
        std::reverse(o.begin(), o.end());
        return o;
    }

private:
    std::vector<uint32_t> lzw_dict;
    uint32_t lzw_mask = 0x1ff;
    int lzw_shift = 0;
    uint32_t lzw_dict_size = 0;
    int lzw_bit_count = 9;
};

class FastOutput {
public:
    FastOutput(size_t size) : data(size), size_written(0) {}
    void append(uint8_t e) {
        if (size_written >= data.size()) throw std::runtime_error("output overflow");
        data[size_written++] = e;
    }
    void extend(const std::vector<uint8_t>& list) {
        for (auto b : list) append(b);
    }
    std::vector<uint8_t> to_vector() {
        if (size_written != data.size()) data.resize(size_written);
        return data;
    }
private:
    std::vector<uint8_t> data;
    size_t size_written;
};

class Decompressor {
public:
    Decompressor() : decoder() {}

    std::vector<uint8_t> decompress(const std::vector<uint8_t>& input) {
        if (input.size() < 12) throw std::runtime_error("input too small");
        // parse header: <4sLL> little-endian
        const uint8_t* ptr = input.data();
        if (!(ptr[0] == 'L' && ptr[1] == 'Z' && ptr[2] == 'W' && ptr[3] == ' ')) {
            // not compressed, return copy
            return input;
        }
        uint32_t unc_size = read_u32_le(ptr + 4);
        uint32_t in_size = read_u32_le(ptr + 8);
        if (in_size != input.size()) throw std::runtime_error("in_size mismatch");

        FastOutput output(unc_size);
        size_t in_ptr = 12;
        decoder.full_reset();

        int max_depth = 0, max_iter = 0;
        int total_calls = 0;

        // recursive lambda using std::function
        std::function<std::optional<uint32_t>(int)> decompress_impl;
        decompress_impl = [&](int depth) -> std::optional<uint32_t> {
            total_calls++;
            if (depth > max_depth) max_depth = depth;
            // fetch word from input starting at in_ptr (up to 3 bytes)
            size_t available = (in_ptr < input.size()) ? std::min<size_t>(3, input.size() - in_ptr) : 0;
            auto [word, shift] = decoder.fetch_word(in_ptr < input.size() ? input.data() + in_ptr : nullptr, available);
            in_ptr += shift;
            if (word == 0x101) return std::nullopt;
            if (word != 0x100) return word;

            if (in_ptr >= input.size()) throw std::runtime_error("unexpected EOF");

            decoder.reset();
            auto maybe_word_L = decompress_impl(depth + 1);
            if (!maybe_word_L.has_value()) return std::nullopt;
            auto word_L = maybe_word_L.value();
            output.extend(decoder.handle_code(word_L));
            int iter = 0;
            while (true) {
                ++iter;
                auto maybe_word_H = decompress_impl(depth + 1);
                if (!maybe_word_H.has_value()) {
                    if (iter > max_iter) max_iter = iter;
                    return std::nullopt;
                }
                uint32_t word_H = maybe_word_H.value();
                decoder.update_dict(word_L, word_H);
                word_L = word_H;
                output.extend(decoder.handle_code(word_L));
            }
        };

        auto root = decompress_impl(1);
        if (root.has_value()) throw std::runtime_error("invalid compressed data");
        auto outvec = output.to_vector();
        std::cout << "max_depth=" << max_depth << " max_iter=" << max_iter << " total_calls=" << total_calls << "\n";
        if (outvec.size() != unc_size) throw std::runtime_error("output size mismatch");
        return outvec;
    }

private:
    Decoder decoder;

    static uint32_t read_u32_le(const uint8_t* p) {
        return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
    }
};