//
// sim/move/group_speed.h -- mods:M1H lane C: [net] move_group_speed (movement plan 4a, Pottinger's group speed), header-only so the
// retail splices (mh/seams/move_proto_speed.cpp) and the libmh twins (sim_unit_state_group_marshal.cpp / _move_walker.cpp) share it.
//
// Retail has no group speed. The carrier is the unit record's own move_step_speed_scale (a double): the per-microstep cost in the walker
// is cfg step_speed[player] * move_step_speed_scale * dir_step_factor. COMMIT (after each wave's group_move_order_commit): when the wave
// is a move order whose GROUND members (cfg type < A_HELI 0xf) have different step_speeds, every member faster than the slowest gets
// scale = slowest / own (> 1.0), TAGGED (the low 4 mantissa bits forced to 0xB, a relative change below 1e-15); any other ground member
// whose scale carries the tag is reset to 1.0. An untagged scale is never written. WALK (the walker's per-tile averaging
// `scale = (passable[new] + scale) * 0.5`): a TAGGED scale >= 1.0 is kept; otherwise the retail averaging runs.
//
// FLOATING POINT. The ONLY float arithmetic here is `slowest / ss`, one IEEE double divide of two cfg doubles. mh.dll (SSE2 divsd) and
// libmh (x87 FDIV at PC=53, /arch:IA32) both round it once to 53 bits, so the quotient is bit-identical on every peer; the tag step and
// the keep test are integer ops on the bit pattern. Proven on awkward ratios in libmh_test sim_move_proto_selftest_walker.cpp (against an
// SSE2 divsd reference and a long-division reference). The walker's own x87 averaging stays the retail idiom.
//
#pragma once

#include <cstdint>
#include <cstring>

#include "addr/mh_structs.gen.h"

namespace mh::sim::mvc {

constexpr uint32_t GS_TAG_MASK = 0xF, GS_TAG = 0xB;
constexpr uint32_t GS_UNIT_TYPE_A_HELI = 0xf; // cfg_enum_E_UNIT_TYPE: types >= this fly (no ground walker, scale not used)

inline uint64_t gs_bits(double d) {
    uint64_t b;
    std::memcpy(&b, &d, sizeof(b));
    return b;
}
inline bool   gs_is_tagged(double d) { return (gs_bits(d) & GS_TAG_MASK) == GS_TAG; }
inline double gs_tagged(double d) {
    uint64_t b = gs_bits(d);
    b          = (b & ~static_cast<uint64_t>(GS_TAG_MASK)) | GS_TAG;
    std::memcpy(&d, &b, sizeof(b));
    return d;
}
// The ratio a faster member gets: one IEEE divide, tagged.
inline double gs_ratio(double slowest, double own_step_speed) { return gs_tagged(slowest / own_step_speed); }

// WALK: keep a tagged scale >= 1.0 (the splice's integer test: the low 4 mantissa bits are the tag, and the high dword, as a SIGNED int,
// is >= 0x3FF00000 -- negatives go to the retail averaging).
inline bool gs_keep_scale(double scale) {
    const uint64_t b = gs_bits(scale);
    return (b & GS_TAG_MASK) == GS_TAG && static_cast<int32_t>(b >> 32) >= 0x3FF00000;
}

struct gs_result {
    bool    mixed   = false;
    double  slowest = 0.0, fastest = 0.0;
    int32_t ground = 0;
};

// W (one player's roster, the wave's scratch and the due-now order queue):
//   int32_t units();                                      the per-player unit capacity
//   mh_map_object_unit &unit(int32_t idx);                 this player's unit idx
//   const mh_cfg_final_struct_Unit &cfg(uint32_t proto);
//   int32_t scratch_unit(int32_t w);                       the wave's member w (group_move_scratch[w].unit_idx)
//   int32_t queue_count(); const mh_llm_strat_order &queue(int32_t k);
//   void log_wave(int32_t legs, int32_t ground, double fastest, double slowest);
// legs = the wave size, is_move = the commit's is_plain_move flag, mgid = the wave's move_group_id.
// Returns the verdict (mixed = the rule fired); writes the members' move_step_speed_scale.
template <class W>
inline gs_result group_speed_apply(W &w, uint32_t player, int32_t legs, int32_t is_move, uint32_t mgid) {
    gs_result     r;
    const int32_t units = w.units();
    if (legs < 1 || legs > 100 || player >= 8) return r;

    double  slowest = 0.0, fastest = 0.0;
    int32_t ground = 0;
    for (int32_t i = 0; i < legs; ++i) {
        const int32_t idx = w.scratch_unit(i);
        if (idx < 0 || idx >= units) continue;
        const auto &c = w.cfg(w.unit(idx).unit_proto_id);
        if (c.type >= GS_UNIT_TYPE_A_HELI) continue;
        const double ss = c.step_speed[player];
        if (!(ss > 0.0)) continue;
        if (ss > slowest) slowest = ss; // a larger step_speed is a larger per-microstep cost: slower
        if (ground == 0 || ss < fastest) fastest = ss;
        ++ground;
    }
    // The rule follows the ORDER, not the wave (tmp/m1/attack_stall.md problem 2): the slowest is taken over every live ground unit of
    // this player carrying the wave's move_group_id (0 = Ctrl/id-0 orders keep the wave-only rule) or holding a queued record of it.
    if (is_move != 0 && ground > 0 && mgid != 0) {
        for (int32_t i = 1; i < units; ++i) {
            const auto &o = w.unit(i);
            if (static_cast<uint32_t>(o.move_group_id) != mgid || !(o.energy > 0.0) || o.unit_proto_id >= 512) continue;
            const auto &c = w.cfg(o.unit_proto_id);
            if (c.type >= GS_UNIT_TYPE_A_HELI) continue;
            const double ss = c.step_speed[player];
            if (ss > slowest) slowest = ss;
        }
        // ...and every unit whose record of this order still waits in the due-now queue (table order).
        const int32_t qn = w.queue_count();
        // (PT-CAP9-U5: the loop bound is the world's own count. It used to clamp at the vanilla 300 as well; with the order layer ON the
        // queue holds up to 4096 records and the world's queue_count() is already bounded by the capacity of the array it points at.)
        for (int32_t k = 0; k < qn; ++k) {
            const auto &q = w.queue(k);
            if ((q.owner_and_kind & 0xfu) != player || (q.owner_and_kind & 0xf0u) != 0x80u || static_cast<uint32_t>(q.args[12]) != mgid) continue;
            if (q.unit_index >= static_cast<uint32_t>(units)) continue;
            const auto &o = w.unit(q.unit_index);
            if (!(o.energy > 0.0) || o.unit_proto_id >= 512) continue;
            const auto &c = w.cfg(o.unit_proto_id);
            if (c.type >= GS_UNIT_TYPE_A_HELI) continue;
            const double ss = c.step_speed[player];
            if (ss > slowest) slowest = ss;
        }
    }
    const bool mixed = is_move != 0 && ground > 0 && slowest != fastest;
    for (int32_t i = 0; i < legs; ++i) {
        const int32_t idx = w.scratch_unit(i);
        if (idx < 0 || idx >= units) continue;
        auto       &u = w.unit(idx);
        const auto &c = w.cfg(u.unit_proto_id);
        if (c.type >= GS_UNIT_TYPE_A_HELI) continue;
        const double ss = c.step_speed[player];
        if (mixed && ss > 0.0 && ss != slowest) u.move_step_speed_scale = gs_ratio(slowest, ss);                        // > 1.0
        else if (u.move_step_speed_scale > 1.0 && gs_is_tagged(u.move_step_speed_scale)) u.move_step_speed_scale = 1.0; // clear ours
    }
    if (mixed) w.log_wave(legs, ground, fastest, slowest);
    r.mixed   = mixed;
    r.slowest = slowest;
    r.fastest = fastest;
    r.ground  = ground;
    return r;
}

} // namespace mh::sim::mvc
