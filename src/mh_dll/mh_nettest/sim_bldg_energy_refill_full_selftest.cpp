//
// sim_bldg_energy_refill_full_selftest.cpp -- `simtest` cases for llm_unit_bldg_energy_refill_full
// (sim/sim_unit_bldg_energy_refill_full.h/.cpp), SIM1B batch B.
//
// EXPECTED BEHAVIOUR HAND-DERIVED FROM THE DISASSEMBLY
// (tmp/decomp/llm_unit_bldg_energy_refill_full_0049a8c2.asm), not read off the .cpp:
//
//   player = player_and_flags & 0xf (0x0049a8df-0x0049a8e5), computed ONCE and used, masked, in
//   BOTH arms -- neither arm ever indexes with the raw player_and_flags value.
//
//   `TEST player_and_flags,0x80; JZ 0x0049a944` (0x0049a8e8-0x0049a8ef) selects the arm: bit 0x80
//   CLEAR -> building arm (0x0049a944-0x0049a9a1); bit 0x80 SET -> unit arm (0x0049a8f1-0x0049a942,
//   falls straight through from entry, no jump at all).
//
//   BUILDING ARM: buildings[player][target_index].energy is OVERWRITTEN (FLD/FSTP, a plain copy --
//   not an accumulation) from cfg_buildings[buildings[player][target_index].building_id].energy.
//   Then, IN ORDER (0x0049a984 / 0x0049a990 / 0x0049a99c), all (masked_player, target_index):
//     1. bldg_update_charge_pips  2. refresh_building  3. bldg_notify_ui
//
//   UNIT ARM: units[player][target_index].energy is likewise overwritten from
//   cfg_units[units[player][target_index].unit_proto_id].energy. Then, IN ORDER
//   (0x0049a931 / 0x0049a93d), both (masked_player, target_index):
//     1. unit_update_damage_smoke  2. unit_notify_ui
//
//   The two arms are mutually exclusive -- an arm never fires the other arm's calls.
//
#include "sim/sim_unit_bldg_energy_refill_full.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// The five outward calls, recorded. unit_bldg_energy_refill_full_calls holds plain function
// pointers (production binds it to the generated mh::call:: thunks), so the recorder is a
// file-scope singleton rather than a capture -- same posture as orders_selftest.cpp's game_calls
// recorder and this migration's own translator-brief precedent (sim_bldg_scrap_stored_units.h).
struct call_log {
    int      pips_n = 0, refresh_n = 0, bldg_ui_n = 0, smoke_n = 0, unit_ui_n = 0;
    uint16_t pips_player = 0, refresh_player = 0, bldg_ui_player = 0;
    uint32_t pips_index = 0, bldg_ui_index = 0;
    int32_t  refresh_index = 0;
    uint32_t smoke_player = 0, unit_ui_player = 0; // called as uint32_t (see the .h prototypes)
    int32_t  smoke_index   = 0;
    uint32_t unit_ui_index = 0;

    // S4: a single shared sequence counter, so call ORDER within an arm is directly assertable
    // rather than inferred from separate per-callback counts.
    int order_pips = 0, order_refresh = 0, order_bldg_ui = 0, order_smoke = 0, order_unit_ui = 0;
    int next_seq = 0;

    void reset() { *this = call_log{}; }
};
call_log g_log;

const unit_bldg_energy_refill_full_calls &recording_calls() {
    static const unit_bldg_energy_refill_full_calls gc = {
        [](uint16_t player, uint32_t index) {
            ++g_log.pips_n;
            g_log.pips_player = player;
            g_log.pips_index  = index;
            g_log.order_pips  = ++g_log.next_seq;
        },
        [](uint16_t player, int32_t index) {
            ++g_log.refresh_n;
            g_log.refresh_player = player;
            g_log.refresh_index  = index;
            g_log.order_refresh  = ++g_log.next_seq;
        },
        [](uint16_t player, uint32_t index) {
            ++g_log.bldg_ui_n;
            g_log.bldg_ui_player = player;
            g_log.bldg_ui_index  = index;
            g_log.order_bldg_ui  = ++g_log.next_seq;
        },
        [](uint32_t player, int32_t index) {
            ++g_log.smoke_n;
            g_log.smoke_player = player;
            g_log.smoke_index  = index;
            g_log.order_smoke  = ++g_log.next_seq;
        },
        [](uint32_t player, uint32_t index) {
            ++g_log.unit_ui_n;
            g_log.unit_ui_player = player;
            g_log.unit_ui_index  = index;
            g_log.order_unit_ui  = ++g_log.next_seq;
        },
    };
    return gc;
}

} // namespace

void run_bldg_energy_refill_full_tests() {
    // ---- S1: BUILDING arm -- plain copy (not accumulation) + all 3 building calls fire exactly
    // once with (player, target_index), and NEITHER unit call fires. ------------------------------
    {
        sim_fixture        fx;
        constexpr uint32_t kPlayer = 2, kIndex = 5, kBid = 9;
        fx.b(kPlayer, kIndex).building_id = kBid;
        fx.b(kPlayer, kIndex).energy      = 17.0; // must be OVERWRITTEN, not added to
        fx.cfg_buildings[kBid].energy     = 250.0;
        sim_view  v                       = fx.view();
        sim_store own                     = fx.store();

        g_log.reset();
        detail::unit_bldg_energy_refill_full(v, own, recording_calls(), kPlayer, kIndex);

        ck_eq_d(fx.b(kPlayer, kIndex).energy, 250.0,
                "S1: building energy is set to the cfg ceiling (plain copy, not 17+250)");
        ck_eq((uint32_t)g_log.pips_n, 1, "S1: bldg_update_charge_pips fires exactly once");
        ck_eq((uint32_t)g_log.refresh_n, 1, "S1: refresh_building fires exactly once");
        ck_eq((uint32_t)g_log.bldg_ui_n, 1, "S1: bldg_notify_ui fires exactly once");
        ck_eq((uint32_t)g_log.pips_player, kPlayer, "S1: bldg_update_charge_pips player arg");
        ck_eq(g_log.pips_index, kIndex, "S1: bldg_update_charge_pips index arg");
        ck_eq((uint32_t)g_log.refresh_player, kPlayer, "S1: refresh_building player arg");
        ck_eq((uint32_t)g_log.refresh_index, kIndex, "S1: refresh_building index arg");
        ck_eq((uint32_t)g_log.bldg_ui_player, kPlayer, "S1: bldg_notify_ui player arg");
        ck_eq(g_log.bldg_ui_index, kIndex, "S1: bldg_notify_ui index arg");
        ck_eq((uint32_t)g_log.smoke_n, 0, "S1: unit_update_damage_smoke does NOT fire on the building arm");
        ck_eq((uint32_t)g_log.unit_ui_n, 0, "S1: unit_notify_ui does NOT fire on the building arm");
    }

    // ---- S2: UNIT arm (bit 0x80 set) -- plain copy + both unit calls fire exactly once, neither
    // building call fires. ----------------------------------------------------------------------
    {
        sim_fixture        fx;
        constexpr uint32_t kPlayer = 4, kIndex = 6, kProto = 11;
        fx.u(kPlayer, kIndex).unit_proto_id = kProto;
        fx.u(kPlayer, kIndex).energy        = 3.0; // must be OVERWRITTEN, not added to
        fx.cfg_units[kProto].energy         = 88.0;
        sim_view  v                         = fx.view();
        sim_store own                       = fx.store();

        g_log.reset();
        detail::unit_bldg_energy_refill_full(v, own, recording_calls(), kPlayer | 0x80u, kIndex);

        ck_eq_d(fx.u(kPlayer, kIndex).energy, 88.0,
                "S2: unit energy is set to the cfg ceiling (plain copy, not 3+88)");
        ck_eq((uint32_t)g_log.smoke_n, 1, "S2: unit_update_damage_smoke fires exactly once");
        ck_eq((uint32_t)g_log.unit_ui_n, 1, "S2: unit_notify_ui fires exactly once");
        ck_eq(g_log.smoke_player, kPlayer, "S2: unit_update_damage_smoke player arg");
        ck_eq((uint32_t)g_log.smoke_index, kIndex, "S2: unit_update_damage_smoke index arg");
        ck_eq(g_log.unit_ui_player, kPlayer, "S2: unit_notify_ui player arg");
        ck_eq(g_log.unit_ui_index, kIndex, "S2: unit_notify_ui index arg");
        ck_eq((uint32_t)g_log.pips_n, 0, "S2: bldg_update_charge_pips does NOT fire on the unit arm");
        ck_eq((uint32_t)g_log.refresh_n, 0, "S2: refresh_building does NOT fire on the unit arm");
        ck_eq((uint32_t)g_log.bldg_ui_n, 0, "S2: bldg_notify_ui does NOT fire on the unit arm");
    }

    // ---- S3: player masking -- player_and_flags carries garbage in the high nibble/other flag
    // bits; only the low nibble (& 0xf) selects the player, in BOTH the roster index and the
    // masked_player call arguments. A record at a DIFFERENT player (the raw byte's value, were it
    // used unmasked) must stay untouched. --------------------------------------------------------
    {
        sim_fixture        fx;
        constexpr uint32_t kIndex = 2, kBid = 14;
        constexpr uint32_t kMaskedPlayer        = 0x3;  // low nibble
        constexpr uint32_t kRawFlags            = 0x53; // 0x50 garbage in the high bits | 0x3, bit 0x80 clear
        fx.b(kMaskedPlayer, kIndex).building_id = kBid;
        fx.b(kMaskedPlayer, kIndex).energy      = 1.0;
        fx.cfg_buildings[kBid].energy           = 42.0;
        // A decoy at a DIFFERENT player row -- proves the write/reads are confined to the masked
        // player and don't spill onto a neighbouring row.
        fx.b(5, kIndex).building_id = 20;
        fx.b(5, kIndex).energy      = 999.0;
        fx.cfg_buildings[20].energy = 111.0;
        sim_view  v                 = fx.view();
        sim_store own               = fx.store();

        g_log.reset();
        detail::unit_bldg_energy_refill_full(v, own, recording_calls(), kRawFlags, kIndex);

        ck_eq_d(fx.b(kMaskedPlayer, kIndex).energy, 42.0,
                "S3: the LOW NIBBLE player row is the one refilled");
        ck_eq_d(fx.b(5, kIndex).energy, 999.0, "S3: an unrelated player row is untouched");
        ck_eq((uint32_t)g_log.pips_player, kMaskedPlayer,
              "S3: the masked player (not the raw flags byte) is passed to the callees");
        ck_eq((uint32_t)g_log.refresh_player, kMaskedPlayer, "S3: refresh_building sees the masked player");
        ck_eq((uint32_t)g_log.bldg_ui_player, kMaskedPlayer, "S3: bldg_notify_ui sees the masked player");
    }

    // ---- S4: call ORDER within an arm, via one shared sequence counter. --------------------------
    {
        sim_fixture        fx;
        constexpr uint32_t kPlayer = 1, kIndex = 3, kBid = 7;
        fx.b(kPlayer, kIndex).building_id = kBid;
        fx.cfg_buildings[kBid].energy     = 5.0;
        sim_view  v                       = fx.view();
        sim_store own                     = fx.store();

        g_log.reset();
        detail::unit_bldg_energy_refill_full(v, own, recording_calls(), kPlayer, kIndex);

        ck_eq((uint32_t)g_log.order_pips, 1, "S4: bldg_update_charge_pips is called 1st");
        ck_eq((uint32_t)g_log.order_refresh, 2, "S4: refresh_building is called 2nd");
        ck_eq((uint32_t)g_log.order_bldg_ui, 3, "S4: bldg_notify_ui is called 3rd");
    }

    // ---- S5: call ORDER within the UNIT arm. -------------------------------------------------------
    {
        sim_fixture        fx;
        constexpr uint32_t kPlayer = 1, kIndex = 3, kProto = 8;
        fx.u(kPlayer, kIndex).unit_proto_id = kProto;
        fx.cfg_units[kProto].energy         = 6.0;
        sim_view  v                         = fx.view();
        sim_store own                       = fx.store();

        g_log.reset();
        detail::unit_bldg_energy_refill_full(v, own, recording_calls(), kPlayer | 0x80u, kIndex);

        ck_eq((uint32_t)g_log.order_smoke, 1, "S5: unit_update_damage_smoke is called 1st");
        ck_eq((uint32_t)g_log.order_unit_ui, 2, "S5: unit_notify_ui is called 2nd");
    }
}

} // namespace mh::sim::test
