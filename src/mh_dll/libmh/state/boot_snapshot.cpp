//
// state/boot_snapshot.cpp -- see boot_snapshot.h for why this exists and what it may not do.
//
#include "state/boot_snapshot.h"

#include <cstring>

#include "state/state_sink.h"

namespace mh::state::boot {
namespace {

// ---- the ordering latch (boot_snapshot.h note 3) ---------------------------------------------
bool g_session_begun = false;

// ---- the TLO registry, materialised out of its `char *` table --------------------------------
//
// The region is 8 x llm_tlo_registry_entry {char *name; uint unused0; byte tlo_index} -- 72 bytes
// of which 32 are POINTERS INTO THE IMAGE'S .rdata. Those cannot be carried: standalone there is no
// image, and even hosted a raw copy would only work because nothing moved. So the snapshot carries
// the DERIVED form -- the names inline -- and the imported table lives here rather than being
// written back over 72 bytes of fabricated pointers.
struct tlo_entry {
    char    name[TLO_NAME_CAP];
    uint8_t index;
};
tlo_entry g_tlo[TLO_ENTRIES];
bool      g_tlo_valid = false;

const region_id RID_TEXT_BLOCK_ = RID_G_TEXT_BLOCK;
const region_id RID_TEXT_PTRS_  = RID_G_TEXT_PTRS;
const region_id RID_TLO_        = RID_TLO_REGISTRY;

// Read the live 72-byte table into g_tlo. ONLY legal while the region still sits at its stock base:
// once a host has bound it somewhere else the `char *`s in it are whatever that host allocated, and
// dereferencing them would be a wild read. A rebased-and-not-yet-imported table stays zeroed, which
// is the fail-safe direction -- tlo_index_for then returns the original's own default.
void materialise_tlo() {
    if (g_tlo_valid) return;
    std::memset(g_tlo, 0, sizeof(g_tlo));
    if (is_rebased(RID_TLO_)) return;
    const uint8_t *p = reinterpret_cast<const uint8_t *>(static_cast<uintptr_t>(live_base(RID_TLO_)));
    if (p == nullptr) return;
    for (uint32_t i = 0; i < TLO_ENTRIES; ++i) {
        const uint8_t *rec  = p + i * 9u; // sizeof(llm_tlo_registry_entry) == 0x9
        const char    *name = *reinterpret_cast<const char *const *>(rec);
        if (name) {
            uint32_t k = 0;
            while (k + 1u < TLO_NAME_CAP && name[k]) {
                g_tlo[i].name[k] = name[k];
                ++k;
            }
            g_tlo[i].name[k] = '\0';
        }
        g_tlo[i].index = rec[8];
    }
    g_tlo_valid = true;
}

// ---- the canonical stream --------------------------------------------------------------------
//
// ONE function decides what a block's bytes ARE, and both the blob writer and the hash go through
// it. A second encoding of that decision is the failure state_sink.h's banner is about: the blob
// and the hash would drift and the oracle would stop watching part of the state without saying so.
uint32_t block_len(const boot_snapshot_block &b) {
    if (b.rid == RID_TLO_) return TLO_ENTRIES * TLO_ENTRY_LEN;
    return b.len;
}

// `s` gets the block's canonical bytes. `stats` (optional) receives the in/out-of-arena tallies
// for the G_TEXT_PTRS normalisation, which is how the `pointed-into` carry declaration for
// G_TEXT_BLOCK is CHECKED rather than merely written down.
template <class S>
void emit_block(const boot_snapshot_block &b, S &s, mh::state::blob::capture_stats *stats) {
    uint32_t *const in_arena  = stats ? &stats->ptrs_in_arena : nullptr;
    uint32_t *const out_arena = stats ? &stats->ptrs_out_arena : nullptr;
    if (b.rid == RID_TLO_) {
        materialise_tlo();
        for (uint32_t i = 0; i < TLO_ENTRIES; ++i) {
            uint8_t rec[TLO_ENTRY_LEN];
            std::memset(rec, 0, sizeof(rec));
            std::memcpy(rec, g_tlo[i].name, TLO_NAME_CAP);
            rec[TLO_NAME_CAP] = g_tlo[i].index;
            sink_bytes(s, rec, sizeof(rec));
        }
        return;
    }

    const uint8_t *p = reinterpret_cast<const uint8_t *>(static_cast<uintptr_t>(live_base(b.rid)));

    if (b.rid == RID_TEXT_PTRS_) {
        const uint32_t arena     = live_base(RID_TEXT_BLOCK_);
        const uint32_t arena_len = live_size(RID_TEXT_BLOCK_);
        for (uint32_t o = 0; o + 4u <= b.len; o += 4u) {
            uint32_t v = 0;
            std::memcpy(&v, p + o, 4); // the region is not guaranteed 4-aligned
            if (arena && v >= arena && v < arena + arena_len) {
                v = PTR_TAG | (v - arena);
                if (in_arena) ++*in_arena;
            } else if (out_arena) {
                ++*out_arena;
            }
            sink_bytes(s, &v, 4);
        }
        // A trailing partial dword (there is none today; the region is 3224 = 806 * 4) would be
        // carried verbatim rather than silently dropped.
        const uint32_t tail = b.len % 4u;
        if (tail) sink_bytes(s, p + b.len - tail, tail);
        return;
    }

    sink_bytes(s, p, b.len);
}

// Write one block's carried bytes back through live_base(). The TLO registry is the one block whose
// destination is NOT a region: its live form is 32 bytes of image `char *`, which cannot be
// fabricated in a host process, so the imported table lives in this TU (see materialise_tlo).
void apply_block(const boot_snapshot_block &b, const uint8_t *src, uint32_t len) {
    if (b.rid == RID_TLO_) {
        for (uint32_t k = 0; k < TLO_ENTRIES; ++k) {
            const uint8_t *rec = src + k * TLO_ENTRY_LEN;
            std::memcpy(g_tlo[k].name, rec, TLO_NAME_CAP);
            g_tlo[k].name[TLO_NAME_CAP - 1] = '\0';
            g_tlo[k].index                  = rec[TLO_NAME_CAP];
        }
        g_tlo_valid = true;
        return;
    }

    uint8_t *dst = reinterpret_cast<uint8_t *>(static_cast<uintptr_t>(live_base(b.rid)));

    if (b.rid == RID_TEXT_PTRS_) {
        // Re-absolutise: a tagged entry is an offset into wherever the arena is bound NOW.
        const uint32_t arena = live_base(RID_TEXT_BLOCK_);
        for (uint32_t o = 0; o + 4u <= b.len; o += 4u) {
            uint32_t v = 0;
            std::memcpy(&v, src + o, 4);
            if (v & PTR_TAG) v = arena + (v & ~PTR_TAG);
            std::memcpy(dst + o, &v, 4);
        }
        const uint32_t tail = b.len % 4u;
        if (tail) std::memcpy(dst + b.len - tail, src + b.len - tail, tail);
        return;
    }

    std::memcpy(dst, src, len);
}

// ---- THE POLICY (state/blob_snapshot.h) --------------------------------------------------------
//
// Everything above is what makes THIS blob this blob; everything the engine does with it -- the
// fingerprint, the size, the canonical hash, capture, and the validate-then-write import -- is
// shared with LIB-WORLD and lives there.
struct boot_policy {
    static constexpr const uint8_t *MAGIC  = boot::MAGIC;
    static constexpr uint32_t       FORMAT = boot::FORMAT;

    static int                                    count() { return BOOT_SNAPSHOT_BLOCK_COUNT; }
    static const mh::state::blob::snapshot_block &at(int i) {
        // The generated table and the engine's block type are the same three fields in the same
        // order; the generated header keeps its own name so the .gen.h stays readable on its own.
        static_assert(sizeof(boot_snapshot_block) == sizeof(mh::state::blob::snapshot_block),
                      "the generated block table must be layout-compatible with the engine's");
        return *reinterpret_cast<const mh::state::blob::snapshot_block *>(&BOOT_SNAPSHOT_BLOCKS[i]);
    }
    static uint32_t canonical_len(const mh::state::blob::snapshot_block &b) {
        return block_len(reinterpret_cast<const boot_snapshot_block &>(b));
    }
    template <class S>
    static void emit(const mh::state::blob::snapshot_block &b, S &s,
                     mh::state::blob::capture_stats *stats) {
        emit_block(reinterpret_cast<const boot_snapshot_block &>(b), s, stats);
    }
    static void apply(const mh::state::blob::snapshot_block &b, const uint8_t *src, uint32_t len) {
        apply_block(reinterpret_cast<const boot_snapshot_block &>(b), src, len);
    }
    // The TLO registry is legally unbound: it is carried in a DERIVED form and imported into this
    // TU's own table, so a host that never binds the 72-byte image region is not an error.
    static bool unbound_ok(const mh::state::blob::snapshot_block &b) { return b.rid == RID_TLO_; }
    static bool refuse_import() { return g_session_begun; }
    // THE `pointed-into` DECLARATION, CHECKED. G_TEXT_BLOCK is carried on the claim that a carried
    // region points into it -- a claim no build-time derivation can make, because the pointers are
    // written at runtime. If nothing points into the arena, the claim has stopped being true and
    // this capture is not the snapshot the accounting describes.
    static int post_capture(const mh::state::blob::capture_stats &st) {
        return st.ptrs_in_arena == 0 ? BOOT_ERR_NO_TEXT_PTRS : BOOT_OK;
    }
};

} // namespace

// ---- the public face: the SHARED engine, over the policy above ---------------------------------

uint32_t schema_fingerprint() {
    return mh::state::blob::schema_fingerprint<boot_policy>();
}

size_t blob_size() {
    return mh::state::blob::blob_size<boot_policy, blob_header>();
}

uint64_t canonical_hash() {
    return mh::state::blob::canonical_hash<boot_policy>();
}

int capture(void *buf, size_t cap, size_t *out_len) {
    blob_header h;
    std::memset(&h, 0, sizeof(h));
    mh::state::blob::capture_stats st;
    // The restore self-check's two counters are stamped by the CALLER (seams/harness.cpp) after the
    // deferred parse runs and the restore has been compared, so capture() leaves them zero. The
    // engine writes the shared prefix and copies the whole header, so a field this function did not
    // set is a field the caller still owns.
    const int rc = mh::state::blob::capture<boot_policy, blob_header>(buf, cap, out_len, &h, &st);
    if (rc != mh::state::blob::OK) return rc;
    // Re-stamp the tallies INTO the written blob: the engine copied the header before emit() had
    // finished counting, which it must (payload_len and content_hash are only known then).
    blob_header *out         = static_cast<blob_header *>(buf);
    out->text_ptrs_in_arena  = st.ptrs_in_arena;
    out->text_ptrs_out_arena = st.ptrs_out_arena;
    return BOOT_OK;
}

int import(const void *blob, size_t n) {
    return mh::state::blob::import <boot_policy, blob_header>(blob, n);
}

// ---- the ordering latch ---------------------------------------------------------------------------

void note_session_begun() {
    g_session_begun = true;
}
bool session_begun() {
    return g_session_begun;
}
void reset_session_latch_for_test() {
    g_session_begun = false;
}

// ---- cfg_GetTloIndex, over the imported table ------------------------------------------------------
//
// The original @0x004b9582 walks the 8-entry registry with a case-insensitive compare and returns 1
// (Jungle) when nothing matches -- the `if (7 < i) return 1;` fallthrough, transcribed here. This is
// what closes the sim/libtrans/sim_lt_cfg_planet.cpp census site: the lookup was always ours to do,
// it just had no data of its own to do it over.
uint8_t tlo_index_for(const char *file_name) {
    materialise_tlo();
    if (file_name == nullptr) return 1u;
    for (uint32_t i = 0; i < TLO_ENTRIES; ++i) {
        const char *a = file_name;
        const char *b = g_tlo[i].name;
        for (;; ++a, ++b) {
            char ca = *a, cb = *b;
            if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
            if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
            if (ca != cb) break;
            if (ca == '\0') return g_tlo[i].index;
        }
    }
    return 1u;
}

} // namespace mh::state::boot

// ---- the C export (libmh.h) ------------------------------------------------------------------------

extern "C" int libmh_import_snapshot(const void *blob, size_t n) {
    return mh::state::boot::import(blob, n);
}
