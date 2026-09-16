//
// orders/issue/issue_group_orders.cpp -- the control-group move/ack-voice wrappers (RI-ORDERS / O4-0,
// O4A-C sweep, batch B unit `group_orders`). See issue_group_orders.h for the per-function spec; this
// file is the transcription.
//
// DERIVED FROM THE .asm, not the .c drafts (all three .c drafts read straightforwardly here, but the
// .asm is still what every branch/store below cites). Addresses are EN /eng/mh.exe.
//
#include "orders/issue/issue_group_orders.h"

namespace mh::orders::issue {

namespace {

// v.units[*v.player_side][unit_id] -- same shape as issue_bldg_orders.cpp's bldg_at, but for the LOCAL
// player only (these three functions never take a player parameter; they always read v.player_side).
const mh::game::mh_map_object_unit &local_unit_of(const issue_view &v, uint32_t unit_id) {
    return v.units[(int32_t)*v.player_side * v.caps.units + (int32_t)unit_id];
}

const mh::game::mh_cfg_final_struct_Unit &cfg_of(const issue_view                   &v,
                                                 const mh::game::mh_map_object_unit &u) {
    return v.cfg_units[u.unit_proto_id];
}

// cfg_enum_E_UNIT_TYPE members (Ghidra enum, see docs/structs.md's cfg_final_struct_Unit.type row).
// Defined locally rather than shared: this domain's unit headers may include only issue_state.h plus
// a named sibling (the containment rule issue_state.h documents), so the sim/ai domains' own
// UNIT_TYPE_A_HELI_CARGO/H_HELI_CARGO (sim/sim_order_enqueue.h, ai/ai_state.h) are not reachable from
// here -- same values, same enum, independently named per the domain-local convention.
inline constexpr uint32_t UNIT_TYPE_A_HELI_CARGO = 0x17;
inline constexpr uint32_t UNIT_TYPE_H_HELI_CARGO = 0x18;

bool is_cargo_heli(uint32_t type) {
    return type == UNIT_TYPE_A_HELI_CARGO || type == UNIT_TYPE_H_HELI_CARGO;
}

// The shared increment-skip-zero stamp both move issuers apply to v.order_seq_id_by_player at exit:
// bump it, and bump it again if that wrapped to 0 -- 0 is never emitted as a seq id.
void stamp_seq_id(const issue_view &v) {
    uint8_t &seq = v.order_seq_id_by_player[*v.player_side];
    ++seq;
    if (seq == 0) ++seq;
}

} // namespace

// ---- llm_strat_group_order_ack_voice @0x004259b2 -------------------------------------------------
void detail::group_order_ack_voice(const issue_view &v, const order_sink &, const issue_calls &c) {
    // JNC @0x004259df: taken (suppressed) when ORDERED and (last_play+cooldown) >= game_clock. See
    // the header comment for why the plain `>=` form agrees with the hardware on NaN inputs here.
    if (*v.ack_voice_last_play_time + *v.ack_voice_cooldown_sec >= *v.game_clock) {
        const int32_t suppressed      = *v.ack_voice_suppressed_count + 1;
        *v.ack_voice_suppressed_count = suppressed;
        if (suppressed > 5) { // 6th consecutive refusal (CMP ..,5 / JLE 0x00425a64 -- `5 < count`)
            c.snd_play(0x44, 100);
            *v.ack_voice_suppressed_count = 0;
        }
    } else {
        const uint32_t race_bucket = (*v.player_race == 2) ? 3u : 0u;
        const uint32_t roll        = c.rand_below_fx(3);
        c.snd_play(v.ack_voice_snd_id_by_race[roll + race_bucket], 100);
        *v.ack_voice_last_play_time   = *v.game_clock;
        *v.ack_voice_suppressed_count = 0;
    }
}

// ---- llm_strat_group_issue_move_order_deferred @0x00444e5f ---------------------------------------
void detail::group_issue_move_order_deferred(const issue_view &v, const order_sink &s,
                                             const issue_calls &c, int32_t op_code, int32_t op_arg,
                                             int32_t modifier) {
    bool    did_move = false;
    int32_t mod      = modifier;
    if (modifier != 0) {
        mod = modifier | (int32_t)v.order_seq_id_by_player[*v.player_side];
    }

    const int32_t count = v.ctrl_groups[0].count;
    for (int32_t i = 0; i < count; ++i) {
        const uint32_t unit_id = v.ctrl_groups[0].unit_ids[i];
        const uint32_t type    = cfg_of(v, local_unit_of(v, unit_id)).type;
        if (is_cargo_heli(type)) continue;

        if ((*v.key_lalt_held & 1) != 0 || (*v.key_lalt_held & 2) != 0) {
            unit_order_move_default(v, s, c, (uint32_t)*v.player_side, unit_id, op_code, op_arg, mod);
        } else {
            unit_order_move(v, s, c, (uint32_t)*v.player_side, unit_id, op_code, op_arg, mod);
        }
        did_move = true;
    }

    // Write order: ack-voice THEN seq-stamp (the opposite order from `_confirmed` below).
    if (did_move) {
        group_order_ack_voice(v, s, c);
    }
    if (mod != 0) {
        stamp_seq_id(v);
    }
}

// ---- llm_strat_group_issue_move_order_confirmed @0x00444fcb --------------------------------------
void detail::group_issue_move_order_confirmed(const issue_view &v, const order_sink &s,
                                              const issue_calls &c, int32_t dst_x, int32_t dst_y) {
    bool    did_move = false;
    int32_t mod      = 0; // only ever set inside the move-with-flags arm below

    const int32_t count = v.ctrl_groups[0].count;
    for (int32_t i = 0; i < count; ++i) {
        const uint32_t                            unit_id = v.ctrl_groups[0].unit_ids[i];
        const mh::game::mh_cfg_final_struct_Unit &cfg     = cfg_of(v, local_unit_of(v, unit_id));
        if (is_cargo_heli(cfg.type)) continue;

        if (cfg.equivalent == 0 || (int32_t)cfg.type < 0xf) {
            // OR AH,0x40 on the zero-extended seq byte -- CONCAT11(0x40, seq) == (0x40<<8) | seq.
            mod = (int32_t)((0x40u << 8) | v.order_seq_id_by_player[*v.player_side]);
            unit_order_move(v, s, c, (uint32_t)*v.player_side, unit_id, dst_x, dst_y, mod);
        } else {
            unit_order_move_confirmed_with_bump(v, s, c, (uint32_t)*v.player_side, unit_id, dst_x,
                                                dst_y);
        }
        did_move = true;
    }

    // Write order: seq-stamp THEN ack-voice (the opposite order from `_deferred` above).
    if (mod != 0) {
        stamp_seq_id(v);
    }
    if (did_move) {
        group_order_ack_voice(v, s, c);
    }
}

// ---- the public forms ---------------------------------------------------------------------------

void group_order_ack_voice() {
    detail::group_order_ack_voice(live_view(), live_sink(), live_calls());
}
void group_issue_move_order_deferred(int32_t op_code, int32_t op_arg, int32_t modifier) {
    detail::group_issue_move_order_deferred(live_view(), live_sink(), live_calls(), op_code, op_arg,
                                            modifier);
}
void group_issue_move_order_confirmed(int32_t dst_x, int32_t dst_y) {
    detail::group_issue_move_order_confirmed(live_view(), live_sink(), live_calls(), dst_x, dst_y);
}

} // namespace mh::orders::issue
