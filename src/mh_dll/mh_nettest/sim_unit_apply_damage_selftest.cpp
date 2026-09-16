//
// sim_unit_apply_damage_selftest.cpp -- `simtest` cases for llm_strat_unit_apply_damage @0x0047e380
// (sim/sim_unit_apply_damage.h/.cpp), SIM1E. This function is PERMANENTLY DO-NOT-ARM (its lethal
// branch reaches llm_strat_unit_state_die_explode, which under a shadow arm fires real sounds/fx-anim
// spawns/UI notifications and touches _G_LLM_STRAT_PLAYERS and beyond) -- this file is the only
// execution evidence it will ever get.
//
// SCOPE (honest, not exhaustive): the early-out (state==DIE_EXPLODE/CORPSE_FOW_DECAY, a FULL body
// skip), the energy-=pending_damage/pending_damage-zero arithmetic, the death test's TWO halves
// (plain energy<=0.0, and the trunc(energy)<0 x87 fallback) at exact/ulp boundaries plus the int32
// truncation-overflow edge, the HQ energy credit (own player's slot 0, distinct from the dying unit's
// own slot), the DEPLOY_TO_BUILDING footprint-unmap call's ARGUMENTS (tile_x/tile_y/building_idx) on
// both sides of that gate, full call order + argument tuples for every outward call via one shared
// trace, the soldier-strip loop's continue/exit boundary (exact + one ulp either side) with per-
// iteration re-read of unit_proto_id/cfg row, and the literal roster-vs-cur_unit-pointer asymmetry the
// header banner flags between the loop's GATE and its BODY.
//
// DOES NOT COVER: the actual tile_objects/passable WRITES a footprint-unmap performs -- that logic
// lives entirely inside llm_map_bldg_footprint_set_passable, an ORIGINAL un-migrated function this
// file only reaches through a recording stub (c.bldg_footprint_set_passable); detail::
// unit_apply_damage itself never touches tile_objects/passable, so this file pins the ARGUMENTS handed
// to that callee, not its effects. Does not cover the damage-smoke LEVEL computation either -- that is
// llm_strat_unit_update_damage_smoke's own body (a sibling function, sim_unit_update_damage_smoke.h),
// reached here only as another recording stub (c.unit_update_damage_smoke); this file pins that it is
// called with the right (player, index), not what it computes internally.
//
// EXPECTED BEHAVIOUR HAND-DERIVED FROM tmp/decomp/llm_strat_unit_apply_damage_0047e380.asm -- not from
// the .cpp. Every assertion below cites the instruction address(es) it pins.
//
#include "sim/sim_unit_apply_damage.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {
using namespace mh::sim;

// ---- shared trace: one sequence proves CALL ORDER across all 7 callees -------------------------
std::vector<const char *> g_trace;
void                      tr(const char *tag) { g_trace.push_back(tag); }

bool trace_eq(const std::vector<const char *> &want) {
    if (g_trace.size() != want.size()) return false;
    for (size_t i = 0; i < want.size(); ++i)
        if (std::strcmp(g_trace[i], want[i]) != 0) return false;
    return true;
}

// ---- DECLARED NEED WORKAROUND: sim_view::unit_death_hq_energy_credit (the header banner's own
// DECLARED NEED, resolved by the conductor as a new sim_view member -- see sim_state.h/.cpp) has NO
// matching sim_fixture member and is left unbound (nullptr) by sim_fixture::view(). Rather than add one
// to the conductor-owned sim_test_support.h, every case here takes fx.view()'s return BY VALUE and
// patches this ONE pointer field to a file-local double before calling detail::unit_apply_damage --
// sim_view is a plain struct of pointers, so this is a same-TU-only override, not a fixture edit. The
// exact real magnitude of DAT_005013d0 is a DATA constant the .asm cannot prove (it proves only the
// FADD *shape*, not the operand's value) -- see g_hq_credit's own comment below for why an arbitrary
// sentinel is used instead of guessing/hardcoding "-1.0".
double g_hq_credit = 0.0;

// ---- return-value/arg knobs and per-callee recorders (7, one per unit_apply_damage_calls member) ---
std::vector<uint16_t> g_set_state_calls;
void                  rec_unit_set_state(uint16_t new_state) {
    tr("unit_set_state");
    g_set_state_calls.push_back(new_state);
}

struct NotifyRemovedCall {
    uint32_t flags, object_index;
    int32_t  hard_remove;
};
std::vector<NotifyRemovedCall> g_notify_removed;
void                           rec_ai_notify_object_removed(uint32_t flags, uint32_t object_index, int32_t hard_remove) {
    tr("ai_notify_object_removed");
    g_notify_removed.push_back({flags, object_index, hard_remove});
}

int  g_die_explode_count = 0;
void rec_unit_state_die_explode() {
    tr("unit_state_die_explode");
    ++g_die_explode_count;
}

struct PlayerIndexCall {
    uint32_t player;
    int32_t  index;
};
std::vector<PlayerIndexCall> g_remove_last;
void                         rec_unit_soldier_remove_last(uint32_t player, int32_t unit_index) {
    tr("unit_soldier_remove_last");
    g_remove_last.push_back({player, unit_index});
}

struct FootprintCall {
    int32_t tile_x, tile_y, building_idx;
};
std::vector<FootprintCall> g_footprint;
void                       rec_bldg_footprint_set_passable(int32_t tile_x, int32_t tile_y, int32_t building_idx) {
    tr("bldg_footprint_set_passable");
    g_footprint.push_back({tile_x, tile_y, building_idx});
}

std::vector<PlayerIndexCall> g_update_damage_smoke;
void                         rec_unit_update_damage_smoke(uint32_t player, int32_t unit_idx) {
    tr("unit_update_damage_smoke");
    g_update_damage_smoke.push_back({player, unit_idx});
}

std::vector<PlayerIndexCall> g_notify_ui;
void                         rec_unit_notify_ui(uint32_t side, uint32_t unit_index) {
    tr("unit_notify_ui");
    g_notify_ui.push_back({side, static_cast<int32_t>(unit_index)});
}

const unit_apply_damage_calls g_calls = {
    &rec_unit_set_state,
    &rec_ai_notify_object_removed,
    &rec_unit_state_die_explode,
    &rec_unit_soldier_remove_last,
    &rec_bldg_footprint_set_passable,
    &rec_unit_update_damage_smoke,
    &rec_unit_notify_ui,
};

void reset_observations() {
    g_trace.clear();
    g_set_state_calls.clear();
    g_notify_removed.clear();
    g_die_explode_count = 0;
    g_remove_last.clear();
    g_footprint.clear();
    g_update_damage_smoke.clear();
    g_notify_ui.clear();
}

// ---- fixture seeding -------------------------------------------------------------------------------
struct Seed {
    uint16_t player = 2;
    int32_t  index  = 5; // nonzero -- distinct from the HQ credit's fixed slot 0
    uint16_t state  = 9; // an "other" state: not DIE_EXPLODE(2)/CORPSE_FOW_DECAY(4)/DEPLOY_TO_BUILDING(0x17)

    double   energy         = 50.0;
    double   pending_damage = 10.0;
    uint16_t proto_id       = 7;
    uint8_t  x = 100, y = 100;

    uint16_t equiv_bid      = 3;     // cfg_units[proto_id].equivalent -> cfg_buildings row + 3rd call arg
    int32_t  soldier_count  = 0;     // cfg_units[proto_id].soldier_count (the soldier-strip GATE)
    double   cfg_energy_max = 100.0; // cfg_units[proto_id].energy (the soldier-strip loop's energy_max)

    int32_t anim1_val     = 12; // cfg_buildings[equiv_bid].anim[1] (a cfg_t_frame_index)
    int32_t sprite_id_val = 55; // anim_frames[anim1_val + 1].sprite_id
    int16_t origin_x      = 65;
    int16_t origin_y      = -65;

    // Arbitrary, exactly-representable, non-default sentinel -- NOT DAT_005013d0's real value. The
    // .asm (0x0047e42b `FADD double ptr [0x005013d0]`) proves an ADD of *some* data constant into
    // units[player][0].energy; it does not encode the constant's magnitude (that lives in the image's
    // data bytes, which this offline oracle has no access to). Using an arbitrary sentinel here tests
    // the ADD mechanism/target cleanly without asserting a magnitude this test cannot independently
    // verify from the disassembly.
    double hq_credit = -4.5;
};

// `extra` runs AFTER fx.reset() and the standard Seed-driven setup, but BEFORE the call -- for cases
// that need EXTRA cfg_units[] rows beyond proto_id (the soldier-strip loop's per-iteration re-read
// tests below). A no-op default keeps the common call shape `seed_and_run(fx, s)` for every other case.
// NOTE: setting extra fx fields BEFORE calling seed_and_run() does NOT work -- fx.reset() inside here
// runs first and would wipe them; this hook exists precisely so that trap has no way to reoccur.
template <typename Extra>
void seed_and_run(sim_fixture &fx, const Seed &s, Extra &&extra) {
    fx.reset();

    unit &u          = fx.u(s.player, s.index);
    u.state          = s.state;
    u.energy         = s.energy;
    u.pending_damage = s.pending_damage;
    u.unit_proto_id  = s.proto_id;
    u.x              = s.x;
    u.y              = s.y;

    // Real-game invariant: cur_unit names the SAME roster slot (player, index) -- see
    // unit_of()/cur_unit's asymmetry note reproduced by
    // test_soldier_gate_reads_roster_loop_reads_cur_unit_pointer() below for the deliberate exception.
    fx.cur_unit_ptr    = &u;
    fx.view_cur_player = s.player;
    fx.view_cur_index  = static_cast<uint16_t>(s.index);

    cfg_unit &cu     = fx.cfg_units[s.proto_id];
    cu.equivalent    = s.equiv_bid;
    cu.soldier_count = s.soldier_count;
    cu.energy        = s.cfg_energy_max;

    int32_t anim1 = s.anim1_val;
    std::memcpy(&fx.cfg_buildings[s.equiv_bid].anim[1 * 4], &anim1, sizeof(anim1));
    fx.anim_frames[static_cast<size_t>(s.anim1_val + 1)].sprite_id = s.sprite_id_val;
    fx.sprite_meta[static_cast<size_t>(s.sprite_id_val)].origin_x  = s.origin_x;
    fx.sprite_meta[static_cast<size_t>(s.sprite_id_val)].origin_y  = s.origin_y;

    extra(fx);

    g_hq_credit = s.hq_credit;
    reset_observations();

    sim_view v                    = fx.view();
    v.unit_death_hq_energy_credit = &g_hq_credit;
    sim_store own                 = fx.store();
    detail::unit_apply_damage(v, own, g_calls);
}

void seed_and_run(sim_fixture &fx, const Seed &s) {
    seed_and_run(fx, s, [](sim_fixture &) {});
}

// =====================================================================================================
void test_early_out() {
    sim_fixture fx;
    Seed        s;
    s.player         = 1;
    s.index          = 4;
    s.energy         = 77.0; // sentinels: must survive UNTOUCHED
    s.pending_damage = 33.0;

    // state == DIE_EXPLODE(2): 0x0047e39d CMP / 0x0047e3a2 JZ -> LAB_0047e3b0 -> 0x0047e3b0 JMP
    // 0x0047e608 (straight to the epilogue -- the WHOLE body, not merely the state transition, is
    // skipped: no energy update, no tail damage-smoke/notify_ui refresh, no callees at all).
    s.state = UNIT_STATE_DIE_EXPLODE;
    seed_and_run(fx, s);
    ck(g_trace.empty(), "early-out state=DIE_EXPLODE(2): NO callees fire at all, not even the tail");
    ck_eq_d(fx.u(1, 4).energy, 77.0, "early-out state=2: energy left UNTOUCHED (full-body skip)");
    ck_eq_d(fx.u(1, 4).pending_damage, 33.0, "early-out state=2: pending_damage left UNTOUCHED");
    ck_eq(static_cast<uint32_t>(fx.u(1, 4).unit_proto_id), static_cast<uint32_t>(s.proto_id),
          "early-out state=2: unit_proto_id untouched");

    // state == CORPSE_FOW_DECAY(4): 0x0047e3a9 CMP / 0x0047e3ae JNZ NOT taken (falls through to
    // LAB_0047e3b0) -- same full skip via the SECOND half of the OR'd early-out guard.
    s.state = UNIT_STATE_CORPSE_FOW_DECAY;
    seed_and_run(fx, s);
    ck(g_trace.empty(), "early-out state=CORPSE_FOW_DECAY(4): NO callees fire at all (0x0047e3ae JNZ not taken)");
    ck_eq_d(fx.u(1, 4).energy, 77.0, "early-out state=4: energy left UNTOUCHED");
    ck_eq_d(fx.u(1, 4).pending_damage, 33.0, "early-out state=4: pending_damage left UNTOUCHED");
}

// =====================================================================================================
void test_energy_pending_damage_update() {
    sim_fixture fx;
    Seed        s;
    s.player         = 0;
    s.index          = 1;
    s.energy         = 60.0;
    s.pending_damage = 15.0; // -> energy_final = 45.0, non-lethal
    s.soldier_count  = 0;    // keep the soldier-strip gate closed so only the arithmetic is exercised
    seed_and_run(fx, s);

    ck_eq_d(fx.u(0, 1).energy, 45.0,
            "energy -= pending_damage (0x0047e3c0 FLD pending_damage / 0x0047e3c3 FSUBR energy / "
            "0x0047e3c6 FSTP energy): 60-15=45");
    ck_eq_d(fx.u(0, 1).pending_damage, 0.0, "pending_damage zeroed unconditionally (0x0047e3ce/0x0047e3d5)");
    ck(trace_eq({"unit_update_damage_smoke", "unit_notify_ui"}),
       "non-lethal, soldier gate closed: only the tail fires");
}

// =====================================================================================================
void test_lethal_boundary_exact_and_ulp() {
    sim_fixture fx;

    // B1: damage == remaining energy EXACTLY -> energy_final = 0.0 -> LETHAL via the plain
    // "energy<=0.0" half (0x0047e3e1 FLDZ / 0x0047e3e3 FCOMP energy / 0x0047e3e9 JNC -> death; JNC
    // taken means ST(0)=0.0 >= src=energy, i.e. energy<=0.0).
    {
        Seed s;
        s.player         = 0;
        s.index          = 2;
        s.state          = 9;
        s.soldier_count  = 0;
        s.energy         = 1.0;
        s.pending_damage = 1.0;
        seed_and_run(fx, s);
        ck_eq_d(fx.u(0, 2).energy, 0.0, "B1: energy zeroed on the lethal branch (0x0047e40a/0x0047e411)");
        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0] == UNIT_STATE_DIE_EXPLODE,
           "B1: damage==remaining energy EXACTLY -> LETHAL (boundary energy<=0.0)");
    }
    // B2: damage ONE ULP BELOW remaining energy -> energy_final = tiny POSITIVE -> NOT lethal (plain
    // test fails; falls through to 0x0047e3f0 FLD / 0x0047e3f3 CALL utils_math_trunc / 0x0047e3fb CMP 0
    // / 0x0047e3ff JGE -> non-lethal, since trunc(tiny positive)==0>=0).
    {
        Seed s;
        s.player         = 0;
        s.index          = 2;
        s.state          = 9;
        s.soldier_count  = 0;
        s.energy         = 1.0;
        s.pending_damage = std::nextafter(1.0, 0.0);
        seed_and_run(fx, s);
        ck(g_set_state_calls.empty(), "B2: damage one ulp BELOW remaining energy -> NOT lethal");
        ck(fx.u(0, 2).energy > 0.0, "B2: post-subtraction energy is a tiny POSITIVE residue, not zeroed");
    }
    // B3: damage ONE ULP ABOVE remaining energy -> energy_final = tiny NEGATIVE -> LETHAL (still caught
    // by the SAME plain energy<=0.0 half as B1 -- 0.0 >= a negative value).
    {
        Seed s;
        s.player         = 0;
        s.index          = 2;
        s.state          = 9;
        s.soldier_count  = 0;
        s.energy         = 1.0;
        s.pending_damage = std::nextafter(1.0, 2.0);
        seed_and_run(fx, s);
        ck(g_set_state_calls.size() == 1, "B3: damage one ulp ABOVE remaining energy -> LETHAL");
        ck_eq_d(fx.u(0, 2).energy, 0.0, "B3: energy zeroed on the lethal branch even though the pre-zero value was tiny-negative, not exactly 0");
    }
}

// =====================================================================================================
void test_truncation_overflow_edge() {
    sim_fixture fx;
    // Comfortably below INT32_MAX: trunc() fits in int32 and is non-negative -> NOT lethal
    // (0x0047e3ff JGE taken).
    {
        Seed s;
        s.player         = 0;
        s.index          = 3;
        s.state          = 9;
        s.soldier_count  = 0;
        s.energy         = 2000000000.0; // < INT32_MAX(2147483647)
        s.pending_damage = 0.0;
        seed_and_run(fx, s);
        ck(g_set_state_calls.empty(), "trunc-in-range (2e9 < INT32_MAX, damage=0): NOT lethal");
    }
    // >= 2^31: the FISTP source is OUT OF int32 RANGE -> per Intel SDM, with the invalid-operation
    // exception masked (the ambient default), x87 stores the INTEGER INDEFINITE value (0x80000000,
    // i.e. a NEGATIVE int32) -> the 0x0047e3ff JGE is NOT taken -> this huge POSITIVE energy (with
    // ZERO damage) is treated as LETHAL. This is the "trunc<0 despite energy>0.0" case the header
    // banner and this file's brief both flag as reachable -- reached here via int32-truncation
    // overflow rather than a genuinely negative energy (which the plain energy<=0.0 half already
    // catches on its own in B1/B3 above, without ever reaching utils_math_trunc).
    {
        Seed s;
        s.player         = 0;
        s.index          = 3;
        s.state          = 9;
        s.soldier_count  = 0;
        s.energy         = 2147483648.0; // 2^31, one past INT32_MAX
        s.pending_damage = 0.0;
        seed_and_run(fx, s);
        ck(g_set_state_calls.size() == 1,
           "SURPRISING (0x0047e3f8 FISTP overflow / 0x0047e3ff JGE): energy=2^31 (huge POSITIVE, "
           "damage=0) still trips LETHAL via int32-truncation overflow, reproduced faithfully from the asm");
    }
}

// =====================================================================================================
void test_hq_energy_credit() {
    sim_fixture fx;
    Seed        s;
    s.player         = 3;
    s.index          = 6;
    s.state          = 9; // != DEPLOY_TO_BUILDING -- isolates the credit from the footprint chain
    s.energy         = 1.0;
    s.pending_damage = 1.0; // exact-zero lethal boundary
    s.soldier_count  = 0;

    fx.reset();
    fx.u(s.player, 0).energy                                            = 88.0; // HQ/mothership slot -- distinct sentinel
    fx.u(s.player, s.index + 1).energy                                  = 12.0; // neighbour slot -- must stay untouched
    fx.u(static_cast<uint16_t>((s.player + 1) % MAX_PLAYERS), 0).energy = 55.0; // another player's HQ

    // seed_and_run() below calls fx.reset() again, which would wipe the sentinels just set -- so seed
    // the sentinels the way seed_and_run leaves the fixture: reproduce its body manually here instead
    // of calling it, to control both the dying unit AND the surrounding sentinel slots in one pass.
    unit &u                                = fx.u(s.player, s.index);
    u.state                                = s.state;
    u.energy                               = s.energy;
    u.pending_damage                       = s.pending_damage;
    u.unit_proto_id                        = s.proto_id;
    fx.cur_unit_ptr                        = &u;
    fx.view_cur_player                     = s.player;
    fx.view_cur_index                      = static_cast<uint16_t>(s.index);
    fx.cfg_units[s.proto_id].soldier_count = s.soldier_count;
    g_hq_credit                            = s.hq_credit;
    reset_observations();

    const double hq_before        = fx.u(s.player, 0).energy;
    sim_view     v                = fx.view();
    v.unit_death_hq_energy_credit = &g_hq_credit;
    sim_store own                 = fx.store();
    detail::unit_apply_damage(v, own, g_calls);

    ck_eq_d(fx.u(s.player, 0).energy, hq_before + s.hq_credit,
            "HQ credit (0x0047e418 CUR_PLAYER / 0x0047e425 FLD units[player][0].energy / 0x0047e42b "
            "FADD DAT_005013d0 / 0x0047e431 FSTP): units[cur_player][0].energy += credit");
    ck_eq_d(fx.u(s.player, s.index).energy, 0.0,
            "HQ credit lands on slot 0, NOT the dying unit's own slot (its energy is the lethal zero, not credited)");
    ck_eq_d(fx.u(s.player, s.index + 1).energy, 12.0, "HQ credit: neighbour slot untouched");
    ck_eq_d(fx.u(static_cast<uint16_t>((s.player + 1) % MAX_PLAYERS), 0).energy, 55.0,
            "HQ credit: another player's HQ slot untouched (indexed by cur_player, not a fixed player)");
}

// =====================================================================================================
void test_deploy_to_building_footprint() {
    sim_fixture fx;

    // D1: NOT the deploy state -> footprint call NEVER fires (0x0047e43c CMP state,0x17 / 0x0047e441
    // JNZ 0x0047e519 skips the whole chain).
    {
        Seed s;
        s.player         = 0;
        s.index          = 2;
        s.state          = 9;
        s.soldier_count  = 0;
        s.energy         = 1.0;
        s.pending_damage = 1.0;
        seed_and_run(fx, s);
        ck(g_footprint.empty(), "D1: state != DEPLOY_TO_BUILDING(0x17) -- bldg_footprint_set_passable NEVER called");
    }

    // D2: DEPLOY_TO_BUILDING, origin_x POSITIVE / origin_y NEGATIVE.
    {
        Seed s;
        s.player         = 0;
        s.index          = 2;
        s.state          = UNIT_STATE_DEPLOY_TO_BUILDING;
        s.soldier_count  = 0;
        s.energy         = 1.0;
        s.pending_damage = 1.0;
        s.proto_id       = 7;
        s.equiv_bid      = 3;
        s.anim1_val      = 12;
        s.sprite_id_val  = 55;
        s.origin_x       = 65;
        s.origin_y       = -65;
        s.x              = 100;
        s.y              = 100;
        seed_and_run(fx, s);
        ck(g_footprint.size() == 1, "D2: DEPLOY_TO_BUILDING -- footprint call fires exactly once");
        // fine_to_tile(65)=2 (0x0047e478-0x0047e482 SAR/SHL/SBB/SAR, C's truncating /32); tile_x =
        // width_mask(0xff, fixture default) & (x - 2) = 0xff & 98 = 98.
        // fine_to_tile(-65)=-2 (same idiom at 0x0047e4d4-0x0047e4dc, NEGATIVE operand); tile_y =
        // height_mask(0x3f, fixture default) & (y - (-2)) = 0x3f & 102 = 38.
        if (g_footprint.size() == 1) {
            ck_eq(static_cast<uint32_t>(g_footprint[0].tile_x), 98u,
                  "D2: tile_x = width_mask & (x - fine_to_tile(origin_x)), origin_x POSITIVE");
            ck_eq(static_cast<uint32_t>(g_footprint[0].tile_y), 38u,
                  "D2: tile_y = height_mask & (y - fine_to_tile(origin_y)), origin_y NEGATIVE");
            ck_eq(static_cast<uint32_t>(g_footprint[0].building_idx), static_cast<uint32_t>(s.equiv_bid),
                  "D2: 3rd arg = cfg_units[proto_id].equivalent (0x0047e508 MOV EBX, re-derived a THIRD time in the asm)");
        }
    }

    // D3: swap the origin sign convention (origin_x NEGATIVE / origin_y POSITIVE) with different tile
    // positions -- catches an X/Y-axis swap bug D2 alone cannot.
    {
        Seed s;
        s.player         = 0;
        s.index          = 2;
        s.state          = UNIT_STATE_DEPLOY_TO_BUILDING;
        s.soldier_count  = 0;
        s.energy         = 1.0;
        s.pending_damage = 1.0;
        s.proto_id       = 7;
        s.equiv_bid      = 3;
        s.anim1_val      = 12;
        s.sprite_id_val  = 55;
        s.origin_x       = -65;
        s.origin_y       = 65;
        s.x              = 50;
        s.y              = 50;
        seed_and_run(fx, s);
        ck(g_footprint.size() == 1, "D3: footprint call fires exactly once (opposite-sign origin)");
        if (g_footprint.size() == 1) {
            ck_eq(static_cast<uint32_t>(g_footprint[0].tile_x), 52u,
                  "D3: tile_x with NEGATIVE origin_x = 0xff & (50-(-2)) = 52");
            ck_eq(static_cast<uint32_t>(g_footprint[0].tile_y), 48u,
                  "D3: tile_y with POSITIVE origin_y = 0x3f & (50-2) = 48");
        }
    }
}

// =====================================================================================================
void test_lethal_call_order() {
    sim_fixture fx;

    // non-deploy lethal: unit_set_state(DIE_EXPLODE) -> ai_notify_object_removed -> unit_state_die_
    // explode -> [0x0047e541 JMP 0x0047e5e2 skips the ENTIRE soldier-strip loop] -> tail. soldier_count
    // is deliberately nonzero here to prove the lethal branch skips the loop regardless of the gate.
    {
        Seed s;
        s.player         = 4;
        s.index          = 7;
        s.state          = 9;
        s.soldier_count  = 5;
        s.energy         = 1.0;
        s.pending_damage = 1.0;
        seed_and_run(fx, s);
        ck(trace_eq({"unit_set_state", "ai_notify_object_removed", "unit_state_die_explode",
                     "unit_update_damage_smoke", "unit_notify_ui"}),
           "lethal/non-deploy: exact call order; soldier-strip loop SKIPPED (0x0047e541 JMP over it) "
           "despite soldier_count>0");
        ck(g_remove_last.empty(), "lethal branch never calls unit_soldier_remove_last");
    }

    // deploy lethal: footprint-unmap fires FIRST, before unit_set_state.
    {
        Seed s;
        s.player         = 4;
        s.index          = 7;
        s.state          = UNIT_STATE_DEPLOY_TO_BUILDING;
        s.soldier_count  = 0;
        s.energy         = 1.0;
        s.pending_damage = 1.0;
        seed_and_run(fx, s);
        ck(trace_eq({"bldg_footprint_set_passable", "unit_set_state", "ai_notify_object_removed",
                     "unit_state_die_explode", "unit_update_damage_smoke", "unit_notify_ui"}),
           "lethal/deploy: footprint-unmap fires BEFORE unit_set_state (0x0047e514 CALL precedes 0x0047e51e)");
    }
}

// =====================================================================================================
void test_ai_notify_object_removed_args() {
    sim_fixture fx;
    {
        Seed s;
        s.player         = 5;
        s.index          = 9;
        s.state          = 9;
        s.soldier_count  = 0;
        s.energy         = 1.0;
        s.pending_damage = 1.0;
        seed_and_run(fx, s);
        ck(g_notify_removed.size() == 1, "notify_object_removed called exactly once");
        if (g_notify_removed.size() == 1) {
            ck_eq(g_notify_removed[0].flags, 0x85u,
                  "flags = cur_player(5) | 0x80 (0x0047e52c MOV AX / 0x0047e532 OR AL,0x80 / 0x0047e534 MOVZX)");
            ck_eq(static_cast<uint32_t>(g_notify_removed[0].object_index), 9u,
                  "object_index = cur_index (0x0047e525 MOVZX EDX)");
            ck_eq(static_cast<uint32_t>(g_notify_removed[0].hard_remove), 0u,
                  "hard_remove = 0 (0x0047e523 XOR EBX,EBX)");
        }
    }
    {
        Seed s;
        s.player         = 0;
        s.index          = 1;
        s.state          = 9;
        s.soldier_count  = 0;
        s.energy         = 1.0;
        s.pending_damage = 1.0;
        seed_and_run(fx, s);
        ck(g_notify_removed.size() == 1 && g_notify_removed[0].flags == 0x80u,
           "flags = cur_player(0) | 0x80 = 0x80 exactly");
    }
}

// =====================================================================================================
void test_soldier_gate_false() {
    sim_fixture fx;
    // soldier_count == 0: 0x0047e56f CMP / 0x0047e576 JLE taken -> straight to tail.
    {
        Seed s;
        s.player         = 0;
        s.index          = 1;
        s.state          = 9;
        s.soldier_count  = 0;
        s.energy         = 50.0;
        s.pending_damage = 0.0; // non-lethal
        seed_and_run(fx, s);
        ck(trace_eq({"unit_update_damage_smoke", "unit_notify_ui"}),
           "soldier_count==0 (0x0047e576 JLE): loop never entered, only the tail fires");
        ck(g_remove_last.empty(), "no soldier_remove_last calls");
        ck_eq(static_cast<uint32_t>(fx.u(0, 1).unit_proto_id), static_cast<uint32_t>(s.proto_id),
              "unit_proto_id unchanged");
    }
    // soldier_count < 0: same JLE (signed comparison, <=0 either way).
    {
        Seed s;
        s.player         = 0;
        s.index          = 1;
        s.state          = 9;
        s.soldier_count  = -3;
        s.energy         = 50.0;
        s.pending_damage = 0.0;
        seed_and_run(fx, s);
        ck(g_remove_last.empty(), "soldier_count<0 (still <=0, JLE signed): loop never entered");
    }
}

// =====================================================================================================
void test_soldier_loop_boundary() {
    sim_fixture fx;

    // proto 7: soldier_count=4, energy_max=100.0 -> threshold = 100 - 100/4 = 75.0 EXACTLY.
    // proto 6 (after one DEC): soldier_count=2, energy_max=100.0 -> threshold = 100-100/2 = 50.0 -- a
    // DIFFERENT config, so a translation that reused row 7's cfg forever (instead of re-reading
    // unit_proto_id+cfg EVERY pass, 0x0047e57d/0x592/0x5ab) would loop instead of stopping here.
    // (row 6 is seeded via seed_and_run's `extra` hook, AFTER fx.reset() runs -- seeding it before the
    // call would be silently wiped by that reset(), the same trap test_soldier_loop_multi_iteration's
    // own comment flags below.)
    auto with_row6 = [](sim_fixture &fx) {
        fx.cfg_units[6].soldier_count = 2;
        fx.cfg_units[6].energy        = 100.0;
    };

    // exactly at the boundary (threshold==cur_energy): CONTINUES -- setnc/CF=0 is the "not less than"
    // (>=) sense, so equality continues (0x0047e5bc FCOMP / 0x0047e5c2 JC not taken).
    {
        Seed s;
        s.player         = 1;
        s.index          = 2;
        s.state          = 9;
        s.proto_id       = 7;
        s.soldier_count  = 4;
        s.cfg_energy_max = 100.0;
        s.energy         = 75.0;
        s.pending_damage = 0.0;
        seed_and_run(fx, s, with_row6);
        ck(g_remove_last.size() == 1,
           "threshold==cur_energy EXACTLY (75.0==75.0): loop CONTINUES for exactly 1 pass, then row 6's "
           "smaller threshold(50) stops it");
        ck_eq(static_cast<uint32_t>(fx.u(1, 2).unit_proto_id), 6u,
              "unit_proto_id decremented exactly once (0x0047e5c9 DEC word ptr [cur_unit+0x2])");
    }
    // one ulp ABOVE the boundary: threshold < cur_energy -> EXITS immediately, zero iterations.
    {
        Seed s;
        s.player         = 1;
        s.index          = 2;
        s.state          = 9;
        s.proto_id       = 7;
        s.soldier_count  = 4;
        s.cfg_energy_max = 100.0;
        s.energy         = std::nextafter(75.0, 1000.0);
        s.pending_damage = 0.0;
        seed_and_run(fx, s, with_row6);
        ck(g_remove_last.empty(),
           "cur_energy one ulp ABOVE threshold(75.0): 0x0047e5c2 JC taken -> loop body never runs, 0 calls");
        ck_eq(static_cast<uint32_t>(fx.u(1, 2).unit_proto_id), 7u, "unit_proto_id UNCHANGED (loop body never reached)");
    }
    // one ulp BELOW the boundary: threshold >= cur_energy -> CONTINUES (same sense as the exact case).
    {
        Seed s;
        s.player         = 1;
        s.index          = 2;
        s.state          = 9;
        s.proto_id       = 7;
        s.soldier_count  = 4;
        s.cfg_energy_max = 100.0;
        s.energy         = std::nextafter(75.0, 0.0);
        s.pending_damage = 0.0;
        seed_and_run(fx, s, with_row6);
        ck(g_remove_last.size() == 1, "cur_energy one ulp BELOW threshold(75.0): loop CONTINUES for 1 pass");
        ck_eq(static_cast<uint32_t>(fx.u(1, 2).unit_proto_id), 6u,
              "unit_proto_id decremented once, then row 6's threshold(50) stops it");
    }
}

// =====================================================================================================
void test_soldier_loop_multi_iteration() {
    sim_fixture fx;
    Seed        s;
    s.player         = 2;
    s.index          = 9;
    s.state          = 9;
    s.proto_id       = 10;
    s.energy         = 1000.0; // constant cur_energy across every re-check (nothing here writes u.energy)
    s.pending_damage = 0.0;
    s.soldier_count  = 5;
    s.cfg_energy_max = 2000.0; // row 10: threshold = 2000 - 2000/5 = 1600 >= 1000 -> continue

    // Rows 9/8/7 must be seeded via seed_and_run's `extra` hook (AFTER fx.reset() runs inside it) --
    // setting them on `fx` before calling seed_and_run would be silently wiped by that reset(), since
    // sim_fixture::reset() memsets the whole cfg_units[] vector. This is exactly the ordering trap
    // caught while drafting this file (see seed_and_run's own comment).
    seed_and_run(fx, s, [](sim_fixture &fx) {
        fx.cfg_units[9].soldier_count = 5;
        fx.cfg_units[9].energy        = 2000.0; // threshold 1600 -> continue
        fx.cfg_units[8].soldier_count = 5;
        fx.cfg_units[8].energy        = 2000.0; // threshold 1600 -> continue
        fx.cfg_units[7].soldier_count = 10;
        fx.cfg_units[7].energy        = 100.0; // threshold = 100-100/10 = 90 < 1000 -> exit
    });

    ck_eq(static_cast<uint32_t>(g_remove_last.size()), 3u,
          "3-row chain (proto 10->9->8->7, re-reading unit_proto_id+cfg row EVERY pass -- "
          "0x0047e57d/0x592/0x5ab): exactly 3 remove_last calls");
    if (g_remove_last.size() == 3) {
        for (const auto &c : g_remove_last)
            ck(c.player == 2 && c.index == 9,
               "each unit_soldier_remove_last(player,index) uses the SAME (cur_player,cur_index) every "
               "pass, never the decrementing proto_id (0x0047e5cd/0x0047e5d4)");
    }
    ck_eq(static_cast<uint32_t>(fx.u(2, 9).unit_proto_id), 7u,
          "unit_proto_id decremented exactly 3 times: 10->9->8->7");
    ck(trace_eq({"unit_soldier_remove_last", "unit_soldier_remove_last", "unit_soldier_remove_last",
                 "unit_update_damage_smoke", "unit_notify_ui"}),
       "3 remove_last calls IN ORDER, then the tail -- no interleaving with anything else");
}

// =====================================================================================================
void test_soldier_gate_reads_roster_loop_reads_cur_unit_pointer() {
    // The header banner (sim_unit_apply_damage.h) flags this as a LITERAL asymmetry preserved from the
    // asm: the GATE at 0x0047e546-0x0047e576 reads unit_proto_id via unit_of(v,player,index) (the
    // ROSTER entry), while the LOOP BODY (0x0047e578 onward) re-reads unit_proto_id via the ambient
    // cur_unit POINTER -- in every other case in this file they are the same object, so this case
    // deliberately makes them DIFFERENT records to prove the translation really uses two different
    // expressions rather than one cached value.
    sim_fixture fx;
    fx.reset();

    unit &roster         = fx.u(3, 4); // what unit_of(v, player, index) reads for the GATE
    unit &other          = fx.u(3, 5); // what cur_unit POINTS AT for the LOOP BODY (and the early-out/energy code)
    roster.state         = 9;
    roster.unit_proto_id = 20;
    other.state          = 9;
    other.unit_proto_id  = 21;
    other.energy         = 50.0;
    other.pending_damage = 0.0;

    fx.cfg_units[20].soldier_count = 7; // GATE sees this via the roster: 7>0 -> TRUE
    fx.cfg_units[21].soldier_count = 1; // the loop body's OWN proto (via cur_unit): threshold=10-10/1=0
    fx.cfg_units[21].energy        = 10.0;

    fx.cur_unit_ptr    = &other; // deliberately NOT &roster
    fx.view_cur_player = 3;
    fx.view_cur_index  = 4; // unit_of(v,3,4) == roster; cur_unit == other

    reset_observations();
    g_hq_credit                   = 0.0;
    sim_view v                    = fx.view();
    v.unit_death_hq_energy_credit = &g_hq_credit;
    sim_store own                 = fx.store();
    detail::unit_apply_damage(v, own, g_calls);

    ck(g_set_state_calls.empty(), "not lethal (cur_unit=`other`: energy=50.0>0, trunc(50)=50>=0)");
    ck(g_remove_last.empty(),
       "GATE true via roster's proto(20).soldier_count=7>0, but the LOOP BODY's own first check uses "
       "cur_unit's proto(21) -- threshold(0) < cur_energy(50) -- so it exits with ZERO iterations "
       "despite the gate passing on a DIFFERENT record's config");
    ck_eq(static_cast<uint32_t>(roster.unit_proto_id), 20u, "roster (unit_of's target) untouched");
    ck_eq(static_cast<uint32_t>(other.unit_proto_id), 21u, "cur_unit's own proto_id also untouched (loop body never entered)");
}

} // namespace

void run_unit_apply_damage_tests() {
    printf("-- llm_strat_unit_apply_damage --\n");
    test_early_out();
    test_energy_pending_damage_update();
    test_lethal_boundary_exact_and_ulp();
    test_truncation_overflow_edge();
    test_hq_energy_credit();
    test_deploy_to_building_footprint();
    test_lethal_call_order();
    test_ai_notify_object_removed_args();
    test_soldier_gate_false();
    test_soldier_loop_boundary();
    test_soldier_loop_multi_iteration();
    test_soldier_gate_reads_roster_loop_reads_cur_unit_pointer();
}

} // namespace mh::sim::test
