//
// sim/resid/sim_clock_resync.cpp -- see sim_clock_resync.h. Translated from the DISASSEMBLY
// (tmp/decomp_sim_resid/llm_strat_clock_resync_units_and_buildings_00499a3a.asm), the exported `.c`
// being a draft.
//
#include "sim/resid/sim_clock_resync.h"

namespace mh::sim {

namespace {

// player_profile.status_flags bit (E_STRAT_PLAYER_STATUS bit1) -- same spelling/value as every other
// sim/ TU testing this field (sim/sim_player_presence_lost.cpp, sim/sim_game_speed_recompute.cpp,
// this batch's own sim/resid/sim_session_clear_presence_flag.cpp). libmh/sim/ keeps its own local copy
// rather than depending on ai/ai_state.h's PLAYER_STATUS_ALIVE.
inline constexpr uint32_t STATUS_ALIVE = 0x2u;

// The 12 anim_dur slots per building (mh_map_object_building::anim_dur, mh_structs.gen.h) -- the
// inner loop's literal bound (`CMP dword ptr [EBP-0x1c],0xc` @0x00499b78).
inline constexpr int32_t ANIM_DUR_SLOTS = 12;

} // namespace

namespace detail {

// ---- llm_strat_clock_resync_units_and_buildings @0x00499a3a ----------------------------------------
void clock_resync_units_and_buildings(const sim_view &v, sim_store &own, double new_time) {
    // 0x00499a52-0x00499bc4: for every player slot 0..7.
    for (int32_t player = 0; player < MAX_PLAYERS; ++player) {
        // 0x00499a6c-0x00499a7a: TEST byte[status_flags],0x2 -- skip a not-ALIVE player entirely
        // (both roster sweeps below).
        if ((v.profiles[player].status_flags & STATUS_ALIVE) == 0) continue;

        // ---- units: resync activity_clock/rotation_clock -----------------------------------------
        // 0x00499a7c-0x00499a8d: live-count header, `units[player][0].unit_above` reinterpreted as a
        // 16-bit word (MOVZX word ptr, NOT the 2-byte unit_above field's own per-byte meaning) --
        // roster slot 0 doubles as a live-count header, same convention `index` below uses for
        // buildings.
        {
            const unit &slot0      = own.unit_at(static_cast<uint32_t>(player), 0);
            uint32_t    live_count = static_cast<uint32_t>(slot0.unit_above[0]) |
                                  (static_cast<uint32_t>(slot0.unit_above[1]) << 8);

            // 0x00499a94-0x00499b08: scan slots 1, 2, 3, ... -- UNBOUNDED, no comparison against
            // UNITS_PER_PLAYER anywhere in this loop (see the header's boundary-case note). Stops
            // only once `live_count` live slots have been found and rewritten.
            int32_t idx = 1;
            while (live_count != 0) {
                unit &u = own.unit_at(static_cast<uint32_t>(player), idx);
                if (u.unit_proto_id != 0) {
                    // 0x00499ace-0x00499adf / 0x00499ae0-0x00499aff: raw 8-byte bit-copy of the
                    // `new_time` parameter into both clocks (see the header's float note -- zero x87
                    // ops in the original).
                    u.activity_clock = new_time;
                    u.rotation_clock = new_time;
                    --live_count;
                }
                ++idx;
            }
        }

        // ---- buildings: resync last_tick_time + all 12 anim_dur slots -----------------------------
        // 0x00499b0a-0x00499b1b: live-count header, `buildings[player][0].index` reinterpreted as a
        // 16-bit word (MOVZX word ptr; `index` is declared `int16_t` but the read is unsigned, so the
        // zero-extension is reproduced with the same bit pattern regardless of the field's own
        // signedness).
        {
            const building &slot0      = own.building_at(static_cast<uint32_t>(player), 0);
            uint32_t        live_count = static_cast<uint32_t>(static_cast<uint16_t>(slot0.index));

            // 0x00499b22-0x00499bba: scan slots 1, 2, 3, ... -- UNBOUNDED, same shape as the unit
            // loop above (no comparison against BUILDINGS_PER_PLAYER anywhere in this loop).
            int32_t idx = 1;
            while (live_count != 0) {
                building &b = own.building_at(static_cast<uint32_t>(player), idx);
                if (b.building_id != 0) {
                    // 0x00499b5f-0x00499b6f: raw 8-byte bit-copy into last_tick_time.
                    b.last_tick_time = new_time;
                    // 0x00499b71-0x00499bb2: all 12 anim_dur slots, same raw bit-copy per slot.
                    for (int32_t k = 0; k < ANIM_DUR_SLOTS; ++k) {
                        b.anim_dur[k] = new_time;
                    }
                    --live_count;
                }
                ++idx;
            }
        }
    }
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void clock_resync_units_and_buildings(double new_time) {
    sim_state st = state();
    detail::clock_resync_units_and_buildings(st.read, st.own, new_time);
}

} // namespace mh::sim
