//
// state/spine.cpp -- the DETERMINISTIC SPINE of libmh.h, and the C face of libmh_import_world
// (tracker LIB-REF).
//
// libmh.h declares a small replay spine -- bind / import / submit_order / sim_step / state_hash /
// save / load -- and until this TU existed only the binds and libmh_import_world's byte half were
// implemented. This file lands the four entries a recorded replay actually drives, and moves the C
// entry of the world import here so that entry can discharge the obligations the INBOUND TABLE
// already says it serves. Each is a thin, typed forwarding into a body libmh already owns: nothing
// here reimplements anything, and that is deliberate -- a spine entry that contained logic would be
// a second implementation of a translated body, verified by nobody.
//
// ---- WHY libmh_import_world LIVES HERE AND mh::state::world::import() DOES NOT -------------------
//
// `gen_libmh_inbound.py` disposition's FOUR rows onto libmh_import_world: llm_map_setup_dimensions,
// llm_map_region_pool_reset, llm_map_build_regions and
// llm_map_init_region_route_step_deltas. (LIB-SPINE-API re-classed them: they used to print as
// `exception: spine`, a blanket excuse keyed on the entry's NAME shape; they now print as
// `measured-impossible / boot-blob` with the reason recorded per row, because this entry's contract
// is blob-in and hosted the world arrives from the original .MP parser instead.) Those are exactly
// the RE-DERIVE obligations in
// tools/data/world_snapshot_dispositions.json -- the step-0 state a bound-region dump structurally
// cannot carry. Until now the table asserted the coverage and the doc asserted the obligation, and
// nothing connected them: a host that imported the blob and started stepping would run on unbuilt
// torus masks and on a nav-region graph made of ANOTHER PROCESS'S HEAP POINTERS.
//
// The re-derive cannot go inside `mh::state::world::import()`, because that function is the BYTE
// ENGINE and `net_selftest worldtest` compares imported memory to the blob payload byte-for-byte
// (arm C) and against the blob's own content hash (arm B). A re-derive there would move both, and
// LIB-WORLD's oracle would be measuring this file instead of the fixture. So the split is: the C++
// `import()` stays bytes-in-bytes-out and worldtest keeps testing exactly what it always did; the C
// ENTRY -- the one a host binds -- is bytes THEN the re-derives, which is the whole contract.
//
// THE ORDER IS THE ORIGINAL'S OWN, not a plausible one (call graph, /eng/mh.exe):
//     map_ReadMap @0x004a3251
//       -> llm_map_setup_dimensions @0x004989c2        (after the width/height globals are set)
//       -> ... the six planes ...
//       -> llm_map_load_passable_plane
//            -> llm_map_region_pool_reset @0x004234b8  (empty the allocator FIRST)
//            -> llm_map_build_regions    @0x00423335   (full rebuild from the passable plane)
// The blob has already delivered width/height and the passable plane by the time we run, so this
// reproduces that sequence over imported bytes instead of over a parsed .MP file.
//
// ---- EXCEPT FOR THE LAST STEP, WHICH IS NOW THE OTHER ORIGINAL (schema 2, 2026-09-11) ------------
//
// `llm_map_build_regions` NO LONGER RUNS HERE. A rebuild from the step-0 `passable` plane is not the
// recording's decomposition -- the recording's is that same builder's output THEN mutated by
// landing, spawn_ai_base and every building placement, and the two disagree on 2181 of 65536 tiles
// (measured; the replay diverged at step 290). So the decomposition is CARRIED, in the blob's FORMAT
// 2 nav trailer, and applied by `world::import_nav()`. The model for that is the game's OTHER map
// path: `llm_map_load_regions` @0x00424a68, which does not build either -- it deserializes the graph
// by index and re-resolves the pointers. state/nav_trailer.h has the format and the full reasoning.
//
// Two consequences of dropping build_regions, both handled below and both easy to miss:
//   * it was the only caller of llm_map_init_region_route_step_deltas @0x00423299 on this path, so
//     that call becomes explicit -- exactly as llm_map_load_regions makes it explicit at its top;
//   * pool_reset's tail clobbers G_LAST_MAP_INDEX and `counter`, two CARRIED bound regions that
//     build_regions used to overwrite with its own (also wrong) numbers. They are now bracketed.
//
// ---- THE ZEROING STEP, WHICH IS NOT OPTIONAL AND IS NOT IN THE ORIGINAL --------------------------
//
// llm_map_region_pool_reset DRAINS THE ACTIVE LIST AND THEN CALLS free() ON EVERY NODE. In the
// original that is correct: the nodes were malloc'd by this process. After an import they were
// malloc'd by the RECORDING process -- measured at LIB-WORLD, 23 of 829 blocks differ between two
// captures of the same scenario and MAP_REGION_GRID alone accounts for 178,851 of the 180,526
// differing bytes, every differing dword a heap address. Calling pool_reset over those heads would
// free wild pointers. So the four carriers of that graph are ZEROED first, which is both what makes
// pool_reset safe and what the graph's disposition asks for ("the importing host must REBUILD it
// from the carried passable plane"). pool_reset then runs in its original position over two empty
// lists -- a legal no-op that leaves the two scalars where the original leaves them, so the row is
// genuinely served rather than skipped.
//
// STRAT_PATH_JOB_RESULT_TABLE is zeroed for the same reason and on its own recorded measurement
// (100 heap pointers, 398 differing bytes across the same two captures): "the host starts with an
// empty table rather than an imported one".
//
// None of the five is a determinism-hash slice, so zeroing them cannot move the step-0 hash. That
// is a claim the acceptance tests rather than assumes: LIB-REF's green step 1 is the full C entry
// -- bytes, zeroing, re-derives -- reproducing the fixture's recorded step-0 `state` exactly.
//
// ---- THE PROCESS-LOCAL POINTER CLASS: the FIXUP TRAILER (mp:X3a) -------------------------------
//
// A step-0 region dump carries whatever the bytes are, pointers included. Two different classes of
// pointer live in it and they were conflated here for a long time:
//
//   (a) A pointer from one STOCK region into another (the seven UI widget-list heads, G_TEXT_PTRS ->
//       G_TEXT_BLOCK, tile_objects_ptr, fow_ptr). For a LIVE mh.exe -> mh.exe import this class is
//       ALREADY SAFE: mh.exe is fixed at 0x00400000 and both peers bind every carried region at its
//       stock VA, so the value means the same thing in both processes. It bites only a STANDALONE
//       host that binds an arena elsewhere -- libref_host re-stamps G_TEXT_PTRS itself, from a
//       generated host-only base table (the 2026-09-11 step-2001 fault).
//   (b) A PROCESS-LOCAL value: a heap / private-memory address (the .TLO buffer, the framebuffer, a
//       COM device, a dialog the sender allocated), or an address inside one of OUR ASLR'd DLL images
//       or a region relocated into one. Copied verbatim it faults the receiver on first use (the
//       rig's 0x691A276C was the sender's image base + 0x10276C). Only the CAPTURING process knows
//       where its own heap and DLLs are, so the capture classifies every carried dword against its
//       own address map and writes the result as an optional trailer -- state/world_fixup.h.
//
// The trailer replaces the old seven-entry LIVE_PRESERVE_REGIONS scan as the primary mechanism (that
// scan is kept as a measured floor and cross-check: anything it still restores after the trailer has
// been applied is logged "(legacy rule)", a free falsifier of the trailer's coverage). The census in
// the header (`region_head_ptrs`, `registry_span_ptrs`) stays a capture-side diagnostic: it is blind
// to interior pointers, heap and DLL values by construction.
//
#include <cstring>

// The C facade by relative path, exactly as state/host_bind.h reaches it: libmh/include is not on
// any module's include path, and adding it for one TU would be a silent layering change.
#include "../../libmh/include/libmh.h"

#include "addr/mh_regions.gen.h"
#include "addr/mh_world_snapshot.gen.h" // mp:X3: the carried-block table the preserve pass scans
#include "ai/ai_state.h"                // ai_say -- the shared trace sink the liveness line writes to
#include "orders/admission_log.h"
#include "orders/order_queue.h"
#include "sim/hostreach/sim_h_map_region_prep.h"
#include "sim/libtrans/sim_lt_map_region_pool.h"
#include "sim/libtrans/sim_lt_map_setup_dimensions.h"
#include "sim/resid/sim_pathfinder_init.h"
#include "sim/sim_register_bldg_type_callbacks.h"
#include "sim/sim_register_state_handlers.h"
#include "sim/sim_state.h"
#include "sim/sim_step.h"
#include "state/region_runtime.h"
#include "state/world_fixup.h"
#include "state/world_snapshot.h"

namespace {

// LIB-SPINE-API: the per-entry one-shot liveness line, the SAME line state/host_in.cpp's `enter()`
// emits for the 45 inbound entries -- so one instrument can be asked which entries a run entered
// and the spine is not a hole in its answer. It has to be spelled here rather than reused, because
// the spine entries are deliberately outside libmh_host_in.gen.h's ENTRIES[] table (they are
// libmh.h's, not the inbound surface's) and so have no entry id, no runtime slot and no gate.
//
// NO GATE IS THE POINT, not an omission. libmh_in_open's refusal protects entries that read bound
// state through the inbound surface; the spine is what a host drives before and around that, and
// making libmh_sim_step refuse on a closed inbound surface would be a behaviour change smuggled in
// behind an instrument. This logs and returns.
void in_live(bool &fired, const char *name) {
    if (fired) return;
    fired = true;
    mh::ai::ai_say("; [libmh_in] %s call #1\n", name);
}

// The carriers of the nav decomposition's heap-pointer graph, plus the path-job result table. Each
// row is a region the fixture carries VERBATIM and whose carried content is a set of pointers into
// the recording process's heap. See the banner.
// NOTE THE ABSENCE OF RID_MAP_REGION_GRID -- it is handled separately below, and the reason is a
// measurement rather than tidiness.
// ---- mp:X3: THE PROCESS-LOCAL RESOURCE HANDLES, PRESERVED ACROSS A LIVE IMPORT ------------------
//
// MEASURED FIRST, ON THE RIG, AND THE MEASUREMENT IS THE WHOLE JUSTIFICATION. mp:X1b left the
// importing peer dying with 0xC000041D within about a sim step of a successful import, and two
// causes were indistinguishable from outside: the REWIND (live-step inputs over an older world) or
// these seven re-derives run against a live session. mp:X3 asked the cheap question -- import and
// then HOLD THE SIM, so no step body ever runs over the imported world -- and the peer died anyway,
// with the crash marker naming it exactly:
//
//     code=0xc0000005  address=0x004a5759  read of 0x0F39E630 (ESI)
//     0x004a5759 is `MOV AL, byte ptr [ESI]` inside llm_strat_render_ground_tile @0x004a5167,
//     where ESI = file_ptr + 0x200 + terrain_id * 0x400.
//
// `file_ptr` is `map::tlo::g::file_ptr` (RID_FILE_PTR, 4 bytes, MF_VIEW|MF_MEASURED) -- the pointer
// to the .TLO tile-graphics buffer THIS PROCESS allocated when it loaded the map. The blob carries
// it verbatim, so after the import it holds the SENDING process's heap address, and the first
// strategic frame the receiver draws blits ground tiles out of it. Not the sim. Not the rewind. The
// RENDERER, on the next frame, through a carried heap pointer.
//
// SO THIS IS A CARRY-POLICY FIX, NOT A RE-DERIVE. The nav graph and the dispatch tables are
// re-derived below because their content can be RECOMPUTED here. A loaded-asset pointer cannot: the
// buffer it names is already correct in this process and nothing in the blob describes it. The
// right answer is simply not to overwrite it -- which is the shape (4) already uses for
// last_map_index / region_alloc_counter one step further on, and for the same reason.
//
// THE POPULATION IS SIX AND IT IS ENUMERATED, not guessed. Of the 829 carried blocks, 357 are four
// bytes wide and exactly six of those are named as pointers by the registry. Every one is MF_VIEW
// and NONE of them backs a determinism-hash slice (no entry in the hash manifest), so this pass
// CANNOT move the step hash -- a property the SNAPCAP/SNAPIMP comparison re-measures on every run
// rather than a claim made here. A seventh would have to be a pointer the registry does not call
// one, which is a naming bug to fix at the registry rather than a reason to widen this by guesswork.
//
// WHAT IT DOES NOT COVER, stated because the honest scope is narrower than "pointers". A pointer
// living at an INTERIOR offset of a bigger carried region is invisible to this table -- that is the
// same blindness the capture's own `region_head_ptrs` census has, and state/spine.cpp's banner
// already records what it cost once (G_TEXT_PTRS, 806 carried dwords, none counted, faulting in
// format_core<wchar_t> after 2000 clean steps). This pass makes the measured crash go away and
// makes the NEXT one cheap to name -- the crash marker names a faulting instruction, and one
// decompile turns that into a region -- it does not close the class.
constexpr mh::state::region_id LIVE_PRESERVE_REGIONS[] = {
    mh::state::RID_FILE_PTR,                    // map::tlo::g::file_ptr -- the .TLO tile-graphics buffer.
                                                // MEASURED: the first one the rig crash marker named.
    mh::state::RID_GFX_DRAW_SURFACE,            // _G_LLM_GFX_DRAW_SURFACE -- the shared secondary
                                                // surface. MEASURED: the SECOND one, after the first
                                                // fix moved the crash into llm_strat_minimap_render.
    mh::state::RID_TILE_VIS_MAP_PTR,            // the per-tile visibility plane the ground blitter reads
    mh::state::RID_TACT_LOS_CACHE_PTR,          // the LOS/fog cache plane, read on the same path
    mh::state::RID_TACT_FOV_DIR_TABLE_PTR,      // the tactical FOV direction table
    mh::state::RID_MENU_SAVE_NAME_PTR,          // the menu's save-name buffer
    mh::state::RID_PTR_S_MENUBCK1_GFX_00604288, // the menu background .GFX
    mh::state::RID_HWND_00824FE8,               // the main window HWND (mp:X3a): WinMain's pump redraws it to make every frame
};

// ---- ...AND THE RULE THE LIST ALONE COULD NOT BE -------------------------------------------------
//
// THE EXPLICIT LIST WAS MEASURED INSUFFICIENT ON ITS SECOND RUN, which is the honest reason this
// criterion exists. Enumerating the six carried 4-byte regions the registry NAMES as pointers fixed
// the ground-tile blitter and moved the fault straight into `llm_strat_minimap_render` @0x004a17b3,
// writing through `_G_LLM_GFX_DRAW_SURFACE` -- a carried 4-byte MF_VIEW region that holds a pointer
// and is not called one. So "named as a pointer" was a property of the NAMING, not of the class, and
// a list grown one rig run at a time is a bisection with a crash per step.
//
// THE CRITERION IS ABOUT THE VALUE, NOT THE NAME. A carried block is preserved when all of:
//   * it is FOUR BYTES and its region is MF_VIEW with neither MF_SAVE nor MF_HASH -- presentation
//     state the receiving process already owns, and provably not a determinism slice;
//   * the LIVE value looks like a process-local heap handle (4-aligned, above the image's extended
//     .bss top, below the user/kernel split); and
//   * the BLOB's value looks like one too and is DIFFERENT. Both halves matter: requiring the
//     incoming value to be a plausible handle is what keeps an ordinary counter that happens to be
//     large from being preserved, and requiring them to differ is what makes the pass a no-op on a
//     capture whose allocator landed in the same place.
//
// WHY THIS CANNOT MOVE THE STEP HASH, which is the property that makes a value-shaped rule safe at
// all: MF_HASH is excluded by construction, so no region the determinism manifest reads can be
// preserved, and the SNAPCAP/SNAPIMP comparison re-measures that on every run rather than trusting
// this paragraph. The residual risk is the opposite one -- preserving a 4-byte MF_VIEW scalar that
// merely looks like a pointer on both peers -- and it is bounded by what MF_VIEW means: view state
// the importing process is already the right owner of. Every preserve is LOGGED by name, so the
// population is auditable from any run's own log instead of being argued about here.
//
// WHAT IT STILL DOES NOT COVER: a pointer at an INTERIOR offset of a larger carried region. That is
// the same blindness the capture's `region_head_ptrs` census has, and the one that cost LIB-REF-LIVE
// a fault in format_core<wchar_t> over G_TEXT_PTRS after 2000 clean steps. Closing it needs a
// generated (holder rid, offset, pointee rid) fixup trailer and a re-record -- a blob-format change.
inline bool looks_like_process_handle(uint32_t v) {
    // The image, including the .bss extended to 0x1064dff, ends below 0x01100000; every heap
    // address this has been measured against (0x0B8A0030, 0x654DDDD0, 0x034F07BC) is above it.
    return v >= 0x01100000u && v < 0x7ff00000u && (v & 3u) == 0u;
}

// The regions the importer zeroes live in state/world_fixup.h (ZEROED_ON_IMPORT), so the capture-side
// classifier and this file read ONE list.

// MAP_REGION_GRID IS HALF POINTER AND HALF DATA, AND CLEARING IT WHOLE THREW THE DATA AWAY.
//
// `llm_map_region_cell` is EIGHT bytes -- `{llm_map_region *region; uint32_t terrain_flags;}` -- so a
// clear_region() over the 512 KB block zeroes 65536 obstacle-proximity flags along with the 65536
// foreign pointers. Only the pointer half is foreign; `terrain_flags` is real carried world state
// that llm_map_compute_obstacle_proximity_flags computed in the recording process and that nothing on
// the import path recomputes.
//
// MEASURED, AND IT PREDATES SCHEMA 2: dumping the standalone host's grid after import and comparing
// it with the recording's own carried block gives `terrain_flags` all-zero on every one of the 59020
// assigned cells, against a real carried distribution (30878 cells at 1, 11715 at 13, 9292 at 29).
// Under schema 1 that was invisible for the usual reason -- MAP_REGION_GRID is MF_VIEW|MF_MEASURED, so
// no determinism slice reads it -- and build_regions, which ran next, writes the `region` half only
// (0x00423335 lines 22-27); it never restored the flags either. Schema 2 simply made the loss
// legible, because now the partition it is sitting next to is exactly right.
//
// So the pointer COLUMN is cleared and the flag column is left alone. The nav trailer then writes
// every one of the 65536 pointer dwords unconditionally, which is what makes clearing them merely
// belt-and-braces rather than load-bearing -- but they are still cleared, because "no foreign pointer
// survives any window in which something might walk it" is a property worth keeping cheap.
// The two halves of the preserve pass. SAVE runs before world::import() writes a byte; RESTORE runs
// immediately after it returns OK and BEFORE anything else -- before the zeroing, before
// pathfinder_init, before import_nav -- so there is no window in which this process holds a foreign
// resource handle at all. A frame can be drawn from another thread's pump at any instant; the
// window is what killed the peer, so the fix does not open a smaller one.
constexpr int LIVE_PRESERVE_N = (int)(sizeof(LIVE_PRESERVE_REGIONS) / sizeof(LIVE_PRESERVE_REGIONS[0]));

// The candidate set is every carried 4-byte MF_VIEW-only block; 335 of the 829 blocks qualify in
// this build. Sized with headroom and CHECKED rather than assumed -- a build that grew past the cap
// would otherwise silently stop protecting the tail of the table, which is the failure mode this
// whole pass exists to remove.
constexpr int LIVE_PRESERVE_SCAN_CAP = 512;

struct handle_slot {
    uint16_t block; // index into WORLD_SNAPSHOT_BLOCKS
    uint32_t live;  // the value this process held before the byte engine ran
};

struct handle_set {
    handle_slot slot[LIVE_PRESERVE_SCAN_CAP];
    int         n;
    bool        overflowed;
};

bool preserve_candidate(int i) {
    const mh::state::region_id r = mh::state::WORLD_SNAPSHOT_BLOCKS[i].rid;
    if (mh::state::WORLD_SNAPSHOT_BLOCKS[i].len != 4u) return false;
    const uint8_t f = mh::state::REGIONS[r].manifests;
    if ((f & mh::state::MF_VIEW) == 0) return false;
    if ((f & (mh::state::MF_SAVE | mh::state::MF_HASH)) != 0) return false;
    return true;
}

bool on_explicit_list(mh::state::region_id r) {
    for (int k = 0; k < LIVE_PRESERVE_N; ++k)
        if (LIVE_PRESERVE_REGIONS[k] == r) return true;
    return false;
}

// SAVE runs before world::import() writes a byte.
void live_handles_save(handle_set &out) {
    out.n          = 0;
    out.overflowed = false;
    for (int i = 0; i < mh::state::WORLD_SNAPSHOT_BLOCK_COUNT; ++i) {
        const mh::state::region_id r = mh::state::WORLD_SNAPSHOT_BLOCKS[i].rid;
        if (!preserve_candidate(i) && !on_explicit_list(r)) continue;
        uint32_t v = 0;
        // Through state/region_runtime.h, never a local ptr<>: that header is the only place
        // allowed to bind an address and check_sim_addresses enforces it. The rule caught this
        // file the first time the pass was written, which is the rule working.
        if (!mh::state::read_region_u32(r, &v)) continue;
        if (out.n >= LIVE_PRESERVE_SCAN_CAP) {
            out.overflowed = true;
            break;
        }
        out.slot[out.n].block = (uint16_t)i;
        out.slot[out.n].live  = v;
        ++out.n;
    }
}

// RESTORE runs immediately after import() returns OK and BEFORE anything else -- before the
// zeroing, before pathfinder_init, before import_nav -- so there is no window in which this process
// holds a foreign resource handle at all. A frame can be drawn from the game's own pump at any
// instant, and a window is exactly what killed the importing peer, so the fix does not open a
// smaller one.
//
// AND IT SAYS SO PER HANDLE, with both values. A silent preserve is the shape that makes the next
// reader wonder whether the pass ran; naming the incoming value is what tells "the blob carried
// another process's pointer" (the case this exists for) apart from "the two agreed anyway" (a
// capture whose allocator happened to land in the same place -- the run that would make the pass
// look unnecessary).
void live_handles_restore(const handle_set &saved) {
    if (saved.overflowed)
        mh::ai::ai_say("; [import] preserve scan OVERFLOWED at %d slots -- the tail of the block "
                       "table was NOT protected\n",
                       LIVE_PRESERVE_SCAN_CAP);
    for (int k = 0; k < saved.n; ++k) {
        const mh::state::region_id r        = mh::state::WORLD_SNAPSHOT_BLOCKS[saved.slot[k].block].rid;
        uint32_t                   imported = 0;
        // The zeroed-on-import set is cleared a few statements later anyway, and the trailer
        // deliberately does not describe it; restoring it here only made the "(legacy rule)" falsifier
        // report a false alarm on every import.
        if (mh::state::world::is_zeroed_on_import(r)) continue;
        if (!mh::state::read_region_u32(r, &imported)) continue;
        const uint32_t live = saved.slot[k].live;
        if (imported == live) continue;
        const bool forced = on_explicit_list(r);
        if (!forced && !(looks_like_process_handle(live) && looks_like_process_handle(imported)))
            continue;
        mh::state::write_region_u32(r, live);
        mh::ai::ai_say("; [import] preserved %s: blob %08X -> live %08X (process-local handle%s) (legacy rule)\n",
                       mh::state::REGIONS[r].name, imported, live, forced ? ", listed" : "");
    }
}

void log_fixup(const mh::state::world::fixup_plan &plan, const mh::state::world::fixup_stats &fs) {
    if (!fs.present) {
        mh::ai::ai_say("; [import] fixup trailer ABSENT\n");
        return;
    }
    char mods[512];
    mh::state::world::fixup_module_list(plan, mods, sizeof(mods));
    mh::ai::ai_say("; [import] fixup trailer v1: entries=%u region=%u rebase=%u preserve=%u degraded=%u "
                   "carried=%u changed=%u (region=%u rebase=%u preserve=%u) hashed_skipped=%u unmapped=%u "
                   "modules=%s\n",
                   fs.entries, fs.region, fs.rebase, fs.preserve, fs.degraded, fs.carried, fs.changed,
                   fs.changed_region, fs.changed_rebase, fs.changed_preserve, fs.hashed_skipped,
                   fs.unmapped, mods);
    for (int i = 0; i < mh::state::WORLD_SNAPSHOT_BLOCK_COUNT; ++i) {
        const uint32_t a = plan.blk_region[i], b = plan.blk_rebase[i], c = plan.blk_preserve[i];
        if (a + b + c == 0) continue;
        mh::ai::ai_say("; [import] fixup holder %s region=%u rebase=%u preserve=%u changed=%u\n",
                       mh::state::WORLD_SNAPSHOT_BLOCKS[i].name, a, b, c, plan.blk_changed[i]);
    }
}

void zero_foreign_pointer_regions() {
    // Through mh::state::clear_region, not a local ptr<>: state/region_runtime.h is the only header
    // allowed to bind an address and check_sim_addresses enforces it. The rule caught this TU the
    // first time it ran here, which is the rule working.
    for (const mh::state::region_id r : mh::state::world::ZEROED_ON_IMPORT) mh::state::clear_region(r);

    mh::sim::sim_state st = mh::sim::state();
    for (int32_t x = 0; x < mh::sim::MAP_GRID_DIM; ++x)
        for (int32_t y = 0; y < mh::sim::MAP_GRID_DIM; ++y) st.own.region_cell_at(x, y).region = nullptr;
}

} // namespace

// ---- versioning ---------------------------------------------------------------------------------

// Returns the value it was COMPILED with, which is the entire point: a host compares its header's
// LIBMH_ABI_VERSION against this and refuses a mismatch, so a header and a binary that drifted apart
// are caught at the handshake instead of at the first struct that changed shape.
extern "C" uint32_t libmh_abi_version(void) {
    return LIBMH_ABI_VERSION;
}

// ---- the world import's C face ------------------------------------------------------------------

namespace {

// mp:X3c (RE-1): after a resync the imported world's SELECTION state names the HOST's selection --
// unit ids that index units[PlayerSide] of THIS peer, which is a different roster. Ownership is
// enforced at selection time, not at issue time (RE-1, measured: the issue path always builds (PlayerSide, id)), so a carried
// selection would let this peer's next order act on ids it never selected. Both representations are
// reset together, never one:
//   * the 10 control groups' counts, the UI-selected building, the click-select target id;
//   * every unit's ctrl_group_id (+0x2f), which is MF_HASH but MASKED by mask_ctrl_group (D2), so this
//     cannot move the verdict hash -- worldtest arm G proves that under the default masks.
constexpr int32_t CTRL_GROUP_COUNT = 10;
} // namespace

namespace mh::state::world {
void reset_local_selection() {
    mh::sim::sim_state st = mh::sim::state();
    for (int32_t g = 0; g < CTRL_GROUP_COUNT; ++g) st.own.ctrl_group_at(g).count = 0;
    st.own.ui_selected_bldg_index() = 0;
    st.own.click_select_target_id() = 0;
    const int32_t per               = st.own.caps().units;
    for (uint32_t p = 0; p < static_cast<uint32_t>(mh::orders::MAX_PLAYERS); ++p)
        for (int32_t u = 0; u < per; ++u) st.own.unit_at(p, u).ctrl_group_id = 0;
}
} // namespace mh::state::world

namespace {

// The shared body of libmh_import_world and libmh_import_world_resync. `rs` is null for the plain
// entry, which must stay byte-for-byte what it was (worldtest arms A-F and the committed fixtures).
struct resync_args {
    const uint32_t      *n_src; // [admission::SOURCES]
    libmh_resync_report *rep;   // may be null
};

int import_world_impl(const void *blob, size_t n, const resync_args *rs);

} // namespace

extern "C" int libmh_import_world(const void *blob, size_t n) {
    static bool fired = false;
    in_live(fired, "libmh_import_world");
    return import_world_impl(blob, n, nullptr);
}

namespace {

int import_world_impl(const void *blob, size_t n, const resync_args *rs) {
    // (0) mp:X3 -- READ the process-local resource handles BEFORE the byte engine overwrites them.
    //     Cheap (six dwords) and unconditional: a boot-time import reads six values it then writes
    //     back unchanged, and a live one is the case this exists for. See the table's banner.
    static handle_set live_handles; // ~4 KB; static rather than a 4 KB stack frame in a hosted DLL

    // (0a) mp:X3a -- VALIDATE the fixup trailer completely and stash the receiver's own dwords for
    //      every PRESERVE entry, all before the byte engine writes anything. A damaged trailer or an
    //      OURS module loaded with a different identity refuses here (-23) with the world untouched.
    //      An absent trailer (every blob recorded before it existed) is an empty plan: zero cost.
    static mh::state::world::fixup_plan fixup_plan_store; // ~330 KB: static, never a stack frame
    const mh::state::world::import_arm  arm = mh::state::world::real_import_arm();
    const int                           frc = mh::state::world::fixup_prepare(blob, n, fixup_plan_store, nullptr, &arm);
    if (frc != 0) {
        mh::ai::ai_say("; [import] fixup trailer REFUSED rc=%d -- world untouched\n", frc);
        return frc;
    }

    live_handles_save(live_handles);

    // (0b) mp:X3c -- a resync SAVES the keep-local regions (identity + transport + live horizons...)
    //      before the byte engine overwrites them, and skips only the session-begun latch.
    static mh::state::world::keep_local_store keep;
    uint32_t                                  kept_regions = 0, kept_bytes = 0;
    if (rs != nullptr) {
        mh::state::world::keep_local_save(keep, &kept_regions, &kept_bytes);
        mh::ai::ai_say("; [worldsync] IMPORT-BEGIN keep_local=%u regions/%u bytes latch=bypassed\n",
                       kept_regions, kept_bytes);
    }

    const int rc =
        rs != nullptr ? mh::state::world::import_resync(blob, n) : mh::state::world::import(blob, n);
    if (rc != mh::state::world::WORLD_OK) return rc;

    // (2) FIRST statement after the byte engine: put every process-local dword where THIS process
    //     needs it. Nothing may run between import() and here -- a frame drawn in the gap would
    //     walk the sender's pointers.
    {
        mh::state::world::fixup_stats fs;
        mh::state::world::fixup_apply(fixup_plan_store, &fs);
        log_fixup(fixup_plan_store, fs);
    }

    //     ...and put them back before ANYTHING can walk one. import() validates completely before
    //     its first write, so a refusal above leaves the live values in place and this is skipped
    //     along with everything else.
    live_handles_restore(live_handles);

    //     mp:X3c: ...and the keep-local regions, in the same no-window position (after the fixup so a
    //     PRESERVE entry cannot resurrect a blob value, before anything can read them).
    if (rs != nullptr) mh::state::world::keep_local_restore(keep);

    // (1) neutralise the imported graph BEFORE anything walks it -- see the banner.
    zero_foreign_pointer_regions();

    // (2) THE HEAP BLOCKS `general` POINTS AT, AND THE ORDER OF (2) BEFORE (3) IS THE BUG FIX.
    //
    //     `general` (RID_GENERAL, 40 B, MF_VIEW|MF_MEASURED) is carried VERBATIM, and two of its ten
    //     fields are pointers into the RECORDING process's heap: `pathfinder_params` (+0xc) and
    //     `pathfinder_workbuf` (+0x1c). The committed fixture carries 0x039A0FD8 and 0x0D623000.
    //     Step (3) below then DEREFERENCES THE FIRST OF THEM FOR A WRITE -- its last store is
    //     `general.pathfinder_params->width_mask` -- so before this call existed, every import wrote
    //     one byte to a recording-process address that means nothing here.
    //
    //     That was measured, not reasoned: `--steps 1` failed 3/30 and 6/100 runs, each failure
    //     corrupting exactly ONE inactive-player PLAYER_DATA slice and a DIFFERENT one per run, plus
    //     occasional 0xC0000005. The arithmetic closes it exactly: carried pointer 0x039A0FD8 +
    //     offsetof(mh_llm_strat_pathfinder_params, width_mask) == 0x16 == 0x039A0FEE, which is
    //     byte-for-byte the address the faulting runs reported writing. The run-to-run variation is
    //     the arena's base moving under ASLR: when 0x039A0FD8 happens to land inside the host's
    //     7.36 MB arena the write silently lands in whichever bound region covers that offset, and
    //     when it does not, the process faults. NEITHER outcome is visible to any hash-based oracle
    //     on its own, because `general` is MF_VIEW -- this is the MF_VIEW class in
    //     person, and it is why the standalone arena exists.
    //
    //     THE FIX IS THE ORIGINAL'S OWN BODY, not a patch. llm_strat_pathfinder_init @0x004614f9
    //     (boot stage 4, from llm_strat_mode_init) is exactly the function that creates these blocks:
    //     it allocates the 0x20-byte params block and the 0x82480-byte workbuf through the game's
    //     allocator, stores both into `general`, zero-fills the workbuf, refills the 100
    //     STRAT_PATH_JOB_RESULT_TABLE `path_steps` buffers that (1) just zeroed, and re-wires
    //     params->{passable,workbuf,job_result_table} to THIS process's bound addresses. So it is a
    //     RE-DERIVE of exactly the disposition-file kind: storage that is heap rather than a bound
    //     region, which a step-0 region dump structurally cannot carry.
    //     (world_snapshot_dispositions.json's `pathfinder_params->width_mask` row says the field is
    //     re-derived by llm_map_setup_dimensions. That is half the obligation: the field cannot be
    //     re-derived until the BLOCK IT LIVES IN has been re-created. The row wants correcting.)
    //
    //     None of the three regions it writes is a determinism-hash slice (`general` and
    //     STRAT_PATH_JOB_RESULT_TABLE are both MF_VIEW), so this cannot move the step-0 hash -- a
    //     claim the acceptance re-measures rather than assumes.
    mh::sim::pathfinder_init();

    // (3) the torus masks + pathfinder_params->width_mask, from the carried width/height.
    //     map_ReadMap's own first call after the dimensions land. Safe only because (2) ran.
    mh::sim::map_setup_dimensions();

    // (4) the allocator, in its original position (llm_map_load_passable_plane's first call). A
    //     no-op over the heads (1) just zeroed, which is what makes it safe to call at all.
    //
    //     BRACKETED, BECAUSE ITS TAIL CLOBBERS TWO CARRIED FACTS. pool_reset's last two stores are
    //     `last_map_index = 0` (0x00423510) and `region_alloc_counter = 1` (0x0042351a). Both are
    //     BOUND REGIONS the blob carries verbatim (G_LAST_MAP_INDEX @0x00708b18 and `counter`
    //     @0x0051de84, both MF_SAVE and NEITHER of them hashed), so after import they already hold
    //     the recording's values and pool_reset overwrites them with an empty pool's. Until schema 2
    //     that was masked: build_regions ran next and left its own rebuild's numbers there, which
    //     were also not the recording's -- wrong either way, and invisible either way, because no
    //     determinism slice reads them. Now that the pool is CARRIED, the carried numbers are the
    //     right ones and nothing downstream would restore them, so they are saved and put back.
    //     (`llm_map_load_regions` has the same need and meets it differently: it re-reads both from
    //     the file after its own reset. Same fact, same fix, different source.)
    const int32_t carried_last_index = mh::sim::state().own.last_map_index();
    const int32_t carried_alloc_ctr  = mh::sim::state().own.region_alloc_counter();
    mh::sim::pool_reset();
    mh::sim::state().own.last_map_index()       = carried_last_index;
    mh::sim::state().own.region_alloc_counter() = carried_alloc_ctr;

    // (5) the route step-delta table. llm_map_build_regions used to supply this as a side effect
    //     (0x00423335 -> 0x00423299) and was the only caller here, so dropping build_regions in (6)
    //     drops this with it -- hence the explicit call, which is exactly what llm_map_load_regions
    //     @0x00424a68 does at ITS top for the same reason: the loader does not build either.
    mh::sim::init_region_route_step_deltas();

    // (6) THE NAV DECOMPOSITION, CARRIED -- and llm_map_build_regions DOES NOT RUN.
    //
    //     This used to be `mh::sim::build_regions()`, a full rebuild from the carried `passable`
    //     plane, on world_snapshot_dispositions.json's instruction. That is measured insufficient:
    //     the recording's step-0 decomposition is build_regions' output THEN MUTATED by landing,
    //     spawn_ai_base and every building placement, and a fresh rebuild reproduces "what the
    //     builder makes of this plane" rather than "what incremental maintenance made of its
    //     history". The two agree on the region count (180) and disagree on 2181 of 65536 tiles;
    //     the replay diverged at step 290. Full reasoning in state/nav_trailer.h.
    //
    //     RUNNING BOTH WOULD BE THE WORST OUTCOME, not a belt-and-braces one: build_regions would
    //     silently overwrite the carried truth, the partition-equality acceptance would pass
    //     trivially against a rebuild compared with itself, and the replay would still drift. So
    //     there is exactly one writer of the decomposition on this path, and it is the trailer.
    const int nrc = mh::state::world::import_nav(blob, n);
    if (nrc != mh::state::world::WORLD_OK) return nrc;

    // (7) THE DISPATCH TABLES, and this one is NOT in world_snapshot_dispositions.json's re_derive
    //     list -- it was found by the first standalone replay, which faulted inside the first sim
    //     step at 0x6CF35E60 writing 0x6CF35E60 (EIP == the fault address: a call through a garbage
    //     pointer). That address is not an mh.exe VA; it is an address inside the RECORDING
    //     PROCESS'S mh.dll, because in a promoted hosted run our own registrar filled those slots
    //     with mh.dll thunk addresses -- which the blob then carried, verbatim and correctly, into a
    //     process where they mean nothing.
    //
    //     `_G_LLM_STRAT_UNIT_STATE_FUNCS[255]` and `_G_LLM_STRAT_BLDG_STATE_FUNCS[255]` are the
    //     per-tick behaviour of every unit and every building in the game (sim_register_state_
    //     handlers.h), and the building-type callback table is its sibling. They are the same class
    //     as the nav-region graph one level up -- carried bytes that are ADDRESSES -- and they are
    //     also lib-completeness clause (b) in person: "it owns its own dispatch tables (fn-ptr
    //     tables are filled by our code with our function pointers)". Re-running the registrars is
    //     what makes that true in a standalone process. Neither table is a determinism-hash slice,
    //     so this cannot move the step-0 hash, which the acceptance re-measures rather than assumes.
    mh::sim::register_state_handlers();
    mh::sim::register_bldg_type_callbacks();

    if (rs != nullptr) {
        // (8) mp:X3c -- RE-1: the imported selection is the host's; reset this peer's own.
        mh::state::world::reset_local_selection();

        // (9) mp:X3c -- ARM the staged re-admission of what this peer admitted after the host's capture
        //     point. Nothing is appended here: the caller feeds the log to ORDER_PENDING a few steps
        //     ahead of the clock (admission::stage_feed), because the whole backlog does not fit in
        //     PENDING at once.
        mh::orders::admission::plan_report pr{};
        const int                          arc = mh::orders::admission::stage_begin(rs->n_src, &pr);
        if (rs->rep != nullptr) {
            rs->rep->kept_regions    = kept_regions;
            rs->rep->kept_bytes      = kept_bytes;
            rs->rep->readmitted      = mh::orders::admission::stage_remaining();
            rs->rep->pending_after   = static_cast<uint32_t>(*mh::orders::state().pending_count);
            rs->rep->log_total       = mh::orders::admission::current().total;
            rs->rep->overflow_resets = mh::orders::admission::current().overflow_resets;
        }
        mh::ai::ai_say("; [worldsync] IMPORT-DONE staged=%u pending_after=%u log_total=%u rc=%d\n",
                       mh::orders::admission::stage_remaining(),
                       static_cast<uint32_t>(*mh::orders::state().pending_count),
                       mh::orders::admission::current().total, arc);
        if (arc != 0) return arc;
    }
    return mh::state::world::WORLD_OK;
}

} // namespace

// mp:X3c -- the resync entry. Same contract as
// libmh_import_world plus: the session-begun latch is bypassed, the keep-local regions survive, the
// local selection is reset, and the PENDING-admission log is re-applied. `n_src` is the host's
// per-source admission counters at the capture step S (9 entries; see orders/admission_log.h).
//
// The admission log is consulted BEFORE the first byte is written: -30 (ring short), -31 (this peer
// is behind the host), -32 (log latched abort) all leave the world untouched.
static_assert(mh::orders::admission::ERR_RING_SHORT == mh::state::world::WORLD_ERR_RESYNC_RING_SHORT);
static_assert(mh::orders::admission::ERR_AHEAD == mh::state::world::WORLD_ERR_RESYNC_AHEAD);
static_assert(mh::orders::admission::ERR_ABORT == mh::state::world::WORLD_ERR_RESYNC_ABORT);
static_assert(mh::orders::admission::ERR_PENDING_FULL == mh::state::world::WORLD_ERR_RESYNC_PENDING_FULL);
static_assert(mh::orders::admission::SOURCES == LIBMH_RESYNC_SOURCES);

extern "C" int libmh_import_world_resync(const void *blob, size_t n, const uint32_t *n_src,
                                         libmh_resync_report *rep) {
    static bool fired = false;
    in_live(fired, "libmh_import_world_resync");
    if (blob == nullptr || n_src == nullptr) return mh::state::world::WORLD_ERR_ARG;
    if (rep != nullptr) {
        if (rep->struct_size < sizeof(libmh_resync_report)) return mh::state::world::WORLD_ERR_ARG;
        std::memset(rep, 0, sizeof(*rep));
        rep->struct_size = sizeof(libmh_resync_report);
    }
    mh::orders::admission::plan_report pr{};
    const int                          prc = mh::orders::admission::plan(n_src, &pr);
    if (prc != 0) {
        mh::ai::ai_say("; [worldsync] IMPORT-REFUSED rc=%d src=%d need=%u (world untouched)\n", prc,
                       pr.bad_src, pr.need);
        return prc;
    }
    resync_args ra{n_src, rep};
    return import_world_impl(blob, n, &ra);
}

// ---- the deterministic spine --------------------------------------------------------------------

// One wire-format order record into the due-now queue.
//
// RAW BYTES, NOT A DECODE, and the fixture is why. tools/fixture_replay.py's recording is produced
// by the harness copying ORDER_SIZE bytes straight out of `order_queue`, and the replay injector
// copies them straight back, so the record crossing here is already the in-memory llm_strat_order.
// Running it through orders/order_codec.h would be a round trip through a format the recorder never
// applied. The queue's own bytes are hashed (HIDX_ORDER_QUEUE, 20400 B, not excluded), so this has
// to be a copy and not a construction.
//
// APPEND AT THE COUNT, which reproduces the harness injector exactly even though the injector
// OVERWRITES from index 0: the dispatcher is count-gated and zeroes the count, so a step always
// begins at 0 and the two write the same records into the same slots. Neither clears the queue's
// tail, so the residue of earlier steps stays in the hashed bytes identically.
//
//   >= 0   the slot the record landed in
//   -1     `record` is null, or `n` is not the record size
//   -2     the queue is full
extern "C" int libmh_submit_order(const void *record, size_t n) {
    static bool fired = false;
    in_live(fired, "libmh_submit_order");
    if (record == nullptr || n != sizeof(mh::orders::order)) return -1;
    const mh::orders::container_state &st = mh::orders::state();
    if (st.queue == nullptr || st.queue_count == nullptr) return -1;
    const int32_t at = *st.queue_count;
    if (at < 0 || at >= mh::orders::QUEUE_CAP) return -2;
    std::memcpy(&st.queue[at], record, n);
    *st.queue_count = at + 1;
    return at;
}

// The llm_strat_sim_step closure, ours since SIM1. The frame DRIVER around it
// (llm_strat_sim_tick's clock advance and its ambient/invasion/advisor tail) is a different body and
// is NOT folded in here -- libmh.h names this entry for the step, and a host that wants the whole
// frame composes it the way the original driver does.
extern "C" void libmh_sim_step(void) {
    static bool fired = false;
    in_live(fired, "libmh_sim_step");
    mh::sim::sim_step();
}

// The lockstep STATE hash over the bound regions -- the state_excluded() slices dropped, folded
// FNV-1a in manifest order, which is byte-for-byte the in-binary determinism harness's derivation
// (the shared implementation is mh::state::world::lockstep_hash).
//
// NO MASK PARAMETER, and that is the right shape rather than a simplification: all three harness
// masks default to 1 (seams/harness.cpp Config), the committed LIB-REF fixture records
// `mask_flags = 7`, and the masks exist to drop per-FRAME state (ctrl_group_id, soldier anim, the
// planets gfx windows) that is not sim state in any configuration. A host that needs an unmasked
// arm is running an A/B, which is a harness job, not an ABI one.
extern "C" uint64_t libmh_state_hash(void) {
    static bool fired = false;
    in_live(fired, "libmh_state_hash");
    uint64_t state = 0;
    mh::state::world::lockstep_hash(mh::state::world::MASK_CTRL_GROUP |
                                        mh::state::world::MASK_SOLDIER_ANIM |
                                        mh::state::world::MASK_PLANETS_GFX,
                                    nullptr, &state);
    return state;
}
