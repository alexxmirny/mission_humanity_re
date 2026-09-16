//
// state/host_bind.h -- the C++ side of the state ABI (SB-BIND T1). See host_bind.cpp for the
// decisions; the C surface is declared in libmh/include/libmh.h.
//
// The hosted caller is mh.dll's seam layer (seams/), which binds the default table at init. That is
// deliberate: the SHIPPING configuration exercises the same entry point a standalone host will, so
// the path cannot rot while nothing standalone exists yet.
//
#pragma once
#include <cstddef>
#include <cstdint>

// For `region_id` -- host_relocated() answers per region, so the enum is part of this surface.
#include "addr/mh_regions.gen.h"

// The C facade carries libmh_region_bind and the extern-C prototypes; reached by relative path,
// the same way host_api.h reaches the generated host-API table.
#include "../../libmh/include/libmh.h"

namespace mh::state {

// Fill `out` with the stock in-binary answer. Writes min(cap, RID_COUNT) entries; returns RID_COUNT
// regardless, so cap 0 is the legal "how big a buffer do I need" probe.
size_t default_binds(libmh_region_bind *out, size_t cap);

// Apply a host's table. Returns the number bound, or a negative code (see libmh.h). Validates the
// whole table before writing anything.
int bind_all(const libmh_region_bind *binds, size_t n);

// Bind the stock table -- the hosted configuration's answer, and a no-op by construction. Returns
// the number of regions bound.
int bind_stock();

// Sampled by bind_stock() around its own call, because a count taken later measures something else:
// by report time the AI island has legitimately relocated 48 regions. Both are -1 until bind_stock()
// has run.
//   moved_before_bind() -- regions ALREADY relocated when the host answered. Must be 0: binding
//                          stock over a relocated region silently un-relocates it.
//   moved_after_bind()  -- must be 0 too. This is the no-op property itself.
int moved_before_bind();
int moved_after_bind();

// ---- the RELOCATING host bind (SB-HOSTFREE) --------------------------------------------------
//
// bind_stock() proves the ABI is wired. This proves it is wired to something that can actually
// move: the host puts every relocatable region in its own arena and answers with those addresses,
// and the run has to come out bit-identical.
//
// WHAT "RELOCATABLE" MEANS, and why it is not "every region". An original accessor is a baked
// disp32 in an instruction we do not own; it cannot follow a bind. Move a region one still reads
// or writes and the original half of the game is on the abandoned .bss while ours is in the arena
// -- so Law 1 forbids it, and `REGIONS[].relocatable` (generated from the accessor census) is where
// that verdict lives. 463 of 827 today. See docs/state-boundary.md D7.2.
struct relocation_report {
    int relocated;              // regions actually moved into the arena
    int blocked;                // refused: a live original accessor still names the address
    int zero_size;              // relocatable, but nothing to move
    int overrunning;            // refused: reach > size, so moving it would move a window over its
                                // NEIGHBOURS and poison their live storage. All ten are also blocked
                                // today; the count exists for the day promotion frees one.
    int under_overrun;          // MOVED, and relying on the save-block decomposition to stay right:
                                // someone else's window runs past its symbol and over this region,
                                // so a block reaching it must be served run by run (SLICED_BLOCKS)
                                // or it would read the stock address after the bytes left --
                                // silently, in bytes only the save format reads. 21 of these sit
                                // under _G_LLM_GAME_SESSION_MODE's 1623-byte block. REPORTED, not
                                // refused, since 2026-09-06.
    int walker_held;            // refused: an ORIGINAL save-block walker still names these bytes
                                // and this configuration has not promoted it. 29 regions today
                                // (WALKER_HELD_COUNT); 0 once all four `[promote]` switches are on.
                                // See relocation_opts::armed_walkers and dead-ends G139.
    int already_moved;          // refused: something (ai::island_move) had rebased it first
    int pinned;                 // deliberately left at stock; 0 or 1. NOT a mutation that can go
                                // red -- see relocation_opts::pin_stock.
    int         corrupted;      // arena copy deliberately filled with 0xCD -- THE mutation; 0 or 1
    uint32_t    bytes;          // arena bytes consumed, including alignment padding
    int         hash_slices;    // strategic determinism slices now reading relocated bytes
    int         tact_slices;    // ...and tactical ones
    const char *pinned_name;    // the pinned region, or nullptr
    const char *corrupted_name; // the corrupted region, or nullptr
    const char *refused;        // why nothing was bound at all, or nullptr on success
};

struct relocation_opts {
    // Copy the stock bytes into the arena before binding. TRUE for any live host: a region bound to
    // an uninitialised arena has lost its contents. FALSE only for an offline fixture, where the
    // stock .bss addresses are not this process's memory and the subject under test is the
    // SELECTION, not the copy.
    bool copy = true;
    // Fill the abandoned stock range with 0xCD afterwards, on ai::island_move's precedent. This is
    // the arm with teeth: without it a consumer that failed to follow the bind reads a plausible
    // stale copy and agrees with itself; with it, it reads garbage and the hash says so.
    bool poison = true;
    // THE MUTATION: answer for ONE region with its stock base while everything else moves. With
    // `poison` on, that stock range is filled with 0xCD first -- because a pin over UNTOUCHED
    // memory diverges from nothing and would prove the opposite of what it is for. So this stages
    // the real defect (a consumer served an address the state has left) and the run MUST go red,
    // naming that region. nullptr for the real arrangement. A run with a pin is a run you discard.
    const char *pin_stock = nullptr;
    // THE MUTATION THAT ACTUALLY GOES RED, and it exists because the one above does not.
    //
    // MEASURED 2026-09-06: a 3000-step soak with `pin_stock=_G_LLM_STRAT_ORDER_QUEUE` reported
    // `golden MATCHED`. The pin applied correctly (379 relocated instead of 380, 12 hashed slices
    // instead of 13, named in the log) -- it simply cannot diverge, for two independent reasons and
    // both of them general:
    //   * every consumer follows the registry, so a region answered at its stock base behaves
    //     EXACTLY as it does in an unrelocated run. The pin removes a move; it does not introduce a
    //     disagreement.
    //   * the poison staged at that base is transient. The first write lands on it and it is gone,
    //     and for live state the write comes before any read.
    // (The order queue adds a third: mh::orders L1-serves it, so the hash reads the module's
    // container and never touches the address at all.)
    //
    // So the mutation that proves the hash is watching RELOCATED bytes has to corrupt the bytes it
    // is watching: relocate this region normally and then fill its ARENA copy with 0xCD. A run with
    // this set MUST diverge, and the report MUST name this region. A run you discard.
    const char *corrupt_arena = nullptr;
    // WHICH ORIGINAL SAVE-BLOCK WALKERS THIS CONFIGURATION HAS PROMOTED -- a mask of WALK_* bits.
    //
    // The four functions in the save block table (SavePlanetToDisk, LoadPlanetFromDisk,
    // game::SaveGame, llm_game_load) reach state through addresses baked into their own instruction
    // stream, so while the ORIGINAL body runs it reads and writes the STOCK address whatever the
    // registry says. A region such a walker names may therefore move only once OUR body has taken
    // that entry point over. `movable_under(r, armed)` is the predicate; anything it refuses is
    // counted in relocation_report::walker_held and left at its stock base, which is consistent
    // (the original and every registry consumer then agree) rather than merely undamaged.
    //
    // DEFAULT 0, matching ship: the save closure is not ours in any configuration today
    // (mh::config::kSaveClosureOwned), so a caller that says nothing gets the conservative answer
    // instead of the optimistic one.
    //
    // MEASURED (dead-ends G139): with 0 here and the 29 held regions moved anyway, a relocated
    // LOADGAME soak diverges at exactly the load step in `order_queue`; with all four armed and the
    // regions moved, the same soak matches its golden over 3000 steps.
    //
    // A VERIFYING PROMOTION IS NOT AN ARMED ONE. `[save] verify != 0` installs through a trampoline
    // so the original stays callable AND RUNS, as the A/B's other arm -- so its baked addresses are
    // live again and the bit must stay clear. The caller is where that is enforced; this struct
    // only carries the answer.
    uint8_t armed_walkers = 0;
};

// THE MASK, as a function of the one question that decides it (fork F2E). Split out of
// harness.cpp's armed_save_walkers() so the ARMED case is demonstrable: the hosted caller feeds this
// `mh::config::save_walkers_ours()`, which is false in every shipping configuration, and a gate
// nothing can ever show non-zero is a gate nobody can tell from a stub. net_selftest's config suite
// calls it with `true` and asserts all four bits, which is the negative arm the item's done_when
// wants -- previously supplied by an ini fragment that no longer has a section to live in.
//
// ALL FOUR WALKERS MOVE TOGETHER because the ownership question is now whole-closure: the four used
// to carry a key each so a half-migrated save closure could arm the half it had, and there is no
// such state any more -- `kSaveClosureOwned` flips when the closure is done, not while it is
// in progress.
constexpr uint8_t save_walker_mask(bool walkers_ours) {
    return walkers_ours ? (uint8_t)(WALK_PLANET_SAVE | WALK_PLANET_LOAD | WALK_CONTAINER_SAVE |
                                    WALK_CONTAINER_LOAD)
                        : (uint8_t)0;
}

// Relocate into [arena, arena+bytes). Returns the number of regions bound (0 = nothing was bound,
// and `out->refused` says why -- an arena too small is refused WHOLE rather than part-applied, for
// the same reason bind_all validates before it writes).
int bind_relocated(void *arena, size_t bytes, const relocation_opts &opts, relocation_report *out);

// Has bind_relocated() moved this region? Read by ai::island_move, which must not copy from a
// stock range the host has already abandoned (and poisoned).
bool host_relocated(region_id r);

// How many regions the relocating bind moved, or -1 if it never ran. The arm-time report reads
// this to decide WHICH postcondition applies: the stock bind's is "nothing moved", the relocating
// bind's is the opposite, and a report that judged both by the first would call the item's own
// arrangement a failure. (bind_stock() hit precisely that once already -- see [statebind].)
int relocated_count();

} // namespace mh::state
