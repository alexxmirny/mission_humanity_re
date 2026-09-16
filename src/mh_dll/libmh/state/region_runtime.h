//
// state/region_runtime.h -- translating a STOCK address to where the bytes are now (RI-STATE / ST2).
//
// The registry has carried two bases since ST2: `REGIONS[rid].base`, the .bss address the game's own
// code and the save FORMAT name, and `live_base(rid)`, where those bytes actually are. Both live in
// the generated header because both are derived from the same table. What lives HERE is the policy
// that joins them: given an address out of a manifest, hand back a pointer to the memory that
// address now denotes.
//
// WHY THIS IS NOT JUST `live_base(covering(addr))`. Two hard-won constraints shape it:
//
//   1. IT MUST BE TOTAL on every build where nothing has moved. ST1 added a `covering()` guard to
//      the save resolver, which made it partial, and the promoted save wrote zero bytes -- the
//      driver also resolves reads that are NOT blocks (the framing word that SIZES the progress
//      blocks, the member predicate's four reads), and no region claims those. So: an address no
//      region covers resolves to ITSELF. Not an error, not a null -- the identity. The rule that a
//      BLOCK must be covered is a compile-time assert in save_driver.cpp and belongs there.
//   2. A REBASED REGION IS THE ONE PLACE A NULL IS RIGHT. If a region has moved and a manifest asks
//      for bytes past the end of where it moved TO, translating regardless would hand back a
//      pointer into whatever follows the module's allocation -- a silent out-of-bounds write on the
//      load path. That is worth failing loudly for, and it cannot regress case 1 because it is
//      unreachable until something actually calls `rebase`. (Making it a BUILD error instead of a
//      runtime one is ST3's interlock; this is the runtime backstop under it.)
//
#pragma once
#include <cstring> // memset -- clear_region, below
#include <cstdint>

#include "addr/mh_regions.gen.h"

namespace mh::state {

// ---- THE STOCK-ADDRESS HALF IS HOSTED-ONLY (LIB-REF-SPLIT, 2026-09-11) -------------------------
//
// Everything from here to the end of this block takes an ORIGINAL-IMAGE ADDRESS as its input. That
// is a coherent question only while the image is mapped, and the standalone build does not carry the
// stock base column at all (MH_STOCK_BASE, addr/mh_regions.gen.h) -- so `covering()` is not declared
// there and neither is anything built on it.
//
// NOT DEGRADED TO A STUB, deliberately. A `region_at` that returned RID_COUNT and a `translate` that
// returned its argument would both compile, both look total, and both hand back a number that means
// "address zero" -- the save driver would resolve every block to the same place and write whatever
// is there. A missing declaration makes each such consumer a COMPILE error naming its own line,
// which is how the rid-keyed re-key found its sites instead of guessing at them.
#ifndef MH_LIBMH_BUILD

// The region a STOCK address belongs to, or RID_COUNT. Thin wrapper over `covering` that hands back
// an id rather than a pointer, because everything downstream keys on the id.
inline region_id region_at(uint32_t addr, uint32_t size) {
    const region *r = covering(addr, size);
    return r == nullptr ? RID_COUNT : static_cast<region_id>(r - &REGIONS[0]);
}

#endif // !MH_LIBMH_BUILD

// NOT part of the stock-address block, and it sits between its two halves on purpose rather than
// being moved: per_player_capacity is keyed by REGION ID and reads the LIVE size the host bound, so
// it means exactly the same thing in both configurations -- it is in fact more useful standalone,
// where a cap-raised host is the whole point. It was briefly swallowed by the guard above when that
// guard was first drawn around a contiguous span; the lesson is that "takes a stock address" is the
// membership test here, not "lives in this neighbourhood".

// The per-player row capacity of a roster region, DERIVED from the size the host BOUND rather than
// from a compile-time constant (SB-BIND T2; docs/state-boundary.md D6.3).
//
// This is the same shape sim_state.cpp's writable_state_tables() already uses for unit_slots and
// friends -- live size divided by the record size -- with the player dimension divided out. It is
// what makes the reimplementation compose with a cap-raised host: bind a roster of 500 records per
// player and the indexing follows, with nothing to declare and nothing to keep in step.
//
// FLOOR DIVISION IS THE RIGHT OPERATION, not a rounding convenience: `live_size` is the region's
// REACH (SB-BIND T1), which for 11 regions exceeds what the symbol measures, so the quotient must
// answer "how many WHOLE records fit" and ignore a partial tail. player_data is the live instance --
// reach 1329136 over a 166140 record gives 8, where an exact division would not.
//
// `players` stays a compile-time constant on purpose. The player count is NOT derived and must not
// be: 8 is baked into the record LAYOUT (four [8] sub-arrays inside game_player_data), so a derived
// value would index past struct members rather than past an array. docs/state-boundary.md D6.9 has
// the measurement and the decision.
inline int32_t per_player_capacity(region_id r, uint32_t elem_size, int32_t players) {
    if (elem_size == 0 || players <= 0) return 0;
    return static_cast<int32_t>(live_size(r) / elem_size / static_cast<uint32_t>(players));
}

#ifndef MH_LIBMH_BUILD // the stock-address block resumes -- see the banner above region_at()

// Where [addr, addr+size) IS, given that `addr` is a stock address. Identity unless the covering
// region has been rebased. Null ONLY for a rebased region the request runs off the end of -- see the
// header comment; do not add a second null path without reading it.
inline void *translate(uint32_t addr, uint32_t size) {
    const region_id rid = region_at(addr, size);
    if (rid == RID_COUNT || !is_rebased(rid))
        return reinterpret_cast<void *>(static_cast<uintptr_t>(addr));
    const uint32_t delta = addr - REGIONS[rid].base;
    if (uint64_t(delta) + size > uint64_t(live_size(rid))) return nullptr;
    return reinterpret_cast<void *>(static_cast<uintptr_t>(live_base(rid) + delta));
}

#endif // !MH_LIBMH_BUILD

// A POINTER-VALUED slot's contents, translated (SB-BIND T3).
//
// A handful of state regions hold a live `T*` INTO another region rather than a value --
// `_G_LLM_STRAT_CUR_UNIT` holds a `unit*` into `units`. While the host binds the stock bases the
// stored address is the address it means; the moment a host binds a RELOCATED copy it names the
// abandoned .bss, so a read through it sees stale bytes and a write lands where nothing reads.
// The full enumeration and its per-slot adjudication is tools/data/pointer_slots.json, generated
// by tools/scan_pointer_slots.py -- generated, because an earlier hand count recorded this shape as
// having exactly one instance and there are four.
//
// TWO DELIBERATE NON-BEHAVIOURS, because this runs on every bind:
//   - a NULL slot stays null. Null means "nothing current" to the game and manufacturing an address
//     would invent a selection.
//   - if `translate` DECLINES (it returns null only for a rebased region the window overruns), the
//     ORIGINAL pointer is returned rather than null. A stale pointer is a visible wrong answer;
//     a manufactured null is indistinguishable from the legitimate "nothing current" and would be
//     the quieter failure. In the hosted configuration neither branch is reachable -- nothing is
//     rebased, so translate is the identity and this whole function is a no-op.
template <class T>
inline T *translate_slot(T *p) {
#ifdef MH_LIBMH_BUILD
    // LIB-REF-SPLIT: THE IDENTITY, and for a reason rather than as a stub. This exists because a
    // slot may hold a pointer the ORIGINAL stored -- a stock address that stops meaning what it says
    // the moment a host binds a relocated copy. A standalone artifact has no original writer: every
    // pointer in every slot was put there by our own code, pointing into memory the host bound, so
    // the address it holds IS the address it means and translating is a no-op by construction.
    //
    // This is the one member of the stock-address family that stays DECLARED standalone, because
    // unlike covering()/translate() it has a correct answer there -- and because its callers
    // (sim_state.cpp's four cur_* slots) are ordinary state-view plumbing that both configurations
    // run. A missing declaration would make them a compile error with nothing to fix.
    return p;
#else
    if (p == nullptr) return nullptr;
    void *q = translate(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p)),
                        static_cast<uint32_t>(sizeof(T)));
    return q == nullptr ? p : static_cast<T *>(q);
#endif
}

// ---- the camera pair, writable (LIFT-R3B's R3b hoist) --------------------------------------
//
// The strategic camera column/row are the one pair of regions a NOTIFY EMITTER writes, so they
// need a mutable binding outside any module's state view -- state/host_events.cpp's cam_set_col/
// _row store through these before emitting, so that llm_strat_spawn_enemy_landing's hashed
// landing_x/landing_y fallback (sim_landing_spot.cpp:147-150) reads a value libmh owns rather
// than one a poll-mode host has not applied yet. They live HERE because this header is the only
// place allowed to bind an address (check_sim_addresses enforces it), and putting them in a
// module's sim_store would be worse: the emitter is not a sim body and has no store to reach.
inline int32_t &map_cam_col() { return *ptr<int32_t>(RID_MAP_CAM_COL); }
inline int32_t &map_cam_row() { return *ptr<int32_t>(RID_MAP_CAM_ROW); }

// ---- clearing a whole region (LIB-REF's world import) ----------------------------------------
//
// The world blob carries every bound region VERBATIM, which is right for state and wrong for the
// one kind of content that cannot survive a process boundary: a POINTER. The nav-region
// decomposition and the path-job result table are pointer graphs into the RECORDING process's
// heap, so the importing C entry zeroes them before rebuilding (state/spine.cpp; the evidence is
// in tools/data/world_snapshot_dispositions.json's re_derive column).
//
// It lives HERE for the reason the camera pair above does: this header is the only place allowed
// to bind an address, and the caller is not a sim body with a store to reach through. Whole-region
// and rid-keyed on purpose -- it takes no address and no length from its caller, so it cannot be
// pointed at a slice of something, and `reach` is the registry's own answer for how far the region
// goes rather than a number a call site remembered.
inline void clear_region(region_id r) {
    void *const p = ptr<void>(r);
    if (p != nullptr) std::memset(p, 0, reach_of(r));
}

} // namespace mh::state
