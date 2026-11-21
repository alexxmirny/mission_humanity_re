// Exact native-queue input snapshots. MHOR v2 is distinct from the UI journal binding profile.
#pragma once

#include <cstddef>
#include <cstdint>

namespace mh::order_stream {
inline constexpr uint32_t MAGIC               = 0x524f484du;
inline constexpr uint32_t VERSION             = 2;
inline constexpr uint32_t RECORD_BYTES        = 68;
inline constexpr size_t   HEADER_BYTES        = 12;
inline constexpr size_t   SAMPLE_HEADER_BYTES = 12;

enum class phase : uint32_t { hash  = 1,
                              entry = 2 };

inline uint32_t read_u32(const uint8_t *p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
inline void write_u32(uint8_t *p, uint32_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
    p[2] = uint8_t(v >> 16);
    p[3] = uint8_t(v >> 24);
}
inline void write_header(uint8_t *out) {
    write_u32(out, MAGIC);
    write_u32(out + 4, VERSION);
    write_u32(out + 8, RECORD_BYTES);
}
inline void write_sample_header(uint8_t *out, uint32_t step, phase p, uint32_t count) {
    write_u32(out, step);
    write_u32(out + 4, static_cast<uint32_t>(p));
    write_u32(out + 8, count);
}

struct view {
    const uint8_t *records = nullptr;
    uint32_t       step    = 0;
    uint32_t       count   = 0;
};

// Validate the ENTIRE stream before publishing any queue bytes. No inferred empty samples,
// partial records, legacy framing, skipped steps, duplicate phases or incomplete last pair.
inline const char *validate(const uint8_t *data, size_t size, uint32_t capacity) {
    if (!data || size < HEADER_BYTES) return "truncated MHOR header";
    if (read_u32(data) != MAGIC) return "invalid MHOR magic";
    if (read_u32(data + 4) != VERSION) return "MHOR v2 required (legacy orders cannot replay faithfully)";
    if (read_u32(data + 8) != RECORD_BYTES) return "invalid MHOR native record size";
    size_t   at        = HEADER_BYTES;
    uint32_t prior     = 0;
    bool     want_hash = true;
    while (at < size) {
        if (size - at < SAMPLE_HEADER_BYTES) return "truncated MHOR sample header";
        const uint32_t step  = read_u32(data + at);
        const uint32_t kind  = read_u32(data + at + 4);
        const uint32_t count = read_u32(data + at + 8);
        if (!step) return "invalid MHOR step zero";
        if (kind != static_cast<uint32_t>(want_hash ? phase::hash : phase::entry))
            return "unordered MHOR HASH/ENTRY phases";
        if (want_hash) {
            if (prior && (prior == UINT32_MAX || step != prior + 1)) return "nonconsecutive MHOR steps";
            prior = step;
        } else if (step != prior) {
            return "MHOR ENTRY step differs from HASH";
        }
        at += SAMPLE_HEADER_BYTES;
        if (count > capacity) return "MHOR queue count exceeds runtime capacity";
        // Divide BEFORE multiplying so hostile counts cannot wrap on a 32-bit host.
        if (count > (size - at) / RECORD_BYTES) return "truncated MHOR native records";
        at += static_cast<size_t>(count) * RECORD_BYTES;
        want_hash = !want_hash;
    }
    return want_hash ? nullptr : "missing final MHOR ENTRY sample";
}

// A borrowed immutable byte stream. reset() refuses malformed input before making it available.
// next() advances only on an exact requested sample; EOF is NOT an empty queue.
class reader {
public:
    bool reset(const uint8_t *data, size_t size, uint32_t capacity) {
        data_ = nullptr;
        size_ = at_ = 0;
        error_      = validate(data, size, capacity);
        if (error_) return false;
        data_ = data;
        size_ = size;
        at_   = HEADER_BYTES;
        return true;
    }
    bool next(uint32_t step, phase kind, view &out) {
        out = {};
        if (!data_ || error_) return false;
        if (at_ == size_) {
            error_ = "missing requested MHOR sample";
            return false;
        }
        if (read_u32(data_ + at_) != step || read_u32(data_ + at_ + 4) != static_cast<uint32_t>(kind)) {
            error_ = "requested MHOR step/phase is absent";
            return false;
        }
        out.step    = step;
        out.count   = read_u32(data_ + at_ + 8);
        out.records = data_ + at_ + SAMPLE_HEADER_BYTES;
        at_ += SAMPLE_HEADER_BYTES + static_cast<size_t>(out.count) * RECORD_BYTES;
        return true;
    }
    const char *error() const { return error_; }
    bool        exhausted() const { return data_ && !error_ && at_ == size_; }

private:
    const uint8_t *data_  = nullptr;
    size_t         size_  = 0;
    size_t         at_    = 0;
    const char    *error_ = nullptr;
};
} // namespace mh::order_stream
