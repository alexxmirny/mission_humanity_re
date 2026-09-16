//
// state_selftest.cpp -- THE SYNTHETIC REBASE FIXTURE (RI-STATE / ST2).
//
// THE CLAIM UNDER TEST, in one sentence: when a state region MOVES, everything that dereferences it
// follows, and nothing keeps reading the address it left.
//
// WHY THAT NEEDS A TEST AT ALL, and why a green determinism run is not one. Before ST2 every
// consumer of a state region spelled a CONSTEXPR .bss address. Such a build passes the determinism
// gate whether or not a region has moved -- if one had, the harness would happily hash the
// abandoned bytes, get the same answer on both peers, and report ALL PAIRS IDENTICAL while
// observing nothing at all ("gates can pass vacuously"). The failure is
// invisible exactly where it matters, so the mechanism has to be watched to fire, and watched to
// fire in BOTH directions:
//
//     poke the RELOCATED copy    -> the region's hash MUST change  (we read the new bytes)
//     poke the ABANDONED address -> the region's hash MUST NOT     (we no longer read those)
//
// The second half is the one with teeth. The first passes on a build that never moved anything.
//
// THE ONE HONEST LIMITATION, stated up front because the alternative is a reader assuming more than
// this proves. The abandoned address here is a STAND-IN -- a heap buffer the fixture registers as
// the region's location first -- and not the literal .bss VA. That is not a shortcut; it is what
// the address space allows. MEASURED, twice: `_G_LLM_STRAT_ORDER_PENDING` (0x00bb9e80) could not be
// reserved because it is inside net_selftest.exe's own CRT heap, `_G_LLM_NET_PEER_HORIZON`
// (0x005d54cc) because it is inside net_selftest.exe's own IMAGE, and a sweep of all 55 whole-region
// hash slices found NOT ONE whose stock VA this process can materialise. The game's .bss spans
// roughly 0x005d0000-0x00fa0000, and in a 32-bit console process the loader and the CRT heap have
// already taken that entire span before main runs.
//
// WHAT THE STAND-IN COSTS, precisely: nothing about the mechanism, because the code path under test
// is `hash_base(i) == live_base(rid) + offset` and it cannot tell a .bss address from a heap one.
// What would be lost is the statement "the address a CONSTEXPR manifest would have used is not the
// address we read" -- so that is asserted DIRECTLY instead (see `the constexpr counterfactual`
// below), which is the actual content of the missing half rather than a proxy for it.
//
// THE REGION IS `_G_LLM_NET_PEER_HORIZON`, chosen because it is claimed by all three consumers at
// once -- the determinism hash (slice `peer_horizon`), a shadow region set
// (llm_net_lockstep_reset_player_horizon) and, through `covering`, the save resolver -- and is owned
// by NOBODY, so `emit_slice` really does take the raw read-bytes-at-an-address path this item is
// about. The order QUEUE / QUEUE_COUNT are excluded on purpose: mh::orders has claimed them since
// ST4, so they answer from the module rather than from an address, which is a different mechanism
// with its own tests.
//
// A NOTE ON WHAT IS *NOT* PROVEN HERE: none of this says a region SHOULD move. Law 1 still freezes
// every layout an original accessor reads. What it says is that the plumbing is ready for the first
// region that does move, and that the readiness is measured rather than assumed.
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstddef> // offsetof -- the mask offset comes from the generated mirror
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "addr/mh_regions.gen.h"
#include "addr/mh_structs.gen.h" // the crew_soldier mirror the anim mask is expressed against
#include "ai/ai_state.h"         // AI0: a real module state view, as the fourth consumer
#include "save/save_live.h"      // the LIVE save resolver, by name -- not a copy of its logic
#include "save/save_state.h"     // SB-BIND T5: the save module's live-arm binds, under test in arm L
#include "sim/sim_state.h"       // SIM0: the sim's MUTABLE view, as the fifth consumer
#include "state/region_owner.h"
#include "state/region_runtime.h"
#include "state/region_view.h"
#include "lockstep/lockstep_state.h" // SB-BIND T4: host_binds() -- the production-arm binds
#include "lockstep/turn_engine.h"    // SB-BIND T4: the lockstep binders under test in arm K
#include "state/host_bind.h"         // SB-BIND T1: the state ABI under test in run_bindtest()
#include "state/roster_caps.h"       // SB-BIND T2: the derived per-player caps

namespace {

int g_checks = 0, g_fails = 0;

void ck(bool ok, const char *what) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL: %s\n", what);
    }
}

using namespace mh::state;
using mh::save::live_binds;

constexpr region_id FIX_RID  = RID_NET_PEER_HORIZON;
constexpr int       FIX_HIDX = HIDX_PEER_HORIZON;
// A region in the SAME shadow set that must NOT move: the "targeted, not global" control.
constexpr region_id SIB_RID = RID_NET_PEER_HORIZON_PENDING;

// A byte pattern with no runs and no symmetry, so a flipped byte cannot coincide with what was
// there and a hash comparison cannot pass by luck.
uint8_t pattern(uint32_t i) { return (uint8_t)((i * 131u + (i >> 5) * 17u + 7u) & 0xffu); }

} // namespace

int run_statetest() {
    printf("=== statetest (ST2: a region moves, and every consumer follows) ===\n");

    const uint32_t stock_base = base_of(FIX_RID);
    const uint32_t len        = size_of(FIX_RID);

    // ---- THE UNREBASED BUILD IS BIT-IDENTICAL TO THE PRE-ST2 ONE ---------------------------------
    //
    // ST2's acceptance asked for the per-region hashes of an unrebased build to be byte-compared
    // against pre-ST2 goldens. This is that comparison, in a stronger and cheaper form than diffing
    // two run logs: it asserts, EXHAUSTIVELY, that every address any consumer can dereference is the
    // constexpr value it replaced. A log diff would only cover the regions a particular run happened
    // to touch and would have to be re-earned by hand each time; this covers all 243 regions and all
    // 55 hash slices, and it runs on every gate from now on.
    //
    // Must come FIRST -- the rebases below deliberately violate it.
    {
        int drift = 0;
        for (int r = 0; r < RID_COUNT; ++r) {
            const auto rid = (region_id)r;
            if (is_rebased(rid) || live_base(rid) != REGIONS[r].base ||
                live_size(rid) != REGIONS[r].size)
                ++drift;
        }
        ck(drift == 0,
           "on an unrebased build EVERY region's live base and size equal the constexpr values -- "
           "which is what makes ST2 provably behaviour-free until something actually moves");

        int hdrift = 0;
        for (int i = 0; i < HASH_REGION_COUNT; ++i)
            if (hash_base(i) != HASH_REGIONS[i].base) ++hdrift;
        ck(hdrift == 0, "...and every one of the 55 hash slices resolves to its stock address");
    }

    // ---- premises ------------------------------------------------------------------------------
    // Stated rather than assumed: if any stops holding, the rest of this file is measuring something
    // other than what it says it is.
    ck(HASH_REGIONS[FIX_HIDX].rid == FIX_RID, "premise: the hash slice belongs to the fixture region");
    ck(HASH_REGIONS[FIX_HIDX].offset == 0 && HASH_REGIONS[FIX_HIDX].len == len,
       "premise: the slice is the WHOLE region, so the raw path is the one under test");
    ck(owner_of(FIX_RID) == nullptr,
       "premise: the fixture region is UNOWNED -- emit_slice reads bytes at an address");
    ck(!is_rebased(FIX_RID) && !is_rebased(SIB_RID), "premise: nothing has rebased either region");
    ck(live_base(FIX_RID) == stock_base && hash_base(FIX_HIDX) == stock_base,
       "premise: the live base starts EQUAL to the constexpr stock base -- which is why ST2 is "
       "behaviour-free on a build where nothing has moved");
    ck(len >= 24 && len <= 256, "premise: the region is a size this fixture can hold twice over");

    static uint8_t home[256], moved[256];
    for (uint32_t i = 0; i < len; ++i) home[i] = pattern(i);

    // ---- put the region at its stand-in "home", and hash it there --------------------------------
    rebase(FIX_RID, (uint32_t)(uintptr_t)home, len);
    ck(hash_base(FIX_HIDX) == (uint32_t)(uintptr_t)home, "the hash slice resolves to the home buffer");
    const uint64_t h_home = hash_slice(FIX_HIDX, true);

    // ---- THE MOVE --------------------------------------------------------------------------------
    std::memcpy(moved, home, len);
    rebase(FIX_RID, (uint32_t)(uintptr_t)moved, len);

    ck(is_rebased(FIX_RID), "after rebase the region reports itself moved");
    ck(live_base(FIX_RID) == (uint32_t)(uintptr_t)moved, "live_base is the new address");
    ck(base_of(FIX_RID) == stock_base,
       "...and the STOCK base is UNCHANGED -- a move does not rewrite the address the save FORMAT "
       "names, which is why `covering` can stay a constexpr lookup");
    ck(hash_base(FIX_HIDX) == (uint32_t)(uintptr_t)moved, "the hash slice resolves to the new address");

    const uint64_t h_moved = hash_slice(FIX_HIDX, true);
    ck(h_moved == h_home,
       "A PURE MOVE CHANGES NO HASH: identical bytes at a new address hash identically, so a "
       "relocation is invisible to the determinism gate rather than a false desync");

    // The constexpr counterfactual -- the statement the stand-in home buffer would otherwise cost.
    ck(hash_base(FIX_HIDX) != base_of(FIX_RID),
       "THE COUNTERFACTUAL: the address a CONSTEXPR manifest would have used is NOT the address the "
       "hash now reads -- which is the entire difference between ST2 and what came before it");

    // ---- the mutation, BOTH ways -----------------------------------------------------------------
    moved[7] ^= 0xa5;
    ck(hash_slice(FIX_HIDX, true) != h_moved, "poking the RELOCATED copy CHANGES the region hash");
    moved[7] ^= 0xa5;
    ck(hash_slice(FIX_HIDX, true) == h_moved, "...and undoing that poke restores it exactly");

    home[7] ^= 0xa5;
    ck(hash_slice(FIX_HIDX, true) == h_moved,
       "poking the ABANDONED location does NOT change it -- THE half a constexpr manifest fails, "
       "because a constexpr manifest is still hashing exactly those bytes");

    // The control for the sentence above. Without it, "the abandoned poke changed nothing" is
    // equally consistent with "the poke never landed" or "that byte is outside the hashed extent",
    // and the test would be asserting its own bug rather than the mechanism.
    rebase(FIX_RID, (uint32_t)(uintptr_t)home, len);
    ck(hash_slice(FIX_HIDX, true) != h_home,
       "CONTROL: with the region back at the abandoned location, that SAME poke IS seen -- so the "
       "poke landed and the byte is hashed, and the silence above was the rebase and nothing else");
    home[7] ^= 0xa5;
    ck(hash_slice(FIX_HIDX, true) == h_home, "control: and undoing it restores the home hash");
    rebase(FIX_RID, (uint32_t)(uintptr_t)moved, len);

    // ---- the mutation HOOK follows too -----------------------------------------------------------
    // mutate_target is what the harness's own D11 negative test pokes. If it kept pointing at the
    // abandoned address, that negative test would poke dead memory and report a region as
    // undetectable when the truth was that the poke missed.
    ck(mutate_target(FIX_HIDX) == moved, "the harness's poke target follows the move");

    // ---- consumer 2: the save driver's resolve ----------------------------------------------------
    // THE FUNCTION THE LIVE DRIVER INSTALLS, not a copy of its logic -- see save_live.h.
    ck(mh::save::identity_resolve(nullptr, stock_base, len) == moved,
       "the LIVE save resolver hands back the relocated address for the region's stock address");
    ck(mh::save::identity_resolve(nullptr, stock_base + 16, 4) == moved + 16,
       "...preserving the offset within the region");

    // Totality, which is not a nicety: ST1 added a coverage guard here, made the resolver partial,
    // and the promoted save wrote a zero-byte file. The driver also resolves reads that are NOT
    // blocks -- the framing word that SIZES the progress blocks -- and no region claims those.
    const uint32_t other = base_of(SIB_RID);
    ck(mh::save::identity_resolve(nullptr, other, 4) == (void *)(uintptr_t)other,
       "TOTALITY: an address in a region that did NOT move resolves to itself");
    ck(mh::save::identity_resolve(nullptr, 0x00e16305u, 2) == (void *)(uintptr_t)0x00e16305u,
       "TOTALITY: the progress-slice framing word -- an address NO region covers -- resolves to "
       "itself rather than to null (the ST1 regression, kept watched)");

    // The one null the resolver may return: a region that moved into a SMALLER allocation than the
    // .bss reserved for it, with a request running off the end of it. Silently translating that
    // would be an out-of-bounds write on the load path.
    rebase(FIX_RID, (uint32_t)(uintptr_t)moved, len - 8);
    ck(mh::save::identity_resolve(nullptr, stock_base, len) == nullptr,
       "a request past the end of a SHRUNK rebased region is refused, not translated");
    ck(mh::save::identity_resolve(nullptr, stock_base, len - 8) == moved,
       "...while one that fits is still served");
    rebase(FIX_RID, (uint32_t)(uintptr_t)moved, len);

    // ---- consumer 3: the registry itself, on the region that did NOT move -------------------------
    // The move must be TARGETED: rebasing one region may not drag its neighbour. Consumer 4 below
    // covers the other half (a moved region takes its readers with it) over a reimplementation
    // module's own view, which is the stronger specimen -- so that half is not restated here.
    ck(live_base(SIB_RID) == (uint32_t)(uintptr_t)other,
       "a region that did NOT move is unchanged -- the move is TARGETED");

    // ---- consumer 4: a MODULE STATE VIEW (RI-AI / AI0) ---------------------------------------------
    //
    // The three consumers above are all harness/plumbing. This one is a reimplementation module's
    // own view of the state it reads, which is the case ST2 exists to make safe: mh::ai::state()
    // resolves player_data, units, buildings and the engage scratch through THIS registry, so a
    // region that moves takes the AI's reads with it.
    //
    // WHAT WOULD FAIL WITHOUT IT, precisely. A view that bound its pointers once (a `static`, which
    // is what mh::orders does and what the obvious implementation here would have been) keeps
    // serving the abandoned .bss after a rebase -- and every consumer AGREES with it, so both peers
    // of a determinism run read the same stale bytes and the gate goes green having observed
    // nothing. That is why mh::ai::state() returns BY VALUE and re-resolves per call, and this is
    // the assertion that the property actually holds rather than being merely intended.
    {
        static uint8_t         ai_home[4096];
        const mh::ai::ai_state stock = mh::ai::state();
        ck((uint32_t)(uintptr_t)stock.read.players == base_of(RID_PLAYER_DATA),
           "AI0: unrebased, the AI view's player_data pointer IS the stock base");

        rebase(RID_PLAYER_DATA, (uint32_t)(uintptr_t)ai_home, sizeof(ai_home));
        const mh::ai::ai_state moved = mh::ai::state();
        ck((void *)moved.read.players == (void *)ai_home,
           "AI0: after a rebase the AI READ view follows the region -- it is the registry's view, "
           "not a private address list");
        ck((void *)moved.own.players == (void *)ai_home,
           "AI0: ...and so does the WRITE store, so the two halves cannot end up addressing "
           "different copies of the same state");
        ck((void *)moved.read.units == (void *)(uintptr_t)base_of(RID_UNITS),
           "AI0: ...while the regions that did NOT move are untouched -- the follow is TARGETED");
        ck((void *)stock.read.players != (void *)moved.read.players,
           "AI0: THE COUNTERFACTUAL -- a view bound once would have returned the first pointer "
           "again here, which is exactly the silent staleness this arrangement removes");

        unrebase(RID_PLAYER_DATA);
        ck((uint32_t)(uintptr_t)mh::ai::state().read.players == base_of(RID_PLAYER_DATA),
           "AI0: and un-rebasing puts the AI view back, so no later test inherits a moved region");
    }

    // ---- consumer 5: the SIM's MUTABLE state interface (RI-SIM / SIM0) ----------------------------
    //
    // Consumer 4 proves the property for a view whose write half is a bag of public pointers. The
    // sim's is not: `sim_store` keeps its pointers PRIVATE and hands out only a reference to one
    // record (sim/sim_state.h, W2). That is the whole point of the encapsulation -- and it also
    // means this consumer cannot be checked the way consumer 4 is, because there is no pointer to
    // read. It is checked through the accessor instead, which is the surface a translation actually
    // uses, and is therefore the stronger of the two statements.
    //
    // RID_UNITS rather than RID_PLAYER_DATA on purpose: `units` is the widest shared region in the
    // subsystem (47 of the 307 members write it) and is the one ST2 will move first, so the region
    // whose staleness would cost most is the one this asserts about.
    {
        // _G_LLM_STRAT_CUR_UNIT is a POINTER-VALUED global (sim_state.h: "two levels of
        // indirection") -- mh::sim::state() DEREFERENCES it once to bind v.cur_unit/own.cur_unit_,
        // unlike every other member here, which is a computed pointer with no read at bind time.
        // Its stock .bss VA is exactly the kind of address this file's own banner says cannot be
        // materialised in this process (0x00e162e0, deep in the game's real .bss range) -- reading
        // it is an access violation whose fault or silence depends on ASLR, not on this test.
        // Rebased to a local slot for the whole block, same shape as RID_UNITS below, so every
        // mh::sim::state() call in this block (there are several, including the final unrebase
        // check) reads valid memory regardless of luck. The slot's CONTENT is never read through
        // (nothing in this block touches cur_unit itself), so a zeroed pointer-sized slot is enough.
        static uintptr_t cur_unit_slot = 0;
        rebase(RID_STRAT_CUR_UNIT, (uint32_t)(uintptr_t)&cur_unit_slot, sizeof(cur_unit_slot));

        // _G_LLM_STRAT_CUR_BUILDING is the SAME pointer-valued-global shape as CUR_UNIT above
        // (mh::sim::state() dereferences it once too, added SIM1B building_tick machinery third
        //-08-13) -- same unmapped-stock-VA hazard, same fix: rebase to a zeroed local
        // slot for the block.
        static uintptr_t cur_building_slot = 0;
        rebase(RID_STRAT_CUR_BUILDING, (uint32_t)(uintptr_t)&cur_building_slot, sizeof(cur_building_slot));

        // _G_LLM_STRAT_CUR_PROJECTILE is the SAME pointer-valued-global shape as CUR_UNIT/CUR_BUILDING
        // above (mh::sim::state dereferences it once too, added SIM1D-08-14,
        // opening SIM1E) -- same unmapped-stock-VA hazard, same fix.
        static uintptr_t cur_projectile_slot = 0;
        rebase(RID_STRAT_CUR_PROJECTILE, (uint32_t)(uintptr_t)&cur_projectile_slot,
               sizeof(cur_projectile_slot));

        // _G_LLM_STRAT_CUR_FX_ANIM is the SAME pointer-valued-global shape as CUR_UNIT/CUR_BUILDING/
        // CUR_PROJECTILE above (mh::sim::state() dereferences it once too, added SIM1E third batch
        // 2026-08-14) -- same unmapped-stock-VA hazard, same fix. This is now the 4th instance of
        // this exact shape; a future pointer-valued sim_view/sim_store member should default to
        // needing it, not be surprised by it (SIM1D's CUR_PROJECTILE writeup already
        // said as much).
        static uintptr_t cur_fx_anim_slot = 0;
        rebase(RID_STRAT_CUR_FX_ANIM, (uint32_t)(uintptr_t)&cur_fx_anim_slot, sizeof(cur_fx_anim_slot));

        static uint8_t           sim_home[4096];
        const mh::sim::sim_state stock = mh::sim::state();
        ck((uint32_t)(uintptr_t)stock.read.units == base_of(RID_UNITS),
           "SIM0: unrebased, the sim view's units pointer IS the stock base");

        rebase(RID_UNITS, (uint32_t)(uintptr_t)sim_home, sizeof(sim_home));
        mh::sim::sim_state moved = mh::sim::state();
        ck((void *)moved.read.units == (void *)sim_home,
           "SIM0: after a rebase the sim READ view follows the region");
        // THE WRITE HALF, through the accessor -- the only way in, and the way every translated sim
        // function reaches the roster. Slot 0 of player 0 is the first record in the region, so its
        // address is the region base.
        ck((void *)&moved.own.unit_at(0, 0) == (void *)sim_home,
           "SIM0: ...and so does the WRITE store, checked through the ACCESSOR because the store "
           "hands out no pointer -- the two halves cannot address different copies of the state");
        ck((void *)moved.read.buildings == (void *)(uintptr_t)base_of(RID_BUILDINGS),
           "SIM0: ...while regions that did NOT move are untouched -- the follow is TARGETED");
        ck((void *)stock.read.units != (void *)moved.read.units,
           "SIM0: THE COUNTERFACTUAL -- an interface bound once would have returned the first "
           "pointer again here");
        // AND THE WRITE REALLY LANDS IN THE MOVED BYTES. The pointer assertions above would still
        // pass if the accessor returned the right address and something else wrote elsewhere; this
        // one reads the relocated buffer back through the raw bytes, which nothing in the interface
        // can fake.
        memset(sim_home, 0, sizeof(sim_home));
        moved.own.unit_at(0, 0).ai_group_index = 0x5aa5;
        ck(*(uint16_t *)(sim_home + offsetof(mh::game::mh_map_object_unit, ai_group_index)) == 0x5aa5,
           "SIM0: a write through the store lands in the RELOCATED bytes, not the abandoned ones");

        unrebase(RID_UNITS);
        ck((uint32_t)(uintptr_t)mh::sim::state().read.units == base_of(RID_UNITS),
           "SIM0: and un-rebasing puts the sim view back, so no later test inherits a moved region");

        unrebase(RID_STRAT_CUR_UNIT);
        unrebase(RID_STRAT_CUR_BUILDING);
        unrebase(RID_STRAT_CUR_PROJECTILE);
        unrebase(RID_STRAT_CUR_FX_ANIM);
    }

    // ---- the soldiers anim-frame mask (2026-08-28) ------------------------------------------------
    // llm_strat_crew_soldier +0x1b `anim_change_count` is advanced ONLY by the two strategic
    // renderers as they DRAW (read 0x004518b0 / INC 0x00451955; read 0x00452443 / INC 0x004524e8) and
    // is read by nothing in the sim, so it diverges between peers holding different cameras while
    // simulation state is identical. mh::state::emit_soldiers masks it out of the VERDICT.
    //
    // THREE ARMS, because the first alone would pass for two wrong reasons -- a mask that swallowed
    // the whole record, and a poke that never landed.
    {
        constexpr int      HIDX   = HIDX_SOLDIERS;
        constexpr uint32_t STRIDE = sizeof(mh::game::mh_llm_strat_crew_soldier);
        constexpr uint32_t ANIM   = offsetof(mh::game::mh_llm_strat_crew_soldier, anim_change_count);
        constexpr uint32_t WANDER = offsetof(mh::game::mh_llm_strat_crew_soldier, idle_wander_flag);
        const region_id    RID    = HASH_REGIONS[HIDX].rid;
        const uint32_t     len    = HASH_REGIONS[HIDX].len;

        static uint8_t soldiers[23200];
        ck(len == sizeof(soldiers) && HASH_REGIONS[HIDX].offset == 0,
           "premise: the soldiers slice is the WHOLE 23200-byte region, so the fixture stands in for it");
        ck(STRIDE == 0x1d && ANIM == 0x1b && WANDER == 0x1c,
           "premise: the record is 0x1d bytes with anim_change_count at +0x1b and idle_wander_flag at "
           "+0x1c -- taken from the generated mirror, so a Ghidra retype that moved the field breaks "
           "this rather than silently masking the wrong byte");
        ck(owner_of(RID) == nullptr, "premise: soldiers is unowned, so emit_slice reads bytes");

        for (uint32_t i = 0; i < len; ++i) soldiers[i] = pattern(i);
        rebase(RID, (uint32_t)(uintptr_t)soldiers, len);

        const uint64_t masked0   = hash_slice(HIDX, true, true);
        const uint64_t unmasked0 = hash_slice(HIDX, true, false);
        ck(masked0 != unmasked0,
           "the masked and unmasked walks of the SAME bytes differ -- so the A/B toggle is real and "
           "the arms below are comparing two different things");

        uint8_t *const anim = &soldiers[3 * STRIDE + ANIM];
        *anim ^= 0x5a;
        ck(hash_slice(HIDX, true, true) == masked0,
           "poking anim_change_count does NOT change the VERDICT hash -- the render-driven byte is "
           "masked out, which is the whole fix");
        ck(hash_slice(HIDX, true, false) != unmasked0,
           "...and the poke REALLY LANDED: the unmasked arm sees it. Without this the arm above is "
           "equally consistent with a poke that missed the hashed extent");
        *anim ^= 0x5a;
        ck(hash_slice(HIDX, true, true) == masked0, "...and undoing it restores the verdict hash");

        // THE OVER-MASK ARM. Masking the record's tail rather than the single byte would swallow
        // idle_wander_flag, which the sim's 1% idle-wander roll writes -- real RNG-driven state.
        uint8_t *const wander = &soldiers[3 * STRIDE + WANDER];
        *wander ^= 0x5a;
        ck(hash_slice(HIDX, true, true) != masked0,
           "the NEIGHBOURING byte idle_wander_flag is STILL hashed -- the mask is one byte, not the "
           "record tail, and that byte is sim RNG state a mask must not hide");
        *wander ^= 0x5a;

        unrebase(RID);
        ck(!is_rebased(RID), "the soldiers fixture leaves the registry unrebased");
    }

    // ---- the planets GRAPHICS mask (LIFT-TABLE S2, 2026-09-09) -------------------------------------
    // cfg_final_struct_Planet carries two NON-CONTIGUOUS graphics windows -- bank[100] at +0x38d
    // (written only by cfg_final_planet_FillBankData, read only by the sprite-bank loaders) and
    // tlo_index + soldier_sprite_bank_offset at +0x425 -- 102 of 1063 bytes with 0x98 bytes of sim
    // between them. mh::state::emit_planets takes them out of the VERDICT so that S3 can hand the
    // graphics tail to the host without a host entry writing hashed state.
    //
    // MORE ARMS THAN THE SOLDIERS MASK, because this mask has TWO windows and FOUR boundaries, and
    // because the decision that the strings stay hashed is a claim a test can hold.
    {
        using planet                = mh::game::mh_cfg_final_struct_Planet;
        constexpr int      HIDX     = HIDX_PLANETS;
        constexpr uint32_t STRIDE   = sizeof(planet);
        constexpr uint32_t BANK     = offsetof(planet, bank);
        constexpr uint32_t BANK_N   = sizeof(planet::bank);
        constexpr uint32_t TLO      = offsetof(planet, tlo_index);
        constexpr uint32_t MAP_NAME = offsetof(planet, map_name);
        constexpr uint32_t TLO_FILE = offsetof(planet, tlo_file);
        const region_id    RID      = HASH_REGIONS[HIDX].rid;
        const uint32_t     len      = HASH_REGIONS[HIDX].len;

        static uint8_t planets[34016];
        ck(len == sizeof(planets) && HASH_REGIONS[HIDX].offset == 0,
           "premise: the planets slice is the WHOLE 34016-byte region, so the fixture stands in for it");
        ck(STRIDE == 0x427 && BANK == 0x38d && BANK_N == 100 && TLO == 0x425 && TLO + 2 == STRIDE,
           "premise: the record is 0x427 bytes with bank[100] at +0x38d and the tlo pair at +0x425 "
           "closing the record -- taken from the generated mirror, so a Ghidra retype that moved a "
           "field breaks this rather than silently masking the wrong bytes");
        ck(len % STRIDE == 0 && len / STRIDE == 32, "premise: exactly 32 whole planet records");
        ck(owner_of(RID) == nullptr, "premise: planets is unowned, so emit_slice reads bytes");

        for (uint32_t i = 0; i < len; ++i) planets[i] = pattern(i);
        rebase(RID, (uint32_t)(uintptr_t)planets, len);

        const uint64_t masked0   = hash_slice(HIDX, true, true, true);
        const uint64_t unmasked0 = hash_slice(HIDX, true, true, false);
        ck(masked0 != unmasked0,
           "the masked and unmasked walks of the SAME bytes differ -- so the A/B toggle is real and "
           "the arms below are comparing two different things");

        // THE UNMASKED ARM IS THE PRE-MASK HASH, EXACTLY -- and this is load-bearing outside the
        // mask's own A/B. The two committed UIREC oracles (spcamp-solo / tutorial-solo .oracle.gz)
        // store ABSOLUTE per-step `state` hashes captured from human recordings made before this
        // mask existed, so a changed planets walk would fail their B-vs-C arm from step 1 with
        // nothing wrong. tools/test_ui.py's ui_write_config therefore pins mask_planets_gfx=0 for
        // every UIREC arm, and that pin is only sound if the unmasked walk reproduces the OLD flat
        // `s.bytes(p, len)` stream bit-for-bit. It does -- hash_sink carries a partial block across
        // raw() calls, so chunking is invisible -- and this arm is what keeps it true.
        {
            mh::state::hash_sink flat(mh::state::sink_mode::VERDICT);
            flat.bytes(planets, len);
            ck(flat.finish() == hash_slice(HIDX, true, true, false),
               "the UNMASKED planets walk equals the flat pre-mask hash of the same bytes -- so "
               "mask_planets_gfx=0 still reproduces every committed .oracle.gz `state` value");
        }

        // WINDOW 1 -- bank[], both of its ends.
        const uint32_t w1[2] = {BANK, BANK + BANK_N - 1};
        for (const uint32_t off : w1) {
            uint8_t *const b = &planets[5 * STRIDE + off];
            *b ^= 0x5a;
            ck(hash_slice(HIDX, true, true, true) == masked0,
               "poking bank[] does NOT change the VERDICT hash -- the gfx window is masked out");
            ck(hash_slice(HIDX, true, true, false) != unmasked0,
               "...and the poke REALLY LANDED: the unmasked arm sees it. Without this the arm above "
               "is equally consistent with a poke that missed the hashed extent");
            *b ^= 0x5a;
        }
        ck(hash_slice(HIDX, true, true, true) == masked0, "...and undoing both restores the verdict");

        // WINDOW 2 -- the two-byte record tail.
        const uint32_t w2[2] = {TLO, TLO + 1};
        for (const uint32_t off : w2) {
            uint8_t *const b = &planets[5 * STRIDE + off];
            *b ^= 0x5a;
            ck(hash_slice(HIDX, true, true, true) == masked0,
               "poking tlo_index / soldier_sprite_bank_offset does NOT change the VERDICT hash");
            ck(hash_slice(HIDX, true, true, false) != unmasked0, "...and that poke landed too");
            *b ^= 0x5a;
        }

        // THE OVER-MASK ARMS -- all four boundaries. A window that ran one byte long either way
        // would swallow real sim state, and each of these four bytes is a different claim:
        //   BANK-1        the last byte of info_flc          (the window does not start early)
        //   BANK+BANK_N   source_mul[0]                      (it does not run past bank[])
        //   TLO-1         the last byte of `enemy`           (window 2 does not start early)
        //   MAP_NAME      the strings STAY hashed -- map_name is the cheapest early desync signal
        //   TLO_FILE      ...and tlo_file has a RUNTIME writer (map_ReadMap @0x004a3361)
        const uint32_t edges[5] = {BANK - 1, BANK + BANK_N, TLO - 1, MAP_NAME, TLO_FILE};
        for (const uint32_t off : edges) {
            uint8_t *const b = &planets[5 * STRIDE + off];
            *b ^= 0x5a;
            ck(hash_slice(HIDX, true, true, true) != masked0,
               "a byte OUTSIDE the two gfx windows is STILL hashed -- the mask is 102 bytes in two "
               "windows, not a record tail, and the strings are deliberately kept");
            *b ^= 0x5a;
        }
        ck(hash_slice(HIDX, true, true, true) == masked0,
           "...and with every poke undone the verdict hash is back where it started");

        // THE SAVE BLOB IS UNTOUCHED (user, 2026-09-09: the save format is not part of this change).
        // PERSIST mode degrades local() to a raw write, so the masked bytes still reach the blob --
        // and this arm is what makes that a tested property rather than a remark about state_sink.
        {
            struct collect final : mh::state::state_sink {
                explicit collect(mh::state::sink_mode m) : mh::state::state_sink(m) {}
                uint8_t  out[34016];
                uint32_t n = 0;
                void     raw(const void *p, uint32_t k) override {
                    if (n + k <= sizeof(out)) memcpy(&out[n], p, k);
                    n += k;
                }
            };
            static collect a(mh::state::sink_mode::PERSIST), b(mh::state::sink_mode::PERSIST);
            mh::state::emit_slice(HIDX, a);
            const uint8_t save0 = planets[5 * STRIDE + BANK];
            planets[5 * STRIDE + BANK] ^= 0x5a;
            mh::state::emit_slice(HIDX, b);
            planets[5 * STRIDE + BANK] = save0;
            ck(a.n == len && b.n == len,
               "PERSIST emits all 34016 bytes -- the mask drops nothing from the save blob");
            ck(memcmp(a.out, b.out, len) != 0,
               "and a poke inside the masked window DOES change the PERSIST stream, so the save "
               "still carries bank[] verbatim");
        }

        unrebase(RID);
        ck(!is_rebased(RID), "the planets fixture leaves the registry unrebased");
    }

    // ---- leave the registry as we found it --------------------------------------------------------
    unrebase(FIX_RID);
    ck(!is_rebased(FIX_RID) && hash_base(FIX_HIDX) == stock_base && live_base(FIX_RID) == stock_base,
       "the fixture leaves the registry unrebased, so no later test inherits a moved region");

    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}

// =================================================================================================
// bindtest -- THE HOST ANSWERS WHERE THE STATE IS (SB-BIND T1; docs/state-boundary.md D4, D6.5).
//
// statetest above proves a region can MOVE and every consumer follows. This proves the other half:
// that something outside libmh can SAY where it lives, through the C ABI a standalone host will
// use, and that saying "where it already is" changes nothing.
//
// THE ARM WITH TEETH IS F. Everything before it would pass on a build where bind() did nothing at
// all, because the hosted answer is the stock answer -- a no-op is exactly what A-E assert, so none
// of them can distinguish "correctly a no-op" from "not wired up". F is the one that fails if the
// wiring is absent, and it is also the arm that JUSTIFIES binding reach rather than size: it
// reproduces, on a relocated region, the nullptr a size-bind would hand the save loader.
// =================================================================================================

// The region the reach-vs-size decision is measured on: its save block is 10400 bytes over a symbol
// that measures 10296, one of the 10 overruns D6.6 enumerates. Chosen over the other nine because
// its overrun is large enough that a wrong bound is unambiguous.
constexpr region_id OVR_RID = RID_UPGRADES;

int run_bindtest() {
    printf("=== bindtest (SB-BIND T1: the host binds the registry) ===\n");
    g_checks = g_fails = 0;

    // ---- A. premise ------------------------------------------------------------------------
    ck(bound_count() == 0, "premise: net_selftest has bound nothing -- no host has answered yet");
    ck(reach_of(OVR_RID) > size_of(OVR_RID),
       "premise: the fixture region's REACH exceeds what its symbol measures (the overrun case)");

    // ---- B. the default table is the stock answer, derived from REGIONS[] --------------------
    ck(libmh_region_count() == (size_t)RID_COUNT, "libmh_region_count() is the registry's size");
    ck(libmh_default_binds(nullptr, 0) == (size_t)RID_COUNT,
       "a null/zero-cap probe still returns the full count, so a host can size its buffer");
    {
        static libmh_region_bind t[RID_COUNT];
        const size_t             n = libmh_default_binds(t, RID_COUNT);
        ck(n == (size_t)RID_COUNT, "the default table fills every region");
        int bad_base = 0, bad_size = 0, bad_count = 0, bad_id = 0;
        for (int i = 0; i < RID_COUNT; ++i) {
            const auto rid = (region_id)i;
            if (t[i].region_id != (uint32_t)i) ++bad_id;
            if ((uintptr_t)t[i].base != (uintptr_t)base_of(rid)) ++bad_base;
            if (t[i].size != reach_of(rid)) ++bad_size;
            if (t[i].count != 0) ++bad_count;
        }
        ck(bad_id == 0 && bad_base == 0, "every default entry is (its own id, its STOCK base)");
        ck(bad_size == 0,
           "...and its size is the region's REACH, not what the symbol measures -- the D6.5 "
           "decision, in the table rather than in prose");
        ck(bad_count == 0,
           "...and count is 0 throughout: the registry carries no element stride, so the stock "
           "table declares no capacity and the compile-time caps stand (T2 changes that)");

        // A cap smaller than the table writes only cap entries and still reports the true size.
        static libmh_region_bind small[4];
        for (int i = 0; i < 4; ++i) small[i].region_id = 0xffffffffu;
        ck(libmh_default_binds(small, 3) == (size_t)RID_COUNT && small[2].region_id == 2 &&
               small[3].region_id == 0xffffffffu,
           "a short cap writes exactly cap entries and does not run off the end");
    }

    // ---- C. binding the stock table is a NO-OP, and that is the hosted arm's whole basis -----
    ck(mh::state::bind_stock() == RID_COUNT, "bind_stock() binds every region through the C entry");
    ck(bound_count() == RID_COUNT, "...and every region now reports itself answered-for");
    {
        int moved = 0, drifted = 0;
        for (int i = 0; i < RID_COUNT; ++i) {
            const auto rid = (region_id)i;
            if (is_rebased(rid)) ++moved;
            if (live_base(rid) != REGIONS[i].base) ++drifted;
        }
        ck(moved == 0,
           "NOT ONE region reports itself moved after the stock bind -- `moved` is derived from "
           "whether the base actually differs, so translate() never leaves its identity path and "
           "island_move() can still claim its 48 regions later");
        ck(drifted == 0, "...and every live base is still the constexpr .bss address");
    }

    // ---- D. the one intended delta: live_size becomes reach ---------------------------------
    {
        int differs = 0, expected = 0;
        for (int i = 0; i < RID_COUNT; ++i) {
            const auto rid = (region_id)i;
            if (reach_of(rid) != size_of(rid)) ++expected;
            if (live_size(rid) != size_of(rid)) ++differs;
        }
        ck(expected > 0 && differs == expected,
           "live_size now reports REACH, and it differs from the symbol's size for EXACTLY the "
           "regions the registry says it should -- no more, no fewer");
    }

    // ---- E. a stock bind resolves the overrunning block exactly as before --------------------
    {
        void *p = mh::state::translate(base_of(OVR_RID), reach_of(OVR_RID));
        ck(p == (void *)(uintptr_t)base_of(OVR_RID),
           "under the stock bind the overrunning save block still resolves to its own address -- "
           "identity, because nothing moved");
    }

    // ---- F. THE ARM WITH TEETH: relocate it, and watch the size choice decide the outcome ----
    //
    // This is the only arm that can tell a wired bind from an unwired one, and it is the
    // measurement behind D6.5 rather than a restatement of it: bound to `size`, the save loader is
    // handed a nullptr for a block the registry itself says is in range; bound to `reach`, it is
    // handed the relocated bytes. Both directions, on the same region, in the same run.
    {
        static uint8_t arena[10400];
        ck(sizeof(arena) >= reach_of(OVR_RID), "premise: the arena covers the region's full reach");
        const auto base32 = (uint32_t)(uintptr_t)&arena[0];

        bind(OVR_RID, base32, size_of(OVR_RID)); // the WRONG choice, made deliberately
        ck(is_rebased(OVR_RID), "a bind to a different base does report the region moved");
        ck(mh::state::translate(base_of(OVR_RID), reach_of(OVR_RID)) == nullptr,
           "bound to SIZE, the overrunning block resolves to NULL -- this is the failure a "
           "size-bind would have shipped, and on the load path it is a refused restore");

        bind(OVR_RID, base32, reach_of(OVR_RID)); // the choice actually taken
        ck(mh::state::translate(base_of(OVR_RID), reach_of(OVR_RID)) == (void *)&arena[0],
           "bound to REACH, the same block resolves into the relocated arena -- the whole of why "
           "the bind table carries reach");

        // ...and the relocation is real, not just an accounting change: a write through the
        // resolved pointer lands in the arena and not in the abandoned .bss.
        auto *q                  = (uint8_t *)mh::state::translate(base_of(OVR_RID), reach_of(OVR_RID));
        q[reach_of(OVR_RID) - 1] = 0xA5;
        ck(arena[reach_of(OVR_RID) - 1] == 0xA5,
           "...and a write to the block's LAST byte lands in the arena -- the tail the symbol's "
           "size does not cover is genuinely addressable");

        unrebase(OVR_RID);
        ck(!is_rebased(OVR_RID), "the fixture puts the region back");
    }

    // ---- F2. THE ORDERING HAZARD: binding stock OVER a relocated region un-relocates it -------
    //
    // Found by running the thing, not by reasoning about it: the first live [statebind] line
    // reported "relocated 48" because the ship default promotes the AI, and ai::island_move() has
    // moved its 48 regions by report time. That is correct and harmless -- the island moves AFTER
    // the bind (MH_Core_Arm_Early since fork F3B; it was the head of MH_Harness_Init before). But it makes the reverse order a live hazard worth a check: a stock
    // bind says "this region is at its .bss address", bind believes it, and the island's relocation
    // is silently undone -- with mh::ai::state() then reading the 0xCD poison island_move left
    // behind. bind_stock() therefore SAMPLES the moved count on both sides of itself, and
    // `before != 0` is the boot-order alarm. This arm proves that alarm fires.
    {
        static uint8_t elsewhere[64];
        bind(OVR_RID, (uint32_t)(uintptr_t)&elsewhere[0], reach_of(OVR_RID));
        ck(is_rebased(OVR_RID), "premise: a region is relocated BEFORE the host answers");

        mh::state::bind_stock();
        ck(mh::state::moved_before_bind() >= 1,
           "bind_stock REPORTS that something was already relocated when it ran -- the boot-order "
           "alarm the hosted [statebind] line reads");
        ck(mh::state::moved_after_bind() == 0 && !is_rebased(OVR_RID),
           "...and the damage it is warning about is real: the stock bind put the region back at "
           "its .bss address, silently abandoning wherever the earlier mover had put it");

        // The clean ordering, for contrast: bind first, and the alarm stays quiet.
        for (int i = 0; i < RID_COUNT; ++i) unrebase((region_id)i);
        mh::state::bind_stock();
        ck(mh::state::moved_before_bind() == 0 && mh::state::moved_after_bind() == 0,
           "bound FIRST, both counters are zero -- which is what the hosted config must report");
    }

    // ---- G. the error paths, and the all-or-nothing property they rest on --------------------
    {
        ck(libmh_bind_regions(nullptr, 0) == 0, "a zero-length bind is legal and binds nothing");
        ck(libmh_bind_regions(nullptr, 1) == -1, "a null table with n != 0 is refused with -1");

        libmh_region_bind bad[2];
        bad[0].region_id = 0;
        bad[0].base      = (void *)(uintptr_t)base_of((region_id)0);
        bad[0].size      = reach_of((region_id)0);
        bad[0].count     = 7;                   // a value we can look for, to prove entry 0 was NOT applied
        bad[1].region_id = (uint32_t)RID_COUNT; // out of range
        bad[1].base      = (void *)0x1000;
        bad[1].size      = 4;
        bad[1].count     = 0;
        ck(libmh_bind_regions(bad, 2) == -2, "a region_id past the end is refused with -2");
        ck(live_count((region_id)0) == 0,
           "...and NOTHING was bound: the table is validated in full before the first write, so a "
           "rejected call cannot leave the registry half-answered");

        bad[1].region_id = 1;
        bad[1].base      = nullptr;
        bad[1].size      = 4;
        ck(libmh_bind_regions(bad, 2) == -3, "a null base with a non-zero size is refused with -3");
        ck(live_count((region_id)0) == 0, "...and again nothing was applied");

        // A null base with size 0 is the legal way to say "this region has no storage".
        bad[1].size = 0;
        ck(libmh_bind_regions(bad, 2) == 2, "a null base with size 0 is accepted");
        ck(live_count((region_id)0) == 7, "...and now the count the host declared IS stored");
    }

    // ---- I. THE DERIVED ROSTER CAPS (SB-BIND T2) ---------------------------------------------
    //
    // The eight per-player caps stop being compile-time constants and become
    // live_size / sizeof(record) / ROSTER_PLAYERS. Two arms, and the second is the one that
    // matters: the first only says the arithmetic reproduces what the constants said, which a
    // hardcoded table would also satisfy.
    {
        const roster_caps c = live_roster_caps();
        ck(c.units == STOCK_ROSTER_CAPS.units && c.buildings == STOCK_ROSTER_CAPS.buildings &&
               c.soldiers == STOCK_ROSTER_CAPS.soldiers && c.storage == STOCK_ROSTER_CAPS.storage &&
               c.turrets == STOCK_ROSTER_CAPS.turrets &&
               c.productions == STOCK_ROSTER_CAPS.productions && c.labs == STOCK_ROSTER_CAPS.labs &&
               c.mines == STOCK_ROSTER_CAPS.mines,
           "on the stock registry all EIGHT derived caps equal the .bss constants they replace");

        // THE MUTATION THE done_when ASKS FOR: bind a roster five times the size and watch the cap
        // follow. The old constant would have said 100 here, so `unit_at(7, 400)` -- a legal slot
        // on a cap-raised host -- would have resolved into player 3's row instead.
        static uint8_t big[8 * 500 * 233];
        bind(RID_UNITS, (uint32_t)(uintptr_t)&big[0], (uint32_t)sizeof(big));
        const roster_caps raised = live_roster_caps();
        ck(raised.units == 500, "binding a 5x roster raises the derived unit cap to 500");
        ck(raised.buildings == STOCK_ROSTER_CAPS.buildings,
           "...and ONLY that one -- the other seven are unmoved, so the derivation is per region "
           "and not one global scale factor");
        // The old constant fails in two DIFFERENT ways depending on the slot, and both are worth
        // stating because only the first is the one the done_when names.
        const int32_t records_raised = 8 * raised.units;            // 4000
        const int32_t records_stock  = 8 * STOCK_ROSTER_CAPS.units; // 800

        // (i) PAST THE END. Player 7 slot 400 is a legal record on a cap-raised host.
        ck(7 * raised.units + 400 < records_raised,
           "player 7 slot 400 is INSIDE the raised roster when indexed with the derived cap");
        ck(7 * STOCK_ROSTER_CAPS.units + 400 > records_stock,
           "...and the old constant would have indexed PAST THE END of the stock roster -- the "
           "mutation SB-BIND's done_when names");

        // (ii) THE QUIETER ONE: a slot legal under BOTH caps still resolves to the wrong player.
        const int32_t flat_new = 1 * raised.units + 50;            // 550, player 1
        const int32_t flat_old = 1 * STOCK_ROSTER_CAPS.units + 50; // 150, player 0 when rows are 500
        ck(flat_new != flat_old && flat_old / raised.units == 0 && flat_new / raised.units == 1,
           "...and for a slot in range under both caps it is worse than an overrun: the old "
           "constant reads player 0's row where player 1's was meant, with nothing out of bounds "
           "to catch it");
        unrebase(RID_UNITS);
        ck(live_roster_caps().units == STOCK_ROSTER_CAPS.units,
           "unbinding restores the stock cap, so no later suite inherits a raised roster");
    }

    // ---- J. POINTER-VALUED SLOTS TRANSLATE (SB-BIND T3) --------------------------------------
    //
    // _G_LLM_STRAT_CUR_UNIT holds a live unit* INTO the units region. Relocate units and the stored
    // pointer still names the abandoned .bss -- so this asserts by READ-BACK, not by assertion: the
    // translated pointer must land in the arena AND see the byte written there.
    //
    // A hand count of these slots is what this costs when it goes wrong, and it is also why the enumeration
    // is generated (tools/scan_pointer_slots.py): the hand count recorded this shape as having ONE instance
    // and there are four.
    {
        static uint8_t arena[8 * 100 * 233];
        const uint32_t stock = base_of(RID_UNITS);
        const auto     span  = (uint32_t)sizeof(mh::game::mh_map_object_unit);

        // A pointer to player 3 slot 7, expressed the way the game stores it: a STOCK address.
        const uint32_t off       = (uint32_t)((3 * 100 + 7) * span);
        auto          *stock_ptr = (mh::game::mh_map_object_unit *)(uintptr_t)(stock + off);

        ck(translate_slot(stock_ptr) == stock_ptr,
           "unrelocated, translate_slot is the IDENTITY -- which is why the hosted arm is a no-op");
        ck(translate_slot((mh::game::mh_map_object_unit *)nullptr) == nullptr,
           "a null slot stays null -- null means 'nothing current' and must not become an address");

        bind(RID_UNITS, (uint32_t)(uintptr_t)&arena[0], (uint32_t)sizeof(arena));
        auto *moved = translate_slot(stock_ptr);
        ck((uint8_t *)moved == &arena[off],
           "relocated, the stored STOCK pointer translates to the same slot in the arena");
        ck(moved != stock_ptr, "...and it is genuinely a different address, not a no-op passing");

        // READ-BACK: write through the arena, see it through the translated pointer.
        arena[off] = 0x5c;
        ck(*(uint8_t *)moved == 0x5c,
           "...and a byte written into the arena is SEEN through the translated pointer -- the "
           "read-back SB-HOSTFREE asks for, rather than an address comparison that could agree "
           "while both sides pointed somewhere dead");

        unrebase(RID_UNITS);
        ck(translate_slot(stock_ptr) == stock_ptr, "unbinding restores the identity");
    }
    {
        // ...AND THE OTHER THREE, by read-back, because `done_when` says all four and one instance
        // of a shape is exactly what G15 recorded before there were four. Each slot holds a live
        // pointer INTO a different registry region, so each has its own arena and its own poke: an
        // arm that proved CUR_UNIT and inferred the rest would be the same inference G15 was.
        //
        // Deliberately small arenas -- one record plus the offset under test -- because the claim is
        // about the ADDRESS ARITHMETIC surviving a rebase, not about the region's real extent.
        static uint8_t bld_arena[4096], prj_arena[4096], fx_arena[4096];
        struct slot_case {
            const char *what;
            region_id   target; // the region the stored pointer points INTO
            uint8_t    *arena;
            uint32_t    bytes;
            uint32_t    off; // the element offset the fixture points at
        };
        const slot_case CASES[3] = {
            {"CUR_BUILDING -> buildings", RID_BUILDINGS, bld_arena, sizeof(bld_arena),
             2u * (uint32_t)sizeof(mh::game::mh_map_object_building)},
            {"CUR_PROJECTILE -> projectile_pool", RID_STRAT_PROJECTILE_POOL, prj_arena,
             sizeof(prj_arena), 3u * (uint32_t)sizeof(mh::game::mh_llm_strat_projectile)},
            {"CUR_FX_ANIM -> fx_anims", RID_STRAT_FX_ANIMS, fx_arena, sizeof(fx_arena),
             1u * (uint32_t)sizeof(mh::game::mh_llm_strat_fx_anim)},
        };
        int followed = 0, read_back = 0, identity = 0;
        for (const auto &c : CASES) {
            ck(c.off + 8u <= c.bytes, "fixture: the arena covers the element under test");
            auto *const stock_p = (uint8_t *)(uintptr_t)(base_of(c.target) + c.off);
            if (translate_slot(stock_p) == stock_p) ++identity;

            bind(c.target, (uint32_t)(uintptr_t)c.arena, c.bytes);
            uint8_t *const moved = translate_slot(stock_p);
            if (moved == c.arena + c.off) ++followed;
            // READ-BACK, guarded like every other one here: if it did not follow, `moved` is an
            // unmapped stock VA and reading through it would fault away the failures already
            // recorded rather than report them.
            if (moved == c.arena + c.off) {
                c.arena[c.off] = 0xa7;
                if (*moved == 0xa7) ++read_back;
            }
            unrebase(c.target);
        }
        ck(identity == 3, "T3: unrelocated, all three remaining slot targets translate to identity");
        ck(followed == 3,
           "T3: relocated, a stored STOCK pointer into buildings / the projectile pool / the fx-anim "
           "pool translates to the same element in each arena");
        ck(read_back == 3,
           "T3: ...and a byte written into each arena is SEEN through the translated pointer -- all "
           "FOUR pointer-valued slots are now read-back-proven, not three proven and one inferred");
    }

    // ---- K. the LOCKSTEP binders follow a rebase, and are not cached (SB-BIND T4) ------------
    //
    // Before T4 mh::lockstep's twelve state structs were each bound in their own TU, from
    // `reinterpret_cast<T *>(mh::addr::NAME)` -- the STOCK .bss address -- inside a
    // `static const X st = {...}`. Two independent defects, and fixing either alone leaves the
    // module exactly as broken:
    //
    //   the ADDRESS  -- a stock VA does not move when a host binds a relocated region, so every
    //                   read would come back from the abandoned .bss and every consumer would agree.
    //   the STATIC   -- a function-local static runs its initializer once. Converting the addresses
    //                   to ptr<>() while leaving it would resolve them at whatever the bind was on
    //                   FIRST CALL and cache that forever, which is the same stale pointer wearing
    //                   the right syntax.
    //
    // So this arm asserts the pair. The counterfactual check below is the one that goes red if the
    // `static` ever comes back: it compares a binder's answer BEFORE a rebase against its answer
    // AFTER, and a cached struct returns the same pointer twice.
    {
        static uint8_t ls_arena[64];
        const uint32_t peer_len = REGIONS[RID_NET_LOCKSTEP_PEER_STATE].reach;
        ck(peer_len <= sizeof(ls_arena), "fixture: the peer-state arena covers the region's reach");

        const mh::lockstep::engine_state stock = mh::lockstep::state();
        ck((uint32_t)(uintptr_t)stock.peer_state == base_of(RID_NET_LOCKSTEP_PEER_STATE),
           "T4: unrebased, the lockstep engine view's peer_state IS the stock base");

        rebase(RID_NET_LOCKSTEP_PEER_STATE, (uint32_t)(uintptr_t)ls_arena, sizeof(ls_arena));
        const mh::lockstep::engine_state moved = mh::lockstep::state();
        ck((void *)moved.peer_state == (void *)ls_arena,
           "T4: after a rebase the lockstep engine view follows the region");
        ck((void *)stock.peer_state != (void *)moved.peer_state,
           "T4: THE COUNTERFACTUAL -- a `static const` binder would have returned the first "
           "pointer again here, which is what these twelve accessors did before T4");

        // A SECOND binder, so this is a property of the tranche rather than of one function.
        ck((const void *)mh::lockstep::timekeeper().peer_state == (const void *)ls_arena,
           "T4: ...and a SECOND binder in a different struct (timekeeper_state) follows the same "
           "region, so this is a property of the tranche and not of one accessor");

        // READ-BACK, not an address comparison: two pointers can agree and both point somewhere
        // dead. Write into the arena as raw bytes and read it through the view.
        //
        // GUARDED, and the guard is not a softening. If the view did NOT follow, its pointer is the
        // stock .bss VA (0x00e58c40), which this process does not map -- so an unguarded read here
        // turns a caught regression into an access violation that takes the whole run's buffered
        // stdout with it, and the three failures already recorded above would never be printed.
        // Measured: reintroducing the `static` crashed the suite with NO output at all. The guard
        // costs nothing, because the branch it skips is only reachable when a louder check has
        // already failed.
        memset(ls_arena, 0, sizeof(ls_arena));
        ls_arena[3] = 0x5c;
        if ((const void *)moved.peer_state == (const void *)ls_arena) {
            ck(moved.peer_state[3] == 0x5c,
               "T4: a byte written into the relocated arena is SEEN through the view");
        } else {
            ck(false,
               "T4: read-back SKIPPED -- the view did not follow the rebase, so reading through it "
               "would fault on an unmapped stock VA rather than report");
        }

        unrebase(RID_NET_LOCKSTEP_PEER_STATE);
        ck((uint32_t)(uintptr_t)mh::lockstep::state().peer_state ==
               base_of(RID_NET_LOCKSTEP_PEER_STATE),
           "T4: and un-rebasing puts it back, so no later check inherits a moved region");
    }
    {
        // The production-arm binds (lockstep_state.h) are the same claim for the addresses that are
        // NOT part of a translated state struct -- the text scratch every floating-message path
        // formats into. It was `mh::addr::G_TEXT_TMP` at fourteen sites before T4.
        static uint8_t txt_arena[512];
        ck((uint32_t)(uintptr_t)mh::lockstep::host_binds().text_scratch == base_of(RID_G_TEXT_TMP),
           "T4: unrebased, host_binds().text_scratch IS the stock G_TEXT_TMP");
        rebase(RID_G_TEXT_TMP, (uint32_t)(uintptr_t)txt_arena, sizeof(txt_arena));
        ck(mh::lockstep::host_binds().text_scratch == (void *)txt_arena,
           "T4: ...and it follows a rebase, re-resolved on THIS call rather than at first use");
        unrebase(RID_G_TEXT_TMP);
    }

    // ---- L. the SAVE module's live-arm binds follow a rebase too (SB-BIND T5) ------------------
    //
    // libmh/save/ always had a binder for its BLOCK addresses (save_table.gen.h, resolved through
    // mh::state::translate by the driver). What it had no binder for was the state the live arm
    // reaches for around the blocks -- the save directory, the LZW workspace, the planet clock, the
    // camera, the version table -- 31 raw `mh::addr::` sites, i.e. stock addresses that a bind does
    // not move. save/save_state.cpp is that binder now, and this is the same claim arm K makes for
    // lockstep, asserted the same way and for the same reason.
    {
        static uint8_t   save_arena[256];
        const live_binds stock = mh::save::binds();
        ck((uint32_t)(uintptr_t)stock.planet_time == base_of(RID_PLANET_TIME),
           "T5: unrebased, the save live-arm's planet_time IS the stock base");

        rebase(RID_PLANET_TIME, (uint32_t)(uintptr_t)save_arena, sizeof(save_arena));
        const live_binds moved = mh::save::binds();
        ck((void *)moved.planet_time == (void *)save_arena,
           "T5: after a rebase the save live-arm follows the region");
        ck((void *)stock.planet_time != (void *)moved.planet_time,
           "T5: THE COUNTERFACTUAL -- binds() returns by value and re-resolves, so the two calls "
           "disagree; a cached bind would have handed back the first pointer twice");
        ck((void *)moved.save_dir == (void *)(uintptr_t)base_of(RID_G_SAVE_DIR),
           "T5: ...while a member whose region did NOT move is untouched -- the follow is TARGETED");

        // READ-BACK, guarded for the same reason arm K's is: if it did not follow, the pointer is an
        // unmapped stock VA and reading through it would fault away the failures already recorded.
        memset(save_arena, 0, sizeof(save_arena));
        if ((const void *)moved.planet_time == (const void *)save_arena) {
            moved.planet_time[2] = 1234.5;
            ck(*(double *)(save_arena + 2 * sizeof(double)) == 1234.5,
               "T5: a write through the bind lands in the RELOCATED bytes, read back raw");
        } else {
            ck(false, "T5: read-back SKIPPED -- the bind did not follow, so writing through it "
                      "would land on an unmapped stock VA rather than report");
        }

        unrebase(RID_PLANET_TIME);
        ck((uint32_t)(uintptr_t)mh::save::binds().planet_time == base_of(RID_PLANET_TIME),
           "T5: and un-rebasing puts it back");
    }

    // ---- M. THE CLOCK SLICE OBSERVES ALL SIX REGIONS UNDER AN INDEPENDENT BIND (SB-HOSTFREE H0) --
    //
    // Every arm above asks whether a CONSUMER follows a rebase. This one asks the prior question --
    // whether the INSTRUMENT does -- and it is the one SB-HOSTFREE has to answer first, because the
    // whole item's proof is "relocate everything and the determinism hash still agrees". A hash that
    // stopped observing a region would agree just as loudly.
    //
    // WHAT WAS WRONG. The manifest carried ONE 48-byte entry, `time_globals`, at CURRENT_GAME_TIME.
    // Those 48 bytes are SIX registry regions -- CURRENT/LAST/TOTAL_GAME_TIME, SIM_STEP_INTERVAL,
    // GAME_TIME_DELTA, game_speed -- adjacent in .bss and nowhere else. Bind them independently, as
    // this item's own arrangement does, and the slice reads 8 live bytes plus 40 bytes of whatever
    // now lies past the relocated host: the clock family drops out of the hash silently, and a green
    // 3000-step run proves nothing about it. The generated static_asserts could not catch it -- they
    // pin a slice inside its region's `reach`, and a hash claim RAISES that reach to its own size,
    // so the containment test was circular for exactly the entry that needed it. The real gate is
    // gen_state_registry.check_slice_overruns; this is the runtime half.
    //
    // THE COUNTERFACTUAL IS THE POINT, and it is asserted directly rather than left to a code
    // reading: the same 48-byte window that the pre-H0 slice would have hashed is computed here on
    // every poke, and it must NOT move. Five pokes that change the right per-region hash while the
    // old window sits still is the blindness, exhibited.
    {
        // Six SEPARATE buffers, and the assignment is DELIBERATELY REVERSED against .bss order, so
        // no relocated neighbour can land adjacent to another by luck -- "independently" has to mean
        // the adjacency is gone, not merely that six rebase() calls were made. 64 bytes each so the
        // 48-byte counterfactual read stays inside its own buffer instead of running off the end.
        static uint8_t    clk[6][64];
        const region_id   crid[6]  = {RID_CURRENT_GAME_TIME, RID_LAST_GAME_TIME, RID_TOTAL_GAME_TIME,
                                      RID_STRAT_SIM_STEP_INTERVAL, RID_GAME_TIME_DELTA,
                                      RID_GAME_SPEED};
        const int         chidx[6] = {HIDX_CURRENT_GAME_TIME, HIDX_LAST_GAME_TIME,
                                      HIDX_TOTAL_GAME_TIME, HIDX_SIM_STEP_INTERVAL,
                                      HIDX_GAME_TIME_DELTA, HIDX_GAME_SPEED};
        const char *const cname[6] = {"current_game_time", "last_game_time", "total_game_time",
                                      "sim_step_interval", "game_time_delta", "game_speed"};

        // PREMISE: six slices, six DISTINCT regions, each one whole. If the manifest ever folds them
        // back together this fails here rather than passing vacuously five checks later.
        int distinct = 0;
        for (int i = 0; i < 6; ++i) {
            const hash_region &hr = HASH_REGIONS[chidx[i]];
            if (hr.rid == crid[i] && hr.offset == 0 && hr.len == size_of(crid[i])) ++distinct;
        }
        ck(distinct == 6, "H0: all six clock doubles are separate whole-region hash slices");

        for (int i = 0; i < 6; ++i) {
            memset(clk[i], 0, sizeof(clk[i]));
            rebase(crid[i], (uint32_t)(uintptr_t)clk[5 - i], sizeof(clk[5 - i]));
        }
        ck((uint32_t)(uintptr_t)clk[5] == live_base(RID_CURRENT_GAME_TIME) &&
               (uint32_t)(uintptr_t)clk[0] == live_base(RID_GAME_SPEED),
           "H0: the six are bound to six separate buffers, in reversed order");

        // The pre-H0 slice, reproduced exactly: 48 raw bytes read from wherever CURRENT_GAME_TIME
        // now lives. This is the read the old manifest entry made.
        auto old_window = [&]() -> uint64_t {
            const uint8_t *p =
                (const uint8_t *)(uintptr_t)hash_base(HIDX_CURRENT_GAME_TIME);
            uint64_t h = 0xcbf29ce484222325ull;
            for (int k = 0; k < 48; ++k) h = (h ^ p[k]) * 0x100000001b3ull;
            return h;
        };
        const uint64_t win0 = old_window();

        // THE FIVE THAT WERE INVISIBLE. Poke each in its own relocated home and require its OWN
        // slice hash to move -- and require the pre-H0 window not to.
        int seen = 0, window_moved = 0;
        for (int i = 1; i < 6; ++i) {
            const uint64_t before = hash_slice(chidx[i], true);
            uint8_t *const home   = (uint8_t *)(uintptr_t)live_base(crid[i]);
            home[0] ^= 0xa5;
            if (hash_slice(chidx[i], true) != before) ++seen;
            if (old_window() != win0) ++window_moved;
            home[0] ^= 0xa5; // leave the fixture as we found it for the next iteration
        }
        ck(seen == 5,
           "H0: a poke to each of the five formerly-swallowed clock regions -- LAST/TOTAL_GAME_TIME, "
           "SIM_STEP_INTERVAL, GAME_TIME_DELTA, game_speed -- changes its own slice hash, with all "
           "six bound independently");
        ck(window_moved == 0,
           "H0: THE COUNTERFACTUAL -- not one of those five pokes moves the 48-byte window the "
           "pre-H0 `time_globals` entry hashed, so the old slice really was blind to all five and "
           "these five checks are not passing for a reason that predates the split");

        // And the host of the old window is still watched, so the split lost nothing.
        const uint64_t before = hash_slice(HIDX_CURRENT_GAME_TIME, true);
        clk[5][0] ^= 0xa5;
        ck(hash_slice(HIDX_CURRENT_GAME_TIME, true) != before,
           "H0: ...while CURRENT_GAME_TIME itself is still observed, so the split narrowed the "
           "window without dropping the byte it always covered");
        clk[5][0] ^= 0xa5;

        for (int i = 0; i < 6; ++i) unrebase(crid[i]);
        int home_again = 0;
        for (int i = 0; i < 6; ++i)
            if (live_base(crid[i]) == base_of(crid[i]) && HASH_REGIONS[chidx[i]].len == 8u &&
                std::strcmp(HASH_REGIONS[chidx[i]].name, cname[i]) == 0)
                ++home_again;
        ck(home_again == 6,
           "H0: all six un-rebase to their stock bases, and each slice is the 8 bytes of the one "
           "region whose name it carries -- the names are checked because a slice labelled for the "
           "family while covering one member is how the pre-H0 entry read as correct");
    }

    // ---- N. THE RELOCATING BIND: what it selects, and what it refuses (SB-HOSTFREE) -----------
    //
    // bind_relocated() is the item's whole arrangement. Its LIVE proof is a 3000-step replay with
    // the game running; what THIS arm settles is the half a replay cannot show without a rig -- the
    // SELECTION (which regions may move and which the census pins), the ARITHMETIC (the counts a
    // report will be believed on), and the REFUSALS.
    //
    // `copy` and `poison` are OFF here, and that is a real limitation stated rather than hidden:
    // the stock .bss addresses are not this process's memory (statetest's opening comment measures
    // exactly that), so copying from them or poisoning them would fault. The bytes are proven to
    // travel by the live run; the subject here is which regions get bound, not what lands in them.
    {
        // Sized from the generated constant plus per-region alignment slack, so the fixture cannot
        // silently under-allocate the day the relocatable set grows.
        static uint8_t    arena[RELOCATABLE_BYTES + 16u * RID_COUNT];
        relocation_opts   o;
        relocation_report rep;
        o.copy = o.poison = false;
        // EVERY SAVE-BLOCK WALKER ARMED. This arm is about the SELECTION and the ARITHMETIC, so it
        // asks for the maximal set; the promotion-conditional refusal is arm N2's subject, and
        // asserting both here would leave neither legible. WALK_PROMOTABLE, not 0xff: WALK_OTHER
        // must stay unsettable, so a mask of "all bits" would quietly cover a walker with no ini
        // switch and defeat the fail-safe the enum is built around.
        o.armed_walkers = WALK_PROMOTABLE;

        const int n = bind_relocated(arena, sizeof(arena), o, &rep);
        ck(rep.refused == nullptr, "H1: the relocating bind is not refused with a full-size arena");
        ck(n == rep.relocated && n > 0, "H1: it reports what it bound, and it bound something");

        // THE ARITHMETIC HAS TO CLOSE. relocated + zero_size accounts for every relocatable region,
        // and blocked accounts for the rest -- a report whose parts do not sum to RID_COUNT is one
        // that has quietly dropped a region, which is the failure a per-count assertion misses.
        // `under_overrun` is NOT a bucket in this sum any more, and that is the 2026-09-06 change:
        // those regions MOVE now (the four blocks that span regions are decomposed), so they are
        // counted in `relocated` and reported separately as "relying on the decomposition".
        ck(rep.relocated + rep.zero_size + rep.overrunning + rep.already_moved ==
               RELOCATABLE_COUNT,
           "H1: the four relocatable-side buckets sum to the generated RELOCATABLE_COUNT");
        ck(rep.relocated == movable_count(),
           "H1: and what it MOVED is exactly the generated is_movable() set -- the bind and the "
           "counts read one predicate, so the non-vacuity assertion below cannot drift from it");
        ck(rep.overrunning == 0,
           "H1: no region is refused for OVERRUNNING its own symbol today -- all ten such regions "
           "are already blocked by an original accessor, so this bucket is empty on purpose and the "
           "day it is not is the day the save-block decomposition tranche is overdue");
        ck(rep.under_overrun > 0 && rep.under_overrun <= rep.relocated,
           "H1: regions lying UNDER someone else's overrunning window now MOVE and are reported, "
           "not refused -- they were refused until the four blocks that span regions were "
           "decomposed (SLICED_BLOCKS + check_save_block_slices). The count is a subset of the "
           "moved ones, and it is the number relying on that decomposition to stay correct");
        ck(rep.blocked == BLOCKED_COUNT,
           "H1: the blocked count is the generated one -- the census decides it, not this bind");
        ck(rep.relocated + rep.zero_size + rep.overrunning + rep.already_moved + rep.blocked ==
               RID_COUNT,
           "H1: ...and the buckets account for EVERY region, so none was silently dropped");

        // A HOST ANSWERS FOR EVERYTHING IT DOES NOT MOVE, TOO. The 364 blocked regions are bound to
        // their stock bases -- a no-op by construction, and what keeps `did every region get an
        // answer?` answerable. Without it a relocating host would be indistinguishable from one
        // that simply forgot 364 regions.
        ck(bound_count() == RID_COUNT,
           "H1: every region is BOUND -- the ones that moved to the arena and the ones the census "
           "pinned to their stock base alike");
        ck(is_bound(RID_UNITS) && !is_rebased(RID_UNITS),
           "H1: ...so a blocked region is BOUND-and-not-moved, which is a different state from "
           "unbound and must not be confused with it");

        // NON-VACUITY. A relocation that moved nothing the determinism hash reads is green over
        // bytes nobody watches. This is the same claim the live acceptance makes, asserted here
        // against the constexpr count so the two cannot disagree.
        ck(rep.hash_slices == hash_slices_on_relocatable() && rep.hash_slices > 0,
           "H1: the determinism hash now reads relocated bytes, and the count matches the "
           "constexpr one -- a relocation the oracle cannot see would be green over dead memory");
        ck(rep.tact_slices == tact_hash_slices_on_relocatable() && rep.tact_slices > 0,
           "H1: ...and the tactical hash does too");

        // TARGETED, NOT GLOBAL: a blocked region is untouched. `units` has original writers AND
        // readers, so it is the case Law 1 is actually about.
        ck(!is_rebased(RID_UNITS) && live_base(RID_UNITS) == base_of(RID_UNITS),
           "H1: a region a live original accessor still names is NOT moved -- `units` stays put");
        ck(is_rebased(RID_STRAT_ORDER_QUEUE),
           "H1: ...while one nothing original reaches IS moved -- the order queue");
        ck(rep.walker_held == 0,
           "H1: with every save-block walker armed, none is held back for an unpromoted walker");

        for (int i = 0; i < RID_COUNT; ++i) unrebase((region_id)i);
    }
    {
        // ---- N2. THE PROMOTION-CONDITIONAL REFUSAL (dead-ends G139) ---------------------------
        //
        // The census clears `order_queue`: no original body reads or writes it. That is true and it
        // is not enough. The ORIGINAL LoadPlanetFromDisk restores it from a save through the
        // immediate 0x00bb4ed0 baked into its own instruction stream -- an access no reference the
        // census reads records, and one `relocatable` would not block even if it did, because we
        // HAVE a translated body for that function and `ours` never blocks. `relocatable` means a
        // translated body EXISTS; it never meant that body is ARMED.
        //
        // WHY THE LIVE ARM DID NOT CATCH IT FOR A WHOLE ITEM: the walker runs on exactly one step of
        // a 3000-step soak. Measured -- the strategic and tactical replays relocate this region and
        // match for 3000 steps, and the same run through a real LOADGAME diverges at the load step
        // and nowhere else.
        static uint8_t    arena[RELOCATABLE_BYTES + 16u * RID_COUNT];
        relocation_opts   o;
        relocation_report rep;
        o.copy = o.poison = false;
        o.armed_walkers   = 0; // the SHIP default: no save seam is promoted

        const int n = bind_relocated(arena, sizeof(arena), o, &rep);
        ck(n > 0 && rep.refused == nullptr, "H1: the unarmed configuration still binds");
        ck(rep.walker_held == WALKER_HELD_COUNT && rep.walker_held > 0,
           "H1: it holds back exactly the generated WALKER_HELD_COUNT -- the bind and the header "
           "read one predicate, so a report cannot claim a scope the data does not support");
        ck(!is_rebased(RID_STRAT_ORDER_QUEUE),
           "H1: THE REGRESSION ITSELF -- `order_queue` is not moved while the loader that restores "
           "it from a save is still the original, whose destination address is an immediate");
        ck(is_bound(RID_STRAT_ORDER_QUEUE) &&
               live_base(RID_STRAT_ORDER_QUEUE) == base_of(RID_STRAT_ORDER_QUEUE),
           "H1: ...and it is BOUND TO STOCK rather than skipped, so the original walker and every "
           "registry consumer agree about where the bytes are -- correct, not merely undamaged");
        ck(rep.relocated == movable_count_under(0u) && rep.relocated + rep.walker_held ==
                                                           movable_count(),
           "H1: what moved is the promotion-conditional set, and the two sets differ by exactly "
           "the held count -- neither number is free to drift from the other");
        ck(bound_count() == RID_COUNT,
           "H1: every region still gets an answer, held ones included");
        for (int i = 0; i < RID_COUNT; ++i) unrebase((region_id)i);

        // ARMING ONE WALKER IS NOT ARMING ITS PAIR, and this is the arm that would have caught the
        // wrong conclusion: `order_queue` is written by LoadPlanetFromDisk AND read by
        // SavePlanetToDisk, so promoting only the loader leaves the saver writing a file from the
        // abandoned .bss. That failure never diverges -- it is in bytes only the save format reads
        // (dead-ends G135's family) -- so nothing downstream would report it.
        o.armed_walkers = WALK_PLANET_LOAD;
        bind_relocated(arena, sizeof(arena), o, &rep);
        ck(!is_rebased(RID_STRAT_ORDER_QUEUE),
           "H1: promoting the LOADER alone does not release a region the SAVER also names");
        ck(rep.walker_held > 0, "H1: ...and it is still counted as held, not silently dropped");
        for (int i = 0; i < RID_COUNT; ++i) unrebase((region_id)i);

        o.armed_walkers = WALK_PLANET_LOAD | WALK_PLANET_SAVE;
        bind_relocated(arena, sizeof(arena), o, &rep);
        ck(is_rebased(RID_STRAT_ORDER_QUEUE),
           "H1: promoting BOTH releases it -- the gate is conditional, not a permanent refusal, so "
           "promoting a walker earns its regions back in the same run");
        for (int i = 0; i < RID_COUNT; ++i) unrebase((region_id)i);
    }
    {
        // THE REFUSALS, each of which would otherwise present as a successful relocation.
        static uint8_t    small[64];
        relocation_opts   o;
        relocation_report rep;
        o.copy = o.poison = false;

        ck(bind_relocated(small, sizeof(small), o, &rep) == 0 && rep.refused != nullptr,
           "H1: an arena too small is refused WHOLE -- a part-applied relocation leaves the "
           "registry answering with a mix of arena and stock addresses and nothing can tell");
        int dirty = 0;
        for (int i = 0; i < RID_COUNT; ++i)
            if (is_rebased((region_id)i)) ++dirty;
        ck(dirty == 0, "H1: ...and the refusal really did bind nothing");

        ck(bind_relocated(nullptr, 1u << 20, o, &rep) == 0 && rep.refused != nullptr,
           "H1: a null arena is refused");
    }
    {
        // THE MUTATION ARM, offline half. The live acceptance pins one region to its stock base --
        // over POISON, because a pin over untouched memory diverges from nothing and would prove
        // the opposite of what it is for -- and requires the run to go red naming that region. The
        // divergence itself needs a replay. What is checkable here is that the pin does what it
        // says, and, more importantly, that a pin naming NOTHING is REFUSED rather than producing a
        // clean relocated run that looks exactly like the result the mutation exists to disprove.
        static uint8_t    arena[RELOCATABLE_BYTES + 16u * RID_COUNT];
        relocation_opts   o;
        relocation_report rep;
        o.copy = o.poison = false;
        o.armed_walkers   = WALK_PROMOTABLE; // ...or the pin target below would not be movable
        o.pin_stock       = REGIONS[RID_STRAT_ORDER_QUEUE].name;

        const int n = bind_relocated(arena, sizeof(arena), o, &rep);
        ck(n > 0 && rep.pinned == 1 && rep.pinned_name != nullptr,
           "H1: the pinned run binds, and reports that exactly one region was pinned");
        ck(!is_rebased(RID_STRAT_ORDER_QUEUE),
           "H1: THE MUTATION -- the pinned region is left at its stock base while the rest move");
        ck(is_rebased(RID_STRAT_ORDER_STAGING),
           "H1: ...and it is the ONLY one left behind, so a divergence localizes to it");
        for (int i = 0; i < RID_COUNT; ++i) unrebase((region_id)i);

        o.pin_stock = "_G_LLM_NO_SUCH_REGION_EXISTS";
        ck(bind_relocated(arena, sizeof(arena), o, &rep) == 0 && rep.refused != nullptr,
           "H1: a pin that names nothing is REFUSED -- a mutation that did not happen would "
           "present as the clean run it exists to disprove");
        for (int i = 0; i < RID_COUNT; ++i) unrebase((region_id)i);

        // AND THE SUBTLER ONE, which is why the guard checks `is_movable` rather than merely "is it
        // a region": a target the bind will not move would make the pin a NO-OP, and the run would
        // come out clean and be read as "the relocation survived the mutation" -- the strongest
        // possible wrong conclusion. `units` is the standing example: the census pins it (9 live
        // original writers, 78 readers), so it is a region, it is not movable, and a pin on it must
        // be refused rather than silently skipped.
        o.pin_stock = REGIONS[RID_UNITS].name;
        ck(!is_movable(RID_UNITS), "H1: fixture: the pin target really is a region the bind refuses");
        ck(bind_relocated(arena, sizeof(arena), o, &rep) == 0 && rep.refused != nullptr,
           "H1: pinning a region the bind will not move is REFUSED -- it would silently be a "
           "no-op mutation and the clean run would read as the mutation surviving");
        for (int i = 0; i < RID_COUNT; ++i) unrebase((region_id)i);

        // AND THE LIFTED REFUSAL ITSELF, asserted rather than left to the bucket count: a region
        // that lies under an overrunning window is movable again BECAUSE the block reaching it is
        // decomposed. If that decomposition is ever dropped, check_save_block_slices goes red first;
        // this is the runtime half of the same statement.
        ck(is_movable(RID_STRAT_LOCAL_PLAYER_SLOT),
           "H1: a region under _G_LLM_GAME_SESSION_MODE's overrunning window is MOVABLE again -- "
           "the block that reaches it is served run by run now, so moving it no longer writes 0xCD "
           "into a save");
    }

    // ---- H. leave the registry as we found it ------------------------------------------------
    for (int i = 0; i < RID_COUNT; ++i) unrebase((region_id)i);
    {
        int dirty = 0;
        for (int i = 0; i < RID_COUNT; ++i) {
            const auto rid = (region_id)i;
            if (is_rebased(rid) || is_bound(rid) || live_base(rid) != REGIONS[i].base ||
                live_size(rid) != REGIONS[i].size || live_count(rid) != 0)
                ++dirty;
        }
        ck(dirty == 0,
           "the fixture leaves the registry unbound and unrebased, so no later suite inherits it");
    }

    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
