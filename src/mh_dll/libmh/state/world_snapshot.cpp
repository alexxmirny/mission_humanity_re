//
// state/world_snapshot.cpp -- see world_snapshot.h for why this exists and what it may not do.
//
#include "state/world_snapshot.h"

#include <cstring>
#include <memory>
#include <new>

#include "state/blob_snapshot.h"
#include "state/boot_snapshot.h" // the session-begun latch -- see refuse_import() below
#include "state/nav_trailer.h"   // FORMAT 2's carried map-region decomposition
#include "state/region_runtime.h"
#include "state/region_view.h"
#include "state/state_sink.h"
#include "state/world_fixup.h" // FORMAT 2's optional fixup trailer (mp:X3a)
#include "mh_storage_policy.h"
// scene_rename.h renames the snapshot namespace, not the canonical route codec's world() API.
#ifdef world
#pragma push_macro("world")
#undef world
#define MH_ORDER_SNAPSHOT_RESTORE_WORLD
#endif
#include "mh_order_tasks.h"
#ifdef MH_WORLD_LIVE_OWNER
#include "state/order_tasks_runtime.h"
#endif
#ifdef MH_ORDER_SNAPSHOT_RESTORE_WORLD
#pragma pop_macro("world")
#undef MH_ORDER_SNAPSHOT_RESTORE_WORLD
#endif

namespace mh::state::world {
namespace {

using mh::state::blob::capture_stats;
using mh::state::blob::snapshot_block;

// ---- the pointer census ------------------------------------------------------------------------
//
// LIB-BOOT checked ONE declaration at capture: that something still pointed into the arena it
// carried on a `pointed-into` claim. The world blob has the general version of that problem -- it
// carries whatever the bytes are, and a POINTER is the one kind of content that is wrong in another
// process however faithfully it is copied. No build-time derivation can see one (the value is
// written, or baked, at runtime), so it is COUNTED, and the two counters ride in the header.
//
// The precise counter is `region_head_ptrs`: a dword whose value EQUALS some carried region's live
// base. That is exactly the measured tile_objects_ptr / fow_ptr class -- an initialised dword
// holding the .bss address of a bound region -- and it finds the class wherever it occurs rather
// than only where somebody looked. `registry_span_ptrs` is the loose one: a dword landing anywhere
// inside the registry's stock address span. It has a known false-positive floor (any integer in an
// 11 MB window) and is reported, never gated on -- a number whose noise is understood is worth more
// than a threshold nobody can justify.
struct census {
    uint32_t bases[WORLD_SNAPSHOT_BLOCK_COUNT]; // sorted live bases of the carried blocks
    int      n       = 0;
    uint32_t span_lo = 0xffffffffu, span_hi = 0;

    void build() {
        n = 0;
        for (int i = 0; i < WORLD_SNAPSHOT_BLOCK_COUNT; ++i) {
            const snapshot_block &b = WORLD_SNAPSHOT_BLOCKS[i];
            if (REGIONS[b.rid].host_native) continue; // pointer-free, no original-image address
            const uint32_t lb = live_base(b.rid);
            if (lb == 0) continue;
            bases[n++] = lb;
            if (lb < span_lo) span_lo = lb;
            const uint32_t len = canonical_region_len(b.rid);
            if (lb + len > span_hi) span_hi = lb + len;
        }
        // insertion sort: n is ~830 and this runs once per capture, on a step the run has already
        // hashed, so the constant factor is irrelevant and the code being obvious is not.
        for (int i = 1; i < n; ++i) {
            const uint32_t v = bases[i];
            int            j = i - 1;
            while (j >= 0 && bases[j] > v) {
                bases[j + 1] = bases[j];
                --j;
            }
            bases[j + 1] = v;
        }
    }

    bool is_head(uint32_t v) const {
        int lo = 0, hi = n - 1;
        while (lo <= hi) {
            const int mid = (lo + hi) / 2;
            if (bases[mid] == v) return true;
            if (bases[mid] < v) lo = mid + 1;
            else hi = mid - 1;
        }
        return false;
    }
    bool in_span(uint32_t v) const { return v >= span_lo && v < span_hi; }
};

census g_census;
bool   g_census_valid = false;

// ---- the block emitter -------------------------------------------------------------------------
//
// VERBATIM (world_snapshot.h note 1): the oracle reproduces a RAW fnv1a over live bytes, so any
// transformation here would move the number away from the one the recording peer printed.
template <class S>
void emit_block(const snapshot_block &b, S &s, capture_stats *stats) {
    const uint32_t len = canonical_region_len(b.rid);
    const uint8_t *p   = reinterpret_cast<const uint8_t *>(static_cast<uintptr_t>(live_base(b.rid)));
    if (p == nullptr) return;
    if (stats && g_census_valid && !REGIONS[b.rid].host_native) {
        for (uint32_t o = 0; o + 4u <= len; o += 4u) {
            uint32_t v = 0;
            std::memcpy(&v, p + o, 4); // a region is not guaranteed 4-aligned
            if (g_census.is_head(v)) ++stats->ptrs_in_arena;
            else if (g_census.in_span(v)) ++stats->ptrs_out_arena;
        }
    }
    sink_bytes(s, p, len);
}

void apply_block(const snapshot_block &b, const uint8_t *src, uint32_t len) {
    uint8_t *dst = reinterpret_cast<uint8_t *>(static_cast<uintptr_t>(live_base(b.rid)));
    if (dst == nullptr) return; // validated unreachable: import() refuses an unbound block first
    std::memcpy(dst, src, len);
}

// The private native snapshot copy renames namespace world; codec member names stay unchanged.
#ifdef world
#pragma push_macro("world")
#undef world
#define MH_ORDER_VALIDATION_RESTORE_WORLD
#endif
bool validate_native_views(const uint8_t *policy, uint32_t policy_bytes,
                           const uint8_t *tasks, uint32_t task_bytes,
                           const uint8_t *campaign, uint32_t campaign_bytes,
                           uint32_t width, uint32_t height, uint32_t system, uint32_t planet) {
    mh::storage_policy::state       incoming, bound;
    mh::order_tasks::state          routes;
    mh::order_tasks::campaign_state worlds;
    const auto                      live_policy = storage_policy_region();
    if (!incoming.bind(const_cast<uint8_t *>(policy), policy_bytes) ||
        !bound.bind(live_policy.data, live_policy.bytes) ||
        !routes.bind(const_cast<uint8_t *>(tasks), task_bytes) ||
        !worlds.bind(const_cast<uint8_t *>(campaign), campaign_bytes) ||
        !routes.checkpoint_ready())
        return false;
    const auto have = bound.shape(), next = incoming.shape();
    const auto route = routes.shape();
    if (next.players != have.players || next.buildings != have.buildings || next.units != have.units ||
        route.players != next.players || route.buildings != next.buildings || route.units != next.units ||
        route.width != next.width || route.height != next.height)
        return false;
    if (next.width != 0) {
        const auto key = routes.world();
        if (next.width != width || next.height != height || key.system != system ||
            key.planet != planet || worlds.world_inc(planet) != key.incarnation)
            return false;
    }
    return routes.validate_lifetimes(&incoming, [](void *opaque, mh::order_tasks::object_kind kind,
                                                   mh::order_tasks::actor_key key) {
        const auto &identities = *static_cast<mh::storage_policy::state *>(opaque);
        const auto  id         = kind == mh::order_tasks::object_kind::unit
                                     ? identities.unit_identity(key.player, key.slot)
                                 : kind == mh::order_tasks::object_kind::building
                                     ? identities.building_identity(key.player, key.slot)
                                     : mh::storage_policy::identity{};
        return key.incarnation != 0 && id.incarnation == key.incarnation;
    });
}
#ifdef MH_ORDER_VALIDATION_RESTORE_WORLD
#pragma pop_macro("world")
#undef MH_ORDER_VALIDATION_RESTORE_WORLD
#endif

// Both callers have already validated the full block framing. A recognized legacy world may
// substitute its privately expanded route bytes; every paired policy/world view stays incoming.
bool validate_native_payload_with_tasks(const uint8_t *payload, uint32_t bytes,
                                        const uint8_t *replacement_tasks, uint32_t replacement_bytes) {
    const uint8_t *policy = nullptr, *tasks = nullptr, *campaign = nullptr;
    uint32_t       policy_bytes = 0, task_bytes = 0, campaign_bytes = 0;
    uint32_t       width = 0, height = 0, system = 0, planet = 0;
    bool           have_width = false, have_height = false, have_system = false, have_planet = false;
    size_t         off = 0;
    for (int i = 0; i < WORLD_SNAPSHOT_BLOCK_COUNT; ++i) {
        const snapshot_block &b   = WORLD_SNAPSHOT_BLOCKS[i];
        uint32_t              len = 0;
        std::memcpy(&len, payload + off + 4u, sizeof(len));
        const uint8_t *src = payload + off + 8u;
        if (b.rid == RID_STORAGE_POLICY) {
            policy       = src;
            policy_bytes = len;
        } else if (b.rid == RID_ORDER_TASKS) {
            tasks      = src;
            task_bytes = len;
        } else if (b.rid == RID_ORDER_CAMPAIGN) {
            campaign       = src;
            campaign_bytes = len;
        } else if (b.rid == RID_WIDTH) {
            std::memcpy(&width, src, sizeof(width));
            have_width = true;
        } else if (b.rid == RID_HEIGHT) {
            std::memcpy(&height, src, sizeof(height));
            have_height = true;
        } else if (b.rid == RID_CURRENTSYSTEM) {
            std::memcpy(&system, src, sizeof(system));
            have_system = true;
        } else if (b.rid == RID_G_PLANET_INDEX) {
            std::memcpy(&planet, src, sizeof(planet));
            have_planet = true;
        }
        off += 8u + len;
    }
    if (replacement_tasks) {
        if (!tasks) return false;
        tasks      = replacement_tasks;
        task_bytes = replacement_bytes;
    }
    return off == bytes && policy && tasks && campaign && have_width && have_height &&
           have_system && have_planet &&
           validate_native_views(policy, policy_bytes, tasks, task_bytes, campaign, campaign_bytes,
                                 width, height, system, planet);
}

bool validate_native_payload(const uint8_t *payload, uint32_t bytes) {
    return validate_native_payload_with_tasks(payload, bytes, nullptr, 0);
}

struct world_policy {
    static constexpr const uint8_t *MAGIC  = world::MAGIC;
    static constexpr uint32_t       FORMAT = world::FORMAT;

    static int                   count() { return WORLD_SNAPSHOT_BLOCK_COUNT; }
    static const snapshot_block &at(int i) { return WORLD_SNAPSHOT_BLOCKS[i]; }
    static uint32_t              canonical_len(const snapshot_block &b) { return canonical_region_len(b.rid); }

    template <class S>
    static void emit(const snapshot_block &b, S &s, capture_stats *stats) {
        emit_block(b, s, stats);
    }
    static void apply(const snapshot_block &b, const uint8_t *src, uint32_t len) {
        apply_block(b, src, len);
    }
    // EVERY block must be bound. Unlike LIB-BOOT's TLO registry there is no block here carried in a
    // derived form, so an unbound region is a host that forgot one -- refuse rather than write a
    // hole nothing downstream can see.
    static bool unbound_ok(const snapshot_block &) { return false; }
    // The SAME latch LIB-BOOT installs, deliberately shared rather than duplicated: it records one
    // process-global fact ("a session has begun and has started rewriting boot-and-session
    // regions"), and importing a world over live session state is the same hazard for the same
    // reason. A second latch would be a second thing to remember to set.
    static bool refuse_import() { return mh::state::boot::session_begun(); }
    // Nothing to assert here: the census is a REPORT, not a gate (see the census banner), and the
    // capture's real self-check needs the built blob and so lives in capture() below.
    static int post_capture(const capture_stats &) { return WORLD_OK; }
};

// This is one recognized stored-world format, not permission to import an arbitrary old schema.
// Its descriptor table differs ONLY at order_tasks' minimum header extent. All other rids,
// names, positions, descriptors and stock reaches remain those of the current table.
inline constexpr uint32_t LEGACY_ORDER_HEADER_BYTES = 96u;
inline constexpr uint32_t LEGACY_ORDER_ROW_BYTES    = 192u + 32u * 48u;
static_assert(mh::order_tasks::SCHEMA == 2u && mh::order_tasks::LEGACY_SCHEMA == 1u &&
                  mh::order_tasks::HEADER_BYTES == 128u && mh::order_tasks::ACTOR_BYTES == 240u &&
                  mh::order_tasks::NODE_BYTES == 48u && mh::order_tasks::CAPACITY == 32u,
              "the stored-world migration recognizes QORD1 -> QORD2 only");

struct schema1_world_policy : world_policy {
    static const snapshot_block &at(int i) {
        const snapshot_block &b = world_policy::at(i);
        if (b.rid != RID_ORDER_TASKS) return b;
        static const snapshot_block old_tasks = {RID_ORDER_TASKS, LEGACY_ORDER_HEADER_BYTES, b.name};
        return old_tasks;
    }
};

// The strict engine has already checked magic/format and refused this fingerprint before any
// apply. Stage only the expanded route allocation; all other source blocks and all trailers
// remain in the original immutable blob. Only the non-reentrant pre-store lifecycle notice may run.
int import_schema1_world(const void *blob, size_t n, const blob_header &h, void (*before_apply)()) {
    if (h.base.block_count != static_cast<uint32_t>(WORLD_SNAPSHOT_BLOCK_COUNT))
        return WORLD_ERR_SCHEMA;
    if (world_policy::refuse_import()) return WORLD_ERR_SESSION_BEGUN;
    if (h.base.payload_len > n - sizeof(blob_header)) return WORLD_ERR_TRUNCATED;

    // Carry the ORIGINAL trailer offsets, never offsets adjusted for the expanded route span.
    // Semantic nav import/allocation still belongs to the existing caller's re-derive sequence.
    if (!h.nav_offset || !h.nav_len) return WORLD_ERR_NAV_MISSING;
    if (uint64_t(h.nav_offset) != uint64_t(sizeof(blob_header)) + h.base.payload_len ||
        uint64_t(h.nav_offset) + h.nav_len + h.fixup_len > n)
        return WORLD_ERR_TRUNCATED;

    const auto    *source         = static_cast<const uint8_t *>(blob);
    const auto    *payload        = source + sizeof(blob_header);
    const uint8_t *old_tasks      = nullptr;
    uint32_t       old_task_bytes = 0, new_task_bytes = 0;
    size_t         off = 0;
    for (int i = 0; i < WORLD_SNAPSHOT_BLOCK_COUNT; ++i) {
        const snapshot_block &b = WORLD_SNAPSHOT_BLOCKS[i];
        if (h.base.payload_len - off < 8u) return WORLD_ERR_TRUNCATED;
        uint32_t rid = 0, len = 0;
        std::memcpy(&rid, payload + off, sizeof(rid));
        std::memcpy(&len, payload + off + 4u, sizeof(len));
        if (rid >= static_cast<uint32_t>(RID_COUNT)) return WORLD_ERR_UNKNOWN_RID;
        if (rid != static_cast<uint32_t>(b.rid)) return WORLD_ERR_SCHEMA;
        if (len > h.base.payload_len - off - 8u) return WORLD_ERR_TRUNCATED;
        const bool native = REGIONS[b.rid].host_native;
        if (native && (!is_bound(b.rid) || live_base(b.rid) == 0 ||
                       live_size(b.rid) < REGIONS[b.rid].size))
            return WORLD_ERR_UNBOUND;
        const uint32_t current_len  = world_policy::canonical_len(b);
        uint32_t       expected_len = current_len;
        if (b.rid == RID_ORDER_TASKS) {
            if (old_tasks || !native || b.len != mh::order_tasks::HEADER_BYTES ||
                current_len < mh::order_tasks::HEADER_BYTES ||
                (current_len - mh::order_tasks::HEADER_BYTES) % mh::order_tasks::ROW_BYTES != 0)
                return WORLD_ERR_SCHEMA;
            const uint32_t rows = (current_len - mh::order_tasks::HEADER_BYTES) /
                                  mh::order_tasks::ROW_BYTES;
            const uint64_t legacy_len = uint64_t(LEGACY_ORDER_HEADER_BYTES) +
                                        uint64_t(rows) * LEGACY_ORDER_ROW_BYTES;
            if (legacy_len > UINT32_MAX) return WORLD_ERR_SCHEMA;
            expected_len   = static_cast<uint32_t>(legacy_len);
            old_tasks      = payload + off + 8u;
            old_task_bytes = len;
            new_task_bytes = current_len;
        }
        if (len != expected_len) return WORLD_ERR_SCHEMA;
        if (!live_base(b.rid) || live_size(b.rid) < (native ? current_len : b.len))
            return WORLD_ERR_UNBOUND;

        // The second pass rereads framing and the callers still need the original trailers.
        // Never accept an input buffer overlapping any destination, even at a later block.
        const uintptr_t src = reinterpret_cast<uintptr_t>(source);
        const uintptr_t dst = static_cast<uintptr_t>(live_base(b.rid));
        if (src <= dst ? size_t(dst - src) < n : size_t(src - dst) < current_len)
            return WORLD_ERR_ARG;
        off += 8u + len;
    }
    if (off != h.base.payload_len) return WORLD_ERR_TRUNCATED;
    if (!old_tasks) return WORLD_ERR_NATIVE_STATE;

    // This is the source blob's hash, not a new QORD2 hash and not an oracle restamp. The sink
    // must be the same implementation before its content-hash field has a useful interpretation.
    if (h.hash_sink_fp != hash_sink_fingerprint()) return WORLD_ERR_SCHEMA;
    hash_sink original(sink_mode::PERSIST);
    original.raw(payload, h.base.payload_len);
    if (original.finish() != h.base.content_hash) return WORLD_ERR_NATIVE_STATE;

    std::unique_ptr<uint8_t[]> tasks(new (std::nothrow) uint8_t[new_task_bytes]);
    if (!tasks || !mh::order_tasks::state::migrate_schema1(tasks.get(), new_task_bytes, old_tasks, old_task_bytes) ||
        !validate_native_payload_with_tasks(payload, h.base.payload_len, tasks.get(), new_task_bytes))
        return WORLD_ERR_NATIVE_STATE;

    // PUBLICATION BOUNDARY. All newly fallible framing/allocation/migration/bind/checkpoint/
    // policy/world/campaign/lifetime work is complete. No resizing, rebinding, fallible callback
    // or validation follows inside this byte publication. Only QORD changes representation.
    if (before_apply) before_apply();
    off = 0;
    for (int i = 0; i < WORLD_SNAPSHOT_BLOCK_COUNT; ++i) {
        const snapshot_block &b   = WORLD_SNAPSHOT_BLOCKS[i];
        uint32_t              len = 0;
        std::memcpy(&len, payload + off + 4u, sizeof(len));
        if (b.rid == RID_ORDER_TASKS) apply_block(b, tasks.get(), new_task_bytes);
        else apply_block(b, payload + off + 8u, len);
        off += 8u + len;
    }
    return WORLD_OK;
}

} // namespace

// ---- the fingerprints ---------------------------------------------------------------------------

uint32_t hash_sink_fingerprint() {
    // The same vector and the same two splits seams/harness.cpp's hash_fingerprint_report uses, for
    // the same reason: it spans a partial tail, a whole block and a length that is neither, so a
    // change to what hash_sink computes moves this with nobody having to remember to bump anything.
    static const uint8_t VEC[21] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
                                    0x10, 0x11, 0x12, 0x13, 0x14};
    hash_sink            a(sink_mode::VERDICT);
    a.raw(VEC, sizeof(VEC));
    const uint64_t h = a.finish();
    return static_cast<uint32_t>(h ^ (h >> 32));
}

// mp:D29: the definition moved to state/region_view.h as a COMPILE-TIME constant, so mh_harness can
// print the same number in configuration (1), where this function does not exist. One definition,
// returned here unchanged -- every blob stamp this function ever wrote is the same value.
uint32_t hash_manifest_fingerprint() { return HASH_MANIFEST_FP; }

// ---- the lockstep hash, folded exactly as the harness folds it -----------------------------------

// HASH-INPUT BEGIN world_lockstep_hash (tools/data/hash_input_epoch.json)
void lockstep_hash(uint32_t mask_flags, uint64_t *out_combined, uint64_t *out_state) {
    uint64_t combined = 1469598103934665603ULL;
    uint64_t state    = 1469598103934665603ULL;
    auto     fold     = [](const void *p, size_t n, uint64_t h) {
        const uint8_t *b = static_cast<const uint8_t *>(p);
        for (size_t i = 0; i < n; ++i) {
            h ^= b[i];
            h *= 1099511628211ULL;
        }
        return h;
    };
    for (int i = 0; i < HASH_REGION_COUNT; ++i) {
        const uint64_t per = hash_slice(i, (mask_flags & MASK_CTRL_GROUP) != 0,
                                        (mask_flags & MASK_SOLDIER_ANIM) != 0,
                                        (mask_flags & MASK_PLANETS_GFX) != 0);
        combined           = fold(&per, sizeof(per), combined);
        if (!HASH_REGIONS[i].excluded) state = fold(&per, sizeof(per), state);
    }
    if (out_combined) *out_combined = combined;
    if (out_state) *out_state = state;
}
// HASH-INPUT END world_lockstep_hash

// ---- the public face: the SHARED engine, over the policy above -----------------------------------

uint32_t schema_fingerprint() {
    return mh::state::blob::schema_fingerprint<world_policy>();
}

size_t blob_size() {
    return mh::state::blob::blob_size<world_policy, blob_header>();
}

size_t capture_capacity() {
    return blob_size() + mh::state::nav::MAX_TRAILER_BYTES + FIXUP_MAX_TRAILER_BYTES;
}

uint64_t canonical_hash() {
    return mh::state::blob::canonical_hash<world_policy>();
}

namespace {

// THE CAPTURE'S OWN SELF-CHECK, and it is the arm against the fixture that is right about its own
// hash and empty of everything the oracle reads. Every determinism-hash SLICE's live bytes must
// appear in the blob byte-for-byte at the offset the block table puts them; if one does not, the
// blob would import a hole into exactly the memory the step-0 comparison walks, and the comparison
// would then be over poison on one side and poison on the other. Checked HERE, in the process that
// still has the truth in front of it, rather than only in the oracle -- a capture that cannot prove
// this refuses rather than writing a file somebody will trust later.
bool hashed_slices_present(const uint8_t *blob) {
    for (int h = 0; h < HASH_REGION_COUNT; ++h) {
        const hash_region &r   = HASH_REGIONS[h];
        const uint32_t     len = hash_len(h);
        // find the block that carries this slice's region, and its payload offset in the blob
        size_t off   = sizeof(blob_header);
        bool   found = false;
        for (int i = 0; i < WORLD_SNAPSHOT_BLOCK_COUNT; ++i) {
            const snapshot_block &b         = WORLD_SNAPSHOT_BLOCKS[i];
            const uint32_t        block_len = canonical_region_len(b.rid);
            if (b.rid == r.rid) {
                if (uint64_t(r.offset) + len > block_len) return false;
                const uint8_t *live =
                    reinterpret_cast<const uint8_t *>(static_cast<uintptr_t>(live_base(b.rid)));
                if (live == nullptr) return false;
                if (std::memcmp(blob + off + 8u + r.offset, live + r.offset, len) != 0)
                    return false;
                found = true;
                break;
            }
            off += 8u + block_len;
        }
        if (!found) return false; // a hashed region with no block -- the generator refuses this,
                                  // and this is the runtime half of that same refusal
    }
    return true;
}

} // namespace

int capture(void *buf, size_t cap, size_t *out_len, const capture_params &p) {
    if (!buf || !out_len) return WORLD_ERR_ARG;
    *out_len = 0;
#ifdef MH_WORLD_LIVE_OWNER
    if (!mh::order_tasks::runtime::checkpoint_ready()) return WORLD_ERR_NATIVE_STATE;
#endif
    const auto policy = storage_policy_region(), tasks = order_tasks_region(), campaign = order_campaign_region();
    uint32_t   w, hgt, sys, pl;
    if (!policy.data || !tasks.data || !campaign.data ||
        !read_region_u32(RID_WIDTH, &w) || !read_region_u32(RID_HEIGHT, &hgt) ||
        !read_region_u32(RID_CURRENTSYSTEM, &sys) || !read_region_u32(RID_G_PLANET_INDEX, &pl))
        return WORLD_ERR_UNBOUND;
    if (!validate_native_views(static_cast<const uint8_t *>(policy.data), policy.bytes,
                               static_cast<const uint8_t *>(tasks.data), tasks.bytes,
                               static_cast<const uint8_t *>(campaign.data), campaign.bytes, w, hgt, sys, pl))
        return WORLD_ERR_NATIVE_STATE;
    blob_header h;
    std::memset(&h, 0, sizeof(h));
    h.lockstep_combined = p.lockstep_combined;
    h.lockstep_state    = p.lockstep_state;
    h.game_clock        = p.game_clock;
    h.step              = p.step;
    h.mask_flags        = p.mask_flags;
    h.hash_sink_fp      = hash_sink_fingerprint();
    h.hash_manifest_fp  = hash_manifest_fingerprint();
    h.hash_slice_count  = static_cast<uint32_t>(HASH_REGION_COUNT);

    g_census.build();
    g_census_valid = true;

    mh::state::blob::capture_stats st;
    const int                      rc = mh::state::blob::capture<world_policy, blob_header>(buf, cap, out_len, &h, &st);
    g_census_valid                    = false;
    if (rc != mh::state::blob::OK) return rc;

    const uint8_t *d = static_cast<const uint8_t *>(buf);
    if (!hashed_slices_present(d)) return WORLD_ERR_HASHED_SLICE_MISSING;

    // The census tallies are known only after emit() has walked every block, and the engine copies
    // the header before that is true (payload_len and content_hash are only known then), so they are
    // stamped into the written blob here. They are outside content_hash by construction, which is
    // correct: they describe the capture, not the world.
    blob_header *out        = static_cast<blob_header *>(buf);
    out->region_head_ptrs   = st.ptrs_in_arena;
    out->registry_span_ptrs = st.ptrs_out_arena;

    // ---- THE NAV TRAILER (FORMAT 2) --------------------------------------------------------------
    //
    // Appended AFTER the block payload, so `payload_len` still describes exactly the blocks and the
    // engine's completeness check is untouched. A capture that cannot serialize the pool REFUSES:
    // the whole failure mode this trailer exists to end is a nav graph that is quietly not the
    // recording's, and a capture that shipped a partial one would be that failure with a new name.
    const size_t nav_at  = *out_len;
    size_t       nav_len = 0;
    const int    nrc     = mh::state::nav::capture(static_cast<uint8_t *>(buf) + nav_at, cap - nav_at, &nav_len);
    if (nrc != mh::state::nav::NAV_OK) return WORLD_ERR_NAV_CAPTURE;
    out->nav_offset = static_cast<uint32_t>(nav_at);
    out->nav_len    = static_cast<uint32_t>(nav_len);
    *out_len        = nav_at + nav_len;

    // ---- THE FIXUP TRAILER (mp:X3a, additive) ----------------------------------------------------
    //
    // Classifies every carried dword against THIS process's own address map and appends the result
    // after the nav trailer. It reads the BLOB'S block payload (a private snapshot), never live
    // memory, and it never writes a byte before `nav_at + nav_len` -- the hashed bytes and the
    // content hash are untouched by construction. A capture that overflows the trailer REFUSES: a
    // partial population is exactly the failure the trailer exists to end.
    const size_t fix_at  = *out_len;
    size_t       fix_len = 0;
    int          frc     = 0;
    if (p.map != nullptr) {
        frc = fixup_capture(d, static_cast<uint8_t *>(buf) + fix_at, cap - fix_at, &fix_len, *p.map);
    } else {
        addr_map *real = new addr_map;
        frc            = build_real_addr_map(*real)
                             ? fixup_capture(d, static_cast<uint8_t *>(buf) + fix_at, cap - fix_at, &fix_len, *real)
                             : -1;
        delete real;
    }
    if (frc != 0) return WORLD_ERR_FIXUP_CAPTURE;
    out->fixup_len = static_cast<uint32_t>(fix_len);
    *out_len       = fix_at + fix_len;
    return WORLD_OK;
}

int import_nav(const void *blob, size_t n) {
    blob_header h;
    if (blob == nullptr || n < sizeof(h)) return WORLD_ERR_ARG;
    std::memcpy(&h, blob, sizeof(h));
    // A v2 blob without a trailer is not a thing a v2 capture can produce, so this is a refusal
    // rather than a skip -- see the nav_offset comment in world_snapshot.h.
    if (h.nav_offset == 0 || h.nav_len == 0) return WORLD_ERR_NAV_MISSING;
    if (static_cast<size_t>(h.nav_offset) + h.nav_len > n) return WORLD_ERR_TRUNCATED;
    const int nrc =
        mh::state::nav::import(static_cast<const uint8_t *>(blob) + h.nav_offset, h.nav_len);
    return nrc == mh::state::nav::NAV_OK ? WORLD_OK : WORLD_ERR_NAV_IMPORT;
}

int import(const void *blob, size_t n, void (*before_apply)()) {
    mh::state::blob::import_policy pol;
    pol.validate_payload = &validate_native_payload;
    pol.before_apply     = before_apply;
    const int rc         = mh::state::blob::import <world_policy, blob_header>(blob, n, pol);
    if (rc != WORLD_ERR_SCHEMA) return rc;
    // ERR_SCHEMA is necessarily pre-apply and follows a valid full header/magic/format. Keep
    // the current path byte-exact and allocation-free; only this one fingerprint gets staging.
    blob_header h;
    std::memcpy(&h, blob, sizeof(h));
    if (h.base.schema != mh::state::blob::schema_fingerprint<schema1_world_policy>()) return rc;
    return import_schema1_world(blob, n, h, before_apply);
}

// mp:X3c: a resync import over a LIVE session. The session latch (boot::session_begun) is a policy
// answer; this entry is the one place that answers differently, and it says so in the name.
int import_resync(const void *blob, size_t n, void (*before_apply)()) {
    mh::state::blob::import_policy pol;
    pol.allow_session_begun = true;
    pol.validate_payload    = &validate_native_payload;
    pol.before_apply        = before_apply;
    return mh::state::blob::import <world_policy, blob_header>(blob, n, pol);
}

} // namespace mh::state::world

// THE C EXPORT (libmh.h) LIVES IN state/spine.cpp, not here, and the split is load-bearing rather
// than filing (LIB-REF, 2026-09-11). `import()` above is the BYTE ENGINE: bytes in, bytes out, no
// normalisation (note 1 in world_snapshot.h), which is what lets `net_selftest worldtest` compare
// imported memory to the blob payload byte-for-byte and against the blob's own content hash. The C
// entry a HOST binds has to do more -- the re-derives the inbound table already says it serves
// (llm_map_setup_dimensions / region_pool_reset / build_regions / init_region_route_step_deltas),
// over a nav graph made of the recording process's heap pointers. Putting those here would move
// both of worldtest's comparisons and leave LIB-WORLD's oracle measuring the fixup instead of the
// fixture. See state/spine.cpp for the sequence and why it is the original's own.
