//
// save/save_driver.cpp -- see save_driver.h. The walk, and nothing else: every address, size and
// ordering decision lives in save_table.gen.h.
//
#include "save_driver.h"

#include "state/region_owner.h"

#include "addr/mh_regions.gen.h"

#include <cstring>

namespace mh::save {

// ---------------------------------------------------------------------------------------------
// RI-STATE / ST1: EVERY BLOCK THIS WALK TOUCHES MUST BE A REGISTERED STATE REGION, checked here at
// COMPILE TIME.
//
// The save format is the widest consumer of raw state addresses in the tree -- ~90 blocks spanning
// ~10 MB -- and until now it was the one that knew least about them: its table is extracted from the
// original's disassembly, independently of every other manifest, which is how its first block came
// to be the order QUEUE without anything connecting it to `mh::orders`. Asserting coverage does not
// by itself make the save driver ask an owner for its bytes (that is ST4, and only for orders); what
// it does is make it IMPOSSIBLE for the two tables to describe different worlds without saying so.
//
// The tool-side check in gen_state_registry.py is the arm that can genuinely go red on a data
// change, because it runs against the COMMITTED registry rather than a freshly merged one. This arm
// is the belt: it catches a hand-edit of either generated header, which the tool cannot see.
namespace {
constexpr bool all_blocks_registered(const table::step *steps, int n) {
    for (int i = 0; i < n; ++i) {
        if (steps[i].kind != table::STEP_BLOCK || !steps[i].addr || !steps[i].size) continue;
#ifdef MH_LIBMH_BUILD
        // LIB-REF-SPLIT: the SAME check, reached by region id instead of by stock address.
        // covering() is not declared standalone -- there is no image to resolve an original .bss
        // address against -- so the block's owning region is looked up in the table the save-table
        // generator resolved from the identical decompose() and the identical registry file
        // (table::BLOCK_OWNERS). Still constexpr, so this is still a compile-time assert and not a
        // weaker standalone check; only the route differs.
        const int oi = table::block_owner_index(steps[i].addr, steps[i].size);
        if (oi < 0 || table::BLOCK_OWNERS[oi].rid == (uint16_t)mh::state::RID_COUNT) return false;
        const mh::state::region *r =
            &mh::state::REGIONS[table::BLOCK_OWNERS[oi].rid]; // the same region covering() returns
#else
        const mh::state::region *r = mh::state::covering(steps[i].addr, steps[i].size);
        if (!r) return false;
#endif
        // ST3, the ownership interlock at its most precise point. A block naming a region that has
        // LEFT the binary is not merely unregistered -- it would block-copy the memory the region
        // abandoned, and both the save and the SV1-P-LOAD A/B would keep agreeing about it, because
        // both arms read the same stale address. So a save block may reach a RELOCATED region only
        // when a module serves it; otherwise this table has to stop naming it.
        //
        // IT IS `relocated`, NOT `owned_by_dll`, since the D4 split (SB-SOLE): the redefined
        // owned_by_dll means `libmh binds this region and no original function writes it`, which in
        // the hosted build is true of 342 regions that have not moved -- and a save block reaching
        // one of those reaches the live bytes, exactly as before. Keying this static_assert on the
        // weaker claim would fail the build for stating a true thing. The generated field is named
        // `relocated` for the same reason (mh_regions.gen.h).
        if (r->relocated && !r->l1) return false;
        // ...and a serializer only rescues a WHOLE-region window. mh::state::owner_serves requires
        // offset==0 and len==size, so a SHIFTED block on a relocated region (player_data's starts
        // +16 into the array and runs +16 past its end) would fall back to the raw address -- which
        // is the memory the region left. Two measured shifted blocks exist today; both are on
        // regions that have NOT left, which is why this is armed rather than firing.
#ifdef MH_LIBMH_BUILD
        // The same "a serializer only rescues a WHOLE-region window" clause, spelled against the
        // resolved OFFSET rather than against r->base -- which is 0 standalone and would make
        // `addr != r->base` true for every block. offset==0 is exactly what `addr == r->base` means.
        if (r->relocated &&
            (table::BLOCK_OWNERS[oi].off != 0 || steps[i].size != r->size)) return false;
#else
        if (r->relocated && (steps[i].addr != r->base || steps[i].size != r->size)) return false;
#endif
    }
    return true;
}
} // namespace
static_assert(all_blocks_registered(table::CONTAINER, table::CONTAINER_STEPS),
              "a game::SaveGame block names an address no state region covers, or one that has "
              "LEFT the binary with no module serving it (ST3) -- run "
              "tools/gen_state_registry.py --check, which names the region and the manifest");
static_assert(all_blocks_registered(table::PLANET, table::PLANET_STEPS),
              "a SavePlanetToDisk block names an address no state region covers, or one that has "
              "LEFT the binary with no module serving it (ST3) -- run "
              "tools/gen_state_registry.py --check, which names the region and the manifest");

namespace {

// The plain (non-block) file calls, with the block layer's return convention on top.
//
// THE TWO DIRECTIONS DO NOT RETURN THE SAME UNIT, and getting that backwards cost a build earlier in
// this subsystem: `write` mirrors fwrite(data, size, 1, fh) and returns the ITEM count, i.e. 1, while
// `read` mirrors fread(data, 1, size, fh) and returns the BYTE count.
//
// THAT ASYMMETRY IS OURS, NOT THE GAME'S -- worth stating since the comment above reads as if it were
// describing the original. The game calls BOTH with count=1 (`MOV EBX,0x1` at 0x00448708/0x00448752),
// so the original's read returns 1 as well; it simply never looks (the dead store at 0x00448764). The
// live binding in save_live.cpp deliberately passes (elem_size=1, count=n) instead, which reads the
// same bytes and makes a short read visible to `strict_reads`. Measured at SV1-P-LOAD L1, when
// read_from_file finally got a committed return type.
bool write_plain(const block_io &io, const void *p, uint32_t n) {
    if (n == 0) return true;
    return io.write(io.ctx, p, n) == 1;
}

bool read_exact(const block_io &io, void *p, uint32_t n) {
    if (n == 0) return true;
    return io.read(io.ctx, p, n) == static_cast<int>(n);
}

// state_io, with the "not mapped is a hard failure" rule from the header applied once.
uint8_t *at(const state_io &st, uint32_t addr, uint32_t size) {
    return static_cast<uint8_t *>(st.resolve(st.ctx, addr, size));
}

bool read_u32_state(const state_io &st, uint32_t addr, uint32_t *out) {
    const uint8_t *p = at(st, addr, 4);
    if (p == nullptr) return false;
    std::memcpy(out, p, 4);
    return true;
}

bool read_u16_state(const state_io &st, uint32_t addr, uint16_t *out) {
    const uint8_t *p = at(st, addr, 2);
    if (p == nullptr) return false;
    std::memcpy(out, p, 2);
    return true;
}

bool applies(const table::step &s, int32_t version) {
    const int32_t v = version < 0 ? 0 : version;
    return v >= static_cast<int32_t>(s.ver_min) && v <= static_cast<int32_t>(s.ver_max);
}

} // namespace

// ---------------------------------------------------------------------------------------------
// THE EMBEDDED-MEMBER PREDICATE, byte for byte the test at 0x0044741a (save) and 0x004479c2 (load):
//
//     Planets[i].system_index == CurrentSystem && (PlanetStatus[i] != 0 || PlanetIndex == i)
//
// Its three inputs all arrive in blocks read EARLIER in the same file (Planets, and the
// SESSION_MODE block, which spans CurrentSystem, PlanetStatus and PlanetIndex), which is the whole
// reason the member count can be implicit. Anything that changes this predicate changes the file
// layout, so it is deliberately one function called from both directions rather than two loops --
// and, since SV1-P-CONTAINER's read oracle, from the oracle that counts the members too.
bool member_included(const state_io &st, int32_t i) {
    uint32_t       sys = 0, cur = 0, status = 0, idx = 0;
    const uint32_t sys_addr =
        table::PLANETS_BASE + static_cast<uint32_t>(i) * table::PLANET_STRIDE + table::PLANET_SYSTEM_OFF;
    if (!read_u32_state(st, sys_addr, &sys)) return false;
    if (!read_u32_state(st, table::CURRENT_SYSTEM_ADDR, &cur)) return false;
    if (sys != cur) return false;
    if (!read_u32_state(st, table::PLANET_STATUS_ADDR + static_cast<uint32_t>(i) * 4, &status))
        return false;
    if (status != 0) return true;
    if (!read_u32_state(st, table::PLANET_INDEX_ADDR, &idx)) return false;
    return idx == static_cast<uint32_t>(i);
}

namespace {

// ---------------------------------------------------------------------------------------------
// STEP_REGIONS, both directions. Framing only -- the node/record marshalling is the pair of exported
// functions at the bottom of this file.
int regions_read(const driver_env &env, region_image *out) {
    for (int i = 0; i < table::REGIONS_STEPS; ++i) {
        const table::step &s   = table::REGIONS[i];
        uint8_t           *dst = at(env.st, s.addr, s.size);
        if (dst == nullptr) return 1;
        if (read_block(env.io, env.ws, dst, s.size) != 0) return 1;
        if (env.stats != nullptr) ++env.stats->blocks_read;
    }
    // N IS IN THE FILE: it is the fourth header block, G_LAST_MAP_INDEX. That is what makes the region
    // graph the format's one genuinely self-describing part.
    uint32_t n = 0;
    if (!read_u32_state(env.st, table::REGION_COUNT_ADDR, &n)) return 1;
    out->nodes    = n;
    out->records  = static_cast<uint8_t *>(arena_alloc(*env.mem, n * table::REGION_RECORD_BYTES));
    out->grid_ids = static_cast<uint8_t *>(arena_alloc(*env.mem, table::REGION_GRID_IDS_BYTES));
    out->terrain  = static_cast<uint8_t *>(arena_alloc(*env.mem, table::REGION_TERRAIN_BYTES));
    if ((n != 0 && out->records == nullptr) || out->grid_ids == nullptr || out->terrain == nullptr)
        return 1;
    for (uint32_t j = 0; j < n; ++j) {
        if (read_block(env.io, env.ws, out->records + j * table::REGION_RECORD_BYTES,
                       table::REGION_RECORD_BYTES) != 0)
            return 1;
        if (env.stats != nullptr) ++env.stats->blocks_read;
    }
    if (read_block(env.io, env.ws, out->grid_ids, table::REGION_GRID_IDS_BYTES) != 0) return 1;
    if (read_block(env.io, env.ws, out->terrain, table::REGION_TERRAIN_BYTES) != 0) return 1;
    if (env.stats != nullptr) {
        env.stats->blocks_read += 2;
        env.stats->region_nodes = n;
    }
    return 0;
}

int regions_write(const driver_env &env, const region_image &img) {
    for (int i = 0; i < table::REGIONS_STEPS; ++i) {
        const table::step &s   = table::REGIONS[i];
        const uint8_t     *src = at(env.st, s.addr, s.size);
        if (src == nullptr) return 1;
        if (write_block(env.io, env.ws, src, s.size) != 0) return 1;
        if (env.stats != nullptr) ++env.stats->blocks_written;
    }
    // The count written is state's, not the image's -- exactly as in the original, where the header
    // block is G_LAST_MAP_INDEX and the record loop is the linked list's length. The two agreeing is
    // an invariant the original never checks; the test asserts it.
    uint32_t n = 0;
    if (!read_u32_state(env.st, table::REGION_COUNT_ADDR, &n)) return 1;
    if (n != img.nodes) return 1;
    for (uint32_t j = 0; j < n; ++j) {
        if (write_block(env.io, env.ws, img.records + j * table::REGION_RECORD_BYTES,
                        table::REGION_RECORD_BYTES) != 0)
            return 1;
        if (env.stats != nullptr) ++env.stats->blocks_written;
    }
    if (write_block(env.io, env.ws, img.grid_ids, table::REGION_GRID_IDS_BYTES) != 0) return 1;
    if (write_block(env.io, env.ws, img.terrain, table::REGION_TERRAIN_BYTES) != 0) return 1;
    if (env.stats != nullptr) {
        env.stats->blocks_written += 2;
        env.stats->region_nodes = n;
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------
// STEP_PROGRESS. THE SLICE SIZE IS NOT IN THE FILE. It is 3 x a word in .data that no block saves, so
// it comes from the loaded config -- meaning a save is only readable by a build whose config agrees on
// it. There is no way for the driver to validate the value itself; what catches a wrong one is the
// block layer REFUSING a header whose uncompressed size disagrees, which turns a config mismatch into
// a failed load rather than a misparse.
bool progress_slice(const driver_env &env, uint32_t *out) {
    uint16_t w = 0;
    if (!read_u16_state(env.st, table::PROGRESS_SLICE_WORD, &w)) return false;
    *out = 3u * w;
    return true;
}

int progress_read(const driver_env &env, uint32_t slice) {
    for (uint32_t k = 0; k < table::PROGRESS_COUNT; ++k) {
        uint8_t *dst = at(env.st, table::PROGRESS_BASE + k * table::PROGRESS_STRIDE, slice);
        if (dst == nullptr) return 1;
        if (read_block(env.io, env.ws, dst, slice) != 0) return 1;
        if (env.stats != nullptr) ++env.stats->blocks_read;
    }
    return 0;
}

int progress_write(const driver_env &env, uint32_t slice) {
    for (uint32_t k = 0; k < table::PROGRESS_COUNT; ++k) {
        const uint8_t *src = at(env.st, table::PROGRESS_BASE + k * table::PROGRESS_STRIDE, slice);
        if (src == nullptr) return 1;
        if (write_block(env.io, env.ws, src, slice) != 0) return 1;
        if (env.stats != nullptr) ++env.stats->blocks_written;
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------
// STEP_MEMBERS. A plain uint32 length then the planet file's bytes VERBATIM, streamed in
// MEMBER_CHUNK pieces through the staging buffer -- not a block, and not compressed.
// The STREAMING form of the two loops below, used when `env.members` is bound. Kept beside the arena
// form rather than replacing it: the arena form is what `savetest` round-trips a real .sav with, and
// the two must stay comparable.
int members_read_streaming(const driver_env &env, container_image *img) {
    img->member_count = 0;
    for (int32_t i = table::MEMBER_FIRST; i < table::MEMBER_LIMIT; ++i) {
        if (!member_included(env.st, i)) continue;
        // 0x00447a95: the member file is opened BEFORE its length is read, and the original checks the
        // .SAV handle rather than the member's -- reproduced by letting begin_write report failure.
        if (env.members->begin_write(env.members->ctx, i) != 0) return 1;
        uint32_t len = 0;
        if (!read_exact(env.io, &len, 4)) {
            env.members->end(env.members->ctx);
            return 1;
        }
        const int32_t signed_len = static_cast<int32_t>(len);
        uint32_t      left       = signed_len > 0 ? len : 0u;
        member       &m          = img->members[img->member_count];
        m.planet                 = i;
        m.length                 = left;
        m.bytes                  = nullptr; // streamed, never held
        while (left != 0) {
            const uint32_t chunk = left > table::MEMBER_CHUNK ? table::MEMBER_CHUNK : left;
            if (!read_exact(env.io, env.ws.staging, chunk) ||
                env.members->write(env.members->ctx, env.ws.staging, chunk) != 0) {
                env.members->end(env.members->ctx);
                return 1;
            }
            left -= chunk;
        }
        if (env.members->end(env.members->ctx) != 0) return 1;
        if (env.stats != nullptr) env.stats->member_bytes += m.length;
        ++img->member_count;
    }
    if (env.stats != nullptr) env.stats->members = img->member_count;
    return 0;
}

int members_write_streaming(const driver_env &env) {
    int32_t seen = 0;
    for (int32_t i = table::MEMBER_FIRST; i < table::MEMBER_LIMIT; ++i) {
        if (!member_included(env.st, i)) continue;
        // 0x004474ed open / seek END / tell / seek 0 -- the length comes from the FILE, not from any
        // image, which is exactly what the arena form cannot express.
        const int32_t len = env.members->begin_read(env.members->ctx, i);
        if (len < 0) return 1;
        uint32_t ulen = static_cast<uint32_t>(len);
        if (!write_plain(env.io, &ulen, 4)) {
            env.members->end(env.members->ctx);
            return 1;
        }
        uint32_t left = ulen;
        while (left != 0) {
            const uint32_t chunk = left > table::MEMBER_CHUNK ? table::MEMBER_CHUNK : left;
            if (env.members->read(env.members->ctx, env.ws.staging, chunk) < 0 ||
                !write_plain(env.io, env.ws.staging, chunk)) {
                env.members->end(env.members->ctx);
                return 1;
            }
            left -= chunk;
        }
        if (env.members->end(env.members->ctx) != 0) return 1;
        if (env.stats != nullptr) env.stats->member_bytes += ulen;
        ++seen;
    }
    if (env.stats != nullptr) env.stats->members = seen;
    return 0;
}

int members_read(const driver_env &env, container_image *img) {
    if (env.members != nullptr) return members_read_streaming(env, img);
    img->member_count = 0;
    for (int32_t i = table::MEMBER_FIRST; i < table::MEMBER_LIMIT; ++i) {
        if (!member_included(env.st, i)) continue;
        uint32_t len = 0;
        if (!read_exact(env.io, &len, 4)) return 1;
        // `CMP [EBP-0x1c],0x0 / JLE` -- the original treats the length as SIGNED and streams nothing
        // for zero or negative. Reproduced, so a hostile length cannot become a huge unsigned read.
        const int32_t signed_len = static_cast<int32_t>(len);
        member       &m          = img->members[img->member_count];
        m.planet                 = i;
        m.length                 = signed_len > 0 ? len : 0u;
        m.bytes                  = m.length != 0 ? static_cast<uint8_t *>(arena_alloc(*env.mem, m.length)) : nullptr;
        if (m.length != 0 && m.bytes == nullptr) return 1;
        uint32_t left = m.length;
        uint8_t *p    = m.bytes;
        while (left != 0) {
            const uint32_t chunk = left > table::MEMBER_CHUNK ? table::MEMBER_CHUNK : left;
            if (!read_exact(env.io, p, chunk)) return 1;
            p += chunk;
            left -= chunk;
        }
        if (env.stats != nullptr) env.stats->member_bytes += m.length;
        ++img->member_count;
    }
    if (env.stats != nullptr) env.stats->members = img->member_count;
    return 0;
}

int members_write(const driver_env &env, const container_image &img) {
    if (env.members != nullptr) return members_write_streaming(env);
    int32_t seen = 0;
    for (int32_t i = table::MEMBER_FIRST; i < table::MEMBER_LIMIT; ++i) {
        if (!member_included(env.st, i)) continue;
        if (seen >= img.member_count) return 1; // state selects more members than the image carries
        const member &m = img.members[seen];
        if (m.planet != i) return 1; // and in a different order: the predicate disagrees
        if (!write_plain(env.io, &m.length, 4)) return 1;
        uint32_t       left = m.length;
        const uint8_t *p    = m.bytes;
        while (left != 0) {
            const uint32_t chunk = left > table::MEMBER_CHUNK ? table::MEMBER_CHUNK : left;
            if (!write_plain(env.io, p, chunk)) return 1;
            p += chunk;
            left -= chunk;
        }
        if (env.stats != nullptr) env.stats->member_bytes += m.length;
        ++seen;
    }
    if (seen != img.member_count) return 1;
    if (env.stats != nullptr) env.stats->members = seen;
    return 0;
}

// STEP_TAIL: everything after the last member. In the original that is one 0x400 media block that
// nothing ever reads; captured opaquely because its contents are a live mciSendCommandA CD/TOC query
// the driver cannot reproduce, and re-emitted byte for byte on save.
int tail_read(const driver_env &env, container_image *img) {
    uint8_t *start = static_cast<uint8_t *>(arena_alloc(*env.mem, 0));
    uint32_t got   = 0;
    for (;;) {
        uint8_t *p = static_cast<uint8_t *>(arena_alloc(*env.mem, 4096));
        if (p == nullptr) return 1;
        const int n = env.io.read(env.io.ctx, p, 4096);
        if (n < 0) return 1;
        got += static_cast<uint32_t>(n);
        if (n < 4096) {
            // Hand the unused slack back so the arena stays tight and the next allocation is adjacent.
            env.mem->used -= static_cast<uint32_t>(4096 - n);
            break;
        }
    }
    img->tail     = got != 0 ? start : nullptr;
    img->tail_len = got;
    if (env.stats != nullptr) env.stats->tail_bytes = got;
    return 0;
}

} // namespace

bool current_progress_slice(const driver_env &env, uint32_t *out) { return progress_slice(env, out); }

namespace {
const step_delegate *find_delegate(const step_delegate *tbl, int n, uint32_t at) {
    for (int i = 0; i < n; ++i)
        if (tbl[i].at == at) return &tbl[i];
    return nullptr;
}
const step_delegate *delegate_for(const driver_env &env, uint32_t at) {
    return find_delegate(env.delegates, env.delegate_count, at);
}
const step_delegate *read_delegate_for(const driver_env &env, uint32_t at) {
    return find_delegate(env.read_delegates, env.read_delegate_count, at);
}
} // namespace

void *arena_alloc(arena &a, uint32_t bytes) {
    if (a.used + bytes < a.used || a.used + bytes > a.size) return nullptr;
    uint8_t *p = a.base + a.used;
    a.used += bytes;
    return p;
}

// ---------------------------------------------------------------------------------------------
// WHERE THE TWO LEGACY HOOKS FIRE, located in the generated table rather than written down.
//
// save_driver.h says what they are; this is the part that must not rot. Hard-coding `0x004478a4`
// and `0x0044795f` here would put the coupling between the table and the compat paths in a place no
// regeneration checks, so instead each is found by a predicate over the table's SHAPE and the match
// count is asserted at compile time. Reshape the table -- add a gate, renumber a version, drop a
// step -- and the build fails naming the hook, which is the only failure mode worth having: the
// alternative is a compat path that silently stops firing on files nobody in this project owns.
constexpr int count_legacy_message_steps() {
    int n = 0;
    for (int i = 0; i < table::CONTAINER_STEPS; ++i)
        if (table::CONTAINER[i].kind == table::STEP_BLOCK && table::CONTAINER[i].discard &&
            table::CONTAINER[i].ver_max == 3)
            ++n;
    return n;
}
constexpr int find_legacy_message_step() {
    for (int i = 0; i < table::CONTAINER_STEPS; ++i)
        if (table::CONTAINER[i].kind == table::STEP_BLOCK && table::CONTAINER[i].discard &&
            table::CONTAINER[i].ver_max == 3)
            return i;
    return -1;
}
constexpr int count_v5_steps() {
    int n = 0;
    for (int i = 0; i < table::CONTAINER_STEPS; ++i)
        if (table::CONTAINER[i].kind == table::STEP_BLOCK && table::CONTAINER[i].ver_min == 5) ++n;
    return n;
}
constexpr int find_first_v5_step() {
    for (int i = 0; i < table::CONTAINER_STEPS; ++i)
        if (table::CONTAINER[i].kind == table::STEP_BLOCK && table::CONTAINER[i].ver_min == 5) return i;
    return -1;
}

// The `ver < 4` narrow message queue: exactly one discarded step capped at version 3, 30 records of
// 0x78 bytes = the 0xe10 the original reads (0x00447899 MOV EDX,0xe10).
inline constexpr int LEGACY_MESSAGE_STEP = find_legacy_message_step();
static_assert(count_legacy_message_steps() == 1,
              "the ver<4 narrow-MESSAGE_QUEUE step is no longer uniquely identifiable in "
              "table::CONTAINER -- re-derive legacy_hooks::message_queue_ansi_to_wide's trigger");
static_assert(LEGACY_MESSAGE_STEP >= 0, "ver<4 legacy message step vanished from table::CONTAINER");
static_assert(table::CONTAINER[LEGACY_MESSAGE_STEP].size ==
                  LEGACY_MESSAGE_RECORDS * LEGACY_MESSAGE_ANSI_STRIDE,
              "the ver<4 block's size no longer equals 30 x 0x78 -- the conversion loop's bounds and "
              "the block it reads have drifted apart");

// Its DESTINATION is the modern MESSAGE_QUEUE step's own address, taken from the table rather than
// written down again: the two paths are either/or over the same array, so one address, one place.
constexpr int count_wide_message_steps() {
    int n = 0;
    for (int i = 0; i < table::CONTAINER_STEPS; ++i)
        if (table::CONTAINER[i].kind == table::STEP_BLOCK && !table::CONTAINER[i].discard &&
            table::CONTAINER[i].ver_min == 4)
            ++n;
    return n;
}
constexpr int find_wide_message_step() {
    for (int i = 0; i < table::CONTAINER_STEPS; ++i)
        if (table::CONTAINER[i].kind == table::STEP_BLOCK && !table::CONTAINER[i].discard &&
            table::CONTAINER[i].ver_min == 4)
            return i;
    return -1;
}
inline constexpr int WIDE_MESSAGE_STEP = find_wide_message_step();
static_assert(count_wide_message_steps() == 1,
              "the ver>=4 MESSAGE_QUEUE step is no longer uniquely identifiable -- the ver<4 "
              "conversion has lost its destination");
static_assert(table::CONTAINER[WIDE_MESSAGE_STEP].size ==
                  LEGACY_MESSAGE_RECORDS * LEGACY_MESSAGE_WIDE_STRIDE,
              "MESSAGE_QUEUE is no longer 30 x 0xf0 -- the conversion would write past it");

// The `ver < 5` pair: two steps gated at version 5, and the reset fires when the FIRST of them is
// skipped (0x00447950 JL 0x0044797e, i.e. before either read).
inline constexpr int LEGACY_V5_FIRST_STEP = find_first_v5_step();
static_assert(count_v5_steps() == 2,
              "table::CONTAINER no longer has exactly the two ver>=5 steps (INVASION_ALERT_TIME, "
              "ADVISOR_NEXT_TIME) -- re-derive legacy_hooks::invasion_alert_reset's trigger");
static_assert(LEGACY_V5_FIRST_STEP >= 0, "ver<5 gate vanished from table::CONTAINER");

// ---------------------------------------------------------------------------------------------

// ---------------------------------------------------------------------------------------------
// ST4: a block whose bytes a MODULE owns is asked of the module, not read from the address.
//
// EXACT COVER ONLY. A block that starts inside a region or runs past it (the save's
// `write(&array[0].some_field, sizeof(array))` idiom displaces several of them) is not a module's
// canonical stream and stays on the raw path -- see mh::state::owner_serves.
//
// WHY IT BUFFERS. write_block compresses the whole block, so it needs it contiguous; there is no
// streaming form to hand the sink. The buffer is file-scope rather than arena-allocated because the
// live container path does not guarantee `env.mem`, and a save is single-threaded and non-reentrant
// here. A claimed block larger than this is a HARD FAILURE, not a quiet fall-back to the raw path:
// falling back would produce a byte-identical file while the delegation silently did not happen,
// which is a gate that cannot go red.
namespace {
uint8_t g_owned_scratch[32768]; // >= the largest claimed block today (the order queue, 20400)

// The claimed region a block covers EXACTLY, or RID_COUNT.
mh::state::region_id owned_block(uint32_t addr, uint32_t size) {
#ifdef MH_LIBMH_BUILD
    // LIB-REF-SPLIT: the generated (rid, off) for this block, in place of covering() + (addr - base).
    // The offset is the same number by construction -- the generator computes it as `cur - base` in
    // decompose(), which is what this line subtracted -- so owner_serves() is asked exactly the
    // question it was asked before.
    const int oi = table::block_owner_index(addr, size);
    if (oi < 0 || table::BLOCK_OWNERS[oi].rid == (uint16_t)mh::state::RID_COUNT)
        return mh::state::RID_COUNT;
    const auto rid = static_cast<mh::state::region_id>(table::BLOCK_OWNERS[oi].rid);
    return mh::state::owner_serves(rid, table::BLOCK_OWNERS[oi].off, size) ? rid
                                                                           : mh::state::RID_COUNT;
#else
    const mh::state::region *r = mh::state::covering(addr, size);
    if (r == nullptr) return mh::state::RID_COUNT;
    const auto rid = static_cast<mh::state::region_id>(r - &mh::state::REGIONS[0]);
    return mh::state::owner_serves(rid, addr - r->base, size) ? rid : mh::state::RID_COUNT;
#endif
}

struct buf_sink final : mh::state::state_sink {
    buf_sink(uint8_t *p, uint32_t cap)
        : mh::state::state_sink(mh::state::sink_mode::PERSIST), p(p), cap(cap) {}
    uint8_t *p;
    uint32_t cap, n = 0;
    bool     over = false;
    void     raw(const void *src, uint32_t k) override {
        if (n + k > cap) {
            over = true;
            return;
        }
        const uint8_t *b = static_cast<const uint8_t *>(src);
        for (uint32_t i = 0; i < k; ++i) p[n + i] = b[i];
        n += k;
    }
};

struct buf_source final : mh::state::state_source {
    buf_source(const uint8_t *p, uint32_t cap) : p(p), cap(cap) {}
    const uint8_t *p;
    uint32_t       cap, n = 0;
    bool           over = false;
    void           raw(void *dst, uint32_t k) override {
        if (n + k > cap) {
            over = true;
            return;
        }
        uint8_t *b = static_cast<uint8_t *>(dst);
        for (uint32_t i = 0; i < k; ++i) b[i] = p[n + i];
        n += k;
    }
};

// Tri-state so the four walks stay two lines each: NOT_OWNED = the caller does the raw path,
// 0 = handled, 1 = handled and failed. Applied to ALL FOUR walks (both containers, both planets)
// rather than only the planet pair that has a claimed region today -- uniformity is what stops a
// future claim on a container block from silently bypassing its owner.
constexpr int NOT_OWNED = -1;

int write_owned_block(const driver_env &env, uint32_t addr, uint32_t size) {
    if (!env.owners_serve) return NOT_OWNED;
    const mh::state::region_id rid = owned_block(addr, size);
    if (rid == mh::state::RID_COUNT) return NOT_OWNED;
    if (size > sizeof(g_owned_scratch)) return 1;
    buf_sink sink(g_owned_scratch, size);
    mh::state::owner_of(rid)->emit(rid, sink);
    // The owner's stream must be EXACTLY the block's size. Short or long means the module and the
    // format disagree about what this region is, which must fail loudly rather than write a
    // truncated block.
    if (sink.over || sink.n != size) return 1;
    if (write_block(env.io, env.ws, g_owned_scratch, size) != 0) return 1;
    if (env.stats != nullptr) ++env.stats->blocks_written;
    return 0;
}

int read_owned_block(const driver_env &env, uint32_t addr, uint32_t size) {
    if (!env.owners_serve) return NOT_OWNED;
    const mh::state::region_id rid = owned_block(addr, size);
    if (rid == mh::state::RID_COUNT) return NOT_OWNED;
    if (size > sizeof(g_owned_scratch)) return 1;
    if (read_block(env.io, env.ws, g_owned_scratch, size) != 0) return 1;
    buf_source src(g_owned_scratch, size);
    mh::state::owner_of(rid)->load(rid, src);
    if (src.over || src.n != size) return 1;
    if (env.stats != nullptr) ++env.stats->blocks_read;
    return 0;
}

// ---- SB-HOSTFREE: the SLICED blocks ----------------------------------------------------------
//
// Four save blocks run past the symbol they start at and into OTHER live regions. In the in-process
// configuration one `translate()` covers the window and everything is fine; under a relocating host
// it is silently wrong, because `covering()` resolves against `reach` -- widened for exactly these
// blocks (D6.5) -- and hands back one pointer to the region the block STARTED in, so the trailing
// regions are read from the copies they abandoned. Nothing fails. The save is just wrong, in bytes
// only the save format reads, which is why no determinism run can see it (dead-ends G135).
//
// So those four are gathered / scattered run by run, each run through its OWN region's live base. A
// gap run -- bytes no region describes -- stays at its stock address, which is correct because
// nothing relocates memory that no region claims.
//
// SAME TRI-STATE AS THE OWNED PATH ABOVE, and applied at all four walks for the same reason: a block
// that silently bypassed its decomposition would be indistinguishable from one that never needed it.
alignas(16) uint8_t g_slice_scratch[table::MAX_SLICED_BLOCK];

const table::sliced_block *sliced_block(uint32_t addr, uint32_t size) {
    for (int i = 0; i < table::SLICED_BLOCK_COUNT; ++i)
        if (table::SLICED_BLOCKS[i].addr == addr && table::SLICED_BLOCKS[i].size == size)
            return &table::SLICED_BLOCKS[i];
    return nullptr;
}

// Where one run's bytes ARE, through the INJECTED resolver rather than through live_base() -- the
// driver never touches the registry directly, and a fixture that maps state into a slab must keep
// working. A region run asks by rid (see state_io::resolve_region for why address resolution cannot
// answer it); a gap run, and any resolver that declines to answer by region, falls back to the run's
// stock address, which is right because nothing relocates memory no region claims.
uint8_t *slice_at(const driver_env &env, const table::block_slice &s) {
    if (s.rid != table::SLICE_GAP && env.st.resolve_region != nullptr)
        return static_cast<uint8_t *>(env.st.resolve_region(env.st.ctx, s.rid, s.off, s.len));
    return at(env.st, s.stock, s.len);
}

int write_sliced_block(const driver_env &env, uint32_t addr, uint32_t size) {
    const table::sliced_block *b = sliced_block(addr, size);
    if (b == nullptr) return NOT_OWNED;
    if (size > sizeof(g_slice_scratch)) return 1;
    uint32_t n = 0;
    for (int i = 0; i < b->count; ++i) {
        const table::block_slice &s = table::BLOCK_SLICES[b->first + i];
        if (n + s.len > size) return 1; // the runs must tile the block exactly
        const uint8_t *src = slice_at(env, s);
        if (src == nullptr) return 1;
        std::memcpy(g_slice_scratch + n, src, s.len);
        n += s.len;
    }
    if (n != size) return 1;
    if (write_block(env.io, env.ws, g_slice_scratch, size) != 0) return 1;
    if (env.stats != nullptr) ++env.stats->blocks_written;
    return 0;
}

int read_sliced_block(const driver_env &env, uint32_t addr, uint32_t size) {
    const table::sliced_block *b = sliced_block(addr, size);
    if (b == nullptr) return NOT_OWNED;
    if (size > sizeof(g_slice_scratch)) return 1;
    if (read_block(env.io, env.ws, g_slice_scratch, size) != 0) return 1;
    uint32_t n = 0;
    for (int i = 0; i < b->count; ++i) {
        const table::block_slice &s = table::BLOCK_SLICES[b->first + i];
        if (n + s.len > size) return 1;
        uint8_t *dst = slice_at(env, s);
        if (dst == nullptr) return 1;
        std::memcpy(dst, g_slice_scratch + n, s.len);
        n += s.len;
    }
    if (n != size) return 1;
    if (env.stats != nullptr) ++env.stats->blocks_read;
    return 0;
}
} // namespace

int load_container(const driver_env &env, container_image *img) {
    std::memset(img, 0, sizeof *img);
    if (!read_exact(env.io, img->header, VERSION_STRING_LEN)) return 1;
    img->version = detect_version(env.vt, img->header);
    if (env.stats != nullptr) env.stats->version = img->version;
    // The load-bearing refusal: a header the table does not contain closes the file and returns 0.
    if (img->version == VERSION_REJECT) return 1;

    for (int i = 0; i < table::CONTAINER_STEPS; ++i) {
        const table::step &s = table::CONTAINER[i];
        switch (s.kind) {
            case table::STEP_BLOCK: {
                if (!applies(s, img->version)) {
                    // THE ver<5 GATE IS A SKIP *AND* A RESET (0x00447950 JL 0x0044797e). Skipping the
                    // two blocks without calling llm_strat_invasion_alert_reset_all would leave the
                    // an earlier pass's invasion alerts and advisor timer standing -- state the file
                    // being loaded has no way to overwrite, precisely because it predates them.
                    if (i == LEGACY_V5_FIRST_STEP) {
                        if (env.legacy == nullptr || env.legacy->invasion_alert_reset == nullptr) return 1;
                        env.legacy->invasion_alert_reset(env.legacy->ctx);
                        if (env.stats != nullptr) ++env.stats->legacy_steps_selected;
                    }
                    continue;
                }
                if (s.discard && env.stats != nullptr) ++env.stats->legacy_steps_selected;
                uint8_t *dst;
                if (s.discard) {
                    // A legacy block: read into scratch and drop it. It must still be CONSUMED, or every
                    // following block is off by one.
                    dst = static_cast<uint8_t *>(arena_alloc(*env.mem, s.size));
                    if (dst == nullptr) return 1;
                } else {
                    // ST4: a claimed region answers for itself. Only in the non-member arm -- a
                    // member block's destination is the arena, not a state region, so there is
                    // nothing for an owner to be the owner OF.
                    if (const int r = read_owned_block(env, s.addr, s.size); r != NOT_OWNED) {
                        if (r != 0) return 1;
                        if (s.discard && env.stats != nullptr) ++env.stats->blocks_discarded;
                        break;
                    }
                    // SB-HOSTFREE: and a block that spans regions is scattered run by run, each
                    // through its own region's live base. Same NOT_OWNED fall-through.
                    if (const int r = read_sliced_block(env, s.addr, s.size); r != NOT_OWNED) {
                        if (r != 0) return 1;
                        if (s.discard && env.stats != nullptr) ++env.stats->blocks_discarded;
                        break;
                    }
                    dst = at(env.st, s.addr, s.size);
                }
                if (dst == nullptr) return 1;
                if (read_block(env.io, env.ws, dst, s.size) != 0) return 1;
                if (env.stats != nullptr) {
                    ++env.stats->blocks_read;
                    if (s.discard) ++env.stats->blocks_discarded;
                }
                // ...EXCEPT THIS ONE, WHICH IS NOT DISCARDED AT ALL on the ver<4 path: it is a narrow
                // message queue, and the original converts all 30 records into the wide MESSAGE_QUEUE
                // before moving on (0x004478b6..0x004478f4). Fires while `dst` is still live, i.e.
                // before the scratch goes back to the arena on the next line.
                if (i == LEGACY_MESSAGE_STEP) {
                    if (env.legacy == nullptr || env.legacy->message_queue_ansi_to_wide == nullptr) return 1;
                    const table::step &w    = table::CONTAINER[WIDE_MESSAGE_STEP];
                    uint8_t           *wide = at(env.st, w.addr, w.size);
                    if (wide == nullptr) return 1;
                    env.legacy->message_queue_ansi_to_wide(env.legacy->ctx, dst, wide);
                }
                if (s.discard) env.mem->used -= s.size;
                break;
            }
            case table::STEP_MEMBERS:
                if (members_read(env, img) != 0) return 1;
                break;
            case table::STEP_TAIL:
                // THE LIVE PATH DOES NOT CAPTURE THE TRAILER, and skipping it is faithful rather than
                // lazy: llm_game_load stops at its last wanted member and never reads another byte
                // (which is exactly why appending capacity at EOF is safe -- see save_ext.h). There is
                // also no arena in the live env to put it in.
                if (env.members != nullptr) break;
                if (tail_read(env, img) != 0) return 1;
                break;
            case table::STEP_MEDIA:
                break; // write side only
            default:
                return 1; // a kind this driver does not implement must not be silently skipped
        }
    }
    return 0;
}

int save_container(const driver_env &env, const container_image &img) {
    if (!write_plain(env.io, img.header, VERSION_STRING_LEN)) return 1;
    if (env.stats != nullptr) env.stats->version = img.version;
    for (int i = 0; i < table::CONTAINER_STEPS; ++i) {
        const table::step &s = table::CONTAINER[i];
        switch (s.kind) {
            case table::STEP_BLOCK: {
                // A version-gated legacy block is never WRITTEN: the writer always stamps the current
                // version, so the write side sees only the modern set.
                if (s.discard || !applies(s, img.version)) continue;
                // ST4: a claimed region answers for itself.
                if (const int r = write_owned_block(env, s.addr, s.size); r != NOT_OWNED) {
                    if (r != 0) return 1;
                    break;
                }
                // SB-HOSTFREE: a block that spans regions is served run by run, each
                // through its own region's live base. NOT_OWNED here means the ordinary
                // single-region path below, exactly as it does for the owner check.
                if (const int r = write_sliced_block(env, s.addr, s.size); r != NOT_OWNED) {
                    if (r != 0) return 1;
                    break;
                }
                const uint8_t *src = at(env.st, s.addr, s.size);
                if (src == nullptr) return 1;
                if (write_block(env.io, env.ws, src, s.size) != 0) return 1;
                if (env.stats != nullptr) ++env.stats->blocks_written;
                break;
            }
            case table::STEP_MEMBERS:
                if (members_write(env, img) != 0) return 1;
                break;
            case table::STEP_MEDIA:
                // LIVE: the trailer is a fresh 0x400-byte report written as an ordinary BLOCK
                // (0x004475b8 llm_build_media_diag_report -> 0x004475bd the block writer, whose result
                // the original does NOT accumulate). ROUND-TRIP: re-emitted verbatim, header included,
                // because it is already a well-formed block and its contents are a live CD/TOC query.
                if (env.media_source != nullptr) {
                    const void *buf = env.media_source(env.delegate_ctx);
                    if (buf == nullptr) return 1;
                    write_block(env.io, env.ws, buf, table::MEDIA_BLOCK_BYTES);
                    if (env.stats != nullptr) env.stats->tail_bytes = table::MEDIA_BLOCK_BYTES;
                    break;
                }
                if (img.tail_len != 0 && !write_plain(env.io, img.tail, img.tail_len)) return 1;
                if (env.stats != nullptr) env.stats->tail_bytes = img.tail_len;
                break;
            case table::STEP_TAIL:
                break; // read side only
            default:
                return 1;
        }
    }
    return 0;
}

int load_planet(const driver_env &env, planet_image *img) {
    std::memset(img, 0, sizeof *img);
    uint32_t slice = 0;
    if (!progress_slice(env, &slice)) return 1;
    img->progress_slice = slice;
    if (env.stats != nullptr) env.stats->progress_slice = slice;

    for (int i = 0; i < table::PLANET_STEPS; ++i) {
        const table::step &s = table::PLANET[i];
        switch (s.kind) {
            case table::STEP_BLOCK: {
                // Same shape as save_planet's, with ONE addition the write side has no use for:
                // `covers == 0` means the delegate emits no blocks at all -- run it, then read this
                // step normally. That is llm_strat_planet_map_session_init, which the original calls
                // at 0x004485cd between the progress loop and the next block.
                const step_delegate *d = s.read_at != 0 ? read_delegate_for(env, s.read_at) : nullptr;
                if (d != nullptr) {
                    if (d->fn(env.delegate_ctx) != 0) return 1;
                    if (d->covers > 0) {
                        i += d->covers - 1;
                        break;
                    }
                }
                // ST4: a claimed region answers for itself.
                if (const int r = read_owned_block(env, s.addr, s.size); r != NOT_OWNED) {
                    if (r != 0) return 1;
                    break;
                }
                // SB-HOSTFREE: a block that spans regions is served run by run, each
                // through its own region's live base. NOT_OWNED here means the ordinary
                // single-region path below, exactly as it does for the owner check.
                if (const int r = read_sliced_block(env, s.addr, s.size); r != NOT_OWNED) {
                    if (r != 0) return 1;
                    break;
                }
                uint8_t *dst = at(env.st, s.addr, s.size);
                if (dst == nullptr) return 1;
                if (read_block(env.io, env.ws, dst, s.size) != 0) return 1;
                if (env.stats != nullptr) ++env.stats->blocks_read;
                break;
            }
            case table::STEP_REGIONS:
                if (const step_delegate *d = read_delegate_for(env, 0)) {
                    if (d->fn(env.delegate_ctx) != 0) return 1;
                } else if (regions_read(env, &img->regions) != 0) {
                    return 1;
                }
                break;
            case table::STEP_PROGRESS:
                if (progress_read(env, slice) != 0) return 1;
                break;
            default:
                return 1;
        }
    }
    return 0;
}

int save_planet(const driver_env &env, const planet_image &img) {
    uint32_t slice = 0;
    if (!progress_slice(env, &slice)) return 1;
    // A slice that changed between load and save would silently re-frame eight blocks.
    if (slice != img.progress_slice) return 1;
    if (env.stats != nullptr) env.stats->progress_slice = slice;

    for (int i = 0; i < table::PLANET_STEPS; ++i) {
        const table::step &s = table::PLANET[i];
        switch (s.kind) {
            case table::STEP_BLOCK: {
                // A delegated RUN: one original call emits `covers` steps, so skip past them all.
                // Delegated steps are NOT counted into blocks_written -- the original reports no
                // per-block result, so a count here would be a guess dressed as an observable.
                // The `write_at != 0` guard is load-bearing: 0 is STEP_REGIONS' key, and a block step
                // that somehow carried no call address would otherwise claim the region delegate.
                const step_delegate *d = s.write_at != 0 ? delegate_for(env, s.write_at) : nullptr;
                if (d != nullptr) {
                    if (d->fn(env.delegate_ctx) != 0) return 1;
                    i += d->covers - 1;
                    break;
                }
                // ST4: a claimed region answers for itself.
                if (const int r = write_owned_block(env, s.addr, s.size); r != NOT_OWNED) {
                    if (r != 0) return 1;
                    break;
                }
                // SB-HOSTFREE: a block that spans regions is served run by run, each
                // through its own region's live base. NOT_OWNED here means the ordinary
                // single-region path below, exactly as it does for the owner check.
                if (const int r = write_sliced_block(env, s.addr, s.size); r != NOT_OWNED) {
                    if (r != 0) return 1;
                    break;
                }
                const uint8_t *src = at(env.st, s.addr, s.size);
                if (src == nullptr) return 1;
                if (write_block(env.io, env.ws, src, s.size) != 0) return 1;
                if (env.stats != nullptr) ++env.stats->blocks_written;
                break;
            }
            case table::STEP_REGIONS:
                if (const step_delegate *d = delegate_for(env, 0)) {
                    if (d->fn(env.delegate_ctx) != 0) return 1;
                } else if (regions_write(env, img.regions) != 0) {
                    return 1;
                }
                break;
            case table::STEP_PROGRESS:
                if (progress_write(env, slice) != 0) return 1;
                break;
            default:
                return 1;
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------
// The region graph's marshalling. See the header for the list-reversal finding.

namespace {
// The node's `next` and its neighbour array hold real POINTERS in a 0x420-byte record whose field
// offsets are the original's. That layout is only reproducible in a 32-bit build -- which this is,
// and must stay, since the DLL injects into a 32-bit process.
static_assert(sizeof(void *) == 4, "the region node layout stores 32-bit pointers");

constexpr uint32_t REC_ID        = 0x00;
constexpr uint32_t REC_B2        = 0x02;
constexpr uint32_t REC_B3        = 0x03;
constexpr uint32_t REC_D4        = 0x04;
constexpr uint32_t REC_CNT       = table::REGION_NEIGHBOUR_COUNT_OFF;
constexpr uint32_t REC_PTRS      = table::REGION_NEIGHBOUR_PTRS_OFF;
constexpr uint32_t REC_DATA      = table::REGION_NEIGHBOUR_DATA_OFF;
constexpr uint32_t NEIGHBOUR_MAX = (REC_DATA - REC_PTRS) / 4; // 128 slots either side

uint16_t rd16(const uint8_t *p, uint32_t off) {
    uint16_t v;
    std::memcpy(&v, p + off, 2);
    return v;
}
uint32_t rd32(const uint8_t *p, uint32_t off) {
    uint32_t v;
    std::memcpy(&v, p + off, 4);
    return v;
}
void wr16(uint8_t *p, uint32_t off, uint16_t v) { std::memcpy(p + off, &v, 2); }
void wr32(uint8_t *p, uint32_t off, uint32_t v) { std::memcpy(p + off, &v, 4); }
} // namespace

int unmarshal_region_records(const uint8_t *records, uint32_t count, uint8_t *nodes,
                             uint8_t **by_index, uint32_t by_index_len, int32_t *list_head_out) {
    int32_t  head = -1;
    uint8_t *prev = nullptr;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *rec  = records + i * table::REGION_RECORD_BYTES;
        uint8_t       *node = nodes + i * table::REGION_NODE_BYTES;
        // The original mallocs 0x420 and assigns only the fields below, so the rest is heap garbage.
        // Zeroing instead is a deliberate, stated departure: it makes the image deterministic, and
        // every byte it covers is one the marshaller never reads back.
        std::memset(node, 0, table::REGION_NODE_BYTES);

        const uint16_t id = rd16(rec, REC_ID);
        if (id >= by_index_len) return 1;
        by_index[id] = node;

        // PUSH ONTO THE HEAD -- this is the reversal. `node->next = head; head = node`.
        wr32(node, table::REGION_NODE_NEXT_OFF, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(prev)));
        prev = node;
        head = static_cast<int32_t>(i);

        wr16(node, REC_ID, id);
        node[REC_B2] = rec[REC_B2];
        node[REC_B3] = rec[REC_B3];
        wr32(node, REC_D4, rd32(rec, REC_D4));
        const uint32_t cnt = rd32(rec, REC_CNT);
        if (cnt > NEIGHBOUR_MAX) return 1;
        wr32(node, REC_CNT, cnt);
        for (uint32_t j = 0; j < cnt; ++j) wr32(node, REC_DATA + j * 4, rd32(rec, REC_DATA + j * 4));
    }
    // Pass two, exactly as the original splits it: the neighbour POINTERS can only be resolved once
    // every node exists, so id -> REGION_BY_INDEX[id] is a second walk over the same records.
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *rec  = records + i * table::REGION_RECORD_BYTES;
        uint8_t       *node = by_index[rd16(rec, REC_ID)];
        if (node == nullptr) return 1;
        const uint32_t cnt = rd32(rec, REC_CNT);
        for (uint32_t j = 0; j < cnt; ++j) {
            const uint32_t nid = rd32(rec, REC_PTRS + j * 4);
            if (nid >= by_index_len) return 1;
            wr32(node, REC_PTRS + j * 4,
                 static_cast<uint32_t>(reinterpret_cast<uintptr_t>(by_index[nid])));
        }
    }
    if (list_head_out != nullptr) *list_head_out = head;
    return 0;
}

int marshal_region_records(const uint8_t *nodes, int32_t list_head, uint8_t *records, uint32_t count,
                           uint32_t *walked_out) {
    const uint8_t *node = list_head < 0 ? nullptr : nodes + list_head * table::REGION_NODE_BYTES;
    uint32_t       w    = 0;
    for (; node != nullptr && w < count; ++w) {
        uint8_t *rec = records + w * table::REGION_RECORD_BYTES;
        // THE ORIGINAL REUSES ONE 0x40c STACK BUFFER for every node and refills only the fields
        // below, so any neighbour slot beyond this node's count still holds the PREVIOUS node's value
        // (and, for the first node, uninitialised stack). Carrying the previous record forward
        // reproduces that exactly; see docs/save-format.md's latent defects.
        if (w != 0) std::memcpy(rec, rec - table::REGION_RECORD_BYTES, table::REGION_RECORD_BYTES);

        wr16(rec, REC_ID, rd16(node, REC_ID));
        rec[REC_B2] = node[REC_B2];
        rec[REC_B3] = node[REC_B3];
        wr32(rec, REC_D4, rd32(node, REC_D4));
        const uint32_t cnt = rd32(node, REC_CNT);
        if (cnt > NEIGHBOUR_MAX) return 1;
        wr32(rec, REC_CNT, cnt);
        for (uint32_t j = 0; j < cnt; ++j) {
            // The record stores the neighbour's ID, obtained by dereferencing the node's pointer --
            // `MOVZX EDX,word ptr [EAX]` at 0x004248cd.
            const uint8_t *nb =
                reinterpret_cast<const uint8_t *>(static_cast<uintptr_t>(rd32(node, REC_PTRS + j * 4)));
            if (nb == nullptr) return 1;
            wr32(rec, REC_PTRS + j * 4, rd16(nb, REC_ID));
            wr32(rec, REC_DATA + j * 4, rd32(node, REC_DATA + j * 4));
        }
        node = reinterpret_cast<const uint8_t *>(
            static_cast<uintptr_t>(rd32(node, table::REGION_NODE_NEXT_OFF)));
    }
    if (walked_out != nullptr) *walked_out = w;
    // MORE NODES IN THE LIST THAN N. The original writes them all and the loader reads only N, so the
    // file silently desynchronises from that point on. Ours refuses instead.
    return node == nullptr ? 0 : 1;
}

} // namespace mh::save
