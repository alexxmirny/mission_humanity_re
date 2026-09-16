//
// orders/issue/issue_state.cpp -- the ORDER-ISSUE domain's PRODUCTION bindings (RI-ORDERS / O4-0).
//
// The wrappers themselves are pure functions of (view, sink, arguments) and live in their own
// translation units, one per family. This file is the only place that says where the view's regions
// actually are and what the sink actually calls -- which is why it is conductor-owned and a
// translation writer never edits it.
//
// A wrapper's TU includes only issue_state.h. It therefore cannot spell an address, cannot reach
// mh::state, and cannot call the container except through the sink -- so the oracle's recording sink
// really does see everything the wrapper emits. That containment is the property the whole domain's
// verifiability rests on; keep it by keeping the bindings here.
//
#include "orders/issue/issue_state.h"

#include "addr/mh_calls.gen.h"
#include "addr/mh_regions.gen.h"
#include "orders/order_queue.h"

#include "orders/issue/issue_bldg_depart.h"
#include "orders/issue/issue_promote.h" // O4-P: promotion_active(), consulted by install_shadow
#include "orders/issue/issue_bldg_footprint.h"
#include "orders/issue/issue_bldg_orders_b.h"
#include "orders/issue/issue_group_orders.h"
#include "orders/issue/issue_order_admin.h"
#include "orders/issue/issue_order_debug.h"
#include "orders/issue/issue_order_player.h"
#include "orders/issue/issue_turret_orders.h"
#include "orders/issue/issue_unit_attack.h"
#include "orders/issue/issue_unit_move.h"
#include "orders/issue/issue_unit_storage.h"
#include "state/host_api.h"
#include "state/host_events.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
#include "crt/crt_select.h" // LIB-CRT: MH_CRT() picks the vendored CRT in the standalone build

namespace mh::orders::issue {

const issue_view &live_view() {
    // Addresses come from the state region REGISTRY, never spelled here -- the same rule, for the
    // same reason, as mh::orders::state() (RI-STATE / ST1: one derivation per region, or two
    // consumers of the same bytes can disagree about where they are).
    //
    // GROWN TO THE WHOLE DOMAIN BY THE O4A-C SWEEP (2026-08-28). Written as member ASSIGNMENTS, not
    // as a brace list, for two independent reasons that happen to agree. (1) The view is 23 members
    // of which nine are pointer-to-scalar, so a POSITIONAL brace list would let two same-typed
    // neighbours be swapped with no diagnostic at all -- exactly what `session_mode` / `planet_index`
    // / `player_race` (three `const int32_t *` in a row) invite. (2) This is the spelling
    // the translation lint's parse_view_bindings() reads (`v.X = ptr<...>(RID_...)`, the
    // sim_state.cpp convention); a designated-initializer list is equally unswappable to a human and
    // INVISIBLE to that parser, which silently drops every member out of the member->region map and
    // then reports the region MISSING in every translation that uses it. Measured here: 23 members
    // bound, 0 resolved, and the lint blamed the translations.
    using namespace mh::state;
    static issue_view v     = {};
    static const bool bound = [] {
        v.buildings     = ptr<mh::game::mh_map_object_building>(RID_BUILDINGS);
        v.cfg_buildings = ptr<const mh::game::mh_cfg_final_struct_Building>(RID_BUILDING);

        v.units        = ptr<mh::game::mh_map_object_unit>(RID_UNITS); // WRITTEN by one row -- see the header
        v.caps         = live_roster_caps();                           // SB-BIND T2: derived row capacities
        v.cfg_units    = ptr<const mh::game::mh_cfg_final_struct_Unit>(RID_UNIT);
        v.unit_storage = ptr<const mh::game::mh_map_object_unit_storage>(RID_UNIT_STORAGE);
        v.geom         = ptr<const mh::game::mh_llm_strat_map_geom>(RID_GENERAL);
        v.ctrl_groups  = ptr<const mh::game::mh_llm_strat_ctrl_group>(RID_STRAT_CTRL_GROUPS);

        v.player_side   = ptr<const uint16_t>(RID_PLAYERSIDE);
        v.session_mode  = ptr<const int32_t>(RID_GAME_SESSION_MODE);
        v.planet_index  = ptr<const int32_t>(RID_G_PLANET_INDEX);
        v.player_race   = ptr<const int32_t>(RID_STRAT_PLAYER_RACE);
        v.game_clock    = ptr<const double>(RID_STRAT_GAME_CLOCK);
        v.key_lalt_held = ptr<const uint8_t>(RID_KEY_LALT_HELD);

        v.ai_group_scratch_list  = ptr<const int32_t>(RID_STRAT_AI_GROUP_UNIT_SCRATCH_LIST);
        v.ai_group_scratch_count = ptr<const int32_t>(RID_STRAT_AI_GROUP_UNIT_SCRATCH_COUNT);

        v.passable               = ptr<const uint8_t>(RID_PASSABLE);
        v.ai_tile_spiral_offsets = ptr<const int8_t>(RID_STRAT_AI_TILE_SPIRAL_OFFSETS);
        v.width_m                = ptr<const uint32_t>(RID_WIDTH_M);
        v.height_m               = ptr<const uint32_t>(RID_HEIGHT_M);

        v.ack_voice_cooldown_sec   = ptr<const double>(RID_STRAT_ORDER_ACK_VOICE_COOLDOWN_SEC);
        v.ack_voice_snd_id_by_race = ptr<const int32_t>(RID_STRAT_ORDER_ACK_VOICE_SND_ID_BY_RACE);

        v.fmt_control_mode_clear = ptr<const wchar_t>(RID_U___D___D_005011CC);
        v.fmt_control_mode_set   = ptr<const wchar_t>(RID_U___D___D_00501250);
        v.text_tmp               = ptr<void>(RID_G_TEXT_TMP);

        v.order_seq_id_by_player = ptr<uint8_t>(RID_STRAT_ORDER_SEQ_ID_BY_PLAYER);
        v.player_control_mask    = ptr<uint8_t>(RID_PLAYER_CONTROL_MASK);

        v.ui_depart_pending_bldg_a    = ptr<int32_t>(RID_STRAT_UI_DEPART_PENDING_BLDG_A);
        v.ui_depart_pending_bldg_b    = ptr<int32_t>(RID_STRAT_UI_DEPART_PENDING_BLDG_B);
        v.ui_planet_sel_action_target = ptr<int32_t>(RID_STRAT_UI_PLANET_SEL_ACTION_TARGET);

        v.ack_voice_last_play_time   = ptr<double>(RID_STRAT_ORDER_ACK_VOICE_LAST_PLAY_TIME);
        v.ack_voice_suppressed_count = ptr<int32_t>(RID_STRAT_ORDER_ACK_VOICE_SUPPRESSED_COUNT);
        return true;
    }();
    (void)bound;
    return v;
}

// OURS, not the originals: the container is promoted (O3), so these bind straight to mh::orders::*.
// That is also why the sink is the only indirection this layer needs -- there is no marshalling
// thunk left in the path.
const order_sink &live_sink() {
    static const order_sink s = {
        []() { mh::orders::scratch_reset(); },
        [](int32_t i, int32_t val) { mh::orders::scratch_set_field(i, val); },
        [](uint16_t unit_id, uint32_t player, uint16_t op_code, uint16_t arg) -> int32_t {
            return mh::orders::dispatch(unit_id, player, op_code, arg);
        },
        [](uint16_t unit_index, uint16_t owner_and_kind, int16_t param0,
           uint16_t order_code) -> int32_t {
            return mh::orders::enqueue(unit_index, owner_and_kind, param0, order_code);
        },
    };
    return s;
}


// ---- the outward calls ---------------------------------------------------------------------------
//
// Bound to the generated marshalling thunks (addr/mh_calls.gen.h), which carry each original's real
// register contract. Filled out by the O4A-C sweep; it was `{0}` while only the pilot existed.
const issue_calls &live_calls() {
    // Bound DIRECTLY to the generated thunks -- every member's signature was written from the thunk
    // it binds, so no adapter lambda is needed and none should be added: the translation lint
    // pairs `issue_calls`' members against this initializer POSITIONALLY, and a lambda body hides the
    // `mh::call::` name the pairing is read from. Designated initializers on top of that, so the
    // member each line binds is visible at the line rather than inferred from counting.
    static const issue_calls c = {
        .snd_play = mh::state::evt::snd_play,

        .rand_below_fx                    = MH_LIBMH_BIND(llm_rand_below_fx),
        .target_class                     = MH_LIBMH_BIND(llm_strat_target_class),
        .unit_state_is_boarding           = MH_LIBMH_BIND(llm_unit_state_is_boarding),
        .unit_in_weapon_range             = MH_LIBMH_BIND(llm_strat_unit_in_weapon_range),
        .unit_select_weapon               = MH_LIBMH_BIND(llm_strat_unit_select_weapon),
        .storage_type_accepts_unit        = MH_LIBMH_BIND(llm_strat_storage_type_accepts_unit),
        .bldg_placement_check_and_preview = MH_LIBMH_BIND(llm_bldg_placement_check_and_preview),

        .unit_get_coords           = MH_LIBMH_BIND(llm_strat_unit_get_coords),
        .bldg_get_coords           = MH_LIBMH_BIND(llm_strat_bldg_get_coords),
        .storage_get_approach_tile = MH_LIBMH_BIND(llm_strat_storage_get_approach_tile),
        .bldg_calc_placement_corner_from_center =
            MH_LIBMH_BIND(llm_bldg_calc_placement_corner_from_center),
        .tile_delta_wrapped = MH_LIBMH_BIND(llm_strat_tile_delta_wrapped),

        .unit_notify_status      = MH_LIBMH_BIND(llm_strat_unit_notify_status),
        .unit_order_move_enqueue = MH_LIBMH_BIND(llm_strat_unit_order_move_enqueue),

        .sprintf_ii = &MH_CRT(w_sprintf__vii),
    };
    return c;
}

// WHAT `inert` MEANS HERE, and what it deliberately does NOT mean.
//
// A shadow arm runs the body TWICE. Restoring state does not un-play a sound, so the ONE member the
// arm must silence is `snd_play` -- the domain's only `effectful` target
// (tools/data/orders_issue_effect_classes.json). Everything else is a pure query or an out-pointer
// helper: running those a second time is idempotent, and STUBBING them would be actively worse than
// not stubbing, because a query forced to answer 0 sends our arm down a branch the original's arm
// never took, and the comparison then reads clean for the wrong reason (the sim domain's own
// "a suppression built only from the positive cases passes its own test vacuously").
//
// `unit_notify_status` is the one judgement call. It writes sim state through the original, so under
// a shadow arm it is covered by the snapshot/restore rather than by suppression -- a site that arms a
// body reaching it MUST declare the regions it touches in dll_shadow_manifest.json, exactly as the
// container's apply_and_dequeue site does for the same callee. Left LIVE here on purpose; a site that
// cannot declare those regions is un-armable and belongs on the offline path instead.
const issue_calls &inert_calls() {
    static issue_calls c       = live_calls();
    static const bool  patched = [] {
        c.snd_play = [](int32_t, int32_t) {};
        return true;
    }();
    (void)patched;
    return c;
}

} // namespace mh::orders::issue
