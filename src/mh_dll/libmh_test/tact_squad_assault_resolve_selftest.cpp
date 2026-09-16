//
// tact_squad_assault_resolve_selftest.cpp -- offline oracle for llm_strat_squad_assault_resolve
// (TACT1A batch A, 2026-08-26). See tact/tact_squad_assault_resolve.h for the derivation.
//
// WHY OFFLINE, NOT RIG: this function's own two direct outward calls
// (llm_strat_unit_remove_from_map, llm_strat_unit_teardown) are themselves two of the seven
// TACT-CUT2-ungated `effectful` shared callees it transitively reaches -- arming it under shadow
// would double-fire a real unit-removal/teardown cascade. Both calls are mocked via the header's
// `squad_assault_resolve_calls` struct, so this oracle never executes either real callee.
//
#include "tact/tact_squad_assault_resolve.h"
#include "sim_test_support.h"
#include "tact_test_support.h"

namespace mh::tact::test {

namespace {

struct call_log {
    int      remove_from_map_calls    = 0;
    uint16_t remove_from_map_player   = 0;
    uint32_t remove_from_map_unit_idx = 0;
    int      teardown_calls           = 0;
    uint32_t teardown_player          = 0;
    uint16_t teardown_unit_idx        = 0;
};

call_log &log() {
    static call_log l;
    return l;
}

void reset_log() { log() = call_log{}; }

void mock_remove_from_map(uint16_t player, uint32_t unit_idx) {
    ++log().remove_from_map_calls;
    log().remove_from_map_player   = player;
    log().remove_from_map_unit_idx = unit_idx;
}

void mock_teardown(uint32_t player, uint16_t unit_idx) {
    ++log().teardown_calls;
    log().teardown_player   = player;
    log().teardown_unit_idx = unit_idx;
}

squad_assault_resolve_calls mock_calls() { return {mock_remove_from_map, mock_teardown}; }

// Seeds the blackboard read view to a harmless default (owner/building/energy_pct all 0, the three
// scale constants at their live value 100.0) so a test only needs to override what it exercises.
void seed_blackboard_defaults(tact_fixture &tfx) {
    tfx.squad_bb_scan_player              = 0;
    tfx.squad_bb_target_owner             = 0;
    tfx.squad_bb_target_building_idx      = 0;
    tfx.squad_bb_target_energy_pct        = 0;
    tfx.squad_assault_power_percent_scale = 100.0;
    tfx.bldg_energy_to_percent_scale      = 100.0;
    tfx.bldg_energy_percent_to_abs_scale  = 100.0;
}

} // namespace

void run_squad_assault_resolve_tests() {
    // T1: non-lethal damage -- loss > 0 and (energy - loss) > 0 -> pending_damage accumulates,
    // NO kill calls, units_lost_total unchanged. energy_pct=10, Unit.energy=50, soldier_count=1,
    // scale=100 -> loss = (10*50)/(1*100) = 5. unit.energy=100 -> 100-5=95 > 0.
    {
        tact_fixture         tfx;
        mh::sim::sim_fixture sfx;
        reset_log();
        seed_blackboard_defaults(tfx);
        tfx.squad_bb_scan_player         = 2;
        tfx.squad_status[0]              = {/*unit_proto_id*/ 0, /*energy_pct*/ 10, /*unit_slot_index*/ 5,
                               /*is_commando*/ 0};
        sfx.cfg_units[7].soldier_count   = 1;
        sfx.cfg_units[7].energy          = 50.0;
        auto &u                          = sfx.units[2 * mh::sim::UNITS_PER_PLAYER + 5];
        u.unit_proto_id                  = 7;
        u.energy                         = 100.0;
        u.pending_damage                 = 3.0;
        tact_view                   v    = tfx.view();
        tact_store                  own  = tfx.store();
        mh::sim::sim_view           sv   = sfx.view();
        mh::sim::sim_store          sown = sfx.store();
        squad_assault_resolve_calls c    = mock_calls();
        detail::squad_assault_resolve(v, own, sv, sown, c);
        ck_eq_d(u.pending_damage, 98.0, "T1: pending_damage += (energy-loss), 3+95, 0x0044d925-0x0044d92d");
        ck_eq((uint32_t)log().remove_from_map_calls, 0u, "T1: non-lethal -- no remove_from_map");
        ck_eq((uint32_t)log().teardown_calls, 0u, "T1: non-lethal -- no teardown");
        ck_eq((uint32_t)sfx.profiles[2].units_lost_total[0], 0u, "T1: units_lost_total unchanged");
    }

    // T2: loss > 0 but delta(=energy-loss) <= 0 -- a GENUINE THIRD ARM, neither kill nor
    // pending_damage: the original's JNC @0x0044d959 jumps straight to the tail, past both the kill
    // block and the pending_damage add (reimpl-verify caught a 2-way collapse here, 2026-08-26 --
    // an earlier draft routed this case into the kill arm). energy_pct=100, Unit.energy=100,
    // soldier_count=1, scale=100 -> loss=100 (>0). unit.energy=50 -> delta=50-100=-50 (<=0).
    {
        tact_fixture         tfx;
        mh::sim::sim_fixture sfx;
        reset_log();
        seed_blackboard_defaults(tfx);
        tfx.squad_bb_scan_player         = 3;
        tfx.squad_status[0]              = {0, 100, 11, 0};
        sfx.cfg_units[9].soldier_count   = 1;
        sfx.cfg_units[9].energy          = 100.0;
        auto &u                          = sfx.units[3 * mh::sim::UNITS_PER_PLAYER + 11];
        u.unit_proto_id                  = 9;
        u.energy                         = 50.0;
        u.pending_damage                 = 7.0; // must survive untouched -- this arm touches nothing
        sfx.planet_index                 = 4;
        tact_view                   v    = tfx.view();
        tact_store                  own  = tfx.store();
        mh::sim::sim_view           sv   = sfx.view();
        mh::sim::sim_store          sown = sfx.store();
        squad_assault_resolve_calls c    = mock_calls();
        detail::squad_assault_resolve(v, own, sv, sown, c);
        ck_eq((uint32_t)log().remove_from_map_calls, 0u,
              "T2: loss>0 && delta<=0 -- NOT a kill, 0x0044d959 JNC skips the kill block entirely");
        ck_eq((uint32_t)log().teardown_calls, 0u, "T2: loss>0 && delta<=0 -- no teardown either");
        ck_eq((uint32_t)sfx.profiles[3].units_lost_total[4], 0u, "T2: units_lost_total untouched");
        ck_eq_d(u.pending_damage, 7.0, "T2: pending_damage UNTOUCHED -- a true no-op arm");
    }

    // T3: loss <= 0 exactly (energy_pct sum is 0) IS the kill path -- unconditionally, regardless of
    // the unit's own remaining energy (the JC @0x0044d900 tests loss alone).
    {
        tact_fixture         tfx;
        mh::sim::sim_fixture sfx;
        reset_log();
        seed_blackboard_defaults(tfx);
        tfx.squad_bb_scan_player         = 1;
        tfx.squad_status[0]              = {0, 0, 6, 0}; // energy_pct sum == 0
        sfx.cfg_units[2].soldier_count   = 1;
        sfx.cfg_units[2].energy          = 40.0;
        auto &u                          = sfx.units[1 * mh::sim::UNITS_PER_PLAYER + 6];
        u.unit_proto_id                  = 2;
        u.energy                         = 999.0; // would NOT be lethal under a `energy - loss <= 0` reading
        tact_view                   v    = tfx.view();
        tact_store                  own  = tfx.store();
        mh::sim::sim_view           sv   = sfx.view();
        mh::sim::sim_store          sown = sfx.store();
        squad_assault_resolve_calls c    = mock_calls();
        detail::squad_assault_resolve(v, own, sv, sown, c);
        ck_eq((uint32_t)log().remove_from_map_calls, 1u,
              "T3: loss<=0 takes the kill arm regardless of unit.energy, 0x0044d900 JC");
    }

    // T4: multi-soldier cursor bookkeeping -- a 3-soldier unit consumes slots 0,1,2 (cursor -> 3);
    // slot 3 is a separate EMPTY slot (cursor -> 4); slot 4 is a SECOND real unit, which must still
    // be reached and processed correctly if the cursor advanced right.
    {
        tact_fixture         tfx;
        mh::sim::sim_fixture sfx;
        reset_log();
        seed_blackboard_defaults(tfx);
        tfx.squad_bb_scan_player         = 0;
        tfx.squad_status[0]              = {0, 10, 20, 0};
        tfx.squad_status[1]              = {0, 20, 20, 0}; // same unit_idx -- multi-soldier tally
        tfx.squad_status[2]              = {0, 30, 20, 0};
        tfx.squad_status[3]              = {0, 999, 0, 0}; // EMPTY slot (unit_slot_index==0) -- must be skipped
        tfx.squad_status[4]              = {0, 5, 30, 0};  // second unit, reached only if cursor==4 here
        sfx.cfg_units[8].soldier_count   = 3;
        sfx.cfg_units[8].energy          = 100.0;
        auto &u1                         = sfx.units[0 * mh::sim::UNITS_PER_PLAYER + 20];
        u1.unit_proto_id                 = 8;
        u1.energy                        = 1000.0;
        sfx.cfg_units[4].soldier_count   = 1;
        sfx.cfg_units[4].energy          = 10.0;
        auto &u2                         = sfx.units[0 * mh::sim::UNITS_PER_PLAYER + 30];
        u2.unit_proto_id                 = 4;
        u2.energy                        = 1000.0;
        tact_view                   v    = tfx.view();
        tact_store                  own  = tfx.store();
        mh::sim::sim_view           sv   = sfx.view();
        mh::sim::sim_store          sown = sfx.store();
        squad_assault_resolve_calls c    = mock_calls();
        detail::squad_assault_resolve(v, own, sv, sown, c);
        // unit1: sum=10+20+30=60, loss=(60*100)/(3*100)=20, 1000-20=980>0 -> pending_damage=980.
        ck_eq_d(u1.pending_damage, 980.0,
                "T4: 3-soldier unit's sum spans slots 0-2, 0x0044d89b loop");
        // unit2 (slot 4, soldier_count=1): sum=5, loss=(5*10)/(1*100)=0.5, 1000-0.5=999.5>0.
        ck_eq_d(u2.pending_damage, 999.5,
                "T4: cursor correctly reached slot 4 after the empty slot-3 skip, 0x0044d84e/0x0044d861");
        ck_eq((uint32_t)log().remove_from_map_calls, 0u, "T4: neither unit is lethal");
    }

    // T5: building damage APPLIED -- target_energy_pct < computed pct.
    // building.energy=80, ENERGY_TO_PERCENT_SCALE=100, Building[cfg].energy=100 ->
    // pct = trunc(80*100/100) = 80. target_energy_pct=50 < 80 -> apply:
    // pending_damage += 80 - (50*100/100) = 80-50 = 30.
    {
        tact_fixture         tfx;
        mh::sim::sim_fixture sfx;
        reset_log();
        seed_blackboard_defaults(tfx);
        tfx.squad_bb_target_owner        = 5;
        tfx.squad_bb_target_building_idx = 12;
        tfx.squad_bb_target_energy_pct   = 50;
        auto &b                          = sfx.buildings[5 * mh::sim::BUILDINGS_PER_PLAYER + 12];
        b.building_id                    = 3;
        b.energy                         = 80.0;
        b.pending_damage                 = 2.0;
        sfx.cfg_buildings[3].energy      = 100.0;
        tact_view                   v    = tfx.view();
        tact_store                  own  = tfx.store();
        mh::sim::sim_view           sv   = sfx.view();
        mh::sim::sim_store          sown = sfx.store();
        squad_assault_resolve_calls c    = mock_calls();
        detail::squad_assault_resolve(v, own, sv, sown, c);
        ck_eq_d(b.pending_damage, 32.0,
                "T5: building pending_damage += energy-(TARGET_PCT*cfg.energy/scale), 2+30, 0x0044da43-0x0044da49");
    }

    // T6: building damage NOT applied -- target_energy_pct (95) is CLEARLY ABOVE computed pct (80),
    // not merely at the boundary: an exact target_energy_pct==pct case degenerates to a ZERO delta
    // either way when TO_PERCENT_SCALE==PERCENT_TO_ABS_SCALE (both 100.0 live), so it cannot
    // distinguish a `<` vs `<=` mutation -- this case picks a value the JGE @0x0044d9e6 refuses on
    // that WOULD observably change pending_damage if the comparison's direction were ever inverted.
    // pending_damage must be left EXACTLY as it was.
    {
        tact_fixture         tfx;
        mh::sim::sim_fixture sfx;
        reset_log();
        seed_blackboard_defaults(tfx);
        tfx.squad_bb_target_owner        = 6;
        tfx.squad_bb_target_building_idx = 8;
        tfx.squad_bb_target_energy_pct   = 95; // > pct (80) -- refused, and NOT the degenerate == case
        auto &b                          = sfx.buildings[6 * mh::sim::BUILDINGS_PER_PLAYER + 8];
        b.building_id                    = 1;
        b.energy                         = 80.0;
        b.pending_damage                 = 9.0;
        sfx.cfg_buildings[1].energy      = 100.0;
        tact_view                   v    = tfx.view();
        tact_store                  own  = tfx.store();
        mh::sim::sim_view           sv   = sfx.view();
        mh::sim::sim_store          sown = sfx.store();
        squad_assault_resolve_calls c    = mock_calls();
        detail::squad_assault_resolve(v, own, sv, sown, c);
        ck_eq_d(b.pending_damage, 9.0, "T6: target_energy_pct >= pct -- building pending_damage untouched");
    }
}

} // namespace mh::tact::test
