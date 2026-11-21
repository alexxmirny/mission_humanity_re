// mh_mapgen -- rule lookups, text format, SHA-256, and the stock-map list.
#include "mh_mapgen/rules.h"
#include <algorithm>
#include <charconv>
#include <utility>

namespace mh_mapgen {

const TileRule *Rules::find_tile(std::uint16_t id) const {
    auto it = std::lower_bound(tiles.begin(), tiles.end(), id, [](const TileRule &t, std::uint16_t v) { return t.id < v; });
    return (it != tiles.end() && it->id == id) ? &*it : nullptr;
}

std::uint32_t Rules::pair_count(char dir, std::uint16_t a, std::uint16_t b) const {
    const std::vector<PairRule> &p  = (dir == 'h') ? h : v;
    auto                         it = std::lower_bound(p.begin(), p.end(), std::make_pair(a, b), [](const PairRule &r, const std::pair<std::uint16_t, std::uint16_t> &k) { return r.a != k.first ? r.a < k.first : r.b < k.second; });
    return (it != p.end() && it->a == a && it->b == b) ? it->n : 0;
}

std::int64_t Rules::stat_or(std::uint32_t size, const std::string &key, std::int64_t dflt) const {
    std::int64_t val = 0;
    return find_stat(size, key, val) ? val : dflt;
}

bool Rules::find_stat(std::uint32_t size, const std::string &key, std::int64_t &out) const {
    for (const StatRule &s : stats)
        if (s.size == size && s.key == key) {
            out = s.value;
            return true;
        }
    return false;
}

// ---- SHA-256 (FIPS 180-4) -------------------------------------------------------------------------
namespace {

constexpr std::uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

void sha256_block(std::uint32_t st[8], const std::uint8_t *p) {
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i) w[i] = (std::uint32_t(p[i * 4]) << 24) | (std::uint32_t(p[i * 4 + 1]) << 16) | (std::uint32_t(p[i * 4 + 2]) << 8) | p[i * 4 + 3];
    for (int i = 16; i < 64; ++i) {
        std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i]             = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = st[0], b = st[1], c = st[2], d = st[3], e = st[4], f = st[5], g = st[6], h = st[7];
    for (int i = 0; i < 64; ++i) {
        std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        std::uint32_t ch = (e & f) ^ (~e & g);
        std::uint32_t t1 = h + S1 + ch + K256[i] + w[i];
        std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        std::uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        std::uint32_t t2 = S0 + mj;
        h                = g;
        g                = f;
        f                = e;
        e                = d + t1;
        d                = c;
        c                = b;
        b                = a;
        a                = t1 + t2;
    }
    st[0] += a;
    st[1] += b;
    st[2] += c;
    st[3] += d;
    st[4] += e;
    st[5] += f;
    st[6] += g;
    st[7] += h;
}

} // namespace

std::string sha256_hex(const std::uint8_t *data, std::size_t size) {
    std::uint32_t st[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::size_t   full  = size / 64;
    for (std::size_t i = 0; i < full; ++i) sha256_block(st, data + i * 64);
    std::uint8_t tail[128] = {0};
    std::size_t  rem       = size - full * 64;
    for (std::size_t i = 0; i < rem; ++i) tail[i] = data[full * 64 + i];
    tail[rem]          = 0x80;
    std::size_t   tl   = (rem < 56) ? 64 : 128;
    std::uint64_t bits = std::uint64_t(size) * 8;
    for (int i = 0; i < 8; ++i) tail[tl - 1 - i] = std::uint8_t(bits >> (8 * i));
    sha256_block(st, tail);
    if (tl == 128) sha256_block(st, tail + 64);
    static const char HEX[] = "0123456789abcdef";
    std::string       out;
    for (int i = 0; i < 8; ++i)
        for (int s = 28; s >= 0; s -= 4) out += HEX[(st[i] >> s) & 0xf];
    return out;
}

// ---- rules text ------------------------------------------------------------------------------------

std::string rules_file_name(const std::string &tileset) {
    std::string stem = tileset;
    std::size_t dot  = stem.find_last_of('.');
    if (dot != std::string::npos && dot > 0) stem.resize(dot);
    for (char &c : stem)
        if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    return stem + ".mgr";
}

namespace {

std::string tag_text(std::uint8_t tag) {
    const int kind = tag >> 4, bits = tag & 15;
    switch (kind) {
        case TAG_PLAIN: return "x" + std::to_string(bits);
        case TAG_MIXED: return "m";
        case TAG_CLIFF: return "cliff";
        case TAG_RAMP: return "ramp";
        case TAG_CLEAR: return "clear";
        default: return "o" + std::to_string(bits);
    }
}

bool tag_parse(std::string_view s, std::uint8_t &tag) {
    if (s == "m") tag = make_tag(TAG_MIXED);
    else if (s == "cliff") tag = make_tag(TAG_CLIFF);
    else if (s == "ramp") tag = make_tag(TAG_RAMP);
    else if (s == "clear") tag = make_tag(TAG_CLEAR);
    else if (s.size() >= 2 && s.size() <= 3 && (s[0] == 'x' || s[0] == 'o')) {
        int v = 0;
        for (std::size_t i = 1; i < s.size(); ++i) {
            if (s[i] < '0' || s[i] > '9') return false;
            v = v * 10 + (s[i] - '0');
        }
        if (v > 15) return false;
        tag = make_tag(s[0] == 'x' ? TAG_PLAIN : TAG_ORIENT, std::uint8_t(v));
    } else return false;
    return true;
}

} // namespace

std::string rules_to_text(const Rules &r) {
    std::string o = "mapgen-rules 3\n";
    o += "tileset " + r.tileset + "\n";
    if (!r.gameplay_profile.empty() || !r.tree_profile.empty()) o += "profile " + r.gameplay_profile + " " + r.tree_profile + "\n";
    o += "maps " + std::to_string(r.maps) + "\n";
    o += "inputs_sha256 " + r.inputs_sha256 + "\n";
    for (const TileRule &t : r.tiles) o += "t " + std::to_string(t.id) + " " + std::to_string(t.freq) + " " + std::to_string(t.pass) + " " + std::to_string(t.mixed) + " " + std::to_string(t.treeok) + " " + std::to_string(t.seen) + "\n";
    for (const PairRule &p : r.h) o += "h " + std::to_string(p.a) + " " + std::to_string(p.b) + " " + std::to_string(p.n) + "\n";
    for (const PairRule &p : r.v) o += "v " + std::to_string(p.a) + " " + std::to_string(p.b) + " " + std::to_string(p.n) + "\n";
    for (const KindRule &k : r.kinds) o += "kind " + std::to_string(k.kind) + " " + std::to_string(k.n) + "\n";
    for (const StatRule &s : r.stats) o += "stat " + std::to_string(s.size) + " " + s.key + " " + std::to_string(s.value) + "\n";
    for (const ClassRule &c : r.classes) o += "class " + std::to_string(c.id) + " " + std::to_string(c.parent) + " " + std::to_string(c.order) + " " + std::to_string(c.share_ppm) + " " + std::to_string(c.pure_uses) + "\n";
    for (const AdjRule &a : r.adj) o += "adj " + std::to_string(a.a) + " " + std::to_string(a.b) + "\n";
    for (const CornerRule &c : r.corners) o += "c " + std::to_string(c.tile) + " " + std::to_string(c.cls[0]) + " " + std::to_string(c.cls[1]) + " " + std::to_string(c.cls[2]) + " " + std::to_string(c.cls[3]) + "\n";
    for (const CaseRule &c : r.cases) o += "case " + std::to_string(c.cls[0]) + " " + std::to_string(c.cls[1]) + " " + std::to_string(c.cls[2]) + " " + std::to_string(c.cls[3]) + " " + tag_text(c.tag) + " " + std::to_string(c.t[0]) + " " + std::to_string(c.t[1]) + " " + std::to_string(c.t[2]) + " " + std::to_string(c.t[3]) + " " + std::to_string(c.n) + "\n";
    for (const StampRule &s : r.stamps) {
        o += "stamp " + std::to_string(s.n) + " " + std::to_string(s.cells.size());
        for (const StampCell &c : s.cells) o += " " + std::to_string(c.y) + " " + std::to_string(c.x) + " " + std::to_string(c.tile);
        o += "\n";
    }
    for (const DecalRule &d : r.decals) {
        o += "decal " + std::to_string(d.cls) + " " + std::to_string(d.n) + " " + std::to_string(d.cells.size());
        for (const StampCell &c : d.cells) o += " " + std::to_string(c.y) + " " + std::to_string(c.x) + " " + std::to_string(c.tile);
        o += "\n";
    }
    for (const TreeSample &t : r.tree_samples) o += "tree " + std::to_string(t.tile) + " " + std::to_string(t.kind) + " " + std::to_string(t.px) + " " + std::to_string(t.py) + " " + std::to_string(t.n) + "\n";
    for (const OreRec &c : r.ore_recs) o += "ore " + std::to_string(c.ch[0]) + " " + std::to_string(c.ch[1]) + " " + std::to_string(c.ch[2]) + " " + std::to_string(c.ch[3]) + " " + std::to_string(c.n) + "\n";
    return o;
}

namespace {

std::vector<std::string_view> split_ws(std::string_view line) {
    std::vector<std::string_view> out;
    std::size_t                   i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) ++i;
        std::size_t s = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r') ++i;
        if (i > s) out.push_back(line.substr(s, i - s));
    }
    return out;
}

template <class T>
bool num(std::string_view s, T &out) {
    auto res = std::from_chars(s.data(), s.data() + s.size(), out);
    return res.ec == std::errc() && res.ptr == s.data() + s.size();
}

} // namespace

bool rules_from_text(std::string_view text, Rules &out, std::string &err) {
    out = Rules{};
    if (text.size() > 32u * 1024u * 1024u) {
        err = "rules text exceeds 32 MiB";
        return false;
    }
    std::size_t pos = 0, lineno = 0;
    bool        magic = false, have_profile = false;
    while (pos < text.size()) {
        std::size_t nl = text.find('\n', pos);
        if (nl == std::string_view::npos) nl = text.size();
        std::string_view line = text.substr(pos, nl - pos);
        pos                   = nl + 1;
        ++lineno;
        if (line.size() > 262144) {
            err = "rules line exceeds limit";
            return false;
        }
        auto f = split_ws(line);
        if (f.empty()) continue;
        auto bad = [&](const char *what) {
            err = "line " + std::to_string(lineno) + ": " + what;
            return false;
        };
        if (!magic) {
            if (f.size() != 2 || f[0] != "mapgen-rules" || f[1] != "3") return bad("expected header \"mapgen-rules 3\" (re-run `mapgen learn`)");
            magic = true;
            continue;
        }
        const std::string_view k = f[0];
        // Bound every growing collection before allocation. The input cap also bounds tokenization.
        if ((k == "t" && out.tiles.size() >= 1024) || (k == "h" && out.h.size() >= 1048576) || (k == "v" && out.v.size() >= 1048576) ||
            (k == "class" && out.classes.size() >= 16) || (k == "adj" && out.adj.size() >= 256) || (k == "c" && out.corners.size() >= 1024) ||
            (k == "case" && out.cases.size() >= 262144) || (k == "stamp" && out.stamps.size() >= 65536) || (k == "decal" && out.decals.size() >= 65536) ||
            (k == "kind" && out.kinds.size() >= 255) || (k == "tree" && out.tree_samples.size() >= 262144) || (k == "ore" && out.ore_recs.size() >= 65536) ||
            (k == "stat" && out.stats.size() >= 65536)) return bad("record limit exceeded");
        if (k == "tileset" && f.size() == 2) out.tileset = std::string(f[1]);
        else if (k == "profile" && f.size() == 3) {
            const auto known = [](std::string_view s) { return s == "diuna256" || s == "jungle" || s == "ksie256" || s == "lod256" || s == "metal256" || s == "ruins256" || s == "skaly256"; };
            if (have_profile || !known(f[1]) || !(known(f[2]) || f[2] == "none")) return bad("invalid or duplicate profile");
            have_profile         = true;
            out.gameplay_profile = std::string(f[1]);
            out.tree_profile     = std::string(f[2]);
        } else if (k == "maps" && f.size() == 2) {
            if (!num(f[1], out.maps)) return bad("bad maps");
        } else if (k == "inputs_sha256" && f.size() == 2) out.inputs_sha256 = std::string(f[1]);
        else if (k == "t" && f.size() == 7) {
            TileRule      t;
            std::uint32_t id, pa, mx, tk, sn;
            if (!num(f[1], id) || id > 0xffff || !num(f[2], t.freq) || !num(f[3], pa) || !num(f[4], mx) || !num(f[5], tk) || !num(f[6], sn) || sn > 3 || pa > 1 || mx > 1 || tk > 1) return bad("bad t line");
            t.id     = std::uint16_t(id);
            t.pass   = std::uint8_t(pa);
            t.mixed  = std::uint8_t(mx);
            t.treeok = std::uint8_t(tk);
            t.seen   = std::uint8_t(sn);
            out.tiles.push_back(t);
        } else if ((k == "h" || k == "v") && f.size() == 4) {
            PairRule      p;
            std::uint32_t a, b;
            if (!num(f[1], a) || !num(f[2], b) || a > 0xffff || b > 0xffff || !num(f[3], p.n)) return bad("bad pair line");
            p.a = std::uint16_t(a);
            p.b = std::uint16_t(b);
            (k == "h" ? out.h : out.v).push_back(p);
        } else if (k == "kind" && f.size() == 3) {
            KindRule kr;
            if (!num(f[1], kr.kind) || !num(f[2], kr.n)) return bad("bad kind line");
            out.kinds.push_back(kr);
        } else if (k == "stat" && f.size() == 4) {
            StatRule s;
            if (!num(f[1], s.size) || !num(f[3], s.value)) return bad("bad stat line");
            s.key = std::string(f[2]);
            out.stats.push_back(s);
        } else if (k == "class" && f.size() == 6) {
            ClassRule c;
            if (!num(f[1], c.id) || !num(f[2], c.parent) || !num(f[3], c.order) || !num(f[4], c.share_ppm) || !num(f[5], c.pure_uses)) return bad("bad class line");
            out.classes.push_back(c);
        } else if (k == "adj" && f.size() == 3) {
            AdjRule a;
            if (!num(f[1], a.a) || !num(f[2], a.b)) return bad("bad adj line");
            out.adj.push_back(a);
        } else if (k == "c" && f.size() == 6) {
            CornerRule    c;
            std::uint32_t t;
            std::int32_t  v[4];
            if (!num(f[1], t) || t > 0xffff || !num(f[2], v[0]) || !num(f[3], v[1]) || !num(f[4], v[2]) || !num(f[5], v[3])) return bad("bad c line");
            c.tile = std::uint16_t(t);
            for (int i = 0; i < 4; ++i) {
                if (v[i] < -1 || v[i] > 15) return bad("bad corner class");
                c.cls[i] = std::int8_t(v[i]);
            }
            out.corners.push_back(c);
        } else if (k == "case" && f.size() == 11) {
            CaseRule      c;
            std::uint32_t v[4], t[4];
            if (!num(f[1], v[0]) || !num(f[2], v[1]) || !num(f[3], v[2]) || !num(f[4], v[3]) || !tag_parse(f[5], c.tag) || !num(f[6], t[0]) || !num(f[7], t[1]) || !num(f[8], t[2]) || !num(f[9], t[3]) || !num(f[10], c.n)) return bad("bad case line");
            for (int i = 0; i < 4; ++i) {
                if (v[i] > 15 || t[i] > 0xffff) return bad("bad case line");
                c.cls[i] = std::uint8_t(v[i]);
                c.t[i]   = std::uint16_t(t[i]);
            }
            out.cases.push_back(c);
        } else if (k == "stamp" && f.size() >= 3) {
            StampRule     s;
            std::uint32_t nc;
            if (!num(f[1], s.n) || !num(f[2], nc) || nc > 4096 || f.size() != 3 + std::size_t(nc) * 3) return bad("bad stamp line");
            for (std::uint32_t i = 0; i < nc; ++i) {
                StampCell     c;
                std::uint32_t t;
                if (!num(f[3 + i * 3], c.y) || !num(f[4 + i * 3], c.x) || !num(f[5 + i * 3], t) || t > 0xffff) return bad("bad stamp line");
                c.tile = std::uint16_t(t);
                s.cells.push_back(c);
            }
            out.stamps.push_back(s);
        } else if (k == "decal" && f.size() >= 4) {
            DecalRule     d;
            std::uint32_t nc;
            if (!num(f[1], d.cls) || d.cls < 0 || d.cls > 15 || !num(f[2], d.n) || !num(f[3], nc) || nc > 12 || f.size() != 4 + std::size_t(nc) * 3) return bad("bad decal line");
            for (std::uint32_t i = 0; i < nc; ++i) {
                StampCell     c;
                std::uint32_t t;
                if (!num(f[4 + i * 3], c.y) || !num(f[5 + i * 3], c.x) || !num(f[6 + i * 3], t) || t > 0xffff || c.y < 0 || c.x < 0) return bad("bad decal line");
                c.tile = std::uint16_t(t);
                d.cells.push_back(c);
            }
            out.decals.push_back(d);
        } else if (k == "tree" && f.size() == 6) {
            TreeSample    t;
            std::uint32_t id;
            if (!num(f[1], id) || id > 0xffff || !num(f[2], t.kind) || !num(f[3], t.px) || !num(f[4], t.py) || !num(f[5], t.n)) return bad("bad tree line");
            t.tile = std::uint16_t(id);
            out.tree_samples.push_back(t);
        } else if (k == "ore" && f.size() == 6) {
            OreRec        o;
            std::uint32_t v[4];
            if (!num(f[1], v[0]) || !num(f[2], v[1]) || !num(f[3], v[2]) || !num(f[4], v[3]) || !num(f[5], o.n)) return bad("bad ore line");
            for (int i = 0; i < 4; ++i) {
                if (v[i] > 0xffff) return bad("bad ore line");
                o.ch[i] = std::uint16_t(v[i]);
            }
            out.ore_recs.push_back(o);
        } else return bad("unrecognised line");
    }
    if (!magic) {
        err = "empty rules file";
        return false;
    }
    if (out.tileset.empty()) {
        err = "missing tileset line";
        return false;
    }
    return true;
}


// Validate before dense model construction or publication. Limits cover parsed and in-memory rules.
bool validate_rules(const Rules &r, std::string &err) {
    auto fail    = [&](const char *s) { err = s; return false; };
    auto profile = [](const std::string &s) { return s == "diuna256" || s == "jungle" || s == "ksie256" || s == "lod256" || s == "metal256" || s == "ruins256" || s == "skaly256"; };
    if ((!r.gameplay_profile.empty() || !r.tree_profile.empty()) && (!profile(r.gameplay_profile) || !(profile(r.tree_profile) || r.tree_profile == "none"))) return fail("invalid profiles");
    if (r.tiles.empty() || r.tiles.size() > 1024) return fail("active tile count must be 1..1024");
    if (r.h.size() > 1048576 || r.v.size() > 1048576 || r.classes.size() > 16 || r.corners.size() > 1024 || r.cases.size() > 262144 || r.stamps.size() > 65536 || r.decals.size() > 65536 || r.tree_samples.size() > 262144 || r.ore_recs.size() > 65536 || r.kinds.size() > 255 || r.stats.size() > 65536) return fail("rule record limit exceeded");
    for (std::size_t i = 0; i < r.tiles.size(); ++i) {
        const auto &t = r.tiles[i];
        if (t.id >= 4096 || !t.freq || t.freq > 1000000000 || t.pass > 1 || t.mixed > 1 || t.treeok > 1 || t.seen > 3 || (i && r.tiles[i - 1].id >= t.id)) return fail("invalid, unsorted or duplicate tile");
    }
    const auto pairs = [&](const std::vector<PairRule> &v) {
        for (std::size_t i = 0; i < v.size(); ++i) {
            const auto &p = v[i];
            if (!p.n || p.n > 1000000000 || !r.find_tile(p.a) || !r.find_tile(p.b) || (i && (v[i - 1].a > p.a || (v[i - 1].a == p.a && v[i - 1].b >= p.b)))) return false;
        }
        return true;
    };
    if (!pairs(r.h) || !pairs(r.v)) return fail("invalid pair reference, frequency or ordering");
    for (std::size_t i = 0; i < r.kinds.size(); ++i)
        if (r.kinds[i].kind < 1 || r.kinds[i].kind > 255 || !r.kinds[i].n || r.kinds[i].n > 1000000000 || (i && r.kinds[i - 1].kind >= r.kinds[i].kind)) return fail("invalid tree kind");
    for (std::size_t i = 0; i < r.stats.size(); ++i) {
        const auto &s = r.stats[i];
        if (s.key.empty() || s.key.size() > 128 || s.value < 0 || s.value > 1000000000000LL || (i && (r.stats[i - 1].size > s.size || (r.stats[i - 1].size == s.size && r.stats[i - 1].key >= s.key)))) return fail("invalid or duplicate statistic");
    }
    if (r.classes.empty() != r.cases.empty()) return fail("incomplete macro model");
    if (!r.gameplay_profile.empty() && r.classes.empty()) return fail("profile requires explicit macro model");
    for (std::size_t i = 0; i < r.classes.size(); ++i) {
        const auto &c = r.classes[i];
        if (c.id != int(i) || c.parent < -1 || c.parent >= int(r.classes.size()) || c.parent == c.id || c.order < -1 || c.order >= 16 || c.share_ppm > 1000000) return fail("invalid class");
        int p = c.parent;
        for (std::size_t depth = 0; p >= 0; ++depth) {
            if (depth >= r.classes.size() || p >= int(r.classes.size()) || p < -1) return fail("cyclic or invalid class hierarchy");
            p = r.classes[std::size_t(p)].parent;
        }
        for (std::size_t j = 0; j < i; ++j)
            if (c.order >= 0 && c.order == r.classes[j].order) return fail("duplicate class order");
    }
    if (!r.classes.empty() && std::none_of(r.classes.begin(), r.classes.end(), [](const ClassRule &c) { return c.order >= 0; })) return fail("no ordered region class");
    for (std::size_t i = 0; i < r.adj.size(); ++i) {
        const auto &a = r.adj[i];
        if (a.a < 0 || a.b < 0 || a.a >= a.b || a.b >= int(r.classes.size()) || (i && (r.adj[i - 1].a > a.a || (r.adj[i - 1].a == a.a && r.adj[i - 1].b >= a.b)))) return fail("invalid or duplicate class adjacency");
    }
    for (std::size_t i = 0; i < r.corners.size(); ++i) {
        const auto &c = r.corners[i];
        if (!r.find_tile(c.tile) || (i && r.corners[i - 1].tile >= c.tile)) return fail("invalid corner tile");
        for (auto k : c.cls)
            if (k < -1 || k >= int(r.classes.size())) return fail("invalid corner class");
    }
    std::uint64_t case_total = 0;
    for (std::size_t ci = 0; ci < r.cases.size(); ++ci) {
        const auto &c = r.cases[ci];
        if (ci) {
            const auto &p    = r.cases[ci - 1];
            bool        less = false, different = false;
            for (int i = 0; i < 4 && !different; ++i)
                if (p.cls[i] != c.cls[i]) {
                    less      = p.cls[i] < c.cls[i];
                    different = true;
                }
            if (!different && p.tag != c.tag) {
                less      = p.tag < c.tag;
                different = true;
            }
            for (int i = 0; i < 4 && !different; ++i)
                if (p.t[i] != c.t[i]) {
                    less      = p.t[i] < c.t[i];
                    different = true;
                }
            if (!different || !less) return fail("unsorted or duplicate macro case");
        }
        case_total += c.n;
        if (case_total > 0xffffffffULL) return fail("case weight sum exceeds model capacity");
        if (!c.n || c.n > 1000000000 || (c.tag >> 4) > TAG_ORIENT || ((c.tag >> 4) >= TAG_MIXED && (c.tag >> 4) <= TAG_CLEAR && (c.tag & 15))) return fail("invalid case frequency or tag");
        for (int i = 0; i < 4; ++i)
            if (c.cls[i] >= r.classes.size() || !r.find_tile(c.t[i])) return fail("invalid case reference");
    }
    const auto cells = [&](const std::vector<StampCell> &v, std::size_t max) {
        if (v.empty() || v.size() > max) return false;
        std::uint64_t used[1024] = {};
        for (const auto &cell : v) {
            if (cell.y < 0 || cell.x < 0 || cell.y > 255 || cell.x > 255 || !r.find_tile(cell.tile)) return false;
            const unsigned      offset = unsigned(cell.y) * 256u + unsigned(cell.x);
            const std::uint64_t bit    = std::uint64_t(1) << (offset & 63u);
            if (used[offset >> 6] & bit) return false;
            used[offset >> 6] |= bit;
        }
        return true;
    };
    for (const auto &s : r.stamps)
        if (!s.n || s.n > 1000000000 || !cells(s.cells, 4096)) return fail("invalid stamp");
    for (const auto &d : r.decals)
        if (d.cls < 0 || d.cls >= int(r.classes.size()) || !d.n || d.n > 1000000000 || !cells(d.cells, 12)) return fail("invalid decal");
    for (std::size_t i = 0; i < r.tree_samples.size(); ++i) {
        const auto &t = r.tree_samples[i];
        if (!r.find_tile(t.tile) || t.kind < 1 || t.kind > 255 || t.px < 0 || t.px > 31 || t.py < 0 || t.py > 31 || !t.n || t.n > 1000000000) return fail("invalid tree sample");
        if (i) {
            const auto &p = r.tree_samples[i - 1];
            if (p.tile > t.tile || (p.tile == t.tile && (p.kind > t.kind || (p.kind == t.kind && (p.px > t.px || (p.px == t.px && p.py >= t.py)))))) return fail("unsorted or duplicate tree sample");
        }
    }
    if (r.tree_profile == "none" && (!r.kinds.empty() || !r.tree_samples.empty())) return fail("none tree profile contains trees");
    std::uint64_t ore_total = 0;
    for (const auto &o : r.ore_recs) {
        if (!o.n || o.n > 1000000000) return fail("invalid ore frequency");
        for (auto ch : o.ch)
            if (ch > 3276) return fail("ore channel exceeds runtime cap");
        ore_total += o.n;
        if (ore_total > 1000000) return fail("expanded ore record limit exceeded");
    }
    err.clear();
    return true;
}
// ---- stock list ------------------------------------------------------------------------------------

bool stock_list_parse(std::string_view text, std::vector<StockEntry> &out, std::string &err) {
    out.clear();
    std::size_t pos = 0, lineno = 0;
    while (pos < text.size()) {
        std::size_t nl = text.find('\n', pos);
        if (nl == std::string_view::npos) nl = text.size();
        std::string_view line = text.substr(pos, nl - pos);
        pos                   = nl + 1;
        ++lineno;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
        if (line.empty() || line[0] == '#') continue;
        if (line.size() < 66) {
            err = "stock list line " + std::to_string(lineno) + ": too short";
            return false;
        }
        StockEntry e;
        e.sha256 = std::string(line.substr(0, 64));
        for (char c : e.sha256)
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
                err = "stock list line " + std::to_string(lineno) + ": sha256 must be 64 lowercase hex digits";
                return false;
            }
        std::size_t s = 64;
        while (s < line.size() && line[s] == ' ') ++s;
        e.name = std::string(line.substr(s));
        out.push_back(e);
    }
    std::sort(out.begin(), out.end(), [](const StockEntry &a, const StockEntry &b) { return a.sha256 < b.sha256; });
    return true;
}

bool stock_list_contains(const std::vector<StockEntry> &list, const std::string &sha256) {
    auto it = std::lower_bound(list.begin(), list.end(), sha256, [](const StockEntry &e, const std::string &s) { return e.sha256 < s; });
    return it != list.end() && it->sha256 == sha256;
}

} // namespace mh_mapgen
