//
// sim_bldg_anim_tick_selftest.cpp -- `simtest` case for the ONE un-shadowable member of SIM1-BLDGCB
// batch H's anim-tick slice: llm_strat_bldg_anim_state_turret @0x00476605 (sim/sim_bldg_anim_tick.h/
// .cpp). llm_strat_bldg_anim_tick has real rig evidence (migration_sweep, arming via a manual
// `buildings` extra_regions declaration -- see the header banner) and needs no oracle here.
//
// PLUS (verification-debt drain, batch H, 2026-08-23):
// llm_strat_bldg_anim_state_helipad_a @0x00477231. This one IS shadow-armed (same `buildings`
// extra_regions declaration as anim_tick) but got 0 calls across TWO all-AI soak runs (15000 then
// 40000 steps @1000%) -- SIM1-BLDGCB records the root cause as scenario shape (the
// soak lane is "_solo", a 1v1 with no alien-race AI player, so an A-race-only building callback
// structurally cannot fire regardless of step count; its "_h"-named non-alien siblings elsewhere in
// this batch DID get real coverage in the same run). Unlike llm_strat_bldg_anim_state_barracks_
// garage_a/_airfield_a (sim_bldg_anim_state_online_a_selftest.cpp), this function has NO callees at
// all (see sim_bldg_anim_tick.h's "CALLEES: NONE" note) -- so, unlike those two, its oracle here covers
// EVERY branch, not just the reachable ones. Still graded T2, not T1, in the ledger: the shadow site
// stays armed and the rig might still reach it (e.g. a 2v2+ multi-race scenario), and an oracle proves
// only self-consistency with the own reading of the disassembly, never equivalence against
// the original executing on real state -- see the migration-session skill's step 6.
//
// EXPECTED BEHAVIOUR HAND-DERIVED FROM tmp/decomp_sim/llm_strat_bldg_anim_state_helipad_a_00477231.asm
// (cross-checked against sim_bldg_anim_tick.h/.cpp's own derivation, not re-derived here) -- NOT read
// off any Ghidra .c draft. Two asm-confirmed differences from its anim_tick sibling, both pinned below:
// the per-slot guard is `0 < anim[slot]` (skips zero AND negative, unlike anim_tick's `!= 0`), and a
// looked-up frame with `time == 0.0` skips the WHOLE elapsed-time update for that slot (anim_tick has
// no such guard at all).
//
// EXPECTED BEHAVIOUR HAND-DERIVED FROM tmp/decomp_sim/llm_strat_bldg_anim_state_turret_00476605.asm --
// NOT read off the .cpp. The whole 0x22-byte body, after the inert `utils_assert_stack_capacity`
// prologue call, is register PUSH/POP pairs and RET: no other CALL, no branch, no memory read or
// write of any kind. This is the BLDG_TICK2_FUNCS table's deliberate empty override installed for the
// A_TURRET/H_TURRET building types (finding 2026-08-23-0149-2), not a stub.
//
// SCOPE (honest, not exhaustive): there is nothing to branch-cover -- the body is empty, and (unlike
// llm_strat_done_default/_done_shuttle) it does not even take a sim_view/sim_store parameter pair, so
// it cannot reach ANY global by construction. Coverage here is that calling it touches NONE of the
// current building's own fields (seeded with distinct sentinels beforehand so the post-call comparison
// is an actual observation, not "it was already zero") -- same shape as the done_handlers' oracle.
//
#include "sim/sim_bldg_anim_tick.h"

#include <cstring>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

constexpr int32_t PLAYER = 3;
constexpr int32_t INDEX  = 2;

// ---- helipad_a helpers, duplicated per-TU per this project's own established precedent (same pattern
// sim_bldg_anim_tick.cpp / sim_bldg_anim_state_online.cpp use for the identical flattened field pair).
inline void store_u32_le(uint8_t *dst, uint32_t value) {
    dst[0] = (uint8_t)(value);
    dst[1] = (uint8_t)(value >> 8);
    dst[2] = (uint8_t)(value >> 16);
    dst[3] = (uint8_t)(value >> 24);
}
inline uint32_t load_u32_le(const uint8_t *src) {
    return (uint32_t)src[0] | ((uint32_t)src[1] << 8) | ((uint32_t)src[2] << 16) |
           ((uint32_t)src[3] << 24);
}
inline int32_t anim_slot_get(const building &b, int32_t slot) {
    return static_cast<int32_t>(load_u32_le(&b.anim[slot * 4]));
}
inline void anim_slot_set(building &b, int32_t slot, int32_t value) {
    store_u32_le(&b.anim[slot * 4], static_cast<uint32_t>(value));
}
inline int32_t cfg_anim_slot_get(const cfg_building &cb, int32_t slot) {
    return static_cast<int32_t>(load_u32_le(&cb.anim[slot * 4]));
}
inline void cfg_anim_slot_set(cfg_building &cb, int32_t slot, int32_t value) {
    store_u32_le(&cb.anim[slot * 4], static_cast<uint32_t>(value));
}

constexpr int32_t HA_PLAYER = 5;
constexpr int32_t HA_BLDG   = 1;
constexpr int32_t FRAME_IDX = 40; // arbitrary Anim[] index used as anim[slot]'s value

// Common setup: one building, sprite_quantity=1 (so only slot 0 is in scope unless a case overrides
// it), a `time`-only Anim[] entry (next=0, forcing chain-restart on the first over-time iteration),
// efficiency set to a value that would change the timing result if wrongly applied differently.
sim_fixture &ha_setup(sim_fixture &f) {
    f.reset();
    f.view_cur_player  = HA_PLAYER;
    f.view_cur_index   = HA_BLDG;
    f.cur_building_ptr = &f.b(HA_PLAYER, HA_BLDG);
    building &b        = *f.cur_building_ptr;
    b.efficiency       = 4.0;
    cfg_building &cb   = f.cfg_buildings[b.building_id];
    cb.sprite_quantity = 1;
    f.game_clock       = 100.0;
    return f;
}

// ---- case 1: the GUARD -- anim[slot] NEGATIVE is SKIPPED (0 < anim[slot], not != 0) -- the finding
// that distinguishes this function from its anim_tick sibling. anim_dur[slot] is left byte-identical.
// anim_dur[0] is seeded ABOVE game_clock so elapsed = (game_clock-anim_dur)*efficiency is NEGATIVE --
// advance_anim_slot's own FIRST line unconditionally sets anim_dur[slot]=game_clock regardless of what
// the while-loop does next, so if the guard wrongly let this through, anim_dur[0] would become
// game_clock (100.0), clearly distinct from the sentinel (150.0). (A same-sign elapsed sentinel here
// would NOT catch a wrongly-passed guard: when the whole elapsed is spent in one step with no chain
// walk, anim_dur[slot] = game_clock - elapsed/efficiency algebraically reduces back to the ORIGINAL
// anim_dur regardless of efficiency -- confirmed by mutation: an earlier version of this case with
// anim_dur[0]=77.0/elapsed>0 passed even after reverting the guard to `!=0`.)
void test_helipad_a_negative_anim_slot_skipped_by_guard() {
    sim_fixture f;
    ha_setup(f);
    building &b = f.b(HA_PLAYER, HA_BLDG);
    anim_slot_set(b, 0, -1);       // negative -- anim_tick's `!= 0` guard would NOT skip this
    b.anim_dur[0]         = 150.0; // ABOVE game_clock(100) -> elapsed would be NEGATIVE if reached
    f.anim_frames[0].time = 999.0; // if wrongly reached (index -1+1=0), time!=0.0 so advance_anim_slot runs
    const sim_view v      = f.view();
    sim_store      s      = f.store();

    detail::bldg_anim_state_helipad_a(v, s);

    ck(b.anim_dur[0] == 150.0,
       "helipad_a FINDING @0x00477286: anim[slot]=-1 is SKIPPED by the `0 < anim[slot]` guard "
       "(unlike anim_tick's `!= 0`) -- anim_dur[0] stays the sentinel 150.0, untouched (if the guard "
       "wrongly let this through, advance_anim_slot's unconditional first line would stamp anim_dur[0] "
       "= game_clock = 100.0 instead)");
}

// ---- case 2: anim[slot] == 0 is ALSO skipped (matches anim_tick on this one value).
void test_helipad_a_zero_anim_slot_skipped() {
    sim_fixture f;
    ha_setup(f);
    building &b = f.b(HA_PLAYER, HA_BLDG);
    anim_slot_set(b, 0, 0);
    b.anim_dur[0]    = 88.0;
    const sim_view v = f.view();
    sim_store      s = f.store();

    detail::bldg_anim_state_helipad_a(v, s);

    ck(b.anim_dur[0] == 88.0, "helipad_a: anim[slot]==0 is skipped -- anim_dur[0] untouched @0x00477286");
}

// ---- case 3: the ZERO-DURATION SKIP -- anim[slot] positive, but the looked-up frame's `time` is
// exactly 0.0 -- skips the WHOLE elapsed-time update (anim_dur[slot] untouched), NOT spent instantly.
// anim_tick has no such guard at all -- the second finding that distinguishes the two functions.
void test_helipad_a_zero_duration_frame_skips_whole_update() {
    sim_fixture f;
    ha_setup(f);
    building &b = f.b(HA_PLAYER, HA_BLDG);
    anim_slot_set(b, 0, FRAME_IDX);
    f.anim_frames[FRAME_IDX + 1].time = 0.0;
    f.anim_frames[FRAME_IDX + 1].next = 0;
    b.anim_dur[0]                     = 55.0; // sentinel
    const sim_view v                  = f.view();
    sim_store      s                  = f.store();

    detail::bldg_anim_state_helipad_a(v, s);

    ck(b.anim_dur[0] == 55.0,
       "helipad_a FINDING @0x004772bb-0x004772c8: time==0.0 skips the WHOLE update -- anim_dur[0] "
       "stays the sentinel 55.0, not spent instantly");
}

// ---- case 4: elapsed <= time on the FIRST pass -- spends the remainder in one step, no chain walk.
void test_helipad_a_elapsed_within_time_spends_remainder() {
    sim_fixture f;
    ha_setup(f);
    building &b = f.b(HA_PLAYER, HA_BLDG);
    anim_slot_set(b, 0, FRAME_IDX);
    f.anim_frames[FRAME_IDX + 1].time = 10.0;
    f.anim_frames[FRAME_IDX + 1].next = 0;
    b.anim_dur[0]                     = 98.0; // elapsed = (100-98)*4 = 8 <= time(10) -> spend remainder, no chain walk
    const sim_view v                  = f.view();
    sim_store      s                  = f.store();

    detail::bldg_anim_state_helipad_a(v, s);

    ck(anim_slot_get(b, 0) == FRAME_IDX, "helipad_a: elapsed<=time -- anim[slot] unchanged (no chain walk)");
    ck(b.anim_dur[0] == 100.0 - 8.0 / 4.0,
       "helipad_a: elapsed<=time -- anim_dur[0] = GAME_CLOCK - elapsed/efficiency = 100 - 2 = 98");
}

// ---- case 5: the CHAIN-FOLLOW branch (Anim[...].next != 0) -- advances anim[slot] by `next` and
// consumes `time` from elapsed, without restarting from the cfg table. `time` is read ONCE, before the
// loop, and stays FIXED for every iteration even as the chain-walk changes anim[slot] (see
// sim_bldg_anim_tick.h's dVar1/dVar2 note) -- elapsed is chosen in (time, 2*time] so the loop takes
// EXACTLY ONE over-time iteration (the chain-follow) before the remainder-spend arm ends it, so the
// asserted anim[slot] is the chain-follow's result, not overwritten by a later iteration.
void test_helipad_a_chain_follow_advances_by_next() {
    sim_fixture f;
    ha_setup(f);
    building &b = f.b(HA_PLAYER, HA_BLDG);
    anim_slot_set(b, 0, FRAME_IDX);
    f.anim_frames[FRAME_IDX + 1].time = 10.0;
    f.anim_frames[FRAME_IDX + 1].next = 7;     // nonzero -> chain-follow, not chain-restart
    b.anim_dur[0]                     = 96.25; // elapsed = (100-96.25)*4 = 15, in (10,20] -> exactly one chain-follow, then
                                               // the remainder (15-10=5) is spent by the next iteration's elapsed<=time arm
    const sim_view v = f.view();
    sim_store      s = f.store();

    detail::bldg_anim_state_helipad_a(v, s);

    ck(anim_slot_get(b, 0) == FRAME_IDX + 7,
       "helipad_a: chain-follow -- anim[slot] = FRAME_IDX + Anim[...].next (40+7=47)");
    ck(b.anim_dur[0] == 100.0 - 5.0 / 4.0,
       "helipad_a: chain-follow -- anim_dur[0] = GAME_CLOCK - remainder/efficiency = 100 - 1.25 = 98.75");
}

// ---- case 6: the CHAIN-RESTART branch (Anim[...].next == 0) -- restarts from cfg_buildings[bid].
// anim[slot+1], NOT from a per-function online-state switch (this function has none). Same
// exactly-one-iteration setup as case 5.
void test_helipad_a_chain_restart_from_cfg_table() {
    sim_fixture f;
    ha_setup(f);
    building     &b  = f.b(HA_PLAYER, HA_BLDG);
    cfg_building &cb = f.cfg_buildings[b.building_id];
    anim_slot_set(b, 0, FRAME_IDX);
    f.anim_frames[FRAME_IDX + 1].time = 10.0;
    f.anim_frames[FRAME_IDX + 1].next = 0; // chain-restart
    cfg_anim_slot_set(cb, 1, 321);         // cfg.anim[slot+1], slot=0 -> cfg.anim[1]
    b.anim_dur[0]    = 96.25;              // elapsed = 15, in (10,20] -> exactly one chain-restart
    const sim_view v = f.view();
    sim_store      s = f.store();

    detail::bldg_anim_state_helipad_a(v, s);

    ck(anim_slot_get(b, 0) == 321,
       "helipad_a: chain-restart -- anim[slot] = cfg_buildings[bid].anim[slot+1] (cfg.anim[1]=321)");
}

// ---- case 7: PER-SLOT LOOPING -- sprite_quantity=2 ticks BOTH slot 0 and slot 1 independently in one
// call, proving the `for (slot=0; slot<sprite_quantity; ++slot)` loop, not a single-slot shortcut. Same
// exactly-one-iteration setup as cases 5/6, independently per slot (different `time`, so a different
// elapsed window).
void test_helipad_a_loops_every_slot_up_to_sprite_quantity() {
    sim_fixture f;
    ha_setup(f);
    building     &b    = f.b(HA_PLAYER, HA_BLDG);
    cfg_building &cb   = f.cfg_buildings[b.building_id];
    cb.sprite_quantity = 2;

    anim_slot_set(b, 0, FRAME_IDX);
    f.anim_frames[FRAME_IDX + 1].time = 10.0;
    f.anim_frames[FRAME_IDX + 1].next = 0;
    cfg_anim_slot_set(cb, 1, 111); // slot 0's restart target (cfg.anim[0+1])
    b.anim_dur[0] = 96.25;         // elapsed = 15, in (10,20] -> exactly one restart

    anim_slot_set(b, 1, FRAME_IDX + 1);
    f.anim_frames[FRAME_IDX + 2].time = 20.0;
    f.anim_frames[FRAME_IDX + 2].next = 0;
    cfg_anim_slot_set(cb, 2, 222); // slot 1's restart target (cfg.anim[1+1])
    b.anim_dur[1]    = 92.5;       // elapsed = (100-92.5)*4 = 30, in (20,40] -> exactly one restart
    const sim_view v = f.view();
    sim_store      s = f.store();

    detail::bldg_anim_state_helipad_a(v, s);

    ck(anim_slot_get(b, 0) == 111, "helipad_a: slot 0 ticked -- anim[0] = cfg.anim[1] = 111");
    ck(anim_slot_get(b, 1) == 222, "helipad_a: slot 1 ALSO ticked in the same call -- anim[1] = cfg.anim[2] = 222");
}

} // namespace

void run_bldg_anim_state_helipad_a_tests() {
    test_helipad_a_negative_anim_slot_skipped_by_guard();
    test_helipad_a_zero_anim_slot_skipped();
    test_helipad_a_zero_duration_frame_skips_whole_update();
    test_helipad_a_elapsed_within_time_spends_remainder();
    test_helipad_a_chain_follow_advances_by_next();
    test_helipad_a_chain_restart_from_cfg_table();
    test_helipad_a_loops_every_slot_up_to_sprite_quantity();
}

void run_bldg_anim_tick_tests() {
    sim_fixture fx;
    fx.reset();

    building &bld   = fx.b(PLAYER, INDEX);
    bld.building_id = 7;
    bld.efficiency  = 0.5;
    for (int i = 0; i < 12; ++i) {
        bld.anim[i * 4 + 0] = (uint8_t)(0x10 + i);
        bld.anim[i * 4 + 1] = (uint8_t)(0x20 + i);
        bld.anim[i * 4 + 2] = (uint8_t)(0x30 + i);
        bld.anim[i * 4 + 3] = (uint8_t)(0x40 + i);
        bld.anim_dur[i]     = 111.0 + i;
    }
    bld.online_state    = 9;
    fx.cur_building_ptr = &bld; // MUTATION-TEST: bldg_anim_state_turret's own.cur_building() must hit this record

    const building before = bld;

    // T1 -- llm_strat_bldg_anim_state_turret @0x00476605: no arguments, no state() call, no return --
    // see sim_bldg_anim_tick.h's `void bldg_anim_state_turret();` (unlike done_default it takes no
    // sim_view/sim_store either, because the disassembly shows no CALL besides the inert prologue and
    // no read/write of anything).
    mh::sim::detail::bldg_anim_state_turret();

    ck(std::memcmp(&bld, &before, sizeof(building)) == 0,
       "bldg_anim_state_turret@0x00476605: the seeded building record is byte-identical after the "
       "call -- the whole body besides the inert prologue is PUSH/POP/RET, no write of any kind");
}

} // namespace mh::sim::test
