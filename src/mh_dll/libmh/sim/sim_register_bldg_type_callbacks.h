//
// sim/sim_register_bldg_type_callbacks.h -- OUR registrar for the two PER-BUILDING-TYPE callback
// tables (RI-SIM / SIM1-BLDGCB, 2026-08-23).
//
// ---- WHAT THIS REPLACES -------------------------------------------------------------------------
//
// `llm_strat_register_bldg_type_callbacks` @0x0045f76a (boot stage 5, re-run on game load) fills two
// 100-entry function-pointer tables:
//
//   _G_LLM_STRAT_BLDG_DONE_FUNCS[100]  @0x00ae4830 -- dispatched by llm_strat_building_tick as
//        bldg_done_funcs[building.building_id]()   (the CALL at 0x0046ff18), once the building is
//        fully built and not decaying: the per-building-kind ECONOMY handler that accumulates that
//        building's population / power / housing / storage contribution.
//   _G_LLM_STRAT_BLDG_TICK2_FUNCS[100] @0x00ae49c0 -- dispatched by
//        llm_strat_bldg_tick_animation_state as bldg_tick2_funcs[building.building_id]() (the CALL
//        at 0x00476437): the per-building-kind animation state machine.
//
// 30 distinct callbacks, ~10.8 KB. They were the sim migration's SECOND structural blind spot, and
// a different one from SIM1-DISPATCH's: the fold that closed that item is seeded from the 68 STATE
// handlers, and this registry is reached across a second indirect edge that no state-handler seed
// is upstream of, so the fold could not pull it in. Worse, `check_dispatch_closure` saw only ONE of
// the 30 -- its candidate set is the roster writers, and the llm_strat_done_* handlers write the
// per-player STAT blocks, which are not roster regions. A clean run of that check was never
// evidence about this registry. (It is the third-order version of the ref-manager blindness.)
//
// SIM1-BLDGCB's batch H has since translated and verified all 30. As with the state tables,
// translating a callback does not make the game RUN it: the tables still hold the ORIGINAL's
// addresses, so a promoted building_tick dispatches straight back into original code. This file
// closes that -- our registrar fills both tables with OUR callback pointers.
//
// ---- IT IS A (TYPE -> CALLBACK) PAIRING PLUS A RUNTIME SCAN, NOT AN ID TABLE ---------------------
//
// This is the one real difference from sim_register_state_handlers.cpp, and it is not a detail. The
// state setters are a flat bounded store -- `if (0 <= id && id < 0xff) table[id] = handler` -- so
// the (state -> handler) pairing is fully determined by the call arguments. THESE setters are not:
// `llm_bldg_register_done_callback` @0x0045f1ae LOOPS over building ids 1..99 and writes the
// callback into every slot whose `Building[id].type` equals the type argument. So:
//
//   * the (building TYPE -> callback) pairing IS extractable, and is extracted -- 50 assignments
//     across the two tables, in addr/mh_bldg_type_callbacks.gen.h;
//   * the (building ID -> callback) map is NOT, at any point before the cfg files are loaded. We
//     reproduce the SCAN, not a table of ids.
//
// Which is why fill_tables() takes the cfg table as an input and why it runs at strategic-mode init
// rather than at DLL load: before the .cfg/.bnk parse has populated Building[], every type byte is
// whatever the .bss held and the scan would match nothing (or match zero-typed slots wholesale).
//
// ---- ONE MORE CONSEQUENCE OF THE SCAN: THIS REGISTRAR DOES NOT COVER EVERY SLOT ------------------
//
// The state registrar fills 0..254 with a default and then overwrites. This one fills only the ids
// whose type falls in the default-fill loop's range (1..0x27) -- a building id whose cfg type is 0
// or >= 0x28, and slot 0 which the id loop never visits, is left holding whatever was already
// there. That is the original's behaviour and is reproduced rather than "fixed": filling those
// slots with our default would be a behaviour change disguised as robustness, and the dispatch
// sites do not range-check, so any difference shows up as a call to a different function rather
// than as an error.
//
// ---- WHY EACH SLOT IS AN ENTRY THUNK AND NOT THE C++ FUNCTION ITSELF -----------------------------
//
// Identical to the state tables' reasoning, including both traps that cost time there:
//
//   1. REGISTER PRESERVATION. The slots are called by ORIGINAL Watcom code, whose __watcall
//      preserves ECX and EDX across a call; MSVC's __cdecl does not. A bare C++ function pointer in
//      a slot clobbers two registers the caller expects to survive -- corruption with no crash at
//      the corruption site.
//   2. EIGHT CALLBACKS TAKE REGISTER PARAMETERS. Every llm_strat_bldg_anim_state_* on the
//      sim_bldg_anim_state_online.cpp side has a committed 4-parameter __watcall prototype
//      (EAX/EDX/EBX/ECX). At a table dispatch those values are AMBIENT. A __cdecl function in the
//      slot would read four "parameters" off the STACK instead: different garbage, silently.
//
// So each slot gets that callback's GENERATED entry thunk (MH_EXPORT_REPLACE's
// mh_export_thunk_<fn>) -- the marshalling the export generator already derives from the committed
// prototype. Passing the ambient registers through unchanged is exactly what the original callback
// does with them.
//
// ---- WHAT THIS DOES NOT DO ----------------------------------------------------------------------
//
// It does not touch the original callbacks' entry bytes; it takes the TABLES. So it calls no
// mh::hook::note_promoted() for any of the 30 -- the C1 interlock's claim is "every byte of this
// body is dead", which this file cannot establish. Same posture, same reason, as the state
// registrar's own closing note.
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h" // bldg_done_fn / bldg_tick2_fn / sim_bldg_callback_tables

namespace mh::sim {

// One original callback and the entry we install in its place.
struct bldg_type_callback_binding {
    const char  *name;        // the ORIGINAL Ghidra symbol, for the log and the oracle
    uintptr_t    original_va; // its entry in the game image (mh::exp::addr_<fn>)
    bldg_done_fn ours;        // our generated entry thunk; same void(*)() shape for both tables
};

namespace detail {

// The 30 bindings, in the generated header's first-appearance order.
const bldg_type_callback_binding *bldg_type_callback_bindings();
int                               bldg_type_callback_binding_count();

// The binding for an ORIGINAL callback VA, or nullptr. A miss cannot happen in a correct build (the
// ASSIGN tables and the X-macro come from the same generated file) -- it is reported rather than
// asserted so the caller can refuse to install a half-filled table.
const bldg_type_callback_binding *bldg_type_binding_for(uintptr_t original_va);

// Fill both tables exactly as the original registrar fills the game's: for each table, the default
// callback into every id whose cfg type is in the default-fill range, then each assigned type in
// the order the original performs those writes.
//
// Writes NOTHING and returns false if a table or the cfg table is shorter than the id loop the
// original runs, or if any VA has no binding -- a partially-filled dispatch table is worse than
// none. `filled_done`/`filled_tick2` receive the number of slots this call made ours, which is a
// cfg-dependent number and therefore the only honest non-vacuity measure available.
//
// Split out from register_bldg_type_callbacks() so the offline oracle can drive it over local
// arrays with a synthetic cfg: this is the whole of the logic, and the live entry point is only the
// binding around it. Same arrangement as detail::fill_tables for the state tables.
bool fill_bldg_callback_tables(const sim_bldg_callback_tables &t, int *filled_done,
                               int *filled_tick2);

} // namespace detail

// Our replacement for llm_strat_register_bldg_type_callbacks (`[promote] bldg_type_callbacks=1`).
void register_bldg_type_callbacks();

// Install the promotion. Ship default is PASSED IN rather than read from a header here -- the
// SHIP_PROMOTE_* constants live in mh/seams/net_internal.h and a reimplementation TU must not
// include a seams header (the layering lint). Same arrangement as install_promotion_state_handlers.
// Returns 1 if the entry is ours in this run.
int install_promotion_bldg_type_callbacks(int default_on);

// True once install_promotion_bldg_type_callbacks() has taken the entry.
bool bldg_type_callbacks_promoted();

// Is `target` one of our 30 entry thunks? Exact set membership against the binding table -- not a
// module-address-range guess, so it cannot be fooled by other DLL code that happens to be nearby.
bool is_our_bldg_type_callback(const void *target);

// The ONE-TIME note each dispatch site emits on its first dispatch through its table, saying whether
// the entry it is about to call is OURS or the game's. SIM1-BLDGCB's done_when asks for exactly
// this, for the tick2 table specifically, and asks for it to be SHOWN rather than inferred from the
// STATE tables' line -- which is a different registry filled by a different registrar at a different
// boot stage. `table_tag` is "done" or "tick2"; each tag notes once per run.
void note_first_bldg_type_dispatch(const char *table_tag, const void *target);

} // namespace mh::sim
