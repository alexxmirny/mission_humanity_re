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
// ---- WHAT THIS ENTRY STILL DOES **NOT** DO, stated rather than implied ---------------------------
//
// The eleven BAKED POINTERS INTO BOUND REGIONS that every capture's own census counts (the blob
// header of the committed fixture says `region_head_ptrs = 11`) are NOT re-stamped here, and the
// reason is structural rather than an omission. A general re-stamp needs, for each carried dword,
// the RECORDING's base of the region it points at. The standalone build does not have that column
// at all -- MH_STOCK_BASE(va) is 0u under MH_LIBMH_BUILD (LIB-REF-SPLIT S5, deliberately: "what
// standalone code may do with a zero base: nothing") -- and the blob carries rid+len per block, not
// a base. So the census count is a CAPTURE-side diagnostic, not an import-side fixup list, and
// manufacturing one would mean changing the blob format and re-recording the fixture.
//
// The disposition file already names the enforcement and it is the right one: "LIB-REF's replay is
// the enforcement, and it will fail loudly rather than subtly (a wild read, not a drift)." Of the
// eleven, seven are UI widget-list heads and one is a tactical queue cursor -- neither class runs in
// a headless strategic replay.
//
// ---- AND THE SENTENCE THAT USED TO FOLLOW WAS MEASURED FALSE (2026-09-11, LIB-REF-LIVE) ---------
//
// It read: "so the population that could bite is small and named". It is not, and a future reader
// must not inherit it. THE CENSUS IS BLIND TO MOST OF THIS CLASS BY CONSTRUCTION: `region_head_ptrs`
// counts dwords whose value EQUALS some carried region's live base, so a pointer INTO a region --
// at any interior offset -- is invisible to it. The eleven are a lower bound on the class, not an
// enumeration of it, and reasoning about "the population" from that number is reasoning from a
// number that was never measuring the population.
//
// What bit: G_TEXT_PTRS, the localized text pool's index -- 806 carried dwords, 720 of them pointing
// at INTERIOR offsets of G_TEXT_BLOCK and one at STRAT_SCENARIO_PLANET_NAME_W. None was counted by
// the census. It is read whenever the sim FORMATS a message, which every replay fixture suppresses
// along with its enqueues, so no replay ever touched one; the first live-loop run faulted on it at
// step 2001 (0xC0000005 in format_core<wchar_t>) after 2000 bit-identical steps. A screen of the
// whole blob then found 41 holder regions with dwords landing in carried spans -- most of it text
// and pixel aliasing, but the adjudication of the rest is owed rather than done.
//
// WHERE THE FIX WENT, and why still not here. The IMPORTING HOST re-stamps the table, which is what
// the disposition file always said this class was ("obligations on the importing host, not gaps"):
// libref_host does it from a generated host-only base table, immediately after import, with four
// adjudicated outcomes per entry and a loud refusal for any fifth. That needs the RECORDING's bases,
// which this translation unit still cannot have and should not want -- MH_STOCK_BASE is 0u here by
// LIB-REF-SPLIT S5, and that is the ruling that keeps the libmh artifact relocatable. A generated
// (holder rid, offset, pointee rid) fixup trailer remains the general fix if the owed adjudication
// finds a population big enough to want one; it is a fixture-format change and belongs with a
// re-record, not here.
//
#include <cstring>

// The C facade by relative path, exactly as state/host_bind.h reaches it: libmh/include is not on
// any module's include path, and adding it for one TU would be a silent layering change.
#include "../../libmh/include/libmh.h"

#include "addr/mh_regions.gen.h"
#include "ai/ai_state.h" // ai_say -- the shared trace sink the liveness line writes to
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
constexpr mh::state::region_id FOREIGN_POINTER_REGIONS[] = {
    mh::state::RID_MAP_REGION_BY_INDEX,         // 4096 llm_map_region*, by region index -- all pointer
    mh::state::RID_MAP_REGION_LIST_HEAD,        // the active-region list head
    mh::state::RID_MAP_REGION_POOL_FREE_HEAD,   // the allocator's free list head
    mh::state::RID_STRAT_PATH_JOB_RESULT_TABLE, // 100 heap pointers; pathfinder_init refills them
};

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
void zero_foreign_pointer_regions() {
    // Through mh::state::clear_region, not a local ptr<>: state/region_runtime.h is the only header
    // allowed to bind an address and check_sim_addresses enforces it. The rule caught this TU the
    // first time it ran here, which is the rule working.
    for (const mh::state::region_id r : FOREIGN_POINTER_REGIONS) mh::state::clear_region(r);

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

extern "C" int libmh_import_world(const void *blob, size_t n) {
    static bool fired = false;
    in_live(fired, "libmh_import_world");
    const int rc = mh::state::world::import(blob, n);
    if (rc != mh::state::world::WORLD_OK) return rc;

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
    return mh::state::world::WORLD_OK;
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
