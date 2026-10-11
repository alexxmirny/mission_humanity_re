#include "mh_net_proto/gameplay_set.h"
#include "byteio.h"
#include <algorithm>

namespace mh_net_proto {
using namespace detail;

const char *gameplay_err_text(GameplayErr e) noexcept {
    switch (e) {
        case GameplayErr::Ok: return "ok";
        case GameplayErr::Truncated: return "truncated";
        case GameplayErr::BadMagic: return "bad magic";
        case GameplayErr::BadFormat: return "bad format";
        case GameplayErr::UnknownType: return "unknown type";
        case GameplayErr::TooMany: return "too many entries";
    }
    return "?";
}

std::uint32_t gameplay_key_id(const char *sec, const char *key) noexcept {
    std::uint32_t h    = 2166136261u;
    auto          fold = [](char c) { return (std::uint8_t)((c >= 'A' && c <= 'Z') ? c + 32 : c); };
    for (const char *p = sec; *p; ++p) h = (h ^ fold(*p)) * 16777619u;
    h = (h ^ 0x2eu) * 16777619u;
    for (const char *p = key; *p; ++p) h = (h ^ fold(*p)) * 16777619u;
    return h == 0 ? 1u : h;
}

std::size_t gameplay_set_encode(const GameplaySet &s, std::uint8_t *out) noexcept {
    if (s.n < 0 || s.n > GAMEPLAY_MAX_ENTRIES) return 0;
    for (int i = 0; i < s.n; ++i)
        if (s.e[i].type != GAMEPLAY_T_INT && s.e[i].type != GAMEPLAY_T_F64) return 0;
    std::uint8_t *p = out;
    *p++            = 'G';
    *p++            = 'P';
    *p++            = GAMEPLAY_FORMAT;
    *p++            = (std::uint8_t)s.n;
    for (int i = 0; i < s.n; ++i) {
        const GameplayEntry &e = s.e[i];
        put_u32(p, e.key_id);
        p += 4;
        *p++ = e.type;
        if (e.type == GAMEPLAY_T_INT) {
            put_u32(p, (std::uint32_t)e.i);
            p += 4;
        } else {
            put_u32(p, (std::uint32_t)(e.f_bits & 0xffffffffu));
            put_u32(p + 4, (std::uint32_t)(e.f_bits >> 32));
            p += 8;
        }
    }
    return std::size_t(p - out);
}

GameplayErr gameplay_set_decode(const std::uint8_t *in, std::size_t len, GameplaySet &out) noexcept {
    out = GameplaySet{};
    if (len < GAMEPLAY_HDR_BYTES) return GameplayErr::Truncated;
    if (in[0] != 'G' || in[1] != 'P') return GameplayErr::BadMagic;
    if (in[2] != GAMEPLAY_FORMAT) return GameplayErr::BadFormat;
    const unsigned n = in[3];
    if (n > (unsigned)GAMEPLAY_MAX_ENTRIES) return GameplayErr::TooMany;
    const std::uint8_t *p   = in + GAMEPLAY_HDR_BYTES;
    const std::uint8_t *end = in + len;
    GameplaySet         tmp;
    for (unsigned i = 0; i < n; ++i) {
        if (end - p < 5) return GameplayErr::Truncated;
        GameplayEntry &e = tmp.e[i];
        e.key_id         = get_u32(p);
        e.type           = p[4];
        p += 5;
        if (e.type == GAMEPLAY_T_INT) {
            if (end - p < 4) return GameplayErr::Truncated;
            e.i = (std::int32_t)get_u32(p);
            p += 4;
        } else if (e.type == GAMEPLAY_T_F64) {
            if (end - p < 8) return GameplayErr::Truncated;
            e.f_bits = (std::uint64_t)get_u32(p) | ((std::uint64_t)get_u32(p + 4) << 32);
            p += 8;
        } else {
            return GameplayErr::UnknownType;
        }
    }
    tmp.n = (int)n;
    out   = tmp;
    return GameplayErr::Ok;
}

std::uint32_t gameplay_set_digest(const GameplaySet &s) noexcept {
    struct KT {
        std::uint32_t k;
        std::uint8_t  t;
    };
    KT  v[GAMEPLAY_MAX_ENTRIES];
    int n = s.n < 0 ? 0 : (s.n > GAMEPLAY_MAX_ENTRIES ? GAMEPLAY_MAX_ENTRIES : s.n);
    for (int i = 0; i < n; ++i) v[i] = {s.e[i].key_id, s.e[i].type};
    std::sort(v, v + n, [](const KT &a, const KT &b) { return a.k != b.k ? a.k < b.k : a.t < b.t; });
    std::uint32_t h   = 2166136261u;
    auto          mix = [&h](std::uint8_t b) { h = (h ^ b) * 16777619u; };
    for (int i = 0; i < n; ++i) {
        for (int b = 0; b < 4; ++b) mix((std::uint8_t)(v[i].k >> (8 * b)));
        mix(v[i].t);
    }
    return h == 0 ? 1u : h;
}

} // namespace mh_net_proto
