// mh_mapgen -- .MP / .mpm codec. See mpm.h for the layout.
#include "mh_mapgen/mpm.h"
#include "byteio.h"
#include <cstring>

namespace mh_mapgen {

namespace {
constexpr std::uint32_t MAX_DIM = 4096; // far above the shipped 256; keeps the size arithmetic far from overflow

bool dims_ok(std::uint32_t w, std::uint32_t h) { return w && h && w <= MAX_DIM && h <= MAX_DIM && (w % 4) == 0 && (h % 4) == 0; }
} // namespace

std::string MpmModel::tileset() const {
    std::size_t n = 0;
    while (n < NAME_LEN && header_raw[NAME_OFF + n]) ++n;
    return std::string(reinterpret_cast<const char *>(header_raw.data() + NAME_OFF), n);
}

std::size_t mpm_ore_words(std::uint32_t w, std::uint32_t h) { return std::size_t(w / 4) * (h / 4) * ORE_WORDS; }
std::size_t mpm_halfres_bytes(std::uint32_t w, std::uint32_t h) { return std::size_t(w / 2) * (h / 2) * 4; }

namespace {
// The model is optional: framing and bounds are identical in decode and validate-only modes.
bool parse_mpm(const std::uint8_t *d, std::size_t size, MpmModel *m, std::string &err) {
    if (m) *m = MpmModel{};
    if (!d || size < MPM_HEADER_LEN) {
        err = "file shorter than the 0x61-byte header";
        return false;
    }
    if (std::memcmp(d, "ver02", 5) != 0) {
        err = "bad magic (expected ver02)";
        return false;
    }
    const std::uint32_t width = rd32(d + 0x05), height = rd32(d + 0x09);
    const std::uint32_t f1 = rd32(d + 0x35), f2 = rd32(d + 0x39);
    const std::int32_t  f3 = rdi32(d + 0x41);
    if (m) {
        std::memcpy(m->header_raw.data(), d, MPM_HEADER_LEN);
        m->width  = width;
        m->height = height;
    }
    if (!dims_ok(width, height)) {
        err = "unsupported dimensions " + std::to_string(width) + "x" + std::to_string(height);
        return false;
    }
    if (f1 > (1u << 24) || f2 > (1u << 24) || f3 < 0 || f3 > (1 << 24)) {
        err = "implausible record counts";
        return false;
    }
    const std::size_t n    = std::size_t(width) * height;
    std::size_t       off  = MPM_HEADER_LEN;
    auto              need = [&](std::size_t bytes) { return bytes <= size - off; };
    if (!need(n * 4 + n)) {
        err = "truncated tile/passability planes";
        return false;
    }
    if (m) {
        m->flags.resize(n);
        m->building.resize(n);
        for (std::size_t i = 0; i < n; ++i) {
            m->flags[i]    = rd16(d + off + i * 4);
            m->building[i] = rd16(d + off + i * 4 + 2);
        }
        m->passable.assign(d + off + n * 4, d + off + n * 5);
    }
    off += n * 5;
    const std::size_t f1b = std::size_t(f1) * 28, hrb = mpm_halfres_bytes(width, height);
    if (!need(f1b + hrb)) {
        err = "truncated f1/half-res planes";
        return false;
    }
    if (m) {
        m->f1_raw.assign(d + off, d + off + f1b);
        m->halfres_raw.assign(d + off + f1b, d + off + f1b + hrb);
    }
    off += f1b + hrb;
    const std::size_t trees_bytes = std::size_t(f2) * TREE_BYTES;
    if (!need(trees_bytes)) {
        err = "truncated tree records";
        return false;
    }
    if (m) {
        m->trees.resize(f2);
        for (std::size_t i = 0; i < f2; ++i) {
            const std::uint8_t *p = d + off + i * TREE_BYTES;
            MpmTree            &t = m->trees[i];
            t.slot                = rd32(p);
            t.kind                = rdi32(p + 4);
            t.px_x                = rdi32(p + 8);
            t.px_y                = rdi32(p + 12);
            t.next                = rdi32(p + 16);
            t.x                   = rdi32(p + 20);
            t.y                   = rdi32(p + 24);
        }
    }
    off += trees_bytes;
    const std::size_t ow = mpm_ore_words(width, height);
    if (!need(ow * 2)) {
        err = "truncated ore plane";
        return false;
    }
    if (m) {
        m->ore.resize(ow);
        for (std::size_t i = 0; i < ow; ++i) m->ore[i] = rd16(d + off + i * 2);
    }
    off += ow * 2;
    const std::size_t landing_bytes = std::size_t(f3) * LANDING_BYTES;
    if (!need(landing_bytes)) {
        err = "truncated landing spots";
        return false;
    }
    if (m) {
        m->landings.resize(std::size_t(f3));
        for (std::size_t i = 0; i < m->landings.size(); ++i) {
            const std::size_t at  = off + i * LANDING_BYTES;
            m->landings[i].x      = rdi32(d + at);
            m->landings[i].y      = rdi32(d + at + 4);
            m->landings[i].status = rdi32(d + at + 8);
        }
    }
    off += landing_bytes;
    if (off != size) {
        const std::size_t rest = size - off;
        if (rest < 4) {
            err = "trailing bytes too short for an .mpm id (" + std::to_string(rest) + ")";
            return false;
        }
        if (m) {
            m->has_tail = true;
            m->tail_id  = rd32(d + off);
            m->tail_desc.assign(d + off + 4, d + size);
        }
    }
    return true;
}
} // namespace

bool read_mpm(const std::uint8_t *d, std::size_t size, MpmModel &m, std::string &err) {
    return parse_mpm(d, size, &m, err);
}

bool validate_mpm(const std::uint8_t *d, std::size_t size, std::string &err) {
    return parse_mpm(d, size, nullptr, err);
}
bool mpm_validate(const MpmModel &m, std::string &err) {
    if (!dims_ok(m.width, m.height)) {
        err = "unsupported dimensions";
        return false;
    }
    const std::size_t n = std::size_t(m.width) * m.height;
    if (m.flags.size() != n || m.building.size() != n || m.passable.size() != n) {
        err = "tile/building/passability plane size mismatch";
        return false;
    }
    if (m.f1_raw.size() % 28 != 0) {
        err = "f1 records not a multiple of 28 bytes";
        return false;
    }
    if (m.halfres_raw.size() != mpm_halfres_bytes(m.width, m.height)) {
        err = "half-res plane size mismatch";
        return false;
    }
    if (m.ore.size() != mpm_ore_words(m.width, m.height)) {
        err = "ore plane size mismatch";
        return false;
    }
    return true;
}

std::vector<std::uint8_t> write_mpm_body(const MpmModel &m) {
    std::string err;
    if (!mpm_validate(m, err)) return {};
    const std::size_t         n = std::size_t(m.width) * m.height;
    std::vector<std::uint8_t> o;
    o.reserve(MPM_HEADER_LEN + n * 5 + m.f1_raw.size() + m.halfres_raw.size() + m.trees.size() * TREE_BYTES + m.ore.size() * 2 + m.landings.size() * LANDING_BYTES + 4 + m.tail_desc.size());
    o.assign(m.header_raw.begin(), m.header_raw.end());
    wr32(o.data() + 0x05, m.width);
    wr32(o.data() + 0x09, m.height);
    wr32(o.data() + 0x35, std::uint32_t(m.f1_raw.size() / 28));
    wr32(o.data() + 0x39, std::uint32_t(m.trees.size()));
    wr32(o.data() + 0x41, std::uint32_t(m.landings.size()));
    for (std::size_t i = 0; i < n; ++i) {
        put16(o, m.flags[i]);
        put16(o, m.building[i]);
    }
    o.insert(o.end(), m.passable.begin(), m.passable.end());
    o.insert(o.end(), m.f1_raw.begin(), m.f1_raw.end());
    o.insert(o.end(), m.halfres_raw.begin(), m.halfres_raw.end());
    for (const MpmTree &t : m.trees) {
        put32(o, t.slot);
        put32(o, std::uint32_t(t.kind));
        put32(o, std::uint32_t(t.px_x));
        put32(o, std::uint32_t(t.px_y));
        put32(o, std::uint32_t(t.next));
        put32(o, std::uint32_t(t.x));
        put32(o, std::uint32_t(t.y));
    }
    for (std::uint16_t w : m.ore) put16(o, w);
    for (const MpmLanding &l : m.landings) {
        put32(o, std::uint32_t(l.x));
        put32(o, std::uint32_t(l.y));
        put32(o, std::uint32_t(l.status));
    }
    return o;
}

std::vector<std::uint8_t> write_mpm(const MpmModel &m) {
    std::vector<std::uint8_t> o = write_mpm_body(m);
    if (!o.empty() && m.has_tail) {
        put32(o, m.tail_id);
        o.insert(o.end(), m.tail_desc.begin(), m.tail_desc.end());
    }
    return o;
}

std::uint32_t crc32_ieee(const std::uint8_t *data, std::size_t size) {
    std::uint32_t c = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < size; ++i) {
        c ^= data[i];
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

std::uint32_t mpm_compute_id(const MpmModel &m) {
    std::vector<std::uint8_t> body = write_mpm_body(m);
    return crc32_ieee(body.data(), body.size());
}

void mpm_set_tail(MpmModel &m, const std::string &description) {
    m.has_tail = true;
    m.tail_desc.assign(description.begin(), description.end());
    m.tail_id = mpm_compute_id(m);
}

void mpm_init(MpmModel &m, const std::string &tileset, std::uint32_t w, std::uint32_t h) {
    m        = MpmModel{};
    m.width  = w;
    m.height = h;
    std::memcpy(m.header_raw.data(), "ver02", 5);
    wr32(m.header_raw.data() + 0x05, w);
    wr32(m.header_raw.data() + 0x09, h);
    for (std::size_t i = 0; i < tileset.size() && i < NAME_LEN; ++i) m.header_raw[NAME_OFF + i] = std::uint8_t(tileset[i]);
    const std::uint32_t n = w * h;
    wr32(m.header_raw.data() + 0x2d, 2 * n);
    wr32(m.header_raw.data() + 0x31, n);
    wr32(m.header_raw.data() + 0x3d, (w / 4) * (h / 4));
    m.flags.assign(n, TILE_FLAG_BASE);
    m.building.assign(n, 0);
    m.passable.assign(n, 0);
    m.halfres_raw.assign(mpm_halfres_bytes(w, h), 0);
    m.ore.assign(mpm_ore_words(w, h), 0);
}

} // namespace mh_mapgen
