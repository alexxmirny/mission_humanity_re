//
// state/world_snapshot.h -- the STEP-0 WORLD blob: every bound region at the instant before the
// first sim step, captured alongside an order+clock recording (tracker LIB-WORLD).
//
// WHY IT EXISTS. LIB-BOOT gave libmh the cfg cluster's OUTPUT so it need not own the 65 KB parser.
// That is not enough to start a replay: a session also needs a loaded map and a landed roster, and
// map loading is excluded pre-fork too (the endgame plan D-E5). So the fixture LIB-REF replays
// against is a memory image -- dump every bound region at replay step 0, and let the standalone
// host import it instead of parsing anything.
//
// WHAT "STEP 0" IS, precisely, because the whole oracle rests on it. on_sim_step() is the ENTRY
// detour on llm_strat_sim_step, so it runs PRE-BODY: it increments the step counter and then hashes,
// which means the harness line `1 <clock> <combined> <state>` is the state BEFORE the first sim step
// executes. The capture fires inside that same hash block, from the same `combined`/`state`
// variables the line prints, one statement later. The pairing is therefore by CONSTRUCTION rather
// than by argument -- there is no window between the hash and the capture for anything to move, and
// the blob's recorded pair can be checked against the run's own log line.
//
// THREE THINGS THAT ARE LOAD-BEARING AND NOT OBVIOUS:
//
// 1. BLOCKS ARE CARRIED VERBATIM. NO NORMALISATION. LIB-BOOT tags G_TEXT_PTRS' entries into
//    arena-relative offsets so its hash is base-independent; this blob must NOT do the equivalent,
//    and the reason is the oracle clause. The value being reproduced is the RECORDING PEER'S
//    LOCKSTEP HASH, which is a raw fnv1a over live bytes -- so any transformation of a hashed
//    region's bytes moves the number away from the one the peer printed and the comparison stops
//    being the comparison the done_when names. Bytes in, bytes out.
//
// 2. THE BAKED POINTERS ARE NOT IN HERE AND CANNOT BE. tile_objects_ptr @0x005d0ad0 and
//    fow_ptr @0x005202c0 are initialised DGROUP dwords holding the stock .bss addresses of
//    `tile_objects` and `fog_of_war` -- measured at the image bytes, with zero writers anywhere in
//    the image. They are not registry regions, so no bound-region dump contains them, and they are
//    wrong the moment a host binds the pointee elsewhere. The importing host owes them; LIB-REF's
//    standalone replay is the enforcement. Same column as the torus masks, for the same reason and
//    with the same file recording it (tools/data/world_snapshot_dispositions.json `re_derive`).
//
// 3. A HASH IS ONLY COMPARABLE UNDER ITS OWN IMPLEMENTATION AND MANIFEST. The header carries a
//    fingerprint of the hash SINK and of the hash MANIFEST, plus the three mask flags the run was
//    hashed under, and import-side consumers refuse a blob that disagrees. Without that, a blob
//    recorded before a manifest append would be compared under a different slice set and the
//    mismatch would read as a broken fixture rather than as a stale one.
//
#pragma once
#include <cstddef>
#include <cstdint>

#include "addr/mh_regions.gen.h"
#include "addr/mh_world_snapshot.gen.h"
#include "state/blob_snapshot.h"

namespace mh::state::world {

// "MHWRLD\0\1" -- distinct from LIB-BOOT's MHBOOT so a boot blob cannot import as a world blob even
// if the block tables happened to line up.
inline constexpr uint8_t MAGIC[8] = {'M', 'H', 'W', 'R', 'L', 'D', 0, 1};
// 2 (was 1): the blob gained the NAV TRAILER -- the map-region decomposition, carried as slot
// indices instead of left to be rebuilt from the `passable` plane. See state/nav_trailer.h for why a
// rebuild is not the recording's partition. FORMAT is mixed into the schema fingerprint, so every
// schema-1 blob now refuses with ERR_SCHEMA (-3) rather than importing without a decomposition --
// which is the behaviour we want: a v1 fixture under a v2 build is not a fixture missing an extra,
// it is a fixture whose nav graph would be silently wrong.
inline constexpr uint32_t FORMAT = 2u;

// The three per-run knobs the determinism hash is taken under (harness [harness] mask_*). They are
// recorded because a hash re-derived under different masks is a different number, and blaming the
// blob for that would be the wrong diagnosis.
inline constexpr uint32_t MASK_CTRL_GROUP   = 1u << 0;
inline constexpr uint32_t MASK_SOLDIER_ANIM = 1u << 1;
inline constexpr uint32_t MASK_PLANETS_GFX  = 1u << 2;

struct blob_header {
    mh::state::blob::common_header base;

    // ---- what the RUN said at this instant -----------------------------------------------------
    uint64_t lockstep_combined; // the run's own `combined` at step 0 -- what the oracle reproduces
    uint64_t lockstep_state;    // ... and its state-only sibling (state_excluded() slices dropped)
    uint64_t game_clock;        // the game-clock bits from the same sample point
    uint32_t step;              // the step counter at capture; 1 == pre-body of the FIRST sim step
    uint32_t mask_flags;        // MASK_* above -- the masks that hash was taken under

    // ---- what the BUILD said, so a stale blob is refused rather than mis-compared ---------------
    uint32_t hash_sink_fp;     // fingerprint of the live hash_sink implementation
    uint32_t hash_manifest_fp; // fingerprint of HASH_REGIONS[] (name, rid, offset, len, excluded)
    uint32_t hash_slice_count; // == HASH_REGION_COUNT at capture

    // ---- the pointer census (LIB-BOOT's `pointed-into` check, generalised) ----------------------
    //
    // A bound-region dump carries whatever the bytes are, pointers included, and a pointer is the
    // one kind of content that is wrong in another process however faithfully it is copied. These
    // two counters are how that stops being invisible: they are counted at capture, because no
    // build-time derivation can see a value written at runtime.
    uint32_t region_head_ptrs;   // dwords whose value EQUALS some carried region's live base -- the
                                 // measured tile_objects_ptr / fow_ptr class, wherever it occurs
    uint32_t registry_span_ptrs; // dwords landing anywhere inside the registry's stock address span

    // ---- the nav trailer (FORMAT 2) -------------------------------------------------------------
    //
    // The map-region decomposition, carried as slot indices. It is NOT a block: WORLD_SNAPSHOT_BLOCKS
    // is one entry per registry region and worldtest's arms B and C walk it per-region, so a
    // synthetic entry there would make both arms ambiguous about what they measure. It sits past the
    // last block payload -- at exactly `sizeof(blob_header) + base.payload_len` -- which leaves the
    // shared engine's `off != payload_len` completeness check measuring the same thing it always did.
    // Being outside `base.content_hash` by construction, the trailer carries its own magic, version
    // and checksum. See state/nav_trailer.h.
    uint32_t nav_offset; // byte offset of the trailer; 0 == absent (never written by a v2 capture)
    uint32_t nav_len;    // bytes

    uint32_t pad_; // explicit rather than implicit: the struct is 8-aligned for the u64s above, so
                   // an odd number of trailing dwords would be padded anyway -- and a file format
                   // whose last four bytes are "whatever the compiler left" is not a file format.
};
static_assert(sizeof(blob_header) == 32 + 24 + 40, "the world blob header layout is a file format");
static_assert(offsetof(blob_header, lockstep_combined) == 32, "the shared prefix is 32 bytes");

// The refusal codes are the shared ones (state/blob_snapshot.h), plus this policy's own -8.
enum world_err : int {
    WORLD_OK                = mh::state::blob::OK,
    WORLD_ERR_ARG           = mh::state::blob::ERR_ARG,
    WORLD_ERR_MAGIC         = mh::state::blob::ERR_MAGIC,
    WORLD_ERR_SCHEMA        = mh::state::blob::ERR_SCHEMA,
    WORLD_ERR_SESSION_BEGUN = mh::state::blob::ERR_SESSION_BEGUN,
    WORLD_ERR_TRUNCATED     = mh::state::blob::ERR_TRUNCATED,
    WORLD_ERR_UNKNOWN_RID   = mh::state::blob::ERR_UNKNOWN_RID,
    WORLD_ERR_UNBOUND       = mh::state::blob::ERR_UNBOUND,
    // The capture's own self-check failed: a slice the determinism hash reads is not in the blob
    // byte-for-byte. See capture() -- this is the vacuous-fixture refusal.
    WORLD_ERR_HASHED_SLICE_MISSING = mh::state::blob::ERR_POLICY,
    // FORMAT 2's own three. They are outside the shared engine's code space because the trailer is
    // outside the shared engine -- see state/nav_trailer.h.
    WORLD_ERR_NAV_CAPTURE = -20, // the live pool could not be serialized (see nav_err for which)
    WORLD_ERR_NAV_MISSING = -21, // a v2 blob with no trailer: impossible from a v2 capture
    WORLD_ERR_NAV_IMPORT  = -22, // the trailer was rejected (see nav_err)
};

// What the caller (seams/harness.cpp) knows and the module does not: the run's own numbers.
struct capture_params {
    uint64_t lockstep_combined = 0;
    uint64_t lockstep_state    = 0;
    uint64_t game_clock        = 0;
    uint32_t step              = 0;
    uint32_t mask_flags        = 0;
};

// Fingerprints, so a hash recorded under one implementation is never silently compared under
// another. Both are cheap and both are recorded in every blob.
uint32_t hash_sink_fingerprint();
uint32_t hash_manifest_fingerprint();

uint32_t schema_fingerprint();

// The header + every block payload: what the BLOCK half of a capture writes, and the offset the nav
// trailer therefore starts at. Unchanged by FORMAT 2 on purpose -- it still answers exactly the
// question it always answered.
size_t blob_size();

// What a capture buffer must actually be: blob_size() plus the largest trailer a legal pool can
// produce. The trailer is variable-length (it scales with the pool), so this is a worst case, not a
// prediction -- `capture()` reports the real total through `out_len`.
size_t   capture_capacity();
uint64_t canonical_hash();

// Read every bound region into `buf`. Returns WORLD_OK or a world_err; `out_len` receives the bytes
// written. The capture SELF-CHECKS before it succeeds: every determinism-hash slice's live bytes
// must appear in the blob byte-for-byte, or it refuses with WORLD_ERR_HASHED_SLICE_MISSING. That is
// the arm against the fixture that hashes to the right value while carrying nothing the oracle
// reads -- caught at capture, in the process that still has the truth in front of it.
int capture(void *buf, size_t cap, size_t *out_len, const capture_params &p);

// Write a captured world back through live_base(). Validation is complete BEFORE the first byte is
// written, so a rejected import cannot leave a half-answered world.
//
// THE BYTE HALF ONLY: this deliberately does NOT import the nav trailer. `net_selftest worldtest`
// compares imported memory against the blob payload byte-for-byte (arm C) and against the blob's own
// content hash (arm B), and rebuilding a heap graph here would move both -- LIB-WORLD's oracle would
// be measuring the fixup instead of the fixture. The trailer is applied by import_nav() below, which
// the C entry (state/spine.cpp) calls in its re-derive sequence and worldtest does not.
int import(const void *blob, size_t n);

// Apply the FORMAT 2 nav trailer: rebuild the map-region pool, both lists, BY_INDEX and the grid's
// pointer dwords from the carried slot indices. Call AFTER import(), and INSTEAD OF
// llm_map_build_regions -- running both would silently replace the carried decomposition with a
// rebuild, which is the exact failure this trailer exists to end.
int import_nav(const void *blob, size_t n);

// The step-0 lockstep hash pair, recomputed over whatever is bound NOW, exactly as
// seams/harness.cpp folds it: per-slice hash_slice() under the given masks, FNV-folded in manifest
// order, with the state-only fold dropping the state_excluded() slices. THE ORACLE'S OTHER HALF --
// this is the function that must reproduce the recording peer's number after an import, and it
// lives here rather than in the test so the test cannot quietly compute a different fold.
void lockstep_hash(uint32_t mask_flags, uint64_t *out_combined, uint64_t *out_state);

} // namespace mh::state::world
