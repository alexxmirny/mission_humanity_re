//
// caps/path_pool.cpp -- see path_pool.h.
//
#include "caps/path_pool.h"

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "addr/mh_structs.gen.h"
#include "caps/caps.h"
#include "caps/path_pool_layout.gen.h"
#include "hook/promoted.h"

namespace mh::caps::path {
namespace {

namespace L = mh::caps::path_layout;

constexpr uint32_t P = L::PLAYERS;

bool     g_relocated              = false;
uint32_t g_host                   = 0;
uint8_t *g_stock_buf_override     = nullptr;
uint8_t *g_stock_flags_override   = nullptr;
int32_t *g_count_override         = nullptr;
void (*g_log)(const char *)       = nullptr;
const char *(*g_probe)(uintptr_t) = nullptr;
bool     g_check                  = false;
uint32_t g_violations             = 0;
bool     g_kind_reported[16]      = {};
uint32_t g_kind_count[16]         = {};

void plog(const char *fmt, ...) {
    if (!g_log) return;
    char    b[400];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(b, sizeof(b), _TRUNCATE, fmt, ap);
    va_end(ap);
    g_log(b);
}

uint8_t *stock_buf() { return g_stock_buf_override ? g_stock_buf_override : reinterpret_cast<uint8_t *>(static_cast<uintptr_t>(L::STOCK_BUFFERS)); }
uint8_t *stock_flags() { return g_stock_flags_override ? g_stock_flags_override : reinterpret_cast<uint8_t *>(static_cast<uintptr_t>(L::STOCK_FLAGS)); }
int32_t *count_array() { return g_count_override ? g_count_override : reinterpret_cast<int32_t *>(static_cast<uintptr_t>(L::STOCK_COUNT)); }

constexpr uint32_t STOCK_PLAYER_BYTES = L::STOCK_SLOTS * L::SLOT_BYTES; // 60000

const uint8_t *unit_at(uint32_t p, uint32_t i) { return mh::caps::rec<const uint8_t>(mh::caps::UNITS, p, i); }
uint8_t       *unit_at_w(uint32_t p, uint32_t i) { return mh::caps::rec<uint8_t>(mh::caps::UNITS, p, i); }
uint16_t       rd16(const uint8_t *u, uint32_t off) {
    uint16_t v;
    memcpy(&v, u + off, 2);
    return v;
}
bool in_use(const uint8_t *u) { return u[offsetof(mh::game::mh_map_object_unit, unit_proto_id)] != 0 || u[offsetof(mh::game::mh_map_object_unit, unit_proto_id) + 1] != 0; }

} // namespace

bool adopt(uint32_t host_base) {
    if (!host_base) return false;
    g_host      = host_base;
    g_relocated = true;
    return true;
}
bool relocated() { return g_relocated; }

void reset() {
    g_relocated          = false;
    g_host               = 0;
    g_stock_buf_override = g_stock_flags_override = nullptr;
    g_count_override                              = nullptr;
    g_probe                                       = nullptr;
    g_check                                       = false;
    g_violations                                  = 0;
    memset(g_kind_reported, 0, sizeof(g_kind_reported));
    memset(g_kind_count, 0, sizeof(g_kind_count));
}

uint8_t *buffers() { return g_relocated ? reinterpret_cast<uint8_t *>(static_cast<uintptr_t>(g_host + L::BUF_OFFSET)) : stock_buf(); }
uint8_t *flags() { return g_relocated ? reinterpret_cast<uint8_t *>(static_cast<uintptr_t>(g_host + L::FLAGS_OFFSET)) : stock_flags(); }
uint32_t buffer_player_bytes() { return g_relocated ? L::BUF_PLAYER_BYTES : STOCK_PLAYER_BYTES; }
uint32_t flags_row() { return g_relocated ? L::ROW_SLOTS : L::STOCK_SLOTS; }
uint32_t slot_limit() { return mh::caps::raised() && g_relocated ? L::RAISED_SLOTS : L::STOCK_SLOTS; }
uint8_t *waypoint(uint32_t player, uint32_t slot, uint32_t entry) { return buffers() + static_cast<size_t>(player) * buffer_player_bytes() + static_cast<size_t>(slot) * L::SLOT_BYTES + static_cast<size_t>(entry) * 2u; }

uint32_t unit_slot(const uint8_t *unit) {
    if (g_relocated && mh::caps::raised()) {
        const uint16_t w = rd16(unit, L::ID_WORD_OFF);
        return w >= L::RAISED_SLOTS ? NO_PATH : w;
    }
    const uint8_t b = unit[L::ID_BYTE_OFF];
    return b == 0xff ? NO_PATH : b;
}

uint8_t unit_slot_u8(const uint8_t *unit) {
    const uint8_t b = unit[L::ID_BYTE_OFF];
    if (!(g_relocated && mh::caps::raised()) || b < 0xfe) return b;
    // byte 0xfe stands for "254 or more", 0xff for none: only 254 itself is exact
    return rd16(unit, L::ID_WORD_OFF) == 0xfe ? b : 0xff;
}

// ---- the canonical mirror -----------------------------------------------------------------------------------------------
void gather_into(const uint8_t *wbuf, const uint8_t *wflags, uint8_t *sbuf, uint8_t *sflags) {
    for (uint32_t p = 0; p < P; ++p) {
        memcpy(sbuf + static_cast<size_t>(p) * STOCK_PLAYER_BYTES, wbuf + static_cast<size_t>(p) * L::BUF_PLAYER_BYTES, STOCK_PLAYER_BYTES);
        memcpy(sflags + p * L::STOCK_SLOTS, wflags + p * L::ROW_SLOTS, L::STOCK_SLOTS);
    }
}

void scatter_from(const uint8_t *sbuf, const uint8_t *sflags, uint8_t *wbuf, uint8_t *wflags) {
    for (uint32_t p = 0; p < P; ++p) {
        uint8_t *b = wbuf + static_cast<size_t>(p) * L::BUF_PLAYER_BYTES;
        memcpy(b, sbuf + static_cast<size_t>(p) * STOCK_PLAYER_BYTES, STOCK_PLAYER_BYTES);
        memset(b + STOCK_PLAYER_BYTES, 0, L::BUF_PLAYER_BYTES - STOCK_PLAYER_BYTES); // a loaded world has nothing past the window
        uint8_t *f = wflags + p * L::ROW_SLOTS;
        memcpy(f, sflags + p * L::STOCK_SLOTS, L::STOCK_SLOTS);
        memset(f + L::STOCK_SLOTS, 0, L::ROW_SLOTS - L::STOCK_SLOTS);
    }
}

void gather() {
    if (g_relocated) gather_into(buffers(), flags(), stock_buf(), stock_flags());
}
void scatter() {
    if (g_relocated) scatter_from(stock_buf(), stock_flags(), buffers(), flags());
}
void set_stock_for_test(uint8_t *b, uint8_t *f, int32_t *c) {
    g_stock_buf_override   = b;
    g_stock_flags_override = f;
    g_count_override       = c;
}

// ---- the save ---------------------------------------------------------------------------------------------------------------
bool slot_record_get(uint32_t player, uint32_t slot, uint8_t *out) {
    if (!g_relocated || player >= P || slot >= L::ROW_SLOTS) return false;
    out[0] = flags()[static_cast<size_t>(player) * L::ROW_SLOTS + slot];
    memcpy(out + 1, waypoint(player, slot, 0), L::SLOT_BYTES);
    for (uint32_t i = 0; i < SLOT_RECORD_BYTES; ++i)
        if (out[i]) return true;
    return false;
}

void slot_record_put(uint32_t player, uint32_t slot, const uint8_t *in) {
    if (!g_relocated || player >= P || slot >= L::ROW_SLOTS) return;
    flags()[static_cast<size_t>(player) * L::ROW_SLOTS + slot] = in[0];
    memcpy(waypoint(player, slot, 0), in + 1, L::SLOT_BYTES);
}

namespace {
int32_t g_live_count[P];
bool    g_count_swapped = false;
} // namespace

void save_begin() {
    if (!g_relocated || !mh::caps::raised()) return;
    save_end(); // a previous save that never closed
    for (uint32_t p = 0; p < P; ++p) {
        const uint8_t *fl   = flags() + static_cast<size_t>(p) * L::ROW_SLOTS;
        uint32_t       busy = 0;
        for (uint32_t s = 0; s < L::STOCK_SLOTS; ++s) busy += fl[s] ? 1u : 0u;
        g_live_count[p]  = count_array()[p];
        count_array()[p] = static_cast<int32_t>(L::STOCK_SLOTS - busy);
    }
    g_count_swapped = true;
}

void save_end() {
    if (!g_count_swapped) return;
    g_count_swapped = false;
    for (uint32_t p = 0; p < P; ++p) count_array()[p] = g_live_count[p];
}

// ---- after a load ---------------------------------------------------------------------------------------------------------
void after_load(bool ids16) {
    if (!g_relocated || !mh::caps::raised()) return;
    const uint32_t row     = mh::caps::g(mh::caps::UNITS).row;
    uint32_t       dropped = 0, kept = 0, past = 0;
    for (uint32_t p = 0; p < P; ++p) {
        const uint8_t *fl = flags() + static_cast<size_t>(p) * L::ROW_SLOTS;
        for (uint32_t i = 1; i < row; ++i) {
            uint8_t *u = unit_at_w(p, i);
            if (!in_use(u)) continue;
            uint16_t id = L::RAISED_SLOTS;
            if (ids16) {
                const uint16_t w = rd16(u, L::ID_WORD_OFF);
                if (w < L::RAISED_SLOTS && fl[w]) {
                    id = w;
                    ++kept;
                    if (w >= L::STOCK_SLOTS) ++past;
                } else if (w != L::RAISED_SLOTS) {
                    ++dropped;
                }
            } else {
                const uint8_t b = u[L::ID_BYTE_OFF];
                if (b < L::STOCK_SLOTS && fl[b]) {
                    id = b;
                    ++kept;
                } else if (b != 0xff) {
                    ++dropped;
                }
            }
            u[L::ID_BYTE_OFF] = id == L::RAISED_SLOTS ? 0xff : static_cast<uint8_t>(id < 0xfe ? id : 0xfe);
            memcpy(u + L::ID_WORD_OFF, &id, 2);
        }
        uint32_t busy = 0;
        for (uint32_t s = 0; s < L::RAISED_SLOTS; ++s) busy += fl[s] ? 1u : 0u;
        count_array()[p] = static_cast<int32_t>(L::RAISED_SLOTS - busy);
    }
    plog("; [caps] path pool: after the load %u unit path(s) kept (%u of them past slot %u), %u dropped (%s)\n", kept, past, L::STOCK_SLOTS - 1, dropped,
         ids16 ? "their slot is not busy" : "no path section in the file: only slots < 100 survive");
}

// ---- U4e --------------------------------------------------------------------------------------------------------------------
void set_promoted_probe_for_test(const char *(*fn)(uintptr_t)) { g_probe = fn; }

const char *libmh_blocks() {
    const char *(*probe)(uintptr_t) = g_probe ? g_probe : &mh::hook::promoted_owner_of;
    for (unsigned i = 0; i < L::POOL_FUNCTION_COUNT; ++i)
        if (const char *who = probe(L::POOL_FUNCTIONS[i])) return who;
    return nullptr;
}

// ---- the dev check ---------------------------------------------------------------------------------------------------------
void     set_logger(void (*fn)(const char *)) { g_log = fn; }
void     set_check(bool on) { g_check = on; }
bool     check_enabled() { return g_check; }
uint32_t violations_total() { return g_violations; }

namespace {
enum kind : unsigned { K_BYTE_WORD = 0,
                       K_NOT_BUSY,
                       K_SINK,
                       K_COUNT,
                       K_DUP,
                       K_HOLE_OFF,
                       K_BEYOND,
                       K_N };
const char *KIND_NAME[K_N] = {"byte/word disagree", "holder of a free slot", "sink slot written", "free count off", "slot held twice", "hole written with the raise OFF", "busy flag past the limit"};

void violation(unsigned k, const char *fmt, ...) {
    ++g_violations;
    ++g_kind_count[k];
    if (g_kind_reported[k]) return;
    g_kind_reported[k] = true;
    char    d[260];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(d, sizeof(d), _TRUNCATE, fmt, ap);
    va_end(ap);
    plog("; [caps] path_check FAIL (%s): %s\n", KIND_NAME[k], d);
}
} // namespace

uint32_t check_now() {
    if (!g_relocated) return 0;
    const uint32_t before = g_violations;
    const bool     raised = mh::caps::raised();
    const uint32_t limit  = raised ? L::RAISED_SLOTS : L::STOCK_SLOTS;
    const uint32_t row    = mh::caps::g(mh::caps::UNITS).row;
    for (uint32_t p = 0; p < P; ++p) {
        const uint8_t *fl = flags() + static_cast<size_t>(p) * L::ROW_SLOTS;
        // the sink: never busy, never written (a stray terminator at entry 0 is the shared-path writer's off-by-one)
        if (fl[L::RAISED_SLOTS]) violation(K_SINK, "player %u: the sink slot's flag is set", p);
        {
            const uint8_t *sk = waypoint(p, L::RAISED_SLOTS, 0);
            for (uint32_t b = 1; b < L::SLOT_BYTES; ++b)
                if (sk[b]) {
                    violation(K_SINK, "player %u: sink slot byte +%u = 0x%02x", p, b, sk[b]);
                    break;
                }
        }
        uint32_t busy = 0;
        for (uint32_t s = 0; s < L::RAISED_SLOTS; ++s) {
            if (fl[s]) {
                ++busy;
                if (s >= limit) violation(K_BEYOND, "player %u: slot %u busy but the limit is %u", p, s, limit);
            }
        }
        if (raised) {
            const int32_t want = static_cast<int32_t>(limit) - static_cast<int32_t>(busy);
            if (count_array()[p] != want) violation(K_COUNT, "player %u: free count %d, %u of %u slots busy (want %d)", p, count_array()[p], busy, limit, want);
        }
        uint8_t held[L::RAISED_SLOTS / 8 + 1] = {};
        for (uint32_t i = 1; i < row; ++i) {
            const uint8_t *u = unit_at(p, i);
            const uint16_t w = rd16(u, L::ID_WORD_OFF);
            if (!raised) {
                if (w) violation(K_HOLE_OFF, "player %u unit %u: +0xda = %u while the raise is OFF", p, i, w);
                continue;
            }
            if (!in_use(u)) continue;
            const uint8_t b = u[L::ID_BYTE_OFF];
            if (w == L::RAISED_SLOTS) {
                if (b != 0xff) violation(K_BYTE_WORD, "player %u unit %u: no path (500) but the byte is 0x%02x", p, i, b);
                continue;
            }
            const uint8_t want_b = w < 0xfe ? static_cast<uint8_t>(w) : static_cast<uint8_t>(0xfe);
            if (w > L::RAISED_SLOTS || b != want_b) {
                violation(K_BYTE_WORD, "player %u unit %u: word %u, byte 0x%02x (want 0x%02x)", p, i, w, b, want_b);
                continue;
            }
            if (!fl[w]) violation(K_NOT_BUSY, "player %u unit %u holds slot %u whose flag is clear", p, i, w);
            if (held[w >> 3] & (1u << (w & 7))) violation(K_DUP, "player %u: slot %u is held by two units (second: %u)", p, w, i);
            held[w >> 3] |= static_cast<uint8_t>(1u << (w & 7));
        }
    }
    return g_violations - before;
}

uint32_t step() { return g_check ? check_now() : 0; }

uint64_t pool_digest(unsigned part) {
    if (!g_relocated) return 0;
    const uint32_t s0 = part == DIGEST_TAILS ? L::STOCK_SLOTS : 0u;
    const uint32_t s1 = part == DIGEST_WINDOW ? L::STOCK_SLOTS : L::ROW_SLOTS;
    uint64_t       h  = 1469598103934665603ULL;
    auto           f  = [&h](const uint8_t *p, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            h ^= p[i];
            h *= 1099511628211ULL;
        }
    };
    for (uint32_t p = 0; p < P; ++p) {
        const uint8_t *fl = flags() + static_cast<size_t>(p) * L::ROW_SLOTS;
        for (uint32_t s = s0; s < s1; ++s) {
            f(waypoint(p, s, 0), L::SLOT_BYTES);
            f(fl + s, 1);
        }
    }
    return h;
}

census take_census(uint32_t player) {
    census c = {};
    if (player >= P) return c;
    const bool     raised = mh::caps::raised() && g_relocated;
    const uint32_t row    = mh::caps::g(mh::caps::UNITS).row;
    for (uint32_t i = 1; i < row; ++i) {
        const uint8_t *u = unit_at(player, i);
        if (!in_use(u)) continue;
        const uint32_t id = unit_slot(u);
        if (id == NO_PATH) continue;
        ++c.holding;
        if (id > c.max_id) c.max_id = id;
    }
    (void)raised;
    const uint8_t *fl = flags() + static_cast<size_t>(player) * flags_row();
    for (uint32_t s = 0; s < flags_row(); ++s) c.flags_set += fl[s] ? 1u : 0u;
    c.free_count = count_array()[player];
    return c;
}

} // namespace mh::caps::path
