//
// sim/sim_bldg_online_secondary.cpp -- see sim_bldg_online_secondary.h. Translated from the
// DISASSEMBLY (tmp/decomp_sim/llm_strat_bldg_online_helipad_h_or_misc_00475233.asm,
// _online_airfield_a_004753e5.asm, _online_airfield_h_00475597.asm, _online_shuttle_a_00475811.asm,
// _online_shuttle_h_004759c3.asm, _online_port_a_00475fa1.asm, _online_port_h_00476153.asm).
//
#include "sim/sim_bldg_online_secondary.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace {

// SAME little-endian 4-byte pack/unpack over a raw uint8_t[] span as
// sim_bldg_register_online.cpp/sim_bldg_anim_trigger.cpp/sim_bldg_liftoff_anim.cpp -- duplicated
// per-TU rather than shared, same precedent every sim/ TU with this helper follows.
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

// online_state's two values across this family -- same posture as
// sim_bldg_register_online.cpp's own BLDG_REGISTER_ONLINE_STATE_DEFAULT/_HELIPAD (no backing Ghidra
// enum). All seven of THIS file's functions write LAST (none is the online_default "write first"
// outlier).
inline constexpr int16_t ONLINE_STATE_1 = 1;
inline constexpr int16_t ONLINE_STATE_2 = 2;

} // namespace

namespace detail {

void bldg_online_helipad_h_or_misc(const sim_view &v, sim_store &own, int16_t player,
                                   int32_t building_index, uint32_t param_3, uint32_t param_4,
                                   double anim_dur) {
    (void)param_3;
    (void)param_4;
    const uint32_t      p   = static_cast<uint32_t>(static_cast<uint16_t>(player));
    building           &b   = own.building_at(p, building_index);
    const cfg_building &cfg = v.cfg_buildings[b.building_id];

    // 0x00475283-0x00475289: anim[0] = cfg.anim[1]
    store_u32_le(&b.anim[0 * 4], load_u32_le(&cfg.anim[1 * 4]));
    // 0x004752c2-0x004752c8: anim[1] = cfg.anim[4]
    store_u32_le(&b.anim[1 * 4], load_u32_le(&cfg.anim[4 * 4]));
    // 0x004752e1: anim[2] = -1, RAW SENTINEL.
    store_u32_le(&b.anim[2 * 4], 0xffffffffu);
    // 0x0047531e-0x00475324: anim[3] = cfg.anim[2]
    store_u32_le(&b.anim[3 * 4], load_u32_le(&cfg.anim[2 * 4]));

    // 0x0047533d-0x004753b8: anim_dur[0..3] = anim_dur, FOUR stamps (two raw dword MOVs each in the
    // asm -- numerically identical to a plain double assignment; see the header banner).
    b.anim_dur[0] = anim_dur;
    b.anim_dur[1] = anim_dur;
    b.anim_dur[2] = anim_dur;
    b.anim_dur[3] = anim_dur;

    // 0x004753d1: online_state = 2 -- decoded directly from the immediate bytes
    // (`66 c7 80 b7 d2 c3 00 02 00`), matching online_vehicles_h/online_shuttle_a's own outlier-check
    // rigor.
    b.online_state = ONLINE_STATE_2;
}

void bldg_online_airfield_a(const sim_view &v, sim_store &own, int16_t player, int32_t building_index,
                            uint32_t param_3, uint32_t param_4, double anim_dur) {
    (void)param_3;
    (void)param_4;
    const uint32_t      p   = static_cast<uint32_t>(static_cast<uint16_t>(player));
    building           &b   = own.building_at(p, building_index);
    const cfg_building &cfg = v.cfg_buildings[b.building_id];

    // 0x00475435-0x0047543b: anim[0] = cfg.anim[1]
    store_u32_le(&b.anim[0 * 4], load_u32_le(&cfg.anim[1 * 4]));
    // 0x00475454: anim[1] = -1, RAW SENTINEL.
    store_u32_le(&b.anim[1 * 4], 0xffffffffu);
    // 0x00475491-0x00475497: anim[2] = cfg.anim[2]
    store_u32_le(&b.anim[2 * 4], load_u32_le(&cfg.anim[2 * 4]));
    // 0x004754d0-0x004754d6: anim[3] = cfg.anim[3]
    store_u32_le(&b.anim[3 * 4], load_u32_le(&cfg.anim[3 * 4]));

    // 0x004754ef-0x0047556a: anim_dur[0..3] = anim_dur, FOUR stamps.
    b.anim_dur[0] = anim_dur;
    b.anim_dur[1] = anim_dur;
    b.anim_dur[2] = anim_dur;
    b.anim_dur[3] = anim_dur;

    // 0x00475583: online_state = 1.
    b.online_state = ONLINE_STATE_1;
}

void bldg_online_airfield_h(const sim_view &v, sim_store &own, int16_t player, int32_t building_index,
                            uint32_t param_3, uint32_t param_4, double anim_dur) {
    (void)param_3;
    (void)param_4;
    const uint32_t      p   = static_cast<uint32_t>(static_cast<uint16_t>(player));
    building           &b   = own.building_at(p, building_index);
    const cfg_building &cfg = v.cfg_buildings[b.building_id];

    // The ONLY function in this file with 6 slots (size 0x27a, not 0x1b2).
    // 0x004755e7-0x004755ed: anim[0] = cfg.anim[1]
    store_u32_le(&b.anim[0 * 4], load_u32_le(&cfg.anim[1 * 4]));
    // 0x00475626-0x0047562c: anim[1] = cfg.anim[2]
    store_u32_le(&b.anim[1 * 4], load_u32_le(&cfg.anim[2 * 4]));
    // 0x00475645: anim[2] = -1, RAW SENTINEL.
    store_u32_le(&b.anim[2 * 4], 0xffffffffu);
    // 0x00475682-0x00475688: anim[3] = cfg.anim[4]
    store_u32_le(&b.anim[3 * 4], load_u32_le(&cfg.anim[4 * 4]));
    // 0x004756c1-0x004756c7: anim[4] = cfg.anim[5]
    store_u32_le(&b.anim[4 * 4], load_u32_le(&cfg.anim[5 * 4]));
    // 0x00475700-0x00475706: anim[5] = cfg.anim[6]
    store_u32_le(&b.anim[5 * 4], load_u32_le(&cfg.anim[6 * 4]));

    // 0x0047571f-0x004757e4: anim_dur[0..5] = anim_dur, SIX stamps.
    b.anim_dur[0] = anim_dur;
    b.anim_dur[1] = anim_dur;
    b.anim_dur[2] = anim_dur;
    b.anim_dur[3] = anim_dur;
    b.anim_dur[4] = anim_dur;
    b.anim_dur[5] = anim_dur;

    // 0x004757fd: online_state = 2.
    b.online_state = ONLINE_STATE_2;
}

void bldg_online_shuttle_a(const sim_view &v, sim_store &own, int16_t player, int32_t building_index,
                           uint32_t param_3, uint32_t param_4, double anim_dur) {
    (void)param_3;
    (void)param_4;
    const uint32_t      p   = static_cast<uint32_t>(static_cast<uint16_t>(player));
    building           &b   = own.building_at(p, building_index);
    const cfg_building &cfg = v.cfg_buildings[b.building_id];

    // 0x00475861-0x00475867: anim[0] = cfg.anim[11] (the ONLY slot-11 source in this batch; every
    // other cfg-anim read in this file is slot 1-6).
    store_u32_le(&b.anim[0 * 4], load_u32_le(&cfg.anim[11 * 4]));
    // 0x00475880: anim[1] = -1, RAW SENTINEL.
    store_u32_le(&b.anim[1 * 4], 0xffffffffu);
    // 0x004758bd-0x004758c3: anim[2] = cfg.anim[1]
    store_u32_le(&b.anim[2 * 4], load_u32_le(&cfg.anim[1 * 4]));
    // 0x004758fc-0x00475902: anim[3] = cfg.anim[3]
    store_u32_le(&b.anim[3 * 4], load_u32_le(&cfg.anim[3 * 4]));

    // 0x0047591b-0x00475996: anim_dur[0..3] = anim_dur, FOUR stamps.
    b.anim_dur[0] = anim_dur;
    b.anim_dur[1] = anim_dur;
    b.anim_dur[2] = anim_dur;
    b.anim_dur[3] = anim_dur;

    // 0x004759af: online_state = 1.
    b.online_state = ONLINE_STATE_1;
}

void bldg_online_shuttle_h(const sim_view &v, sim_store &own, int16_t player, int32_t building_index,
                           uint32_t param_3, uint32_t param_4, double anim_dur) {
    (void)param_3;
    (void)param_4;
    const uint32_t      p   = static_cast<uint32_t>(static_cast<uint16_t>(player));
    building           &b   = own.building_at(p, building_index);
    const cfg_building &cfg = v.cfg_buildings[b.building_id];

    // 0x00475a13-0x00475a19: anim[0] = cfg.anim[11]
    store_u32_le(&b.anim[0 * 4], load_u32_le(&cfg.anim[11 * 4]));
    // 0x00475a32: anim[1] = -1, RAW SENTINEL.
    store_u32_le(&b.anim[1 * 4], 0xffffffffu);
    // 0x00475a6f-0x00475a75: anim[2] = cfg.anim[3] (mirror of shuttle_a's anim[3]/anim[2] pair -- the
    // two functions swap which slot gets cfg[1] vs cfg[3], confirmed independently from each one's
    // own displacement bytes, not assumed symmetric).
    store_u32_le(&b.anim[2 * 4], load_u32_le(&cfg.anim[3 * 4]));
    // 0x00475aae-0x00475ab4: anim[3] = cfg.anim[1]
    store_u32_le(&b.anim[3 * 4], load_u32_le(&cfg.anim[1 * 4]));

    // 0x00475acd-0x00475b48: anim_dur[0..3] = anim_dur, FOUR stamps.
    b.anim_dur[0] = anim_dur;
    b.anim_dur[1] = anim_dur;
    b.anim_dur[2] = anim_dur;
    b.anim_dur[3] = anim_dur;

    // 0x00475b61: online_state = 1.
    b.online_state = ONLINE_STATE_1;
}

void bldg_online_port_a(const sim_view &v, sim_store &own, int16_t player, int32_t building_index,
                        uint32_t param_3, uint32_t param_4, double anim_dur) {
    (void)param_3;
    (void)param_4;
    const uint32_t      p   = static_cast<uint32_t>(static_cast<uint16_t>(player));
    building           &b   = own.building_at(p, building_index);
    const cfg_building &cfg = v.cfg_buildings[b.building_id];

    // 0x00475ff1-0x00475ff7: anim[0] = cfg.anim[1]
    store_u32_le(&b.anim[0 * 4], load_u32_le(&cfg.anim[1 * 4]));
    // 0x00476030-0x00476036: anim[1] = cfg.anim[6]
    store_u32_le(&b.anim[1 * 4], load_u32_le(&cfg.anim[6 * 4]));
    // 0x0047604f: anim[2] = -1, RAW SENTINEL.
    store_u32_le(&b.anim[2 * 4], 0xffffffffu);
    // 0x0047608c-0x00476092: anim[3] = cfg.anim[2]
    store_u32_le(&b.anim[3 * 4], load_u32_le(&cfg.anim[2 * 4]));

    // 0x004760ab-0x00476126: anim_dur[0..3] = anim_dur, FOUR stamps.
    b.anim_dur[0] = anim_dur;
    b.anim_dur[1] = anim_dur;
    b.anim_dur[2] = anim_dur;
    b.anim_dur[3] = anim_dur;

    // 0x0047613f: online_state = 1.
    b.online_state = ONLINE_STATE_1;
}

void bldg_online_port_h(const sim_view &v, sim_store &own, int16_t player, int32_t building_index,
                        uint32_t param_3, uint32_t param_4, double anim_dur) {
    (void)param_3;
    (void)param_4;
    const uint32_t      p   = static_cast<uint32_t>(static_cast<uint16_t>(player));
    building           &b   = own.building_at(p, building_index);
    const cfg_building &cfg = v.cfg_buildings[b.building_id];

    // 0x004761a3-0x004761a9: anim[0] = cfg.anim[1]
    store_u32_le(&b.anim[0 * 4], load_u32_le(&cfg.anim[1 * 4]));
    // 0x004761e2-0x004761e8: anim[1] = cfg.anim[2]
    store_u32_le(&b.anim[1 * 4], load_u32_le(&cfg.anim[2 * 4]));
    // 0x00476201: anim[2] = -1, RAW SENTINEL.
    store_u32_le(&b.anim[2 * 4], 0xffffffffu);
    // 0x0047623e-0x00476244: anim[3] = cfg.anim[3]
    store_u32_le(&b.anim[3 * 4], load_u32_le(&cfg.anim[3 * 4]));

    // 0x0047625d-0x004762d8: anim_dur[0..3] = anim_dur, FOUR stamps.
    b.anim_dur[0] = anim_dur;
    b.anim_dur[1] = anim_dur;
    b.anim_dur[2] = anim_dur;
    b.anim_dur[3] = anim_dur;

    // 0x004762f1: online_state = 2.
    b.online_state = ONLINE_STATE_2;
}

} // namespace detail

// ---- the public wrappers ---------------------------------------------------------------------------

void bldg_online_helipad_h_or_misc(int16_t player, int32_t building_index, uint32_t param_3,
                                   uint32_t param_4, double anim_dur) {
    sim_state st = state();
    detail::bldg_online_helipad_h_or_misc(st.read, st.own, player, building_index, param_3, param_4,
                                          anim_dur);
}
void bldg_online_airfield_a(int16_t player, int32_t building_index, uint32_t param_3, uint32_t param_4,
                            double anim_dur) {
    sim_state st = state();
    detail::bldg_online_airfield_a(st.read, st.own, player, building_index, param_3, param_4, anim_dur);
}
void bldg_online_airfield_h(int16_t player, int32_t building_index, uint32_t param_3, uint32_t param_4,
                            double anim_dur) {
    sim_state st = state();
    detail::bldg_online_airfield_h(st.read, st.own, player, building_index, param_3, param_4, anim_dur);
}
void bldg_online_shuttle_a(int16_t player, int32_t building_index, uint32_t param_3, uint32_t param_4,
                           double anim_dur) {
    sim_state st = state();
    detail::bldg_online_shuttle_a(st.read, st.own, player, building_index, param_3, param_4, anim_dur);
}
void bldg_online_shuttle_h(int16_t player, int32_t building_index, uint32_t param_3, uint32_t param_4,
                           double anim_dur) {
    sim_state st = state();
    detail::bldg_online_shuttle_h(st.read, st.own, player, building_index, param_3, param_4, anim_dur);
}
void bldg_online_port_a(int16_t player, int32_t building_index, uint32_t param_3, uint32_t param_4,
                        double anim_dur) {
    sim_state st = state();
    detail::bldg_online_port_a(st.read, st.own, player, building_index, param_3, param_4, anim_dur);
}
void bldg_online_port_h(int16_t player, int32_t building_index, uint32_t param_3, uint32_t param_4,
                        double anim_dur) {
    sim_state st = state();
    detail::bldg_online_port_h(st.read, st.own, player, building_index, param_3, param_4, anim_dur);
}


} // namespace mh::sim
