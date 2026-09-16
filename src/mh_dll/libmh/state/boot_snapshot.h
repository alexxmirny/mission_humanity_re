//
// state/boot_snapshot.h -- the post-cfg prototype snapshot: capture, import, and the canonical
// hash that makes the two comparable (tracker LIB-BOOT).
//
// WHY A SNAPSHOT AND NOT A PARSER. The cfg -> prototype load runs ONCE per process (boot stage 5;
// _G_LLM_BOOT_STAGE has an init writer and its own INC, so the stage machine cannot repeat) and
// what it produces is read-only afterwards. Importing that output is strictly simpler than owning
// the parser that produces it, and the parser is the expensive half: 56 cfg_* functions / ~65 KB,
// none of them ours. The same trick is REJECTED everywhere else -- the recurring roster writers
// fire all session long and are owned as sim code -- and that distinction is the whole design.
//
// WHAT IS CARRIED IS DERIVED, NOT LISTED. addr/mh_boot_snapshot.gen.h is generated from the write
// closure of boot stage 5's root by tools/gen_boot_snapshot.py, which also refuses to be green
// while any region that closure writes lacks a verdict. This file walks that table and nothing
// else, so "what is in the snapshot" cannot drift away from "what was accounted for".
//
// THREE THINGS THAT ARE NOT OBVIOUS AND ARE LOAD-BEARING:
//
// 1. THE WALK IS state_sink/state_source IN PERSIST MODE, deliberately NOT region_view::emit_slice.
//    emit_slice is indexed by HASH_REGIONS[] -- the determinism set -- and most of this table is
//    MF_VIEW/MF_SAVE only: Progress, Building, Unit, Anim, Projects, Weapon and Upgrades are all
//    outside the determinism hash. Routing this walk through emit_slice would silently drop the
//    majority of the snapshot. It is also why `Progress` being wrong IS visible here while it is
//    invisible to every determinism oracle we own (dead-ends G128) -- this hash is not that hash.
//
// 2. G_TEXT_PTRS IS NORMALISED TO OFFSETS, in the blob AND in the hash. The region is 806 absolute
//    pointers into G_TEXT_BLOCK. Carrying them verbatim would (a) make the blob unusable by a host
//    that binds the arena anywhere else and (b) make the hash a function of WHERE the bytes live
//    rather than of what they say -- which is exactly the peer-local-pointer failure state_sink.h's
//    own banner warns about. An entry inside the arena is stored as `PTR_TAG | offset`; bit 31 is
//    free because this image's whole address space ends below 0x01100000. Entries outside the arena
//    are stored verbatim and COUNTED, so a pointer that stops landing in the block is visible.
//
// 3. IMPORT MUST PRECEDE THE FIRST SESSION BEGIN, and that is enforced rather than documented.
//    `Planets` is one region and TWO writers: cfg_ConstructPlanets fills slots 1..N once at boot,
//    while llm_strat_scenario_planet_clone rewrites the reserved slot 0x1f at EVERY session start
//    -- and READS what it does not overwrite (Planets[1].icon_index @0x0045baa0). G_TEXT_PTRS has
//    the same split (cfg_SaveText at boot; the same clone per session). So an import after a
//    session has begun would overwrite live session state with boot values. `import()` refuses with
//    BOOT_ERR_SESSION_BEGUN once the latch below is set.
//
#pragma once
#include <cstddef>
#include <cstdint>

#include "addr/mh_boot_snapshot.gen.h"
#include "addr/mh_regions.gen.h"
#include "state/blob_snapshot.h"

namespace mh::state::boot {

// "MHBOOT\0\1" -- 8 bytes so the header stays 8-aligned and a truncated file cannot look valid.
inline constexpr uint8_t  MAGIC[8]      = {'M', 'H', 'B', 'O', 'O', 'T', 0, 1};
inline constexpr uint32_t FORMAT        = 2u; // 2: the header carries the restore self-check
inline constexpr uint32_t PTR_TAG       = 0x80000000u;
inline constexpr uint32_t TLO_NAME_CAP  = 32u; // the shipped names are "Jungle.tlo"-sized
inline constexpr uint32_t TLO_ENTRIES   = 8u;
inline constexpr uint32_t TLO_ENTRY_LEN = TLO_NAME_CAP + 4u; // name[32] + index (dword, for align)

// The prefix is the SHARED one (state/blob_snapshot.h) and the tail is this item's. Nesting rather
// than repeating the six prefix fields is byte-for-byte the same layout -- same fields, same order,
// same offsets -- so blobs captured before the engine was extracted still import.
struct blob_header {
    mh::state::blob::common_header base;

    uint32_t text_ptrs_in_arena;  // how many G_TEXT_PTRS entries were normalised
    uint32_t text_ptrs_out_arena; // ... and how many were not. Both reported, neither assumed.

    // ---- THE CAPTURE'S OWN RESTORE SELF-CHECK, carried with the blob ---------------------------
    //
    // The capture CALLS llm_tutorial_load_script (the deferred cfg-class parse) and then puts back
    // every region that call writes. "Puts back" is a claim, and it was very nearly left as one: the
    // obvious proof -- run the tutorial with the capture armed and diff the frame -- is not a proof
    // at all, because that frame is not reproducible under an armed harness even with the capture
    // DISARMED (measured 2026-09-11: 15.2% vs 15.5% against the same baseline, i.e. the instability
    // is the harness, not the capture). A proxy that cannot distinguish the two hypotheses is not
    // evidence for either.
    //
    // So the capture checks itself, at the moment it can: it snapshots the restore set BEFORE the
    // call, and after restoring compares live memory against that snapshot.
    //
    //   restore_changed   how many restore regions the parse ACTUALLY modified. A zero here means
    //                     the restore was a no-op over untouched memory and the check proved
    //                     nothing -- the vacuous-pass shape, so the oracle refuses it.
    //   restore_mismatch  how many differ from their pre-call bytes AFTER the restore. Must be 0.
    uint32_t restore_changed;
    uint32_t restore_mismatch;
};
static_assert(sizeof(blob_header) == 48, "the boot blob header layout is a file format");
static_assert(offsetof(blob_header, text_ptrs_in_arena) == 32, "ditto -- the prefix is 32 bytes");

// The refusal codes are the SHARED ones (state/blob_snapshot.h); these names are kept because they
// are what the arms, the harness log and every recorded rc= in the item's evidence spell. -8 is
// spelled BOOT_ERR_NO_TEXT_PTRS here and ERR_POLICY there: the engine has no opinion about what a
// policy asserts at capture, and this policy asserts exactly one thing.
enum boot_err : int {
    BOOT_OK                = mh::state::blob::OK,
    BOOT_ERR_ARG           = mh::state::blob::ERR_ARG,           // null ptr / buffer too small
    BOOT_ERR_MAGIC         = mh::state::blob::ERR_MAGIC,         // not a boot snapshot / bad format
    BOOT_ERR_SCHEMA        = mh::state::blob::ERR_SCHEMA,        // captured against another table
    BOOT_ERR_SESSION_BEGUN = mh::state::blob::ERR_SESSION_BEGUN, // trap (a): Planets[0x1f] rewritten
    BOOT_ERR_TRUNCATED     = mh::state::blob::ERR_TRUNCATED,     // payload ends inside a block
    BOOT_ERR_UNKNOWN_RID   = mh::state::blob::ERR_UNKNOWN_RID,   // a rid this build does not have
    BOOT_ERR_UNBOUND       = mh::state::blob::ERR_UNBOUND,       // a carried region has no base
    BOOT_ERR_NO_TEXT_PTRS  = mh::state::blob::ERR_POLICY,        // nothing pointed into the arena
};

// The fingerprint of the generated table (rid, len and name of every block, in order). A blob
// carrying a different one was captured against a different schema and is refused rather than
// written into memory that has since moved.
uint32_t schema_fingerprint();

// The exact number of bytes `capture` will write. A caller sizes its buffer with this.
size_t blob_size();

// The CANONICAL content hash over the block table, read out of live memory through live_base().
// Base-independent by construction (see note 2 above), which is what makes an in-binary capture
// comparable with an import into a host's own arena.
uint64_t canonical_hash();

// Read the live prototype tables into `buf`. Returns BOOT_OK, or a boot_err. `out_len` receives the
// bytes written. The capture does NOT call the cfg parser; it is a read of what the parser left.
int capture(void *buf, size_t cap, size_t *out_len);

// Write a captured blob back through live_base(). Refuses (without writing anything) on a bad
// header, a schema mismatch, a truncated payload, an unknown or unbound region, or a session that
// has already begun. Validation is complete BEFORE the first byte is written, so a rejected import
// cannot leave the world half-answered.
int import(const void *blob, size_t n);

// The ordering latch (note 3). Set from the two session-begin bodies; monotone.
void note_session_begun();
bool session_begun();
// Test support ONLY -- an arm that proves the refusal fires has to be able to un-fire it.
void reset_session_latch_for_test();

// The tileset lookup cfg_GetTloIndex @0x004b9582 performs, over the IMPORTED registry rather than
// over the original image's `char *` table. Case-insensitive; returns 1 (Jungle) for an unknown
// name, matching the original's fallthrough.
uint8_t tlo_index_for(const char *file_name);

} // namespace mh::state::boot
