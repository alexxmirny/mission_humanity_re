//
// caps/ovf.h -- the OVERFLOW of the raised roster arrays: the records past the vanilla window
// (patching:PT-CAP8 S2, slice E; spec tmp/capraise/s23/spec.md sections 1 and 3).
//
// WHAT "OVERFLOW" IS. With the cap raise ON, the game addresses wide arrays (caps.h): buildings
// [p][0..500), unit_storage / turrets / mines / labs / productions [p][0..127), the projectile pool
// [0..4000). The lockstep hash, the scene dump and the world resync only ever read the STOCK mirror (the
// vanilla window, canon_gather/scatter), so a record past the window is invisible to every one of them.
// This module is the pure half of making it visible; desync_watch.cpp, scene_dump.cpp and world_sync.cpp
// (slice F) and the save hook (slice G) are its callers.
//
// OVERFLOW IS NOT A REGISTRY REGION (spec E1). No manifest entry, no HASH_REGIONS row, no rid, no `len`
// change: with the raise OFF nothing here is allocated, stepped or folded, so every hash value, every
// fingerprint and every recorded fixture is untouched by construction.
//
//   * 11 EXTENSION SLICES (8 + units p0-3, units p4-7, soldiers: PT-CAP9-U1), each a logical stream = the contiguous SEGMENTS (one per player-row tail) of the
//     wide arrays, player-major. A slice is <= 512 KB so the v2 localiser takes it to byte level.
//   * `tracker` keeps an incremental sum-of-block-hashes per slice, exactly the scheme of
//     mh::state::inc::tracker (inc_state.h): a shadow, a memcmp scan, a 64-byte block hash that is a pure
//     function of (region, block, bytes). The region id is HASH_REGION_COUNT + e, so a block hash computed
//     by the localiser over the evidence buffer is the tracker's.
//   * the CODEC ("MHOV") is one sparse format for saves, scene dumps and the world resync.
//
// Everything in this header is host-testable (`net_selftest ovftest`): the slice geometry derives from
// mh::caps::stock() / RAISED_ROW, the memory it reads from caps::g(a) (a test points the table at a
// calloc'd block with adopt_relocation()).
//
#pragma once
#include <cstddef>
#include <cstdint>

#include "caps/caps.h"
#include "state/inc_state.h"

namespace mh::caps::ovf {

// PT-CAP9-U7d: slices 0..10 are the roster arrays' tails (the first MHOV kinds); slices 11.. hash the rest of the raise's state -- the path pool
// (tails past slot 100, every busy flag, the window slots 0..99) and the order QUEUE tail. The PENDING and STAGING tails are NOT hashed: the stock
// manifest excludes both arrays as peer-local in-flight state (hash_region.excluded), and measured on the rig their tails differ between healthy peers
// (tmp/capraise/u7def/det_orders_1.log: staging, rows_orders_1.log: pending -> a false, permanent DESYNC).
inline constexpr int      ROSTER_SLICES   = 11;
inline constexpr int      SLICES          = 21;
inline constexpr uint32_t MAX_SLICE_BYTES = 512u * 1024u; // the v2 localiser's MAX_BLOCKS * 64

// ---- the folds (same FNV-1a-64 as mh::desync::fold_state / fnv1a) -------------------------------------
inline constexpr uint64_t FNV64_OFFSET = 1469598103934665603ULL;
inline constexpr uint64_t FNV64_PRIME  = 1099511628211ULL;
inline constexpr uint32_t FNV32_OFFSET = 2166136261u;
inline constexpr uint32_t FNV32_PRIME  = 16777619u;

inline uint64_t fnv64(const void *p, size_t n, uint64_t h = FNV64_OFFSET) {
    const uint8_t *b = static_cast<const uint8_t *>(p);
    for (size_t i = 0; i < n; ++i) {
        h ^= b[i];
        h *= FNV64_PRIME;
    }
    return h;
}
inline uint32_t fnv32(const void *p, size_t n, uint32_t h = FNV32_OFFSET) {
    const uint8_t *b = static_cast<const uint8_t *>(p);
    for (size_t i = 0; i < n; ++i) {
        h ^= b[i];
        h *= FNV32_PRIME;
    }
    return h;
}

// E2: the ON state hash = the OFF fold continued over the extension sums (little-endian bytes; SLICES of them).
inline uint64_t fold_ext(uint64_t h, const uint64_t *sums) {
    for (int e = 0; e < SLICES; ++e) h = fnv64(&sums[e], sizeof(sums[e]), h);
    return h;
}
// E2: the v2 fingerprint when the extension is armed; an ON and an OFF peer read as a build mismatch.
inline constexpr uint32_t OVF_KIND = 0x564F484Du ^ 1u; // 'MHOV' ^ 1
// PT-CAP9-U7d: the fingerprint also folds the SLICE SET -- the count, every slice's length and which ones are active in this run -- so a peer that
// hashes a different set (an older build, or a layer that was refused) reads as a build mismatch at the handshake instead of a desync.
uint64_t fp_on(uint64_t fp_off);

// ---- the slices -------------------------------------------------------------------------------------
enum slice_src : uint8_t { SRC_ROSTER      = 0,
                           SRC_PATH_TAIL   = 1,
                           SRC_PATH_FLAGS  = 2,
                           SRC_PATH_WINDOW = 3,
                           SRC_ORDERS      = 4 };
struct slice_info {
    const char *name;       // the localiser's region_name
    uint8_t     src;        // slice_src: where the segments live (a roster tail unless said otherwise)
    uint8_t     arr;        // MH_CAPS_IDX_*
    uint8_t     p0, p1;     // player rows [p0, p1)
    uint32_t    rec0, rec1; // records [rec0, rec1) of each row: the vanilla row .. the raised row
    uint32_t    elem;
    uint32_t    seg_len; // (rec1 - rec0) * elem: one player's tail
    uint32_t    len;     // (p1 - p0) * seg_len: the slice's stream length
    // SRC_PATH_*: p0..p1 = players; rec0..rec1 = path slots (TAIL 100..501, WINDOW 0..100) or, for FLAGS, the whole 0..501 row; elem = 600 (flags: 1).
    // SRC_ORDERS: arr = orders::which; p0..p1 = 0..1 (one array); rec0..rec1 = the records of that array this slice covers; elem = 0x44.
};
// Derived from caps::stock() / RAISED_ROW on first use; never retyped.
const slice_info &slice(int e);
// A slice is ACTIVE when its array is relocated (caps::wide). An inactive slice has no memory behind it: its sum is 0, it is never
// scanned, gathered or encoded. (v0.2.2 arrays and the v0.2.3 units/soldiers are relocated by separate manifests and either may be refused.)
bool active(int e);
// Segment k (player p0 + k) of slice e: the LIVE address of its first byte (caps::g(a), the wide array).
const uint8_t *segment(int e, int k);
// Total stream bytes of all slices (the per-step scan) and of the roster slices alone (2,690,052 for the shipped geometry; 1,851,652 of them are the v0.2.2 arrays).
uint32_t total_bytes();
uint32_t roster_bytes();
// "buildings p2 #137 +0x12" for a slice offset (the localiser's describe()).
void describe(int e, uint32_t off, char *out, size_t cap);
// Copy stream bytes [off, off + n) of slice e from the live segments (n may straddle two segments).
void gather(int e, uint32_t off, uint32_t n, uint8_t *out);
// From-scratch sum of slice e over the live memory (the ground truth for verify() and MH_Caps_OvfSums).
uint64_t recompute(int e);
void     recompute_all(uint64_t *out8);
// FNV-1a-64 over every slice's stream in slice order (= the wide tails): the save round-trip digest (slice G).
uint64_t digest();
// The same, with every unit's path slot id (+0xaa, +0xda/+0xdb) zeroed: the id of a slot >= 100 is not persisted by the planet save (U4d), so this is the
// digest a save/load round trip must preserve (the harness CAP SAVE / CAP LOAD lines).
uint64_t digest_persisted();

// ---- the incremental tracker ------------------------------------------------------------------------
// Mirrors inc::tracker minus masks and the order-queue boundary. Allocation-free: attach() takes an arena
// of arena_bytes().
class tracker {
public:
    static size_t arena_bytes();
    void          attach(void *arena);
    // Copy every segment into the stream shadow and hash every block. False (nothing done) when the
    // arrays are not relocated: the segments would not exist.
    bool prime();
    // One step: scan the segments against the shadow; for each changed 64-byte STREAM block hand the OLD
    // shadow bytes to `j` (region HASH_REGION_COUNT + e), copy, rehash. Primes on first use.
    void update(mh::state::inc::journal *j);

    uint64_t       sum(int e) const { return s_[e].sum; }
    void           sums(uint64_t *out8) const;
    const uint8_t *shadow(int e) const { return s_[e].shadow; }
    // The periodic self-check: slices whose incremental sum disagrees with recompute(e); `first_bad`
    // (optional) receives the first one, or -1.
    int  verify(int *first_bad = nullptr) const;
    bool primed() const { return primed_; }
    // Last update(): changed 64-B blocks.
    uint32_t last_changed_blocks = 0;

private:
    struct slot {
        uint8_t  *shadow;
        uint64_t *bh;
        uint32_t  len, nblocks;
        uint64_t  sum;
    };
    void refresh_block(int e, uint32_t b, mh::state::inc::journal *j);
    slot s_[SLICES];
    bool primed_ = false;
};

// The block hash of slice e, block b, over `bytes` (n <= 64): the tracker's, with keep = all 0xff.
inline uint64_t block_hash(int e, uint32_t b, const uint8_t *bytes, uint32_t n) {
    return mh::state::inc::block_hash((uint32_t)mh::state::HASH_REGION_COUNT + (uint32_t)e, b, bytes, n, nullptr);
}

// ---- the MHOV codec -----------------------------------------------------------------------------------
// VERSION 1 (v0.2.2): one layout fingerprint over the seven v0.2.2 arrays; sections 0..8 only (the nine wide arrays).
// VERSION 2 (PT-CAP9-U7a, v0.2.3): no header fingerprint (layout_fp = 0) -- every section carries its OWN geometry and a kind_ver, so a
//   section kind this build does not know can be told apart from a damaged one; the header carries FEATURE bits; a section carries FLAGS.
//   Reader rules: an unknown / newer VERSION is refused; an unknown FEATURE bit is refused; a section of an unknown kind, a newer kind_ver, a
//   geometry this build does not share, or memory this run did not relocate is refused UNLESS it carries SEC_OPTIONAL, in which case it is
//   skipped (framed by nrec * (4 + elem), counted in the header total, applied to nothing).
//   Writer rule: write VERSION 1 whenever every record fits a v1 section (rc builds keep loading such saves); VERSION 2 only when a
//   v2-only kind has records (KIND_PATH_SLOT, KIND_ORDER_*; FEAT_PATH_ID16 only while the path pool is relocated).
// Kinds 0..8 are the wide arrays (MH_CAPS_IDX_*); 9.. are added by v2:
//   9  PATH_SLOT       elem 601 = {busy flag, 300 waypoints x {heading, run}}, slots 100..500 of 8 players (500 is the sink). SAVE only.
//   10 ORDER_QUEUE, 11 ORDER_PENDING, 12 ORDER_STAGING (PT-CAP9-U7c): the three order arrays past their vanilla window (300 / 1000 / 300 records of 0x44
//      bytes, raised to 4096 / 32768 / 2048). ONE array each, so players = 1 and every record's `player` is 0; `idx` is the record index. Unlike the
//      sparse roster kinds a section holds EVERY record in [stock, live count) -- zero ones included -- because the highest index + 1 IS the live count
//      the stock blocks cannot carry (the queue count is saved separately and not mirrored): orders::finish_scatter() restores a count only when the
//      tail came back whole. The queue rides saves and world blobs; PENDING and STAGING are WORLD blobs only (the vanilla planet save has no block for
//      them). A required section: a build whose order layer is not ON refuses it (OVF_ERR_NOT_RELOCATED) rather than run 150 stale orders.
inline constexpr uint32_t MHOV_MAGIC         = 0x564F484Du; // 'MHOV' little-endian
inline constexpr uint16_t MHOV_VERSION_1     = 1;
inline constexpr uint16_t MHOV_VERSION       = 2; // the highest this build reads and writes
inline constexpr unsigned KIND_PATH_SLOT     = 9;
inline constexpr unsigned KIND_ORDER_QUEUE   = 10;
inline constexpr unsigned KIND_ORDER_PENDING = 11;
inline constexpr unsigned KIND_ORDER_STAGING = 12;
inline constexpr unsigned KINDS_KNOWN        = 13;      // kinds 0..12
inline constexpr uint16_t KIND_VER           = 1;       // the only kind_ver any known kind has
inline constexpr uint32_t FEAT_PATH_ID16     = 1u << 0; // the units' 16-bit path ids (+0xda) in this file are valid: trust them (path_pool.h)
inline constexpr uint32_t FEAT_KNOWN         = FEAT_PATH_ID16;
inline constexpr uint16_t SEC_OPTIONAL       = 1u << 0;

#pragma pack(push, 1)
struct ovf_header {
    uint32_t magic;
    uint16_t version;
    uint16_t nsec;      // sections that follow
    uint32_t body_len;  // bytes after the header
    uint32_t layout_fp; // v1: fnv32 over {arr, players, elem, vanilla_row, raised_row} of the 7 v0.2.2 arrays (UNITS/SOLDIERS sections are checked field by field, so a v0.2.2 save still loads). v2: 0
    uint64_t body_sum;  // fnv64 over the body
    uint32_t records;   // total records (cross-check), skipped sections included
    uint32_t features;  // v1: 0 (was `reserved`). v2: FEAT_* bits
};
struct ovf_section {
    uint8_t  arr; // the KIND (the wide array index, or KIND_PATH_SLOT)
    uint8_t  players;
    uint16_t elem;
    uint16_t vanilla_row;
    uint16_t raised_row;
    uint32_t nrec;
    uint16_t flags;    // v1: 0 (was `reserved`). v2: SEC_*
    uint16_t kind_ver; // v1: 0. v2: KIND_VER
};
struct ovf_record { // followed by `elem` bytes
    uint8_t  player;
    uint8_t  pad;
    uint16_t idx;
};
#pragma pack(pop)
static_assert(sizeof(ovf_header) == 32 && sizeof(ovf_section) == 16 && sizeof(ovf_record) == 4, "MHOV is a file format");

enum ovf_err : int {
    OVF_OK                = 0,
    OVF_ERR_ARG           = -1,
    OVF_ERR_MAGIC         = -2,
    OVF_ERR_VERSION       = -3,
    OVF_ERR_TRUNCATED     = -4,
    OVF_ERR_SUM           = -5,
    OVF_ERR_LAYOUT        = -6,
    OVF_ERR_SECTION_ORDER = -7, // sections not strictly increasing by array (duplicate or out of order)
    OVF_ERR_RANGE         = -8, // player / idx outside the array's window
    OVF_ERR_ELEM          = -9,
    OVF_ERR_COUNT         = -10, // a section's nrec / the header's record total disagree with the body
    OVF_ERR_TRAILING      = -11, // bytes left over inside / after body_len
    OVF_ERR_NOT_RELOCATED = -12, // apply(): there are no wide arrays to write
    OVF_ERR_RECORD_ORDER  = -13, // records not sorted (player, idx) / duplicated
    OVF_ERR_TOO_SMALL     = -14, // encode(): the output buffer cannot hold the blob
    OVF_ERR_FEATURE       = -15, // v2: an unknown header feature bit / section flag (a newer writer)
    OVF_ERR_KIND          = -16, // v2: a required section of an unknown kind or a newer kind_ver
};
inline constexpr size_t ENCODE_FAIL = static_cast<size_t>(-1);

uint32_t layout_fp();
// Worst case blob size of a WORLD blob (header + 9 sections + every overflow record present).
size_t max_bytes();
// ... of a SAVE blob: the same plus the path-slot section (8 players x 401 slots x (4 + 601) bytes) and the order QUEUE section.
size_t max_bytes_save();
// ... of a WORLD blob with the order layer ON: max_bytes() plus the three order sections (queue + pending + staging tails).
size_t max_bytes_world();
enum encode_mode : unsigned {
    FOR_WORLD = 0, // scene dump / world resync: the wide arrays + the three order arrays past their windows (=> version 2); the path pool is not part of a world blob until PT-CAP9-U7e
    FOR_SAVE  = 1, // the planet-file trailer: + the path-slot section when the pool has anything past the window, + the order queue past 300 (=> version 2)
};
// Encode every non-zero overflow record. 0 = nothing past the window (the caller appends NOTHING);
// ENCODE_FAIL = `cap` too small or the arrays are not relocated. A record counts when ANY byte is
// non-zero (caps::overflow_records()'s test), so a zero record restores as zero and the round trip is
// exact for the whole wide array.
size_t encode(uint8_t *out, size_t cap, unsigned mode = FOR_WORLD);
// Full validation, writes nothing. Returns OVF_OK or an ovf_err.
int check(const void *blob, size_t n);
// check() first, then write the records into the wide arrays (and the path pool). The caller has already zeroed the tails
// (canon_scatter()). Refused = nothing written. *features (optional) receives the header's FEAT_* bits (0 for a v1 blob).
int apply(const void *blob, size_t n, uint32_t *features = nullptr);
// Records of section kind `kind` in a validated blob (0 when absent), or -1 when the blob is refused.
int section_records(const void *blob, size_t n, unsigned kind);
// Records in a validated blob (header count), or -1.
int record_count(const void *blob, size_t n);
// body_sum of a body (exposed so tests can re-seal a mutated blob).
inline uint64_t body_sum(const void *body, size_t n) { return fnv64(body, n); }

// ---- the world blob trailer (scene dump + world resync) -----------------------------------------------------
// The MHOV trailer sits AFTER the fixup trailer, at nav_offset + nav_len + fixup_len. The world readers only
// ever test `> n` against their own trailers, so extra bytes there change no FORMAT and no fingerprint (the
// same additive rule as mp:X3a). Appended by mh.dll only, never by libmh.
inline constexpr int WORLD_ERR_CAPS_OVF = -40; // a world import refused: the blob carries overflow and [caps] raise is OFF
// A refused trailer's world return code: WORLD_ERR_CAPS_OVF + the (negative) ovf_err, i.e. -41 .. -54.
inline int world_rc_refused(int ovf_rc) { return WORLD_ERR_CAPS_OVF + ovf_rc; }
// The overflow trailer of a world blob of `n` bytes, or null (nav_offset 0 = no trailers at all; nothing
// after the fixup trailer = no overflow). *tlen = its bytes.
inline const uint8_t *world_trailer(const void *blob, size_t n, uint32_t nav_offset, uint32_t nav_len, uint32_t fixup_len, size_t *tlen) {
    const size_t off = static_cast<size_t>(nav_offset) + nav_len + fixup_len;
    if (!blob || nav_offset == 0 || n <= off) return nullptr;
    *tlen = n - off;
    return static_cast<const uint8_t *>(blob) + off;
}

// Append the overflow trailer to a world blob of `*got` bytes in `blob` (capacity `cap`). Only while
// raised(); nothing past the window appends nothing. False = capacity (the caller fails the capture).
bool append_to_world_blob(uint8_t *blob, size_t *got, size_t cap);

} // namespace mh::caps::ovf
