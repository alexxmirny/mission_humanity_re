//
// sim_unit_purge_unregistered_selftest.cpp -- `simtest` cases for llm_strat_unit_purge_unregistered
// (sim/sim_unit_purge_unregistered.h/.cpp), SIM1A.
//
// The function sweeps ALL of a player's roster slots [1,99] itself -- so each case here sets up
// exactly ONE nonzero roster slot (unit_proto_id != 0) and relies on the fixture's zero-init to
// make every other slot a no-op (unit_proto_id==0 skips immediately), rather than calling the
// function once per unit under test.
//
#include "sim/sim_unit_purge_unregistered.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

struct log_t {
    int     is_boarding_calls = 0;
    int32_t last_state        = -1;
    int32_t return_value      = 0; // what the mock returns (0 = not boarding)

    void reset() { *this = log_t{}; }
};
log_t g_log;

const purge_unregistered_calls &recording_calls() {
    static const purge_unregistered_calls c = {
        [](int32_t state) -> int32_t {
            ++g_log.is_boarding_calls;
            g_log.last_state = state;
            return g_log.return_value;
        },
    };
    return c;
}

// Packs a map_t_unit_full_id link the same way the .cpp's unit_full_id_word() unpacks it: high
// nibble = owner, low 12 bits = roster index.
void pack_link(uint8_t (&packed)[2], uint32_t owner, uint32_t index) {
    const uint16_t link = (uint16_t)(((owner & 0xf) << 12) | (index & 0xfff));
    packed[0]           = (uint8_t)(link & 0xff);
    packed[1]           = (uint8_t)((link >> 8) & 0xff);
}

// ---- (a) a properly-registered BUILDING-slot unit survives untouched. ---------------------------
void test_purge_building_registered_survives() {
    sim_fixture    f;
    const sim_view v   = f.view();
    sim_store      own = f.store();

    const uint32_t player = 2;
    const int32_t  slot   = 10;

    unit &u          = own.unit_at(player, slot);
    u.unit_proto_id  = 1;
    u.energy         = 50.0;
    u.pending_damage = -999.0; // sentinel: must NOT be overwritten
    u.x              = 5;
    u.y              = 6;

    f.cfg_units[1].type = 1;              // < UNIT_TYPE_A_HELI -> building-slot classification
    f.t(5, 6).building  = (uint16_t)slot; // registered: this tile names THIS slot

    g_log.reset();
    g_log.return_value = 0; // not boarding
    detail::unit_purge_unregistered(v, own, recording_calls(), player);

    ck_eq_d(u.pending_damage, -999.0,
            "purge_unregistered: registered building-slot unit, not boarding -> pending_damage untouched");
}

// ---- (b) an UNregistered building-slot unit gets pending_damage = energy (marked for removal). ----
void test_purge_building_unregistered_gets_marked() {
    sim_fixture    f;
    const sim_view v   = f.view();
    sim_store      own = f.store();

    const uint32_t player = 2;
    const int32_t  slot   = 11;

    unit &u          = own.unit_at(player, slot);
    u.unit_proto_id  = 1;
    u.energy         = 50.0;
    u.pending_damage = -999.0; // sentinel
    u.x              = 7;
    u.y              = 8;

    f.cfg_units[1].type = 1;   // < UNIT_TYPE_A_HELI -> building-slot classification
    f.t(7, 8).building  = 999; // NOT this slot -> unregistered

    g_log.reset();
    g_log.return_value = 0;
    detail::unit_purge_unregistered(v, own, recording_calls(), player);

    ck_eq_d(u.pending_damage, 50.0,
            "purge_unregistered: unregistered building-slot unit -> pending_damage = energy (marked)");
}

// ---- (c) a properly-registered NON-building (mobile) unit, found via the tile's unit-stack chain,
// survives untouched. ------------------------------------------------------------------------------
void test_purge_mobile_registered_via_chain_survives() {
    sim_fixture    f;
    const sim_view v   = f.view();
    sim_store      own = f.store();

    const uint32_t player = 3;
    const int32_t  slot   = 12;

    unit &u          = own.unit_at(player, slot);
    u.unit_proto_id  = 2;
    u.energy         = 77.0;
    u.pending_damage = -999.0; // sentinel
    u.x              = 9;
    u.y              = 10;

    f.cfg_units[2].type = UNIT_TYPE_A_HELI;             // >= UNIT_TYPE_A_HELI -> mobile chain-walk classification
    pack_link(f.t(9, 10).unit, player, (uint32_t)slot); // this unit is the FIRST chain link, self-ref

    g_log.reset();
    g_log.return_value = 0; // not boarding
    detail::unit_purge_unregistered(v, own, recording_calls(), player);

    ck_eq_d(u.pending_damage, -999.0,
            "purge_unregistered: registered mobile unit (found via the unit_above chain), not boarding "
            "-> pending_damage untouched");
}

// ---- (d) a REGISTERED unit whose boarding-mock returns true still gets marked for removal, even
// though registration passed -- the two conditions are OR'd, not exclusive. -------------------------
void test_purge_mobile_registered_but_boarding_gets_marked() {
    sim_fixture    f;
    const sim_view v   = f.view();
    sim_store      own = f.store();

    const uint32_t player = 3;
    const int32_t  slot   = 13;

    unit &u          = own.unit_at(player, slot);
    u.unit_proto_id  = 3;
    u.energy         = 33.0;
    u.pending_damage = -999.0; // sentinel
    u.x              = 11;
    u.y              = 12;
    u.state          = 0x1234; // arbitrary; passed straight through to the mock

    f.cfg_units[3].type = UNIT_TYPE_A_HELI;
    pack_link(f.t(11, 12).unit, player, (uint32_t)slot); // self-registered -- registration PASSES

    g_log.reset();
    g_log.return_value = 1; // boarding == true
    detail::unit_purge_unregistered(v, own, recording_calls(), player);

    ck(g_log.is_boarding_calls == 1 && g_log.last_state == 0x1234,
       "purge_unregistered: is_boarding is called with the unit's own state, zero-extended");
    ck_eq_d(u.pending_damage, 33.0,
            "purge_unregistered: registered BUT boarding==true -> still pending_damage = energy (marked)");
}

// ---- bonus: energy<=0.0 skips the slot entirely, before classification even runs (an unregistered
// unit with zero energy must NOT be marked -- the very first guard short-circuits everything else). -
void test_purge_zero_energy_skips_before_classification() {
    sim_fixture    f;
    const sim_view v   = f.view();
    sim_store      own = f.store();

    const uint32_t player = 5;
    const int32_t  slot   = 14;

    unit &u          = own.unit_at(player, slot);
    u.unit_proto_id  = 4;
    u.energy         = 0.0;    // boundary: energy<=0.0 skips
    u.pending_damage = -999.0; // sentinel
    u.x              = 1;
    u.y              = 1;

    f.cfg_units[4].type = 1;   // would be building-slot...
    f.t(1, 1).building  = 999; // ...and unregistered, IF the energy guard did not short-circuit first

    g_log.reset();
    detail::unit_purge_unregistered(v, own, recording_calls(), player);

    ck_eq_d(u.pending_damage, -999.0,
            "purge_unregistered: energy<=0.0 skips the slot entirely, even though it would otherwise be "
            "unregistered and marked");
    ck(g_log.is_boarding_calls == 0,
       "purge_unregistered: energy<=0.0 short-circuits before is_boarding is ever reached");
}

} // namespace

void run_unit_purge_unregistered_tests() {
    test_purge_building_registered_survives();
    test_purge_building_unregistered_gets_marked();
    test_purge_mobile_registered_via_chain_survives();
    test_purge_mobile_registered_but_boarding_gets_marked();
    test_purge_zero_energy_skips_before_classification();
}

} // namespace mh::sim::test
