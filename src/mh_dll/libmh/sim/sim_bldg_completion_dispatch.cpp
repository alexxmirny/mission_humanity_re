//
// sim/sim_bldg_completion_dispatch.cpp -- see sim_bldg_completion_dispatch.h. Translated from the
// DISASSEMBLY (tmp/decomp_sim/llm_strat_bldg_completion_dispatch_004795dd.asm), cross-checked against
// the Ghidra .c draft ONLY for the building-TYPE jump-table's targets (the raw .asm export does not
// carry the table bytes; the decompiler does) -- every other block below was independently re-derived
// from the raw byte offsets, per the header banner's ARM-BY-ARM section.
//
#include "sim/sim_bldg_completion_dispatch.h"

#include "addr/mh_calls.gen.h" // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"       // ai_say / trace_budget -- the shared trace sink, not AI state
#include "state/host_api.h"
#include "state/host_events.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
#include "fp/x87.h"         // CRT-X87: the shared x87 truncation helpers
#include "crt/crt_select.h" // LIB-CRT: MH_CRT() picks the vendored CRT in the standalone build

namespace mh::sim {

const bldg_completion_dispatch_calls &live_bldg_completion_dispatch_calls() {
    static const bldg_completion_dispatch_calls c = {
        MH_LIBMH_BIND(llm_strat_mine_scan_deposit_slot),
        MH_LIBMH_BIND(llm_strat_population_add),
        MH_LIBMH_BIND(llm_resource_add),
        MH_LIBMH_BIND(llm_strat_load_base_layout_dmp),
        MH_CRT(utils_sprintf__vssii), // DECLARED NEED 8 -- not yet in mh_calls.gen.h
        MH_LIBMH_BIND(llm_strat_bldg_uses_workers),
        MH_LIBMH_BIND(llm_strat_bldg_unassign_workers),
        MH_LIBMH_BIND(llm_strat_bldg_assign_workers),
        MH_LIBMH_BIND(llm_strat_bldg_set_staffed_flag),
        MH_LIBMH_BIND(llm_strat_bldg_clear_staffed_flag),
        MH_LIBMH_BIND(game_UpdateProgress),
        MH_LIBMH_BIND(llm_strat_bldg_register_online),
        MH_LIBMH_BIND(llm_strat_bldg_link_to_network_if_adjacent),
        MH_LIBMH_BIND(llm_strat_prod_deliver_arrivals),
        MH_LIBMH_BIND(llm_strat_unit_spawn_docked),
        MH_LIBMH_BIND(llm_strat_unit_add_docked),
        MH_LIBMH_BIND(llm_strat_unit_type_group_index),
        MH_LIBMH_BIND(llm_strat_reason_to_housing_bldg),
        MH_CRT(w_sprintf__vss),
        MH_CRT(w_sprintf__vsss),
        mh::state::evt::text_print_u32,
        MH_LIBMH_BIND(llm_unit_apply_production_completion),
        MH_LIBMH_BIND(llm_strat_ai_notify_unit_lifecycle),
        MH_LIBMH_BIND(llm_strat_invasion_chance_roll),
    };
    return c;
}

namespace {

// ---- llm_strat_bldg_state members this function dispatches on -- no committed C++ enum exists yet
// (mh_structs.gen.h's `state` field comment carries `[llm_strat_bldg_state]` only as a tag), so these
// are this TU's own copy of the established per-TU-anonymous-namespace precedent. ALL SIX VALUES
// independently cross-checked against existing declarations of the same names/values in
// sim_bldg_add_workers.cpp / sim_bldg_finish_order.cpp / sim_bldg_remove_workers.cpp /
// sim_bldg_state_mine.cpp / sim_order_dispatch_bldg.cpp / sim_refresh_building.cpp /
// sim_bldg_state_charge.cpp before being re-derived here from this function's own CMP immediates.
inline constexpr uint16_t BLDG_STATE_CONSTRUCTION        = 0x64;
inline constexpr uint16_t BLDG_STATE_CHARGE_GATE         = 0x69;
inline constexpr uint16_t BLDG_STATE_PROD_WORKING        = 0x6d;
inline constexpr uint16_t BLDG_STATE_PROD_BLOCKED_NOTIFY = 0x6e;
inline constexpr uint16_t BLDG_STATE_MINE_EXTRACTING     = 0x74;
inline constexpr uint16_t BLDG_STATE_UPGRADING           = 0x82;
inline constexpr uint16_t BLDG_STATE_RESEARCHING         = 0x89;

// ---- cfg_enum_E_BUILDING members the CONSTRUCTION arm's type-switch compares against -- an EXISTING
// Ghidra enum (rule 17a); redeclared file-local per the established per-TU precedent
// (sim_bldg_unmap_footprint.cpp's own switch over the SAME enum uses these exact values).
inline constexpr uint8_t BLDG_TYPE_A_MINE   = 2;
inline constexpr uint8_t BLDG_TYPE_H_MINE   = 22;
inline constexpr uint8_t BLDG_TYPE_A_MOTHER = 6;
inline constexpr uint8_t BLDG_TYPE_H_MOTHER = 0x1a;
inline constexpr uint8_t BLDG_TYPE_A_PORT   = 0x0c;
inline constexpr uint8_t BLDG_TYPE_H_PORT   = 0x20;

// player_profile::status_flags bit 3 (E_STRAT_PLAYER_STATUS, no committed Ghidra enum -- see
// mh_structs.gen.h's own field comment) -- AI-controlled. This exact bit/site is already cross-
// referenced BY NAME in that field comment: "b3 also gates AI base-layout .DMP injection at
// llm_strat_bldg_completion_dispatch 0x479908", confirming this read before this TU existed.
inline constexpr uint32_t STRAT_PLAYER_STATUS_AI_CONTROLLED = 0x8u;

// game_e_race member -- same value/name precedent as sim_game_get_starting_unit.h's / sim_landing_
// spot.cpp's own independent RACE_ALIEN declarations (no committed Ghidra enum exists yet either).
inline constexpr uint32_t RACE_ALIEN = 2u;

// _G_LLM_GAME_SESSION_MODE == 1 is single-player (sim_order_dispatch_bldg.cpp's own comment: "3 is MP
// lockstep").
inline constexpr int32_t SESSION_MODE_SINGLE_PLAYER = 1;

// The shared "%s: %s" / "%s: %s (%s)" message formats -- SAME literal addresses (0x005012ec /
// 0x0050133a) sim_bldg_state_charge.cpp's TEXT_FMT_NAME_REASON / sim_bldg_state_prod.cpp's
// TEXT_FMT_NAME_REASON_HOUSING already establish; duplicated per-TU per those files' own precedent.
constexpr const wchar_t *TEXT_FMT_NAME_REASON         = L"%s: %s";
constexpr const wchar_t *TEXT_FMT_NAME_REASON_HOUSING = L"%s: %s (%s)";

// The AI base-layout .DMP path format + the literal "init\" component -- Ghidra auto-labelled string
// content (s_%s%s_%02d%02d.DMP_0050139e / s_init\_005d05f0), not DAT_/UNK_ placeholders, so the label
// itself already supplies the value (rule 17b's "auto-label you had to read" applies to unresolved
// DAT_/UNK_/FUN_ tokens, not to Ghidra's own content-bearing s_ string labels).
constexpr const char *DMP_PATH_FORMAT = "%s%s_%02d%02d.DMP";
constexpr const char *DMP_PATH_INIT   = "init\\";

// llm_strat_bldg_register_online's param_3/param_4 at THIS call site trace to EBX/ECX left behind by
// several intervening calls (uses_workers/set_staffed_flag/clear_staffed_flag/game_UpdateProgress at
// 0x00479c9c-0x00479d29), NOT freshly loaded from WORKER_MGMT immediately before the call at
// 0x00479d3e (only EAX=player/EDX=building_index are reloaded there; the asm literally passes
// whatever register_online's own callers left in EBX/ECX). RESOLVED 2026-08-22 (reimpl-verify raised
// it, conductor investigated): register_online is a pure switch dispatcher that forwards param_3/
// param_4 UNCHANGED to one of 12 per-building-type handlers; checked 6 of the 12
// (llm_strat_bldg_online_{vehicles_h,default,barracks_garage_a,port_a,shuttle_h,helipad_h_or_misc})
// via fresh decompiles and NONE of them read param_3 or param_4 anywhere in their bodies -- every one
// only uses player/building_index/anim_dur. Same dead-parameter shape as THIS function's own
// param_3/param_4 (an earlier pass's plate). Hardcoding 0 is therefore harmless regardless of the true
// register-garbage value, since nothing downstream ever reads it.
inline constexpr uint32_t REGISTER_ONLINE_PARAM3_UNRESOLVED = 0u;
inline constexpr uint32_t REGISTER_ONLINE_PARAM4_UNRESOLVED = 0u;

// utils_math_trunc @0x004d0596 (`MH_UNAVAILABLE__parameter_storage_not_marshallable` in
// mh_calls.gen.h -- ST0 in, ST0 out, x87-register-only). This function's own call site (0x0047964c)
// is an ORDINARY call to it, not compiler-inlined -- reproduced as this TU's own copy of the
// sim_bldg_mother_reelect_primary.cpp / ai_active_unit_tick.cpp precedent's exact instruction
// sequence. FISTP width confirmed 32-bit AT THIS SITE (opcode bytes `db 5d b8` @0x00479651 -> 0xDB
// ModRM 0x5D, reg field 3 -> 0xDB /3 == FISTP m32int).
int32_t trunc_to_int32(double val) {
    return ::mh::fp::trunc_i32(val);
}

// ---- WORKER_MGMT (see the header banner) -- byte-identical logic at THIS function's own three call
// sites (CONSTRUCTION's tail, CHARGE_GATE's whole body, UPGRADING's tail), factored into one local
// helper per the sim_bldg_state_charge.cpp `charge_gate_complete_cycle` precedent for a duplicate
// WITHIN one original function's own compiled output.
void worker_mgmt(const sim_view &v, sim_store &own, const bldg_completion_dispatch_calls &c, uint16_t player,
                 int32_t bidx) {
    building           &b  = own.building_at(player, bidx);
    const cfg_building &cb = v.cfg_buildings[b.building_id];

    const int32_t uses = c.bldg_uses_workers(player, bidx);
    if (uses == 0) {
        if (b.current_workers != 0) {
            c.bldg_unassign_workers(player, bidx, b.current_workers);
        }
    } else if (cb.worker_count < static_cast<int32_t>(b.current_workers)) {
        c.bldg_unassign_workers(player, bidx, b.current_workers - static_cast<uint32_t>(cb.worker_count));
    } else if (static_cast<int32_t>(b.current_workers) < cb.worker_count && v.population[player].human != 0) {
        // (human - (human>>31))>>1 -- the round-toward-zero-halving idiom, PROVEN bit-exact to
        // `human/2` (verified for both signs; C's `/` truncates toward zero since C99/C++11). Written
        // as the shift form anyway per translator-brief rule 8, matching WORKER_MGMT's OTHER call
        // sites (the final accumulator halving/quartering below uses the same proof-then-shift-form
        // choice).
        const int32_t human = v.population[player].human;
        const int32_t half  = (human - (human >> 31)) >> 1;
        c.bldg_assign_workers(player, bidx, half);
    }

    // Independent re-check (0x00479c78-0x00479cbf pattern): current_workers==0 AND a FRESH
    // uses_workers() call both true -> clear staffed; otherwise set staffed.
    if (b.current_workers == 0 && c.bldg_uses_workers(player, bidx) != 0) {
        c.bldg_clear_staffed_flag(player, bidx);
    } else {
        c.bldg_set_staffed_flag(player, bidx);
    }
}

} // namespace

namespace detail {

void bldg_completion_dispatch(const sim_view &v, sim_store &own, const bldg_completion_dispatch_calls &c,
                              uint32_t param_1, uint32_t param_2, uint32_t /*param_3*/, uint32_t /*param_4*/,
                              double param_5) {
    // param_3/param_4 are accepted (matching the committed prototype) but CONFIRMED dead -- see the
    // header banner's PARAM_3/PARAM_4 ARE DEAD section.
    const uint16_t player = static_cast<uint16_t>(param_1);
    const int32_t  bidx   = static_cast<int32_t>(param_2);
    const int32_t  planet = *v.planet_index;
    const bool     is_local_player =
        static_cast<int32_t>(player) == static_cast<int32_t>(*v.player_side);

    // ---- 0x004795fa-0x00479663: the "escalation bonus", local player only ---------------------------
    int32_t escalation_bonus = 0;
    bool    did_something    = false;
    // DECLARED NEED 1: sim_view::mother_lost_escalation_interval (no region registered yet).
    if (is_local_player && own.planet_mother_lost_time_at(planet) > 0.0) {
        const int32_t trunc_intervals = trunc_to_int32(
            (*v.game_clock - own.planet_mother_lost_time_at(planet)) / *v.mother_lost_escalation_interval);
        escalation_bonus = trunc_intervals * 5;
        did_something    = true;
    }

    building      &b     = own.building_at(player, bidx);
    const uint16_t state = b.state;

    switch (state) {
        case BLDG_STATE_CONSTRUCTION: {
            // 0x004796eb-0x00479724: local-player-only "planet invention already acquired" bump.
            if (is_local_player &&
                progress_of(v, player, v.cfg_planets[planet].invention_index).acquired == 1) {
                own.bldg_completion_accum_mut() += 10;
                did_something = true;
            }

            // 0x004797d2-0x0047980c: advance to the building TYPE's next-state table entry (LOW WORD).
            b.state = static_cast<uint16_t>(
                static_cast<uint32_t>(v.cfg_buildings[b.building_id].state_transition_ids[1]) & 0xffffu);

            // 0x00479813-0x00479867: x/y read unconditionally; only the MOTHER arm below uses them.
            const uint8_t bx = b.x;
            const uint8_t by = b.y;

            const cfg_building &cb0 = v.cfg_buildings[b.building_id];
            switch (cb0.type) {
                case BLDG_TYPE_A_MINE:
                case BLDG_TYPE_H_MINE:
                    for (int32_t i = 0; i < 4; ++i) {
                        c.mine_scan_deposit_slot(static_cast<uint8_t>(i), player, bidx);
                    }
                    break;
                case BLDG_TYPE_A_MOTHER:
                case BLDG_TYPE_H_MOTHER: {
                    player_profile &prof = own.profile_at(player);
                    if (prof.mother_established == 0 || !is_local_player) {
                        if (prof.status_flags & STRAT_PLAYER_STATUS_AI_CONTROLLED) {
                            const char *race_str = (prof.race == RACE_ALIEN) ? "A" : "H";
                            // DECLARED NEED 7 (dmp_path_scratch) + DECLARED NEED 8 (utils_sprintf__vssii).
                            c.utils_sprintf__vssii(own.dmp_path_scratch(), DMP_PATH_FORMAT, DMP_PATH_INIT, race_str,
                                                   planet, prof.landing_spot_index[planet]);
                            c.load_base_layout_dmp(player, own.dmp_path_scratch());
                        }
                        prof.mother_established = 1;
                        if (cb0.human_transport != 0) {
                            c.population_add(player, cb0.human_transport);
                        }
                        // 0x004799eb-0x00479a3c: capacity[1..9], NOT [0].
                        for (int32_t i = 1; i < 10; ++i) {
                            c.resource_add(player, i, cb0.capacity[i]);
                        }
                    }
                    if (prof.primary_mother_bldg[planet] == 0) {
                        prof.primary_mother_bldg[planet] = bidx;
                    }
                    if (is_local_player) {
                        // DECLARED NEED 5 (ui_base_marker_coords_at). Torus-wrapped by `general`'s masks
                        // (map_width_mask/map_height_mask -- the SAME `general` struct sim_bldg_footprint_
                        // set_passable.cpp cross-checks by address).
                        auto &marker = own.ui_base_marker_coords_at(0);
                        marker.cam_col =
                            static_cast<int32_t>((bx + cb0.width / 2) & static_cast<int32_t>(map_width_mask(v)));
                        marker.cam_row =
                            static_cast<int32_t>((by + cb0.height / 2) & static_cast<int32_t>(map_height_mask(v)));
                    }
                    break;
                }
                default:
                    break;
            }

            worker_mgmt(v, own, c, player, bidx);

            // 0x00479cbf-0x00479d2e
            if (progress_of(v, player, v.cfg_buildings[b.building_id].invention).acquired != 1) {
                c.game_UpdateProgress(player, static_cast<uint16_t>(v.cfg_buildings[b.building_id].invention));
            }

            // 0x00479d2e-0x00479d4a -- see the header banner's REGISTER_ONLINE'S PARAM_3/PARAM_4 section.
            c.bldg_register_online(static_cast<int16_t>(player), bidx, REGISTER_ONLINE_PARAM3_UNRESOLVED,
                                   REGISTER_ONLINE_PARAM4_UNRESOLVED, param_5);
            c.bldg_link_to_network_if_adjacent(player, bidx);

            // 0x00479d6f-0x00479da1
            if (v.cfg_buildings[b.building_id].type == BLDG_TYPE_H_PORT ||
                v.cfg_buildings[b.building_id].type == BLDG_TYPE_A_PORT) {
                c.prod_deliver_arrivals();
            }

            // 0x00479da6-0x00479dd8: other-player only.
            if (!is_local_player) {
                // DECLARED NEED 4 (foreign_bldg_change_flag_mut).
                if (own.foreign_bldg_event_pending() != 0) {
                    own.foreign_bldg_change_flag_mut() = 1;
                    own.foreign_bldg_event_pending()   = 0;
                }
            }
            break;
        }

        case BLDG_STATE_CHARGE_GATE:
            worker_mgmt(v, own, c, player, bidx);
            break;

        case BLDG_STATE_PROD_WORKING: {
            const uint8_t  sub_id           = b.sub_id;
            const uint32_t active_unit_type = v.productions[player * v.caps.productions + sub_id].active_unit_type;
            const uint16_t bid_for_text     = b.building_id; // read once, used for the housing message below

            // 0x0047a294-0x0047a2c1: dispatch on Unit[active_unit_type].move_op_code == 0xf.
            int32_t spawned;
            if (v.cfg_units[active_unit_type].move_op_code == 0xf) {
                spawned = c.unit_spawn_docked(static_cast<uint16_t>(active_unit_type), player, 0);
            } else {
                spawned = c.unit_add_docked(active_unit_type, player, 0);
            }

            if (spawned == 0) {
                if (is_local_player) {
                    const int32_t group_idx   = c.unit_type_group_index(static_cast<int32_t>(active_unit_type));
                    const int32_t housing_bid = c.reason_to_housing_bldg(static_cast<uint32_t>(group_idx), *v.player_race);
                    if (housing_bid < 1) {
                        c.w_sprintf__vss(own.text_scratch(), TEXT_FMT_NAME_REASON,
                                         v.text_ptrs[v.cfg_buildings[bid_for_text].id], v.text_ptrs[group_idx]);
                    } else {
                        c.w_sprintf__vsss(own.text_scratch(), TEXT_FMT_NAME_REASON_HOUSING,
                                          v.text_ptrs[v.cfg_buildings[bid_for_text].id], v.text_ptrs[group_idx],
                                          v.text_ptrs[v.cfg_buildings[housing_bid].id]);
                    }
                    c.game_ui_PrintTextMessage(own.text_scratch());
                }
                c.unit_apply_production_completion(player, static_cast<int32_t>(active_unit_type));
                c.ai_notify_unit_lifecycle(player, static_cast<uint16_t>(active_unit_type), 0, 2);
            } else {
                c.ai_notify_unit_lifecycle(player, static_cast<uint16_t>(active_unit_type),
                                           static_cast<uint32_t>(spawned), 1);
            }

            // ---- 0x0047a732-0x0047a773: local-player-only accum += 1. CORRECTED 2026-08-22
            // (reimpl-verify): this block is UNCONDITIONAL across both the spawned==0 and spawned!=0
            // branches above -- all four sub-paths (spawn_docked/add_docked x success/failure) converge
            // on the same LAB_0047a732 footer (traced: 0x0047a356 JMP->0x0047a4fc JMP->0x0047a732;
            // 0x0047a4fc fallthrough->0x0047a732; 0x0047a58c JMP->0x0047a732; 0x0047a72d falls straight
            // into 0x0047a732). The prior placement (inside `if (spawned == 0)` only) was wrong -- moved
            // out here to run after both branches. The OTHER-player branch at 0x0047a775-0x0047a7b1 is
            // the DEAD SUBTREE the header banner documents (an IDIV whose result is never stored/branched
            // on) -- still omitted.
            if (is_local_player &&
                progress_of(v, player, v.cfg_planets[planet].invention_index).acquired == 1) {
                own.bldg_completion_accum_mut() += 1;
                did_something = true;
            }
            break;
        }

        case BLDG_STATE_MINE_EXTRACTING: {
            const uint8_t sub_id = b.sub_id;
            // DECLARED NEED 2: sim_view::bldg_completion_slot_count (no region registered yet).
            const int32_t slot_count = static_cast<int32_t>(*v.bldg_completion_slot_count);
            for (int32_t i = 0; i < slot_count; ++i) {
                // DECLARED NEED 6: llm_mine_deposit_slot (mines[].deposit_slot is currently raw bytes).
                const auto &slot = v.mines[player * v.caps.mines + sub_id].deposit_slot[i];
                if (slot.resource_id == 0) continue;

                const int32_t cx     = slot.tile_x_q4;
                const int32_t cy     = slot.tile_y_q4;
                int32_t       amount = static_cast<int32_t>(slot.extract_rate);

                // DECLARED NEED 3: sim_view::resources / sim_store::resources_at (the [64][64] plane).
                // CORRECTED 2026-08-22 (reimpl-verify): the original ZERO-extends cell.value[resource_id]
                // (MOVZX at 0x0047a898/0x0047a8ba) -- cast through uint16_t before widening to int32_t, or
                // a deposit value with its high bit set reads as negative and silently skips
                // resource_add/depletion. Same fix already established for the same field in
                // ai_site_worth.cpp.
                const map_resources &cell              = v.resources[cx * 64 + cy];
                const int32_t        deposit_u         = static_cast<int32_t>(static_cast<uint16_t>(cell.value[slot.resource_id]));
                const bool           capped_by_deposit = deposit_u <= amount;
                if (capped_by_deposit) amount = deposit_u;

                const int32_t cap_room = v.storage_stats[player].cap_prev[slot.resource_id] -
                                         player_resource_of(v, player, slot.resource_id);
                const bool capped_by_storage = cap_room < amount;
                if (capped_by_storage) amount = cap_room;

                if (amount > 0) {
                    c.resource_add(player, slot.resource_id, amount);
                    map_resources &cell_mut = own.resources_at(cx, cy);
                    cell_mut.value[slot.resource_id] =
                        static_cast<int16_t>(cell_mut.value[slot.resource_id] - amount);
                    if (cell_mut.value[slot.resource_id] == 0) {
                        // Reset-then-OR-merge (0x0047a982-0x0047a9e4), literal form.
                        cell_mut.value[0] = 0;
                        for (int32_t k = 1; k < 8; ++k) {
                            cell_mut.value[0] = static_cast<int16_t>(cell_mut.value[0] | cell_mut.value[k]);
                        }
                    }
                }
                if (capped_by_storage || capped_by_deposit) {
                    c.mine_scan_deposit_slot(static_cast<uint8_t>(i), player, bidx);
                }
            }
            break;
        }

        case BLDG_STATE_UPGRADING: {
            const cfg_building &cb_before  = v.cfg_buildings[b.building_id];
            const cfg_building &cb_upgrade = v.cfg_buildings[cb_before.upgrade_index];
            // FLD energy; FMUL upgrade.energy; FDIV before.energy -- order confirmed, not
            // algebraically rearranged.
            b.energy      = (b.energy * cb_upgrade.energy) / cb_before.energy;
            b.building_id = static_cast<uint16_t>(cb_before.upgrade_index);

            if (progress_of(v, player, v.cfg_buildings[b.building_id].invention).acquired != 1) {
                c.game_UpdateProgress(player, static_cast<uint16_t>(v.cfg_buildings[b.building_id].invention));
            }
            worker_mgmt(v, own, c, player, bidx);
            break;
        }

        case BLDG_STATE_RESEARCHING: {
            // 0x0047aa0d-0x0047aa51: local-player-only accum += 15 (NOT the same delta as the other arms).
            if (is_local_player &&
                progress_of(v, player, v.cfg_planets[planet].invention_index).acquired == 1) {
                own.bldg_completion_accum_mut() += 15;
                did_something = true;
            }
            const uint8_t sub_id     = b.sub_id;
            const int32_t project_id = v.labs[player * v.caps.labs + sub_id].active_project_id;
            c.game_UpdateProgress(player, static_cast<uint16_t>(v.cfg_projects[project_id].invention));
            break;
        }

        default:
            break;
    }

    // ---- 0x0047aa94-0x0047ab15: the shared footer -----------------------------------------------------
    if (did_something) {
        const int32_t total = own.bldg_completion_accum_mut() + escalation_bonus;
        if (total >= 0x46 && *v.tutorial_step == 0 && *v.session_mode == SESSION_MODE_SINGLE_PLAYER) {
            const int32_t roll = c.invasion_chance_roll(1);
            if (roll == 0) {
                // (accum - (accum>>31))>>1 -- PROVEN bit-exact to accum/2 (round toward zero).
                const int32_t accum             = own.bldg_completion_accum_mut();
                own.bldg_completion_accum_mut() = (accum - (accum >> 31)) >> 1;
            } else {
                // SBB-based /4 idiom, truncating toward zero -- CORRECTED 2026-08-22 (reimpl-verify):
                // 0x0047aad2-0x0047aae3 is EDX=SAR(accum,31); SHL EDX,2 (EDX = 0 or -4); SBB EAX,EDX,
                // i.e. accum - EDX - CF. CF after that SHL is bit30 of the pre-shift EDX (well-defined
                // for a 1-31 shift count), which is 1 iff accum<0 -- so the original computes
                // accum-(-4)-1 = accum+3 for negative accum, NOT accum+4. The prior draft's
                // `(accum - sign4) >> 2` with sign4 = 0 or -4 was off by one for negative accum
                // (added 4, not 3). Reproduced as the equivalent bias-then-shift form.
                const int32_t accum             = own.bldg_completion_accum_mut();
                const int32_t bias              = (accum >> 31) & 3; // 0, or 3 when accum < 0
                own.bldg_completion_accum_mut() = (accum + bias) >> 2;
            }
            own.planet_mother_lost_time_at(planet) = *v.game_clock;
        }
    }
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void bldg_completion_dispatch(uint32_t param_1, uint32_t param_2, uint32_t param_3, uint32_t param_4,
                              double param_5) {
    sim_state st = state();
    detail::bldg_completion_dispatch(st.read, st.own, live_bldg_completion_dispatch_calls(), param_1, param_2,
                                     param_3, param_4, param_5);
}


} // namespace mh::sim
