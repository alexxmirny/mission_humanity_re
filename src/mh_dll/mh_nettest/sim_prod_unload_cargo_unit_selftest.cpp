#include <cstring>

#include "sim/sim_prod_unload_cargo_unit.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- fixture helpers -------------------------------------------------------------------------

prod_shuttle_slot &slot_of(sim_fixture &f, uint32_t player, int32_t shuttle_slot) {
    return f.prod_shuttle_slots[player * PROD_SHUTTLE_SLOTS_PER_PLAYER + (uint32_t)shuttle_slot];
}

// Writes the packed proto-id/control-group word (entry offset +0x0, per the header's MANIFEST
// ENTRY LAYOUT note) for one 14-byte cargo-manifest entry.
void write_manifest_word(sim_fixture &f, uint32_t player, int32_t shuttle_slot, uint32_t cargo_index,
                         uint16_t packed_word) {
    prod_shuttle_slot &s   = slot_of(f, player, shuttle_slot);
    const uint32_t     off = cargo_index * 0xeu;
    std::memcpy(&s.cargo_manifest_raw[off], &packed_word, sizeof(packed_word));
}

// Reads the same word back (post-mask, or post-zero on the success path).
uint16_t read_manifest_word(sim_fixture &f, uint32_t player, int32_t shuttle_slot, uint32_t cargo_index) {
    prod_shuttle_slot &s   = slot_of(f, player, shuttle_slot);
    const uint32_t     off = cargo_index * 0xeu;
    uint16_t           w;
    std::memcpy(&w, &s.cargo_manifest_raw[off], sizeof(w));
    return w;
}

// Writes entry+0x2 (double energy) and entry+0xa (int32_t experience), per the header's MANIFEST
// ENTRY LAYOUT note (the CORRECTED layout: +0xa is experience, not position/facing bytes).
void write_manifest_energy_experience(sim_fixture &f, uint32_t player, int32_t shuttle_slot,
                                      uint32_t cargo_index, double energy, int32_t experience) {
    prod_shuttle_slot &s   = slot_of(f, player, shuttle_slot);
    const uint32_t     off = cargo_index * 0xeu;
    std::memcpy(&s.cargo_manifest_raw[off + 0x2], &energy, sizeof(energy));
    std::memcpy(&s.cargo_manifest_raw[off + 0xa], &experience, sizeof(experience));
}

// ---- recorder ---------------------------------------------------------------------------------

struct spawn_call_t {
    uint16_t proto_id;
    uint16_t player;
    uint32_t probe_slot;
};
struct notify_call_t {
    uint16_t player;
    uint16_t unit_type;
    uint32_t unit_id;
    uint32_t kind;
};
struct ctrlgroup_call_t {
    int32_t unit_id;
    void   *group_count_ptr;
    int32_t group_index;
};

struct puc_recorder {
    std::vector<spawn_call_t>     spawn_docked;
    std::vector<notify_call_t>    notify_lifecycle;
    std::vector<ctrlgroup_call_t> ctrlgroup_add;
    std::vector<uint32_t>         set_event; // sequence of event-type args, in call order
    // control knobs
    int32_t spawn_ret = 0; // 0 = spawn fails; nonzero = the new unit's id
    void    reset() { *this = puc_recorder{}; }
};
puc_recorder g_puc;

const prod_unload_cargo_unit_calls &rec_calls() {
    static const prod_unload_cargo_unit_calls c = {
        [](uint16_t unit_proto_id, uint16_t player, uint32_t probe_slot) -> int32_t {
            g_puc.spawn_docked.push_back({unit_proto_id, player, probe_slot});
            return g_puc.spawn_ret;
        },
        [](uint16_t player, uint16_t unit_type, uint32_t unit_id, uint32_t kind) {
            g_puc.notify_lifecycle.push_back({player, unit_type, unit_id, kind});
        },
        [](int32_t unit_id, int32_t *group_count_ptr, int32_t group_index) {
            g_puc.ctrlgroup_add.push_back({unit_id, group_count_ptr, group_index});
        },
        [](uint32_t type) -> uint32_t {
            g_puc.set_event.push_back(type);
            return 0;
        },
    };
    return c;
}

// ==== tests ======================================================================================
// From sim_prod_unload_cargo_unit.h: the shuttle_slot/sub_id read off the BUILDING record
// (0x0048ea46-0x0048ea7d), the packed-word bit-12-15 extraction into ctrl_group_index
// (0x0048ea9c-0x0048eaa6, SAR ..,0xc), the mask-out of those bits from the STORED word
// (0x0048eac5, AND byte,0xf), the unconditional units[player][0].order bump
// (0x0048ead0-0x0048ead6) with its matching failure-path DEC (0x0048ec2e-0x0048ec3f), and the
// success-path tail (notify kind=4, energy/experience restore, entry zeroed, optional ctrlgroup
// re-add, three UI-refresh events).

// Bit math: packed_word = 0x72D4 = 0111_0010_1101_0100b.
//   bits 12-15 (ctrl_group_index) = 0x72D4 >> 12 = 0x7.
//   AND byte,0xf on the high byte (0x72) -> 0x72 & 0x0f = 0x02 -> masked word = 0x02D4 = 0x2D4
//   (724), the bare proto id (bits 0-11). ctrl(7) and proto(724) are deliberately far apart in
//   magnitude so a shift-amount or byte-vs-word masking mistake cannot pass by coincidence.
void test_shuttle_slot_selects_correct_manifest_record_and_passes_sub_id() {
    sim_fixture f;
    g_puc.reset();
    constexpr uint16_t PLAYER       = 5;
    constexpr int32_t  BUILDING_IDX = 2;
    constexpr int32_t  SHUTTLE_SLOT = 6; // != 0, so a "always reads slot 0" bug is observable
    constexpr uint8_t  SUB_ID       = 0x2a;
    constexpr uint32_t CARGO_INDEX  = 3;

    building &b    = f.b(PLAYER, BUILDING_IDX);
    b.shuttle_slot = SHUTTLE_SLOT;
    b.sub_id       = SUB_ID;

    write_manifest_word(f, PLAYER, SHUTTLE_SLOT, CARGO_INDEX, 0x72D4);
    // Decoy at shuttle_slot 0, SAME cargo_index -- if the translation ignored building.shuttle_slot
    // and always read slot 0, the captured proto id below would be 0x111 instead of 0x2D4.
    write_manifest_word(f, PLAYER, 0, CARGO_INDEX, 0x1111);

    g_puc.spawn_ret = 0; // fail path -- keeps this test isolated to the read/select/spawn-args step

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::prod_unload_cargo_unit(v, own, rec_calls(), PLAYER, BUILDING_IDX, CARGO_INDEX);

    ck_eq((uint32_t)g_puc.spawn_docked.size(), 1u, "puc: unit_spawn_docked called exactly once");
    ck(g_puc.spawn_docked.size() == 1 && g_puc.spawn_docked[0].proto_id == 0x2D4u,
       "puc: spawn proto_id = masked bare id (0x2D4) from the SHUTTLE_SLOT-selected record, not "
       "the slot-0 decoy");
    ck(g_puc.spawn_docked.size() == 1 && g_puc.spawn_docked[0].player == PLAYER,
       "puc: spawn player = the caller's player");
    ck(g_puc.spawn_docked.size() == 1 && g_puc.spawn_docked[0].probe_slot == SUB_ID,
       "puc: spawn probe_slot = building.sub_id (0x2a)");
}

// Bit math: packed_word = 0x91A9 = 1001_0001_1010_1001b.
//   ctrl_group_index = 0x91A9 >> 12 = 0x9 (the LAST valid ctrl-group index, 0-9 domain).
//   masked word: high byte 0x91 & 0x0f = 0x01 -> 0x01A9 = 0x1A9 (425), the bare proto id.
void test_ctrl_group_index_extracted_from_bits_12_15_and_readded_on_success() {
    sim_fixture f;
    g_puc.reset();
    constexpr uint16_t PLAYER       = 4;
    constexpr int32_t  BUILDING_IDX = 1;
    constexpr int32_t  SHUTTLE_SLOT = 3;
    constexpr uint8_t  SUB_ID       = 0x15;
    constexpr uint32_t CARGO_INDEX  = 2;

    building &b    = f.b(PLAYER, BUILDING_IDX);
    b.shuttle_slot = SHUTTLE_SLOT;
    b.sub_id       = SUB_ID;
    write_manifest_word(f, PLAYER, SHUTTLE_SLOT, CARGO_INDEX, 0x91A9);

    g_puc.spawn_ret = 88; // success

    sim_view       v   = f.view();
    sim_store      own = f.store();
    const uint32_t r =
        detail::prod_unload_cargo_unit(v, own, rec_calls(), PLAYER, BUILDING_IDX, CARGO_INDEX);

    ck_eq(r, 0u, "puc(ctrl-group extraction): spawn succeeded -> returns 0");
    ck_eq((uint32_t)g_puc.ctrlgroup_add.size(), 1u,
          "puc: ctrl_group_index(9) != 0 -> exactly one ctrlgroup_add_member call");
    ck(g_puc.ctrlgroup_add.size() == 1 && g_puc.ctrlgroup_add[0].unit_id == 88,
       "puc: ctrlgroup_add unit_id = the spawned unit's id");
    ck(g_puc.ctrlgroup_add.size() == 1 && g_puc.ctrlgroup_add[0].group_index == 9,
       "puc: ctrlgroup_add group_index = 0x91A9's high nibble (9), i.e. bits 12-15 via >>12, NOT a "
       "different shift amount");
    ck(g_puc.ctrlgroup_add.size() == 1 &&
           g_puc.ctrlgroup_add[0].group_count_ptr == (void *)&f.ctrl_groups[9].count,
       "puc: ctrlgroup_add's address-escape targets &_G_LLM_STRAT_CTRL_GROUPS[9].count exactly");
    ck(g_puc.notify_lifecycle.size() == 1 && g_puc.notify_lifecycle[0].unit_type == 0x1A9,
       "puc(ctrl-group extraction): notify unit_type = masked bare proto id (0x1A9=425), confirming "
       "the ctrl-group nibble did not leak into it");
}

// Bit math: packed_word = 0x53C7 = 0101_0011_1100_0111b.
//   ctrl_group_index = 0x53C7 >> 12 = 0x5.
//   masked word: high byte 0x53 & 0x0f = 0x03 -> 0x03C7 = 0x3C7 (967), the bare proto id.
// spawn is made to FAIL here so the entry is observed in its post-mask, pre-zero state -- the
// success path's separate "zero the entry" step (0x0048ebdf) would otherwise mask this check.
void test_mask_clears_ctrl_group_bits_leaves_bare_proto_id_in_memory() {
    sim_fixture f;
    g_puc.reset();
    constexpr uint16_t PLAYER       = 2;
    constexpr int32_t  BUILDING_IDX = 0;
    constexpr int32_t  SHUTTLE_SLOT = 1;
    constexpr uint8_t  SUB_ID       = 0x00;
    constexpr uint32_t CARGO_INDEX  = 5;

    building &b    = f.b(PLAYER, BUILDING_IDX);
    b.shuttle_slot = SHUTTLE_SLOT;
    b.sub_id       = SUB_ID;
    write_manifest_word(f, PLAYER, SHUTTLE_SLOT, CARGO_INDEX, 0x53C7);

    g_puc.spawn_ret = 0; // fail -- entry must NOT be zeroed (that only happens on success)

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::prod_unload_cargo_unit(v, own, rec_calls(), PLAYER, BUILDING_IDX, CARGO_INDEX);

    const uint16_t stored = read_manifest_word(f, PLAYER, SHUTTLE_SLOT, CARGO_INDEX);
    ck_eq((uint32_t)stored, 0x03C7u,
          "puc: the STORED word has its ctrl-group nibble masked out (0x53C7 -> 0x03C7), leaving "
          "the bare proto id -- verified independently of the spawn call's own argument");
    ck_eq((uint32_t)(stored >> 12), 0u, "puc: bits 12-15 of the stored word are all clear post-mask");
    ck(g_puc.spawn_docked.size() == 1 && g_puc.spawn_docked[0].proto_id == 0x3C7u,
       "puc: the spawn call re-reads the NOW-MASKED memory (0x0048eb00) and sees 0x3C7, matching "
       "the direct memory readback above");
}

// The header's own hazard note: a bare INC (unconditional, before the spawn attempt) targets
// units[player][0] -- the roster HEADER row, not any live unit slot, and specifically NOT the
// slot the newly spawned unit occupies. Uses a nonzero player so "units[player][0]" and
// "units[0][0]" are distinguishable, and checks both rows plus the new unit's own (untouched)
// order field to nail down exactly which record moves.
void test_unconditional_order_increment_persists_on_success_targets_player_row_zero() {
    sim_fixture f;
    g_puc.reset();
    constexpr uint16_t PLAYER       = 6;
    constexpr int32_t  BUILDING_IDX = 4;
    constexpr int32_t  SHUTTLE_SLOT = 2;
    constexpr uint8_t  SUB_ID       = 0x11;
    constexpr uint32_t CARGO_INDEX  = 1;
    constexpr int32_t  NEW_UNIT_ID  = 77;

    building &b    = f.b(PLAYER, BUILDING_IDX);
    b.shuttle_slot = SHUTTLE_SLOT;
    b.sub_id       = SUB_ID;
    write_manifest_word(f, PLAYER, SHUTTLE_SLOT, CARGO_INDEX, 0x01F4); // ctrl=0, proto=0x1F4(500)

    f.u(PLAYER, 0).order = 40;  // baseline, nonzero/distinct
    f.u(0, 0).order      = 999; // sentinel on player 0's OWN row -- must stay untouched
    g_puc.spawn_ret      = NEW_UNIT_ID;

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::prod_unload_cargo_unit(v, own, rec_calls(), PLAYER, BUILDING_IDX, CARGO_INDEX);

    ck_eq((uint32_t)f.u(PLAYER, 0).order, 41u,
          "puc: units[player][0].order bumped 40 -> 41 and the bump PERSISTS after a successful "
          "spawn -- proves the unconditional increment actually happened, not merely that it was "
          "reverted (a bare failure-path net-zero would be invisible)");
    ck_eq((uint32_t)f.u(0, 0).order, 999u,
          "puc: units[0][0].order (a DIFFERENT player's row) is untouched -- the bump is indexed by "
          "the caller's player, not hardcoded to player 0");
    ck_eq((uint32_t)f.u(PLAYER, NEW_UNIT_ID).order, 0u,
          "puc: the newly spawned unit's OWN order field is untouched -- the bump targets the "
          "unrelated per-player header row 0, not the unit that was actually created, exactly the "
          "oddity the header's HEADER-ROW INCREMENT note calls out");
}

// The matching DEC (0x0048ec2e-0x0048ec3f): on spawn failure the increment is undone and the
// function returns 1. Also confirms none of the success-only effects (notify/ctrlgroup/events)
// fire, and that the manifest entry's leading word is left in its post-mask (NOT zeroed) state --
// zeroing is a success-only step (0x0048ebdf).
void test_spawn_failure_reverts_header_increment_and_returns_one() {
    sim_fixture f;
    g_puc.reset();
    constexpr uint16_t PLAYER       = 1;
    constexpr int32_t  BUILDING_IDX = 0;
    constexpr int32_t  SHUTTLE_SLOT = 1;
    constexpr uint8_t  SUB_ID       = 0x09;
    constexpr uint32_t CARGO_INDEX  = 0;

    building &b    = f.b(PLAYER, BUILDING_IDX);
    b.shuttle_slot = SHUTTLE_SLOT;
    b.sub_id       = SUB_ID;
    write_manifest_word(f, PLAYER, SHUTTLE_SLOT, CARGO_INDEX, 0x00AB); // ctrl=0, proto=0xAB(171)

    f.u(PLAYER, 0).order = 12; // baseline
    g_puc.spawn_ret      = 0;  // FAILS

    sim_view       v   = f.view();
    sim_store      own = f.store();
    const uint32_t r   = detail::prod_unload_cargo_unit(v, own, rec_calls(), PLAYER, BUILDING_IDX,
                                                        CARGO_INDEX);

    ck_eq(r, 1u, "puc(spawn fails): returns 1");
    ck_eq((uint32_t)f.u(PLAYER, 0).order, 12u,
          "puc(spawn fails): INC then matching DEC -> units[player][0].order net-unchanged (12)");
    ck_eq((uint32_t)g_puc.notify_lifecycle.size(), 0u,
          "puc(spawn fails): ai_notify_unit_lifecycle NOT called");
    ck_eq((uint32_t)g_puc.ctrlgroup_add.size(), 0u, "puc(spawn fails): ctrlgroup_add NOT called");
    ck_eq((uint32_t)g_puc.set_event.size(), 0u, "puc(spawn fails): no UI-refresh events fired");
    ck_eq((uint32_t)read_manifest_word(f, PLAYER, SHUTTLE_SLOT, CARGO_INDEX), 0x00ABu,
          "puc(spawn fails): the entry's leading word is left masked, NOT zeroed -- zeroing is a "
          "success-only step");
}

// ctrl_group_index == 0 -- the guard around the ctrlgroup_add_member call (0x0048ebee) is
// specifically skipped, while every OTHER success-path effect still fires.
void test_ctrl_group_index_zero_skips_readd_but_other_success_effects_still_fire() {
    sim_fixture f;
    g_puc.reset();
    constexpr uint16_t PLAYER       = 3;
    constexpr int32_t  BUILDING_IDX = 2;
    constexpr int32_t  SHUTTLE_SLOT = 4;
    constexpr uint8_t  SUB_ID       = 0x30;
    constexpr uint32_t CARGO_INDEX  = 6;

    building &b    = f.b(PLAYER, BUILDING_IDX);
    b.shuttle_slot = SHUTTLE_SLOT;
    b.sub_id       = SUB_ID;
    write_manifest_word(f, PLAYER, SHUTTLE_SLOT, CARGO_INDEX, 0x02EE); // ctrl=0, proto=0x2EE(750)

    g_puc.spawn_ret = 33; // success

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::prod_unload_cargo_unit(v, own, rec_calls(), PLAYER, BUILDING_IDX, CARGO_INDEX);

    ck_eq((uint32_t)g_puc.ctrlgroup_add.size(), 0u,
          "puc(ctrl_group_index==0): ctrlgroup_add_member NOT called");
    ck_eq((uint32_t)g_puc.notify_lifecycle.size(), 1u,
          "puc(ctrl_group_index==0): ai_notify_unit_lifecycle STILL fires");
    ck_eq((uint32_t)g_puc.set_event.size(), 3u,
          "puc(ctrl_group_index==0): all three UI-refresh events STILL fire");
}

// Full success path: energy/experience restore (raw copy from entry+0x2/+0xa), the lifecycle
// notify, the entry's leading word zeroed, and the three UI-refresh events in order -- plus the
// unconditional order bump, all observed together in one realistic run.
void test_success_path_restores_fields_notifies_zeroes_entry_and_fires_events_in_order() {
    sim_fixture f;
    g_puc.reset();
    constexpr uint16_t PLAYER       = 3;
    constexpr int32_t  BUILDING_IDX = 5;
    constexpr int32_t  SHUTTLE_SLOT = 8;
    constexpr uint8_t  SUB_ID       = 0x40;
    constexpr uint32_t CARGO_INDEX  = 4;
    constexpr int32_t  NEW_UNIT_ID  = 250;

    building &b    = f.b(PLAYER, BUILDING_IDX);
    b.shuttle_slot = SHUTTLE_SLOT;
    b.sub_id       = SUB_ID;
    // ctrl=2, proto=0x100(256): 0x2100 >> 12 = 0x2; high byte 0x21 & 0x0f = 0x01 -> masked 0x0100.
    write_manifest_word(f, PLAYER, SHUTTLE_SLOT, CARGO_INDEX, 0x2100);
    write_manifest_energy_experience(f, PLAYER, SHUTTLE_SLOT, CARGO_INDEX, /*energy=*/123.5,
                                     /*experience=*/-77); // negative, to catch a sign-truncation bug

    f.u(PLAYER, 0).order = 5; // baseline for the unconditional bump, checked here too
    g_puc.spawn_ret      = NEW_UNIT_ID;

    sim_view       v   = f.view();
    sim_store      own = f.store();
    const uint32_t r =
        detail::prod_unload_cargo_unit(v, own, rec_calls(), PLAYER, BUILDING_IDX, CARGO_INDEX);

    ck_eq(r, 0u, "puc(success): returns 0");

    unit &nu = f.u(PLAYER, NEW_UNIT_ID);
    ck_eq_d(nu.energy, 123.5, "puc(success): energy restored verbatim from entry+0x2 (raw copy, no FP op)");
    ck_eq((uint32_t)nu.experience, (uint32_t)(int32_t)-77,
          "puc(success): experience restored verbatim from entry+0xa, INCLUDING sign (-77)");

    ck(g_puc.notify_lifecycle.size() == 1 && g_puc.notify_lifecycle[0].player == PLAYER &&
           g_puc.notify_lifecycle[0].unit_type == 0x100 &&
           g_puc.notify_lifecycle[0].unit_id == (uint32_t)NEW_UNIT_ID &&
           g_puc.notify_lifecycle[0].kind == 4u,
       "puc(success): ai_notify_unit_lifecycle(player, unit_type=masked proto id 0x100, new unit id, "
       "kind=4)");

    ck(g_puc.ctrlgroup_add.size() == 1 && g_puc.ctrlgroup_add[0].group_index == 2 &&
           g_puc.ctrlgroup_add[0].unit_id == NEW_UNIT_ID,
       "puc(success): ctrlgroup_add_member(new unit id, &ctrl_groups[2].count, group_index=2)");

    ck_eq((uint32_t)read_manifest_word(f, PLAYER, SHUTTLE_SLOT, CARGO_INDEX), 0u,
          "puc(success): the entry's leading word is zeroed (0x0048ebdf, a single 2-byte store)");

    ck(g_puc.set_event.size() == 3 &&
           g_puc.set_event[0] == PROD_UNLOAD_CARGO_UNIT_BUILD_PROJECTS_REFRESH &&
           g_puc.set_event[1] == PROD_UNLOAD_CARGO_UNIT_MAP_OBJECTS_REFRESH &&
           g_puc.set_event[2] == EVENT_INFO_REFRESH,
       "puc(success): exactly three set_event calls, IN ORDER: BUILD_PROJECTS_REFRESH(7), "
       "MAP_OBJECTS_REFRESH(14), INFO_REFRESH(6)");

    ck_eq((uint32_t)f.u(PLAYER, 0).order, 6u,
          "puc(success): the unconditional units[player][0].order bump (5 -> 6) also holds in this "
          "full success run");
}

// cargo_index is used ONLY through its low 16 bits (every original reference is a MOVZX off the
// stack slot) -- reproduced via `& 0xffffu`. Dirty upper bits must not perturb which manifest
// entry is read.
void test_cargo_index_high_bits_masked_regression() {
    sim_fixture f;
    g_puc.reset();
    constexpr uint16_t PLAYER            = 7;
    constexpr int32_t  BUILDING_IDX      = 0;
    constexpr int32_t  SHUTTLE_SLOT      = 1;
    constexpr uint8_t  SUB_ID            = 0x77;
    constexpr uint32_t CARGO_INDEX_CLEAN = 2;
    constexpr uint32_t CARGO_INDEX_DIRTY = 0xbeef0000u | CARGO_INDEX_CLEAN; // low16==2, garbage above bit15

    building &b    = f.b(PLAYER, BUILDING_IDX);
    b.shuttle_slot = SHUTTLE_SLOT;
    b.sub_id       = SUB_ID;
    write_manifest_word(f, PLAYER, SHUTTLE_SLOT, CARGO_INDEX_CLEAN, 0x0321); // ctrl=0, proto=0x321(801)

    g_puc.spawn_ret = 0; // fail -- keep this test isolated to the spawn-args read

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::prod_unload_cargo_unit(v, own, rec_calls(), PLAYER, BUILDING_IDX, CARGO_INDEX_DIRTY);

    ck(g_puc.spawn_docked.size() == 1 && g_puc.spawn_docked[0].proto_id == 0x321u,
       "puc(dirty cargo_index bits): upper 16 bits masked off -> reads the SAME entry (cargo_index="
       "2) as the clean value would, proto_id=0x321");
}

} // namespace

void run_prod_unload_cargo_unit_tests() {
    test_shuttle_slot_selects_correct_manifest_record_and_passes_sub_id();
    test_ctrl_group_index_extracted_from_bits_12_15_and_readded_on_success();
    test_mask_clears_ctrl_group_bits_leaves_bare_proto_id_in_memory();
    test_unconditional_order_increment_persists_on_success_targets_player_row_zero();
    test_spawn_failure_reverts_header_increment_and_returns_one();
    test_ctrl_group_index_zero_skips_readd_but_other_success_effects_still_fire();
    test_success_path_restores_fields_notifies_zeroes_entry_and_fires_events_in_order();
    test_cargo_index_high_bits_masked_regression();
}

} // namespace mh::sim::test
