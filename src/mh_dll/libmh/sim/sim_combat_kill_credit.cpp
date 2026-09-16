//
// sim/sim_combat_kill_credit.cpp -- see sim_combat_kill_credit.h. Translated from the DISASSEMBLY
// (tmp/decomp_sim1e2/llm_strat_apply_area_damage_0044c3fb.asm,
// tmp/decomp_sim1e2/llm_strat_bldg_kill_credit_0044c67f.asm,
// tmp/decomp_sim1e2/llm_strat_unit_kill_credit_0044cac9.asm), cross-checked field-by-field against
// the header's own DECLARED-NEED / FIELDS sections.
//
#include "sim/sim_combat_kill_credit.h"

#include "addr/mh_calls.gen.h"       // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"             // ai_say / trace_budget -- the shared trace sink, not AI state
#include "sim/sim_bldg_get_coords.h" // already-reimplemented sibling: llm_strat_bldg_get_coords
#include "sim/sim_unit_fine_pos.h"   // already-reimplemented sibling: llm_strat_unit_get_coords
#include "sim/sim_unit_notify.h"     // already-reimplemented sibling: llm_strat_unit_notify_ui
#include "state/host_api.h"
#include "state/host_events.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const kill_credit_calls &live_kill_credit_calls() {
    static const kill_credit_calls c = {
        MH_LIBMH_BIND(llm_math_scale_pct),
        mh::state::evt::snd_play,
        mh::state::evt::text_queue_id,
        MH_LIBMH_BIND(llm_strat_ai_bldg_register_visible_building),
    };
    return c;
}

namespace {

// The tile<->fine conversion (`SAR EDX,0x1f / SHL EDX,0x5 / SBB EAX,EDX / SAR EAX,0x5`), applied to
// CAM_PAN_TARGET_COL/_ROW by both kill-credit functions' notify tails. Value-for-value C's truncating
// `/ 32` -- see sim_projectile_tick.cpp's identically-named/identically-derived helper; re-derived
// locally per this project's per-TU convention.
inline int32_t fine_to_tile(int32_t fine) { return fine / 32; }

// tile_object::unit and unit::unit_above are both `uint8_t[2]` in the generated header (the generator
// renders map::t::unit_full_id as a raw byte pair, not a scalar) -- this reassembles the little-endian
// word the original addresses with a single `MOVZX reg, word ptr [...]`. High nibble = owning player,
// low 12 bits = roster index. Same idiom as ai/ai_holding_pen.cpp's local helper of the same name; not
// shared across translation units (each TU that needs it defines its own, per the "no new shared
// helpers" rule).
inline uint16_t unit_full_id_word(const uint8_t (&packed)[2]) {
    return static_cast<uint16_t>(packed[0] | (packed[1] << 8));
}

// cfg_building::type / cfg_unit-adjacent building-type constants this file needs that are not already
// declared in ai/ai_state.h or sim_order_dispatch_bldg.cpp (libmh/sim/ does not depend on libmh/ai/, so the
// two MOTHER values are re-declared here per that same established duplication convention; MAIN_BASE
// is a fresh derivation -- see the header banner's DECLARED-NEED-adjacent note on why it is trusted
// without a prior cross-validated occurrence: both values were read directly off raw CMP immediates,
// not inferred from a jump-table label, and A_MAIN_BASE+0x14... actually +0x14 is wrong -- the pair
// obeys the SAME "+0x14... " no: the tree's OWN "+20" H-vs-A pairing rule (0x0e + 20 = 0x22) holds
// exactly, matching every other verified pair in sim_order_dispatch_bldg.cpp).
inline constexpr uint8_t BLDG_TYPE_A_MOTHER    = 0x06;
inline constexpr uint8_t BLDG_TYPE_H_MOTHER    = 0x1a;
inline constexpr uint8_t BLDG_TYPE_A_MAIN_BASE = 0x0e;
inline constexpr uint8_t BLDG_TYPE_H_MAIN_BASE = 0x22;

} // namespace

namespace detail {

// ---- llm_strat_bldg_kill_credit @0x0044c67f --------------------------------------------------------

void bldg_kill_credit(const sim_view &v, sim_store &own, const kill_credit_calls &c,
                      uint32_t victim_player, int32_t victim_building_index, double damage,
                      uint32_t killer_info, int32_t killer_unit_index) {
    const uint16_t      player = static_cast<uint16_t>(victim_player);
    building           &b      = own.building_at(player, victim_building_index);
    const cfg_building &bt     = v.cfg_buildings[b.building_id];

    // 0x0044c6bc-0x0044c6f7: MAIN_BASE damage multiplier. DECLARED NEED: v.bldg_main_base_damage_mult
    // does not exist yet -- see the header banner.
    if (bt.type == BLDG_TYPE_H_MAIN_BASE || bt.type == BLDG_TYPE_A_MAIN_BASE) {
        damage = damage * *v.bldg_main_base_damage_mult;
    }

    // 0x0044c6fa-0x0044c7b6: pending_damage/energy before-and-after compare (see header banner step 2).
    const double pre_pending   = b.pending_damage;
    const double energy_before = b.energy;
    b.pending_damage += damage;
    const bool just_died = (pre_pending < energy_before) && (energy_before <= b.pending_damage);

    if (just_died) {
        // 0x0044c7c7-0x0044c83d: genuine cross-player kill -> award experience + notify.
        if (((killer_info & 0x80u) != 0u) && ((killer_info & 0xfu) != static_cast<uint32_t>(player))) {
            own.unit_at(killer_info & 0xfu, killer_unit_index).experience += bt.kill_score;
            mh::sim::notify_ui(killer_info & 0xfu, static_cast<uint32_t>(killer_unit_index));
        }
        // 0x0044c83d-0x0044c85f: buildings_killed_total[planet] += 1, ALWAYS on just_died (not gated
        // by the cross-player check above -- own.profile_at() is player_profile, NOT v.players; see
        // sim_state.h's alias-distinction comment).
        own.profile_at(static_cast<int32_t>(killer_info & 0xfu))
            .buildings_killed_total[*v.planet_index] += 1;
    }

    if (player != static_cast<uint16_t>(killer_info & 0xfu)) {
        building &slot0 = own.building_at(player, 0);
        if (bt.type == BLDG_TYPE_H_MOTHER || bt.type == BLDG_TYPE_A_MOTHER) {
            // 0x0044c8cc-0x0044c9b8: MOTHER-type notify. DECLARED NEED:
            // v.bldg_lost_feedback_cooldown_mother does not exist yet -- see the header banner.
            if (slot0.cycle_progress + *v.bldg_lost_feedback_cooldown_mother < *v.game_clock) {
                if (player == static_cast<uint16_t>(*v.player_side)) {
                    // 0x0044c901-0x0044c936: sim_active DOES gate the snd_play here (flags genuinely
                    // consulted, unlike the non-mother arm below).
                    if (*v.sim_active != 0) {
                        const int32_t race_base = (*v.player_race == 2) ? 0x12 : 0;
                        c.snd_play(race_base + 1, 100);
                    }
                    mh::sim::bldg_get_coords(player, victim_building_index, &own.cam_pan_target_col(),
                                             &own.cam_pan_target_row());
                    own.cam_pan_target_col() = fine_to_tile(own.cam_pan_target_col());
                    own.cam_pan_target_row() = fine_to_tile(own.cam_pan_target_row());
                    c.ui_print_queue_text_id(2);
                }
                slot0.cycle_progress = *v.game_clock;
            }
            slot0.last_tick_time = *v.game_clock;
        } else {
            // 0x0044c9bd-0x0044ca47: non-MOTHER notify. The original's OWN sim_active compare here is
            // DEAD (its flags are clobbered by the next CMP before any conditional jump reads them --
            // see the header banner) so, faithfully, NO snd_play call exists in this arm at all.
            // DECLARED NEED: v.bldg_lost_feedback_cooldown_other does not exist yet.
            if (slot0.last_tick_time + *v.bldg_lost_feedback_cooldown_other < *v.game_clock &&
                player == static_cast<uint16_t>(*v.player_side)) {
                mh::sim::bldg_get_coords(player, victim_building_index, &own.cam_pan_target_col(),
                                         &own.cam_pan_target_row());
                own.cam_pan_target_col() = fine_to_tile(own.cam_pan_target_col());
                own.cam_pan_target_row() = fine_to_tile(own.cam_pan_target_row());
                c.ui_print_queue_text_id(3);
            }
            slot0.last_tick_time = *v.game_clock;
        }
    }

    // 0x0044ca5d-0x0044caa4: a THIRD, fresh re-derivation of energy<=pending_damage (see header banner
    // step 4) -- deliberately re-read, not reused from `just_died` above.
    const bool now_dead = b.energy <= b.pending_damage;
    c.ai_bldg_register_visible_building(
        victim_building_index, static_cast<uint32_t>(player) | 0x40u,
        static_cast<uint32_t>(killer_unit_index), killer_info & 0xffffu, now_dead ? 1 : 0);
}

// ---- llm_strat_unit_kill_credit @0x0044cac9 --------------------------------------------------------

void unit_kill_credit(const sim_view &v, sim_store &own, const kill_credit_calls &c,
                      uint32_t victim_player, int32_t victim_unit_index, double damage,
                      uint32_t killer_info, int32_t killer_unit_index) {
    const uint16_t  player = static_cast<uint16_t>(victim_player);
    unit           &u      = own.unit_at(player, victim_unit_index);
    const cfg_unit &ut     = v.cfg_units[u.unit_proto_id];

    // 0x0044caea-0x0044cb23: damage scaled by the VICTIM's own armor_prob row (indexed by
    // victim_player, not killer -- read exactly as the assembly computes it, see header banner).
    const double scaled_damage = c.math_scale_pct(damage, ut.armor_prob[player]);

    // 0x0044cb26-0x0044cbe2: pending_damage/energy before-and-after compare (see header banner step 2).
    const double pre_pending   = u.pending_damage;
    const double energy_before = u.energy;
    u.pending_damage += scaled_damage;
    const bool just_died = (pre_pending < energy_before) && (energy_before <= u.pending_damage);

    if (just_died) {
        // 0x0044cbf3-0x0044cc69: genuine cross-player kill -> award experience + notify.
        if (((killer_info & 0x80u) != 0u) && ((killer_info & 0xfu) != static_cast<uint32_t>(player))) {
            own.unit_at(killer_info & 0xfu, killer_unit_index).experience += ut.kill_score;
            mh::sim::notify_ui(killer_info & 0xfu, static_cast<uint32_t>(killer_unit_index));
        }
        // 0x0044cc69-0x0044cc8b: units_killed_total[planet] += 1, ALWAYS on just_died.
        own.profile_at(static_cast<int32_t>(killer_info & 0xfu)).units_killed_total[*v.planet_index] +=
            1;
    }

    // 0x0044cc8b-0x0044cda7: victim!=killer AND the victim's cfg type is not a crewed/soldier-bearing
    // one (`Unit[proto].soldier_count < 1`, JG-skips when >0 -- read directly off 0x0044ccc2/c9).
    if (player != static_cast<uint16_t>(killer_info & 0xfu) && ut.soldier_count < 1) {
        unit &slot0 = own.unit_at(player, 0);
        if (slot0.activity_clock + *v.unit_lost_feedback_cooldown < *v.game_clock &&
            player == static_cast<uint16_t>(*v.player_side)) {
            // 0x0044cd04-0x0044cd36: sim_active DOES gate the snd_play here (flags genuinely consulted,
            // unlike bldg_kill_credit's non-mother arm).
            if (*v.sim_active != 0) {
                const int32_t race_base = (*v.player_race == 2) ? 0x12 : 0;
                c.snd_play(race_base + 2, 100);
            }
            mh::sim::get_coords(player, victim_unit_index, &own.cam_pan_target_col(),
                                &own.cam_pan_target_row());
            own.cam_pan_target_col() = fine_to_tile(own.cam_pan_target_col());
            own.cam_pan_target_row() = fine_to_tile(own.cam_pan_target_row());
            c.ui_print_queue_text_id(0x73);
        }
        slot0.activity_clock = *v.game_clock;
    }

    // 0x0044cda7-0x0044cdee: a THIRD, fresh re-derivation of energy<=pending_damage (see header banner
    // step 4) -- deliberately re-read, not reused from `just_died` above.
    const bool now_dead = u.energy <= u.pending_damage;
    c.ai_bldg_register_visible_building(
        victim_unit_index, static_cast<uint32_t>(player) | 0x80u,
        static_cast<uint32_t>(killer_unit_index), killer_info & 0xffffu, now_dead ? 1 : 0);
}

// ---- llm_strat_apply_area_damage @0x0044c3fb -------------------------------------------------------

void apply_area_damage(const sim_view &v, sim_store &own, const kill_credit_calls &c, int32_t x,
                       int32_t y, int32_t target_kind, double damage, int32_t ring_count,
                       uint32_t /*owner_filter_zeroed*/, uint32_t killer_info,
                       int32_t killer_unit_index) {
    // owner_filter_zeroed is REAL DEAD CODE in the original (unconditionally clobbered to 0 at entry
    // before ever being read -- see the header banner), so its value is never consulted here either;
    // the parameter is kept only for call-site ABI compatibility with addr/mh_calls.gen.h.
    for (int32_t ring = 0; ring < ring_count; ++ring) {
        uint32_t col = map_width_mask(v) & static_cast<uint32_t>(x - ring);
        for (int32_t xi = ring * 2 + 1; xi != 0; --xi) {
            uint32_t row = map_height_mask(v) & static_cast<uint32_t>(y - ring);
            for (int32_t yi = ring * 2 + 1; yi != 0; --yi) {
                const double ring_damage = damage / (static_cast<double>(ring) + 1.0);

                const tile_object &t =
                    tile_at(v, static_cast<int32_t>(col), static_cast<int32_t>(row));

                if (target_kind == 1 && t.building != 0) {
                    const uint32_t owner    = static_cast<uint32_t>(t.class_owner) & 0xfu;
                    const uint8_t  class_hi = static_cast<uint8_t>(t.class_owner & 0xf0u);
                    // Literal JC/JBE/JZ cascade transcription -- see header banner on why this is not
                    // simplified to `==0x40`/`==0x80` despite being provably equivalent.
                    if (class_hi > 0x3fu) {
                        if (class_hi < 0x41u) {
                            detail::bldg_kill_credit(v, own, c, owner, t.building, ring_damage,
                                                     killer_info & 0xffffu, killer_unit_index);
                        } else if (class_hi == 0x80u) {
                            // Reads the SAME `.building` field, not `.unit` -- see header banner.
                            detail::unit_kill_credit(v, own, c, owner, t.building, ring_damage,
                                                     killer_info & 0xffffu, killer_unit_index);
                        }
                    }
                }

                if (target_kind == 2) {
                    uint16_t link = unit_full_id_word(t.unit);
                    while (link != 0) {
                        const uint32_t link_owner = static_cast<uint32_t>(link >> 12) & 0xfu;
                        const uint32_t link_index = static_cast<uint32_t>(link) & 0x0fffu;
                        detail::unit_kill_credit(v, own, c, link_owner,
                                                 static_cast<int32_t>(link_index), ring_damage,
                                                 killer_info & 0xffffu, killer_unit_index);
                        link = unit_full_id_word(
                            unit_of(v, link_owner, static_cast<int32_t>(link_index)).unit_above);
                    }
                }

                row = map_height_mask(v) & (row + 1);
            }
            col = map_width_mask(v) & (col + 1);
        }
    }
}

} // namespace detail

// ---- the public wrappers ----------------------------------------------------------------------

void bldg_kill_credit(uint32_t victim_player, int32_t victim_building_index, double damage,
                      uint32_t killer_info, int32_t killer_unit_index) {
    sim_state st = state();
    detail::bldg_kill_credit(st.read, st.own, live_kill_credit_calls(), victim_player,
                             victim_building_index, damage, killer_info, killer_unit_index);
}

void unit_kill_credit(uint32_t victim_player, int32_t victim_unit_index, double damage,
                      uint32_t killer_info, int32_t killer_unit_index) {
    sim_state st = state();
    detail::unit_kill_credit(st.read, st.own, live_kill_credit_calls(), victim_player,
                             victim_unit_index, damage, killer_info, killer_unit_index);
}

void apply_area_damage(int32_t x, int32_t y, int32_t target_kind, double damage, int32_t ring_count,
                       uint32_t owner_filter_zeroed, uint32_t killer_info, int32_t killer_unit_index) {
    sim_state st = state();
    detail::apply_area_damage(st.read, st.own, live_kill_credit_calls(), x, y, target_kind, damage,
                              ring_count, owner_filter_zeroed, killer_info, killer_unit_index);
}


} // namespace mh::sim
