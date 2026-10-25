//
// caps/ovf.cpp -- the overflow extension of the raised roster arrays (see ovf.h).
//
#include "caps/ovf.h"

#include "caps/orders.h"
#include "caps/path_pool.h"
#include "caps/path_pool_layout.gen.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace mh::caps::ovf {
namespace {

constexpr unsigned ARRAYS    = WIDE_COUNT;   // the wide arrays, MH_CAPS_IDX_BUILDINGS .. SOLDIERS
constexpr unsigned FP_ARRAYS = RAISED_COUNT; // the layout fingerprint covers the v0.2.2 seven only (see ovf.h)

const char *const ARRAY_NAME[ARRAYS] = {"buildings", "unit_storage", "turrets", "mines", "labs", "productions", "projectiles", "units", "soldiers"};

struct def {
    const char *name;
    arr         a;
    bool        split; // true: the first / second half of the array's player rows
    int         half;  // 0 or 1 when split
};
const def DEFS[ROSTER_SLICES] = {
    {"ovf_buildings_p0_3", BUILDINGS, true, 0},
    {"ovf_buildings_p4_7", BUILDINGS, true, 1},
    {"ovf_unit_storage", UNIT_STORAGE, false, 0},
    {"ovf_turrets", TURRETS, false, 0},
    {"ovf_mines", MINES, false, 0},
    {"ovf_labs", LABS, false, 0},
    {"ovf_productions", PRODUCTIONS, false, 0},
    {"ovf_projectiles", PROJECTILES, false, 0},
    {"ovf_units_p0_3", UNITS, true, 0},
    {"ovf_units_p4_7", UNITS, true, 1},
    {"ovf_soldiers", SOLDIERS, false, 0},
};

namespace PLY = mh::caps::path_layout;
namespace ORD = mh::caps::orders;

// PT-CAP9-U7d: the slices past the roster (see ovf.h for why pending / staging are not among them).
constexpr int PATH_TAIL0 = ROSTER_SLICES;  // 11..14: players 2k..2k+1, slots 100..500 (500 = the sink)
constexpr int PATH_FLAGS = PATH_TAIL0 + 4; // 15: every busy flag [8][501]
constexpr int PATH_WIN0  = PATH_FLAGS + 1; // 16..19: players 2k..2k+1, slots 0..99
constexpr int ORD_QUEUE  = PATH_WIN0 + 4;  // 20
static_assert(ORD_QUEUE + 1 == SLICES, "the slice table and ovf::SLICES disagree");

const char *const PATH_TAIL_NAME[4] = {"ovf_path_tail_p0_1", "ovf_path_tail_p2_3", "ovf_path_tail_p4_5", "ovf_path_tail_p6_7"};
const char *const PATH_WIN_NAME[4]  = {"ovf_path_window_p0_1", "ovf_path_window_p2_3", "ovf_path_window_p4_5", "ovf_path_window_p6_7"};

struct table_t {
    slice_info s[SLICES];
    uint32_t   total, roster;
    table_t() {
        total = 0;
        for (int e = 0; e < ROSTER_SLICES; ++e) {
            const def  &d  = DEFS[e];
            const geom &sk = stock(d.a);
            slice_info &x  = s[e];
            x.name         = d.name;
            x.src          = SRC_ROSTER;
            x.arr          = static_cast<uint8_t>(d.a);
            x.p0           = d.split ? static_cast<uint8_t>(d.half * (sk.players / 2)) : 0;
            x.p1           = d.split ? static_cast<uint8_t>((d.half + 1) * (sk.players / 2)) : static_cast<uint8_t>(sk.players);
            x.rec0         = sk.row;
            x.rec1         = RAISED_ROW[d.a];
            x.elem         = sk.elem;
            x.seg_len      = (x.rec1 - x.rec0) * x.elem;
            x.len          = static_cast<uint32_t>(x.p1 - x.p0) * x.seg_len;
            total += x.len;
        }
        roster    = total;
        auto fill = [&](int e, const char *name, uint8_t src, uint8_t arr, uint8_t p0, uint8_t p1, uint32_t r0, uint32_t r1, uint32_t elem) {
            slice_info &x = s[e];
            x.name        = name;
            x.src         = src;
            x.arr         = arr;
            x.p0          = p0;
            x.p1          = p1;
            x.rec0        = r0;
            x.rec1        = r1;
            x.elem        = elem;
            x.seg_len     = (r1 - r0) * elem;
            x.len         = static_cast<uint32_t>(p1 - p0) * x.seg_len;
            total += x.len;
        };
        for (int k = 0; k < 4; ++k) {
            fill(PATH_TAIL0 + k, PATH_TAIL_NAME[k], SRC_PATH_TAIL, 0, static_cast<uint8_t>(2 * k), static_cast<uint8_t>(2 * k + 2), PLY::STOCK_SLOTS, PLY::ROW_SLOTS, PLY::SLOT_BYTES);
            fill(PATH_WIN0 + k, PATH_WIN_NAME[k], SRC_PATH_WINDOW, 0, static_cast<uint8_t>(2 * k), static_cast<uint8_t>(2 * k + 2), 0, PLY::STOCK_SLOTS, PLY::SLOT_BYTES);
        }
        fill(PATH_FLAGS, "ovf_path_flags", SRC_PATH_FLAGS, 0, 0, static_cast<uint8_t>(PLY::PLAYERS), 0, PLY::ROW_SLOTS, 1);
        fill(ORD_QUEUE, "ovf_order_queue", SRC_ORDERS, static_cast<uint8_t>(ORD::QUEUE), 0, 1, ORD::stock_cap(ORD::QUEUE), ORD::wide_cap(ORD::QUEUE), ORD::REC_BYTES);
    }
};
const table_t &T() {
    static const table_t t;
    return t;
}

inline uint32_t nblocks_of(uint32_t len) { return (len + mh::state::inc::BLOCK - 1) / mh::state::inc::BLOCK; }

} // namespace

const slice_info &slice(int e) { return T().s[e]; }
bool              active(int e) {
    const slice_info &s = slice(e);
    switch (s.src) {
        case SRC_ROSTER: return wide(static_cast<arr>(s.arr));
        case SRC_ORDERS: return ORD::on();
        default: return mh::caps::path::relocated();
    }
}
uint32_t total_bytes() { return T().total; }
uint32_t roster_bytes() { return T().roster; }

namespace {
bool any_active() {
    for (int e = 0; e < SLICES; ++e)
        if (active(e)) return true;
    return false;
}
} // namespace

uint64_t fp_on(uint64_t fp_off) {
    uint64_t      h = fnv64(&OVF_KIND, sizeof(OVF_KIND), fp_off);
    const int32_t n = SLICES;
    h               = fnv64(&n, sizeof(n), h);
    for (int e = 0; e < SLICES; ++e) {
        const uint32_t v[2] = {slice(e).len, active(e) ? 1u : 0u};
        h                   = fnv64(v, sizeof(v), h);
    }
    return h;
}

const uint8_t *segment(int e, int k) {
    const slice_info &s = slice(e);
    switch (s.src) {
        case SRC_PATH_TAIL:
        case SRC_PATH_WINDOW: return mh::caps::path::waypoint(static_cast<uint32_t>(s.p0 + k), s.rec0, 0);
        case SRC_PATH_FLAGS: return mh::caps::path::flags() + static_cast<size_t>(s.p0 + k) * PLY::ROW_SLOTS;
        case SRC_ORDERS: return ORD::wide_record(static_cast<ORD::which>(s.arr), s.rec0);
        default: return rec<uint8_t>(static_cast<arr>(s.arr), static_cast<uint32_t>(s.p0 + k), s.rec0);
    }
}

void describe(int e, uint32_t off, char *out, size_t cap) {
    if (!cap) return;
    out[0] = 0;
    if (e < 0 || e >= SLICES) return;
    const slice_info &s = slice(e);
    if (off >= s.len) return;
    const uint32_t k    = off / s.seg_len;
    const uint32_t w    = off % s.seg_len;
    const uint32_t idx  = s.rec0 + w / s.elem;
    const uint32_t byte = w % s.elem;
    switch (s.src) {
        case SRC_PATH_TAIL:
        case SRC_PATH_WINDOW: _snprintf_s(out, cap, _TRUNCATE, "path pool p%u slot %u waypoint %u +%u", (unsigned)(s.p0 + k), (unsigned)idx, byte / 2u, byte % 2u); return;
        case SRC_PATH_FLAGS: _snprintf_s(out, cap, _TRUNCATE, "path busy flag p%u slot %u", (unsigned)(s.p0 + k), (unsigned)w); return;
        case SRC_ORDERS: _snprintf_s(out, cap, _TRUNCATE, "order queue #%u +0x%X", (unsigned)idx, (unsigned)byte); return;
        default: break;
    }
    _snprintf_s(out, cap, _TRUNCATE, "%s p%u #%u +0x%X", ARRAY_NAME[s.arr], (unsigned)(s.p0 + k), (unsigned)idx, (unsigned)byte);
}

void gather(int e, uint32_t off, uint32_t n, uint8_t *out) {
    const slice_info &s = slice(e);
    while (n) {
        const uint32_t k    = off / s.seg_len;
        const uint32_t w    = off % s.seg_len;
        const uint32_t take = (s.seg_len - w) < n ? (s.seg_len - w) : n;
        memcpy(out, segment(e, static_cast<int>(k)) + w, take);
        out += take;
        off += take;
        n -= take;
    }
}

uint64_t recompute(int e) {
    if (!active(e)) return 0;
    const slice_info &s  = slice(e);
    const uint32_t    nb = nblocks_of(s.len);
    uint8_t           tmp[mh::state::inc::BLOCK];
    uint64_t          sum = 0;
    for (uint32_t b = 0; b < nb; ++b) {
        const uint32_t n = (s.len - b * mh::state::inc::BLOCK) < mh::state::inc::BLOCK ? (s.len - b * mh::state::inc::BLOCK) : mh::state::inc::BLOCK;
        gather(e, b * mh::state::inc::BLOCK, n, tmp);
        sum += block_hash(e, b, tmp, n);
    }
    return sum;
}

void recompute_all(uint64_t *out8) {
    for (int e = 0; e < SLICES; ++e) out8[e] = recompute(e);
}

uint64_t digest() {
    uint64_t h = FNV64_OFFSET;
    for (int e = 0; e < ROSTER_SLICES; ++e) { // the save round-trip digests cover the roster tails only (U7d: the other slices are hash-only)
        if (!active(e)) continue;
        const slice_info &s = slice(e);
        for (int k = 0; k < s.p1 - s.p0; ++k) h = fnv64(segment(e, k), s.seg_len, h);
    }
    return h;
}

uint64_t digest_persisted() {
    uint64_t h = FNV64_OFFSET;
    for (int e = 0; e < ROSTER_SLICES; ++e) { // the save round-trip digests cover the roster tails only (U7d: the other slices are hash-only)
        if (!active(e)) continue;
        const slice_info &s = slice(e);
        for (int k = 0; k < s.p1 - s.p0; ++k) {
            if (s.arr != UNITS) {
                h = fnv64(segment(e, k), s.seg_len, h);
                continue;
            }
            // PT-CAP9-U4: a unit's path slot id (+0xaa byte, +0xda 16-bit id) is not part of what a planet save carries past the vanilla
            // 100 slots (the path buffers of slots >= 100 are not in the file until U4d), so a save/load round trip clears it.
            std::vector<uint8_t> seg(segment(e, k), segment(e, k) + s.seg_len);
            for (uint32_t r = 0; r + s.elem <= s.seg_len; r += s.elem) {
                seg[r + 0xaa] = 0;
                seg[r + 0xda] = 0;
                seg[r + 0xdb] = 0;
            }
            h = fnv64(seg.data(), s.seg_len, h);
        }
    }
    return h;
}

// ---- the tracker ----------------------------------------------------------------------------------------
size_t tracker::arena_bytes() {
    size_t t = 0;
    for (int e = 0; e < SLICES; ++e) t += static_cast<size_t>(nblocks_of(slice(e).len)) * 8u + ((static_cast<size_t>(slice(e).len) + 7u) & ~static_cast<size_t>(7));
    return t;
}

void tracker::attach(void *arena) {
    uint8_t *m = static_cast<uint8_t *>(arena);
    primed_    = false;
    for (int e = 0; e < SLICES; ++e) {
        slot &x   = s_[e];
        x.len     = slice(e).len;
        x.nblocks = nblocks_of(x.len);
        x.bh      = reinterpret_cast<uint64_t *>(m);
        x.shadow  = m + static_cast<size_t>(x.nblocks) * 8u;
        x.sum     = 0;
        m += static_cast<size_t>(x.nblocks) * 8u + ((static_cast<size_t>(x.len) + 7u) & ~static_cast<size_t>(7));
    }
    last_changed_blocks = 0;
}

bool tracker::prime() {
    if (!any_active()) return false;
    for (int e = 0; e < SLICES; ++e) {
        slot             &x = s_[e];
        const slice_info &s = slice(e);
        x.sum               = 0;
        if (!active(e)) continue;
        for (int k = 0; k < s.p1 - s.p0; ++k) memcpy(x.shadow + static_cast<size_t>(k) * s.seg_len, segment(e, k), s.seg_len);
        x.sum = 0;
        for (uint32_t b = 0; b < x.nblocks; ++b) {
            const uint32_t off = b * mh::state::inc::BLOCK;
            const uint32_t n   = (x.len - off) < mh::state::inc::BLOCK ? (x.len - off) : mh::state::inc::BLOCK;
            x.bh[b]            = block_hash(e, b, x.shadow + off, n);
            x.sum += x.bh[b];
        }
    }
    last_changed_blocks = 0;
    primed_             = true;
    return true;
}

void tracker::refresh_block(int e, uint32_t b, mh::state::inc::journal *j) {
    slot          &x   = s_[e];
    const uint32_t off = b * mh::state::inc::BLOCK;
    const uint32_t n   = (x.len - off) < mh::state::inc::BLOCK ? (x.len - off) : mh::state::inc::BLOCK;
    uint8_t        live[mh::state::inc::BLOCK];
    gather(e, off, n, live);
    if (memcmp(live, x.shadow + off, n) == 0) return;
    if (j) j->old_block((int)mh::state::HASH_REGION_COUNT + e, b, x.shadow + off, n);
    memcpy(x.shadow + off, live, n);
    x.sum -= x.bh[b];
    x.bh[b] = block_hash(e, b, live, n);
    x.sum += x.bh[b];
    ++last_changed_blocks;
}

void tracker::update(mh::state::inc::journal *j) {
    last_changed_blocks = 0;
    if (!primed_) {
        prime();
        return;
    }
    if (!any_active()) return;
    for (int e = 0; e < SLICES; ++e) {
        if (!active(e)) continue;
        const slice_info &s  = slice(e);
        slot             &x  = s_[e];
        const int         ns = s.p1 - s.p0;
        for (int k = 0; k < ns; ++k) {
            const uint8_t *live = segment(e, k);
            const uint32_t soff = static_cast<uint32_t>(k) * s.seg_len;
            const uint8_t *sh   = x.shadow + soff;
            for (uint32_t c = 0; c < s.seg_len; c += mh::state::inc::CHUNK) {
                const uint32_t cn = (s.seg_len - c) < mh::state::inc::CHUNK ? (s.seg_len - c) : mh::state::inc::CHUNK;
                if (memcmp(live + c, sh + c, cn) == 0) continue;
                const uint32_t b0 = (soff + c) / mh::state::inc::BLOCK;
                const uint32_t b1 = (soff + c + cn - 1) / mh::state::inc::BLOCK;
                for (uint32_t b = b0; b <= b1; ++b) refresh_block(e, b, j);
            }
        }
    }
}

void tracker::sums(uint64_t *out8) const {
    for (int e = 0; e < SLICES; ++e) out8[e] = s_[e].sum;
}

int tracker::verify(int *first_bad) const {
    int bad = 0, first = -1;
    for (int e = 0; e < SLICES; ++e) {
        if (recompute(e) == s_[e].sum) continue;
        if (first < 0) first = e;
        ++bad;
    }
    if (first_bad) *first_bad = first;
    return bad;
}

// ---- the codec --------------------------------------------------------------------------------------------
uint32_t layout_fp() {
    uint32_t h = FNV32_OFFSET;
    for (unsigned a = 0; a < FP_ARRAYS; ++a) {
        const geom    &sk   = stock(static_cast<arr>(a));
        const uint32_t v[5] = {a, sk.players, sk.elem, sk.row, RAISED_ROW[a]};
        h                   = fnv32(v, sizeof(v), h);
    }
    return h;
}

namespace {

namespace PL = mh::caps::path_layout;

constexpr uint32_t PATH_SLOTS_PAST = PL::ROW_SLOTS - PL::STOCK_SLOTS; // slots 100..500 (500 = the sink)

// What this build knows about a section kind: its geometry, and whether the memory behind it exists in this run.
struct kind_geom {
    uint32_t players, elem, vrow, rrow;
    bool     available;
};
kind_geom geom_of(unsigned k) {
    if (k < ARRAYS) {
        const geom &sk = stock(static_cast<arr>(k));
        return {sk.players, sk.elem, sk.row, RAISED_ROW[k], wide(static_cast<arr>(k))};
    }
    if (k >= KIND_ORDER_QUEUE && k <= KIND_ORDER_STAGING) {
        const auto w = static_cast<mh::caps::orders::which>(k - KIND_ORDER_QUEUE);
        return {1u, mh::caps::orders::REC_BYTES, mh::caps::orders::stock_cap(w), mh::caps::orders::wide_cap(w), mh::caps::orders::on()};
    }
    return {PL::PLAYERS, mh::caps::path::SLOT_RECORD_BYTES, PL::STOCK_SLOTS, PL::ROW_SLOTS, mh::caps::path::relocated()};
}

} // namespace

size_t max_bytes() {
    size_t t = sizeof(ovf_header) + ARRAYS * sizeof(ovf_section);
    for (unsigned a = 0; a < ARRAYS; ++a) {
        const geom &sk = stock(static_cast<arr>(a));
        t += static_cast<size_t>(sk.players) * (RAISED_ROW[a] - sk.row) * (sizeof(ovf_record) + sk.elem);
    }
    return t;
}

namespace {
// The worst case of order section `w`: every record from the stock window to the raised cap.
size_t order_section_max(mh::caps::orders::which w) {
    return sizeof(ovf_section) + static_cast<size_t>(mh::caps::orders::wide_cap(w) - mh::caps::orders::stock_cap(w)) * (sizeof(ovf_record) + mh::caps::orders::REC_BYTES);
}
} // namespace

size_t max_bytes_save() {
    return max_bytes() + sizeof(ovf_section) + static_cast<size_t>(PL::PLAYERS) * PATH_SLOTS_PAST * (sizeof(ovf_record) + mh::caps::path::SLOT_RECORD_BYTES) +
           order_section_max(mh::caps::orders::QUEUE);
}

size_t max_bytes_world() {
    return max_bytes() + order_section_max(mh::caps::orders::QUEUE) + order_section_max(mh::caps::orders::PENDING) + order_section_max(mh::caps::orders::STAGING);
}

namespace {
bool all_zero(const uint8_t *p, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i)
        if (p[i]) return false;
    return true;
}
} // namespace

size_t encode(uint8_t *out, size_t cap, unsigned mode) {
    if (!relocated() && !units_relocated()) return ENCODE_FAIL;
    if (!out || cap < sizeof(ovf_header)) return ENCODE_FAIL;
    size_t   pos  = sizeof(ovf_header); // header written last
    uint32_t nsec = 0, total = 0;
    size_t   sec_at_of[KINDS_KNOWN];
    for (unsigned a = 0; a < ARRAYS; ++a) {
        if (!wide(static_cast<arr>(a))) continue;
        const geom  &sk     = stock(static_cast<arr>(a));
        const size_t sec_at = pos;
        uint32_t     nrec   = 0;
        if (pos + sizeof(ovf_section) > cap) return ENCODE_FAIL;
        pos += sizeof(ovf_section);
        for (uint32_t p = 0; p < sk.players; ++p)
            for (uint32_t r = sk.row; r < RAISED_ROW[a]; ++r) {
                const uint8_t *rp = rec<uint8_t>(static_cast<arr>(a), p, r);
                if (all_zero(rp, sk.elem)) continue;
                if (pos + sizeof(ovf_record) + sk.elem > cap) return ENCODE_FAIL;
                ovf_record h = {static_cast<uint8_t>(p), 0, static_cast<uint16_t>(r)};
                memcpy(out + pos, &h, sizeof(h));
                memcpy(out + pos + sizeof(h), rp, sk.elem);
                pos += sizeof(h) + sk.elem;
                ++nrec;
            }
        if (nrec == 0) {
            pos = sec_at; // an array with nothing past the window has no section
            continue;
        }
        ovf_section sec = {static_cast<uint8_t>(a), static_cast<uint8_t>(sk.players), static_cast<uint16_t>(sk.elem),
                           static_cast<uint16_t>(sk.row), static_cast<uint16_t>(RAISED_ROW[a]), nrec, 0, 0};
        memcpy(out + sec_at, &sec, sizeof(sec));
        sec_at_of[nsec] = sec_at;
        ++nsec;
        total += nrec;
    }
    // PT-CAP9-U7b: the path-slot pool past the window -- a SAVE only, and only when some slot 100..500 holds a byte (a busy flag or stale waypoints).
    bool v2 = false;
    if (mode == FOR_SAVE && mh::caps::path::relocated()) {
        const size_t sec_at = pos;
        uint32_t     nrec   = 0;
        if (pos + sizeof(ovf_section) > cap) return ENCODE_FAIL;
        pos += sizeof(ovf_section);
        for (uint32_t p = 0; p < PL::PLAYERS; ++p)
            for (uint32_t s = PL::STOCK_SLOTS; s < PL::ROW_SLOTS; ++s) {
                uint8_t tmp[mh::caps::path::SLOT_RECORD_BYTES];
                if (!mh::caps::path::slot_record_get(p, s, tmp)) continue;
                if (pos + sizeof(ovf_record) + sizeof(tmp) > cap) return ENCODE_FAIL;
                ovf_record h = {static_cast<uint8_t>(p), 0, static_cast<uint16_t>(s)};
                memcpy(out + pos, &h, sizeof(h));
                memcpy(out + pos + sizeof(h), tmp, sizeof(tmp));
                pos += sizeof(h) + sizeof(tmp);
                ++nrec;
            }
        if (nrec == 0) {
            pos = sec_at;
        } else {
            ovf_section sec = {static_cast<uint8_t>(KIND_PATH_SLOT), static_cast<uint8_t>(PL::PLAYERS), static_cast<uint16_t>(mh::caps::path::SLOT_RECORD_BYTES),
                               static_cast<uint16_t>(PL::STOCK_SLOTS), static_cast<uint16_t>(PL::ROW_SLOTS), nrec, 0, 0};
            memcpy(out + sec_at, &sec, sizeof(sec));
            sec_at_of[nsec] = sec_at;
            ++nsec;
            total += nrec;
            v2 = true;
        }
    }
    // PT-CAP9-U7c: the order arrays past their vanilla windows. A section holds EVERY record in [stock, live count) -- the count is the highest index + 1,
    // and it is the one thing the stock blocks cannot carry. The queue rides saves and worlds; pending and staging worlds only (the planet save has no block for them).
    if (mh::caps::orders::on()) {
        for (unsigned k = KIND_ORDER_QUEUE; k <= KIND_ORDER_STAGING; ++k) {
            if (mode != FOR_WORLD && k != KIND_ORDER_QUEUE) continue;
            const auto     w     = static_cast<mh::caps::orders::which>(k - KIND_ORDER_QUEUE);
            const uint32_t from  = mh::caps::orders::stock_cap(w);
            const uint32_t count = mh::caps::orders::live_count(w);
            if (count <= from) continue;
            const size_t need = sizeof(ovf_section) + static_cast<size_t>(count - from) * (sizeof(ovf_record) + mh::caps::orders::REC_BYTES);
            if (pos + need > cap) return ENCODE_FAIL;
            const size_t sec_at = pos;
            pos += sizeof(ovf_section);
            for (uint32_t i = from; i < count; ++i) {
                ovf_record h = {0, 0, static_cast<uint16_t>(i)};
                memcpy(out + pos, &h, sizeof(h));
                memcpy(out + pos + sizeof(h), mh::caps::orders::wide_record(w, i), mh::caps::orders::REC_BYTES);
                pos += sizeof(h) + mh::caps::orders::REC_BYTES;
            }
            ovf_section sec = {static_cast<uint8_t>(k), 1, static_cast<uint16_t>(mh::caps::orders::REC_BYTES), static_cast<uint16_t>(from),
                               static_cast<uint16_t>(mh::caps::orders::wide_cap(w)), count - from, 0, 0};
            memcpy(out + sec_at, &sec, sizeof(sec));
            sec_at_of[nsec] = sec_at;
            ++nsec;
            total += count - from;
            v2 = true;
        }
    }
    if (total == 0) return 0;
    if (v2) { // every section of a v2 blob names its kind version
        for (uint32_t i = 0; i < nsec; ++i) {
            const uint16_t kv = KIND_VER;
            memcpy(out + sec_at_of[i] + offsetof(ovf_section, kind_ver), &kv, sizeof(kv));
        }
    }
    ovf_header h;
    h.magic     = MHOV_MAGIC;
    h.version   = v2 ? MHOV_VERSION : MHOV_VERSION_1;
    h.nsec      = static_cast<uint16_t>(nsec);
    h.body_len  = static_cast<uint32_t>(pos - sizeof(ovf_header));
    h.layout_fp = v2 ? 0u : layout_fp();
    h.body_sum  = fnv64(out + sizeof(ovf_header), h.body_len);
    h.records   = total;
    h.features  = v2 && mh::caps::path::relocated() ? FEAT_PATH_ID16 : 0u; // the units' 16-bit path ids are valid only in a build that has the pool
    memcpy(out, &h, sizeof(h));
    return pos;
}

namespace {

// Walks a blob, validating it; visit(kind, player, idx, bytes, elem) per APPLICABLE record (skipped OPTIONAL sections are framed, not visited).
template <class V>
int walk(const void *blob, size_t n, V &&visit, uint32_t *features_out) {
    if (!blob) return OVF_ERR_ARG;
    if (features_out) *features_out = 0;
    if (n < sizeof(ovf_header)) return OVF_ERR_TRUNCATED;
    ovf_header h;
    memcpy(&h, blob, sizeof(h));
    if (h.magic != MHOV_MAGIC) return OVF_ERR_MAGIC;
    const bool v2 = h.version == MHOV_VERSION;
    if (h.version != MHOV_VERSION_1 && !v2) return OVF_ERR_VERSION;
    if (n - sizeof(h) < h.body_len) return OVF_ERR_TRUNCATED;
    if (n - sizeof(h) > h.body_len) return OVF_ERR_TRAILING;
    const uint8_t *body = static_cast<const uint8_t *>(blob) + sizeof(h);
    if (fnv64(body, h.body_len) != h.body_sum) return OVF_ERR_SUM;
    if (v2) {
        if (h.features & ~FEAT_KNOWN) return OVF_ERR_FEATURE;
        if (h.layout_fp != 0) return OVF_ERR_LAYOUT;
    } else {
        if (h.layout_fp != layout_fp()) return OVF_ERR_LAYOUT;
        if (h.features != 0) return OVF_ERR_LAYOUT;
    }
    size_t   pos  = 0;
    uint32_t seen = 0;
    int      prev = -1;
    for (unsigned s = 0; s < h.nsec; ++s) {
        if (h.body_len - pos < sizeof(ovf_section)) return OVF_ERR_TRUNCATED;
        ovf_section sec;
        memcpy(&sec, body + pos, sizeof(sec));
        pos += sizeof(sec);
        bool skip = false;
        if (!v2) {
            if (sec.arr >= ARRAYS) return OVF_ERR_RANGE;
            if (static_cast<int>(sec.arr) <= prev) return OVF_ERR_SECTION_ORDER;
            if (!wide(static_cast<arr>(sec.arr))) return OVF_ERR_NOT_RELOCATED; // the blob names an array this build did not relocate
            const geom &sk = stock(static_cast<arr>(sec.arr));
            if (sec.elem != sk.elem) return OVF_ERR_ELEM;
            if (sec.players != sk.players || sec.vanilla_row != sk.row || sec.raised_row != RAISED_ROW[sec.arr] || sec.flags != 0 || sec.kind_ver != 0)
                return OVF_ERR_LAYOUT;
        } else {
            if (static_cast<int>(sec.arr) <= prev) return OVF_ERR_SECTION_ORDER;
            if (sec.flags & ~SEC_OPTIONAL) return OVF_ERR_FEATURE;
            int err = OVF_OK;
            if (sec.arr >= KINDS_KNOWN || sec.kind_ver != KIND_VER) {
                err = OVF_ERR_KIND;
            } else {
                const kind_geom kg = geom_of(sec.arr);
                if (sec.elem != kg.elem) err = OVF_ERR_ELEM;
                else if (sec.players != kg.players || sec.vanilla_row != kg.vrow || sec.raised_row != kg.rrow) err = OVF_ERR_LAYOUT;
                else if (!kg.available) err = OVF_ERR_NOT_RELOCATED;
            }
            if (err != OVF_OK) {
                if (!(sec.flags & SEC_OPTIONAL)) return err;
                skip = true;
            }
        }
        prev             = sec.arr;
        const size_t per = sizeof(ovf_record) + sec.elem;
        if (static_cast<uint64_t>(sec.nrec) * per > h.body_len - pos) return OVF_ERR_TRUNCATED;
        if (skip) { // an OPTIONAL section this build cannot apply: framed by its own counts, applied to nothing
            pos += static_cast<size_t>(sec.nrec) * per;
            seen += sec.nrec;
            continue;
        }
        const kind_geom kg   = geom_of(sec.arr);
        int64_t         last = -1;
        for (uint32_t r = 0; r < sec.nrec; ++r) {
            ovf_record rh;
            memcpy(&rh, body + pos, sizeof(rh));
            if (rh.pad != 0 || rh.player >= kg.players || rh.idx < kg.vrow || rh.idx >= kg.rrow) return OVF_ERR_RANGE;
            const int64_t key = (static_cast<int64_t>(rh.player) << 16) | rh.idx;
            if (key <= last) return OVF_ERR_RECORD_ORDER;
            last = key;
            visit(sec.arr, rh.player, rh.idx, body + pos + sizeof(rh), sec.elem);
            pos += per;
        }
        seen += sec.nrec;
    }
    if (seen != h.records) return OVF_ERR_COUNT;
    if (pos != h.body_len) return OVF_ERR_TRAILING;
    if (features_out) *features_out = v2 ? h.features : 0u;
    return OVF_OK;
}

} // namespace

int check(const void *blob, size_t n) {
    return walk(blob, n, [](unsigned, unsigned, unsigned, const uint8_t *, uint32_t) {}, nullptr);
}

int apply(const void *blob, size_t n, uint32_t *features) {
    if (features) *features = 0;
    if (!relocated() && !units_relocated()) return OVF_ERR_NOT_RELOCATED;
    const int rc = check(blob, n);
    if (rc != OVF_OK) return rc;
    return walk(
        blob, n,
        [](unsigned k, unsigned p, unsigned idx, const uint8_t *bytes, uint32_t elem) {
            if (k < ARRAYS) memcpy(rec<uint8_t>(static_cast<arr>(k), p, idx), bytes, elem);
            else if (k == KIND_PATH_SLOT) mh::caps::path::slot_record_put(p, idx, bytes);
            else mh::caps::orders::import_record(static_cast<mh::caps::orders::which>(k - KIND_ORDER_QUEUE), idx, bytes);
        },
        features);
}

int record_count(const void *blob, size_t n) {
    if (check(blob, n) != OVF_OK) return -1;
    ovf_header h;
    memcpy(&h, blob, sizeof(h));
    return static_cast<int>(h.records);
}

int section_records(const void *blob, size_t n, unsigned kind) {
    if (check(blob, n) != OVF_OK) return -1;
    ovf_header h;
    memcpy(&h, blob, sizeof(h));
    const uint8_t *body = static_cast<const uint8_t *>(blob) + sizeof(h);
    size_t         pos  = 0;
    for (unsigned s = 0; s < h.nsec; ++s) {
        ovf_section sec;
        memcpy(&sec, body + pos, sizeof(sec));
        pos += sizeof(sec);
        if (sec.arr == kind) return static_cast<int>(sec.nrec);
        pos += static_cast<size_t>(sec.nrec) * (sizeof(ovf_record) + sec.elem);
    }
    return 0;
}

bool append_to_world_blob(uint8_t *blob, size_t *got, size_t cap) {
    if (!raised()) return true;
    const size_t n = encode(blob + *got, cap - *got);
    if (n == ENCODE_FAIL) return false;
    *got += n;
    return true;
}

} // namespace mh::caps::ovf
