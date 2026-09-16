//
// sim/sim_bldg_register_online.cpp -- see sim_bldg_register_online.h. Translated from the DISASSEMBLY
// (tmp/decomp_sim/llm_strat_bldg_register_online_004747d6.asm, _online_default_004749ed.asm,
// _online_barracks_garage_a_00474bfc.asm, _online_barracks_h_00474d4a.asm,
// _online_garage_h_00474f33.asm, _online_helipad_a_004750e5.asm).
//
#include "sim/sim_bldg_register_online.h"

#include "addr/mh_calls.gen.h"  // typed callables for the 7 out-of-scope original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const register_online_calls &live_register_online_calls() {
    static const register_online_calls c = {
        MH_LIBMH_BIND(llm_strat_bldg_online_helipad_h_or_misc),
        MH_LIBMH_BIND(llm_strat_bldg_online_airfield_a),
        MH_LIBMH_BIND(llm_strat_bldg_online_airfield_h),
        MH_LIBMH_BIND(llm_strat_bldg_online_shuttle_a),
        MH_LIBMH_BIND(llm_strat_bldg_online_shuttle_h),
        MH_LIBMH_BIND(llm_strat_bldg_online_port_a),
        MH_LIBMH_BIND(llm_strat_bldg_online_port_h),
    };
    return c;
}

namespace {

// ---- little-endian 4-byte pack/unpack over a raw uint8_t[] span --------------------------------
// SAME pattern sim_bldg_anim_trigger.cpp / sim_bldg_liftoff_anim.cpp / sim_map_create_building.cpp
// establish for the identical pair of flattened fields (mh_map_object_building::anim[48] /
// mh_cfg_final_struct_Building::anim[48], both really cfg_t_frame_index[12]) -- duplicated per-TU
// rather than shared, same precedent every other sim/ TU with this helper follows.
inline void store_u32_le(uint8_t *dst, uint32_t value) {
    dst[0] = (uint8_t)(value);
    dst[1] = (uint8_t)(value >> 8);
    dst[2] = (uint8_t)(value >> 16);
    dst[3] = (uint8_t)(value >> 24);
}
inline uint32_t load_u32_le(const uint8_t *src) {
    return (uint32_t)src[0] | ((uint32_t)src[1] << 8) | ((uint32_t)src[2] << 16) |
           ((uint32_t)src[3] << 24);
}

// ---- register_online's own dispatch-boundary constants -------------------------------------------
// INFERRED from the handler function's own name, NOT from a shipped string or an existing Ghidra
// enum (see the header banner's UNCERTAINTIES note) -- cfg_enum_E_BUILDING has no member list in
// this image's exported metadata. BLDG_TYPE_A_PORT/BLDG_TYPE_H_PORT at the SAME two values (0xc,
// 0x20) are already independently named in sim_bldg_completion_dispatch.cpp, which corroborates the
// scheme without proving these five.
inline constexpr uint8_t BLDG_TYPE_A_BARRACKS = 0x7;         // online_barracks_garage_a's low end
inline constexpr uint8_t BLDG_TYPE_A_GARAGE   = 0x8;         // online_barracks_garage_a's high end
inline constexpr uint8_t BLDG_TYPE_A_HELIPAD  = 0xa;         // online_helipad_a
inline constexpr uint8_t BLDG_TYPE_A_PORT     = 0xc;         // online_port_a (out of scope)
inline constexpr uint8_t BLDG_TYPE_A_SHUTTLE  = 0xd;         // online_shuttle_a (out of scope)
inline constexpr uint8_t BLDG_TYPE_H_BARRACKS = 0x1b;        // -> online_vehicles_h. The cfg
                                                             // enum literal LIES: 0x1b is the
                                                             // VEHICLE quarters (in-game
                                                             // "GARAGE"). Kept spelled
                                                             // H_BARRACKS because it mirrors a
                                                             // real cfg key; the handler was
                                                             // renamed 2026-08-24, finding
                                                             // 2026-08-23-0521-2.
inline constexpr uint8_t BLDG_TYPE_H_GARAGE = 0x1c;          // -> online_soldiers_h. Same
                                                             // inversion: 0x1c is the SOLDIER
                                                             // quarters (in-game "BARRACKS").
inline constexpr uint8_t BLDG_TYPE_H_HELIPAD_OR_MISC = 0x1e; // online_helipad_h_or_misc (out of scope)
inline constexpr uint8_t BLDG_TYPE_H_PORT            = 0x20; // online_port_h (out of scope)
inline constexpr uint8_t BLDG_TYPE_H_SHUTTLE         = 0x21; // online_shuttle_h (out of scope)

// online_state's two values across this family -- no backing Ghidra enum (same posture
// sim_bldg_anim_trigger.cpp's own ANIM_TRIGGER_ONLINE_STATE / sim_bldg_liftoff_anim.cpp's
// LIFTOFF_ONLINE_STATE document). `_DEFAULT` covers online_default/barracks_garage_a/vehicles_h/
// soldiers_h; `_HELIPAD` is the online_helipad_a outlier (see the header banner).
inline constexpr int16_t BLDG_REGISTER_ONLINE_STATE_DEFAULT = 1;
inline constexpr int16_t BLDG_REGISTER_ONLINE_STATE_HELIPAD = 2;

} // namespace

namespace detail {

void bldg_online_default(const sim_view &v, sim_store &own, int32_t player, int32_t building_index,
                         uint32_t param_3, uint32_t param_4, double anim_dur) {
    // param_3(EBX)/param_4(ECX) are DEAD -- see the header banner.
    (void)param_3;
    (void)param_4;

    building &b = own.building_at(static_cast<uint32_t>(player), building_index);

    // 0x00474a1a-0x00474a23: online_state = 1, UNCONDITIONAL, BEFORE the loop (unlike the other four
    // handlers below, which all write it LAST).
    b.online_state = BLDG_REGISTER_ONLINE_STATE_DEFAULT;

    // 0x00474a2a-0x00474ad3: for (i = 0; i < count; ++i) { anim[i] = cfg.anim[i+1]; anim_dur[i] =
    // anim_dur; }. count = cfg_buildings[building_id].sprite_quantity (0x00474a47, MOVZX byte, so
    // always in [0,255] -- the CMP/JG loop-continue test is faithfully a plain `i < count`). The
    // asm re-reads building_id and re-derives the cfg row address on every iteration
    // (0x00474a2a-0x00474a41); this translation reads `building_id` and binds `cfg` ONCE before the
    // loop instead, which is safe because nothing in this closure writes building_id between the
    // read and the loop's end (see translator-brief rule 16 -- this is not a cache of shared MUTABLE
    // state, cfg_buildings is boot-loaded and read-only). The SOURCE index is `i+1`, one slot AHEAD
    // of the destination index `i` -- confirmed twice independently (the cfg-array field-offset
    // arithmetic lands at 0x153, which is cfg.anim's byte offset 4 = slot 1, for i=0) and cross-
    // checked against the destination address arithmetic landing at building.anim's byte offset 0
    // = slot 0 for the same i=0.
    const cfg_building &cfg   = v.cfg_buildings[b.building_id];
    const int32_t       count = cfg.sprite_quantity;
    for (int32_t i = 0; i < count; ++i) {
        store_u32_le(&b.anim[i * 4], load_u32_le(&cfg.anim[(i + 1) * 4]));
        b.anim_dur[i] = anim_dur;
    }
}

void bldg_online_barracks_garage_a(const sim_view &v, sim_store &own, int16_t player,
                                   int32_t building_index, uint32_t param_3, uint32_t param_4,
                                   double anim_dur) {
    (void)param_3;
    (void)param_4;

    const uint32_t      p   = static_cast<uint32_t>(static_cast<uint16_t>(player));
    building           &b   = own.building_at(p, building_index);
    const cfg_building &cfg = v.cfg_buildings[b.building_id];

    // 0x00474c4c-0x00474c52: anim[0] = cfg.anim[2]
    store_u32_le(&b.anim[0 * 4], load_u32_le(&cfg.anim[2 * 4]));
    // 0x00474c6b: anim[1] = -1, a RAW SENTINEL (cfg_t_frame_index "no frame"), NOT a cfg lookup.
    store_u32_le(&b.anim[1 * 4], 0xffffffffu);
    // 0x00474ca8-0x00474cae: anim[2] = cfg.anim[1]
    store_u32_le(&b.anim[2 * 4], load_u32_le(&cfg.anim[1 * 4]));

    // 0x00474cc7-0x00474d1d: anim_dur[0..2] = anim_dur, THREE stamps of the same value.
    b.anim_dur[0] = anim_dur;
    b.anim_dur[1] = anim_dur;
    b.anim_dur[2] = anim_dur;

    // 0x00474d23-0x00474d36: online_state = 1, UNCONDITIONAL, LAST.
    b.online_state = BLDG_REGISTER_ONLINE_STATE_DEFAULT;
}

void bldg_online_vehicles_h(const sim_view &v, sim_store &own, int32_t player, int32_t building_index,
                            uint32_t param_3, uint32_t param_4, double anim_dur) {
    (void)param_3;
    (void)param_4;

    building           &b   = own.building_at(static_cast<uint32_t>(player), building_index);
    const cfg_building &cfg = v.cfg_buildings[b.building_id];

    // 0x00474d94-0x00474d9a: anim[0] = cfg.anim[1]
    store_u32_le(&b.anim[0 * 4], load_u32_le(&cfg.anim[1 * 4]));
    // 0x00474dcd-0x00474dd3: anim[1] = cfg.anim[4]
    store_u32_le(&b.anim[1 * 4], load_u32_le(&cfg.anim[4 * 4]));
    // 0x00474de9: anim[2] = -1, RAW SENTINEL.
    store_u32_le(&b.anim[2 * 4], 0xffffffffu);
    // 0x00474e20-0x00474e26: anim[3] = cfg.anim[6]
    store_u32_le(&b.anim[3 * 4], load_u32_le(&cfg.anim[6 * 4]));
    // 0x00474e59-0x00474e5f: anim[4] = cfg.anim[8]
    store_u32_le(&b.anim[4 * 4], load_u32_le(&cfg.anim[8 * 4]));

    // 0x00474e75-0x00474f09: anim_dur[0..4] = anim_dur, FIVE stamps of the same value.
    b.anim_dur[0] = anim_dur;
    b.anim_dur[1] = anim_dur;
    b.anim_dur[2] = anim_dur;
    b.anim_dur[3] = anim_dur;
    b.anim_dur[4] = anim_dur;

    // 0x00474f0f-0x00474f1f: online_state = 1, UNCONDITIONAL, LAST.
    b.online_state = BLDG_REGISTER_ONLINE_STATE_DEFAULT;
}

void bldg_online_soldiers_h(const sim_view &v, sim_store &own, int16_t player, int32_t building_index,
                            uint32_t param_3, uint32_t param_4, double anim_dur) {
    (void)param_3;
    (void)param_4;

    const uint32_t      p   = static_cast<uint32_t>(static_cast<uint16_t>(player));
    building           &b   = own.building_at(p, building_index);
    const cfg_building &cfg = v.cfg_buildings[b.building_id];

    // 0x00474f83-0x00474f89: anim[0] = cfg.anim[2]
    store_u32_le(&b.anim[0 * 4], load_u32_le(&cfg.anim[2 * 4]));
    // 0x00474fc2-0x00474fc8: anim[1] = cfg.anim[4]
    store_u32_le(&b.anim[1 * 4], load_u32_le(&cfg.anim[4 * 4]));
    // 0x00474fe1: anim[2] = -1, RAW SENTINEL.
    store_u32_le(&b.anim[2 * 4], 0xffffffffu);
    // 0x0047501e-0x00475024: anim[3] = cfg.anim[1]
    store_u32_le(&b.anim[3 * 4], load_u32_le(&cfg.anim[1 * 4]));

    // 0x0047503d-0x004750af: anim_dur[0..3] = anim_dur, FOUR stamps of the same value.
    b.anim_dur[0] = anim_dur;
    b.anim_dur[1] = anim_dur;
    b.anim_dur[2] = anim_dur;
    b.anim_dur[3] = anim_dur;

    // 0x004750c8-0x004750d1: online_state = 1, UNCONDITIONAL, LAST.
    b.online_state = BLDG_REGISTER_ONLINE_STATE_DEFAULT;
}

void bldg_online_helipad_a(const sim_view &v, sim_store &own, int16_t player, int32_t building_index,
                           uint32_t param_3, uint32_t param_4, double anim_dur) {
    (void)param_3;
    (void)param_4;

    const uint32_t      p   = static_cast<uint32_t>(static_cast<uint16_t>(player));
    building           &b   = own.building_at(p, building_index);
    const cfg_building &cfg = v.cfg_buildings[b.building_id];

    // 0x00475135-0x0047513b: anim[0] = cfg.anim[1]
    store_u32_le(&b.anim[0 * 4], load_u32_le(&cfg.anim[1 * 4]));
    // 0x00475154: anim[1] = -1, RAW SENTINEL.
    store_u32_le(&b.anim[1 * 4], 0xffffffffu);
    // 0x00475191-0x00475197: anim[2] = cfg.anim[3]
    store_u32_le(&b.anim[2 * 4], load_u32_le(&cfg.anim[3 * 4]));

    // 0x004751b0-0x00475206: anim_dur[0..2] = anim_dur, THREE stamps of the same value.
    b.anim_dur[0] = anim_dur;
    b.anim_dur[1] = anim_dur;
    b.anim_dur[2] = anim_dur;

    // 0x0047520c-0x0047521f: online_state = 2 -- the OUTLIER (every other function in this file
    // writes 1). Confirmed by decoding the immediate operand bytes directly
    // (`66 c7 80 b7 d2 c3 00 02 00`: disp=0x00c3d2b7, imm16=0x0002), not assumed from the sibling
    // pattern. See the header banner's note on online_state's per-family overload.
    b.online_state = BLDG_REGISTER_ONLINE_STATE_HELIPAD;
}

void bldg_register_online(const sim_view &v, sim_store &own, int16_t player, int32_t building_index,
                          uint32_t param_3, uint32_t param_4, double anim_dur,
                          const register_online_calls &c) {
    // param_3(EBX)/param_4(ECX) are DEAD in THIS function's own body -- see the header banner. They
    // are still forwarded to every callee below (in-scope and out-of-scope alike), matching the
    // committed prototypes exactly.
    (void)param_3;
    (void)param_4;

    const uint32_t  p    = static_cast<uint32_t>(static_cast<uint16_t>(player));
    const building &b    = building_of(v, p, building_index);
    const uint8_t   type = v.cfg_buildings[b.building_id].type;

    // `player32` is the SAME zero-extended value the asm computes fresh before every CALL
    // (`MOVZX EAX, word ptr [spilled player]`) -- used only for the two callees whose OWN committed
    // prototype is `int32_t player` (online_default, online_vehicles_h); the other three in-scope
    // handlers and all seven out-of-scope siblings take `int16_t player` and get `player` forwarded
    // unchanged (same bit pattern, no re-extension needed).
    const int32_t player32 = static_cast<int32_t>(static_cast<uint16_t>(player));

    // 0x0047481c-0x00474856 (and the LAB_00474874/LAB_004748a8 sub-chains): the full CMP/JC/JBE
    // range chain, re-derived branch-by-branch -- see the header banner's type -> target table for
    // the complete mapping and the correction of the task brief's own (incorrect) range summary.
    if (type < BLDG_TYPE_A_BARRACKS) { // type < 0x7 -> default
        bldg_online_default(v, own, player32, building_index, param_3, param_4, anim_dur);
    } else if (type <= BLDG_TYPE_A_GARAGE) { // [0x7,0x8] -> barracks_garage_a
        bldg_online_barracks_garage_a(v, own, player, building_index, param_3, param_4, anim_dur);
    } else if (type < BLDG_TYPE_A_HELIPAD) { // ==0x9 -> airfield_a (out of scope)
        c.online_airfield_a(player, building_index, param_3, param_4, anim_dur);
    } else if (type == BLDG_TYPE_A_HELIPAD) { // ==0xa -> helipad_a
        bldg_online_helipad_a(v, own, player, building_index, param_3, param_4, anim_dur);
    } else if (type < BLDG_TYPE_A_PORT) { // ==0xb -> default
        bldg_online_default(v, own, player32, building_index, param_3, param_4, anim_dur);
    } else if (type == BLDG_TYPE_A_PORT) { // ==0xc -> port_a (out of scope)
        c.online_port_a(player, building_index, param_3, param_4, anim_dur);
    } else if (type == BLDG_TYPE_A_SHUTTLE) { // ==0xd -> shuttle_a (out of scope)
        c.online_shuttle_a(player, building_index, param_3, param_4, anim_dur);
    } else if (type < BLDG_TYPE_H_BARRACKS) { // [0xe,0x1a] -> default
        bldg_online_default(v, own, player32, building_index, param_3, param_4, anim_dur);
    } else if (type == BLDG_TYPE_H_BARRACKS) { // ==0x1b -> vehicles_h
        bldg_online_vehicles_h(v, own, player32, building_index, param_3, param_4, anim_dur);
    } else if (type <= BLDG_TYPE_H_GARAGE) { // ==0x1c -> soldiers_h
        bldg_online_soldiers_h(v, own, player, building_index, param_3, param_4, anim_dur);
    } else if (type < BLDG_TYPE_H_HELIPAD_OR_MISC) { // ==0x1d -> airfield_h (out of scope)
        c.online_airfield_h(player, building_index, param_3, param_4, anim_dur);
    } else if (type <= BLDG_TYPE_H_HELIPAD_OR_MISC) { // ==0x1e -> helipad_h_or_misc (out of scope)
        c.online_helipad_h_or_misc(player, building_index, param_3, param_4, anim_dur);
    } else if (type < BLDG_TYPE_H_PORT) { // ==0x1f -> default
        bldg_online_default(v, own, player32, building_index, param_3, param_4, anim_dur);
    } else if (type <= BLDG_TYPE_H_PORT) { // ==0x20 -> port_h (out of scope)
        c.online_port_h(player, building_index, param_3, param_4, anim_dur);
    } else if (type == BLDG_TYPE_H_SHUTTLE) { // ==0x21 -> shuttle_h (out of scope)
        c.online_shuttle_h(player, building_index, param_3, param_4, anim_dur);
    } else { // type > 0x21 -> default
        bldg_online_default(v, own, player32, building_index, param_3, param_4, anim_dur);
    }
}

} // namespace detail

// ---- the public wrappers ---------------------------------------------------------------------------

void bldg_online_default(int32_t player, int32_t building_index, uint32_t param_3, uint32_t param_4,
                         double anim_dur) {
    sim_state st = state();
    detail::bldg_online_default(st.read, st.own, player, building_index, param_3, param_4, anim_dur);
}

void bldg_online_barracks_garage_a(int16_t player, int32_t building_index, uint32_t param_3,
                                   uint32_t param_4, double anim_dur) {
    sim_state st = state();
    detail::bldg_online_barracks_garage_a(st.read, st.own, player, building_index, param_3, param_4,
                                          anim_dur);
}

void bldg_online_vehicles_h(int32_t player, int32_t building_index, uint32_t param_3, uint32_t param_4,
                            double anim_dur) {
    sim_state st = state();
    detail::bldg_online_vehicles_h(st.read, st.own, player, building_index, param_3, param_4, anim_dur);
}

void bldg_online_soldiers_h(int16_t player, int32_t building_index, uint32_t param_3, uint32_t param_4,
                            double anim_dur) {
    sim_state st = state();
    detail::bldg_online_soldiers_h(st.read, st.own, player, building_index, param_3, param_4, anim_dur);
}

void bldg_online_helipad_a(int16_t player, int32_t building_index, uint32_t param_3, uint32_t param_4,
                           double anim_dur) {
    sim_state st = state();
    detail::bldg_online_helipad_a(st.read, st.own, player, building_index, param_3, param_4, anim_dur);
}

void bldg_register_online(int16_t player, int32_t building_index, uint32_t param_3, uint32_t param_4,
                          double anim_dur) {
    sim_state st = state();
    detail::bldg_register_online(st.read, st.own, player, building_index, param_3, param_4, anim_dur,
                                 live_register_online_calls());
}


} // namespace mh::sim
