//
// ai/ai_group_task_workers.cpp -- see ai_group_task_workers.h. Translated from the DISASSEMBLY, not
// from the exported .c drafts (their stack-slot labels disagree with the addresses the assembly
// actually reads/writes -- see the header's per-function notes, especially group_rally_formup_worker's).
//
#include "ai/ai_group_task_workers.h"

#include <bit>
#include <cmath>


namespace mh::ai {
namespace detail {

namespace {
// DAT_00504813, verified via ReVA read-memory (E0 86 44 54 FB 21 19 40 little-endian). See the
// header comment for why this is NOT recomputed as 2*pi.
inline constexpr double RANDOM_POINT_NEAR_ANGLE_SCALE =
    std::bit_cast<double>(UINT64_C(0x401921FB544486E0));
} // namespace

void unit_group_assign_by_type(const ai_view &v, const ai_store &own, const ai_calls &gc,
                               uint32_t player, int32_t group_index, int32_t unit_id,
                               int32_t storage_slot) {
    (void)v;
    (void)own;

    gc.unit_order_exit_storage_enqueue((uint16_t)player, (uint32_t)unit_id, storage_slot, 0, 0);

    int32_t type_code;
    if (gc.unit_is_ai_ground((uint16_t)player, (uint32_t)unit_id) != 0 ||
        gc.unit_is_ai_soldier(player, (uint32_t)unit_id) != 0) {
        type_code = 2;
    } else if (gc.unit_is_ai_plane(player, (uint32_t)unit_id) != 0) {
        type_code = 4;
    } else if (gc.unit_is_ai_heli(player, (uint32_t)unit_id) != 0) {
        type_code = 3;
    } else {
        return; // 0x004d58cb JZ straight to the epilogue -- no move.
    }
    gc.group_member_move(player, group_index, type_code, unit_id);
}

void group_rally_formup_worker(const ai_view &v, const ai_store &own, const ai_calls &gc,
                               int32_t player_id, int32_t group_index) {
    uint32_t centroid_x = 0; // committed llm_strat_ai_group_compute_centroid out-params
    uint32_t centroid_y = 0;
    gc.group_compute_centroid(player_id, group_index, &centroid_x, &centroid_y);

    // See the header note on why this is (a, b) = (x, y) despite the exported .c's mislabeled locals.
    own.players[player_id].ai_groups[group_index].active_param_a = centroid_x;
    own.players[player_id].ai_groups[group_index].active_param_b = centroid_y;

    int32_t  spiral_index = 0;
    uint32_t unit_id      = v.players[player_id].ai_groups[group_index].head_unit;
    while (unit_id != 0) {
        const spiral_offset &off = v.spiral_offsets[spiral_index];
        const uint32_t       x   = (uint32_t)((int32_t)off.dx + centroid_x) & *v.map_width_mask;
        const uint32_t       y   = (uint32_t)((int32_t)off.dy + centroid_y) & *v.map_height_mask;
        gc.unit_flag_and_move((uint32_t)player_id, (int32_t)unit_id, x, y);
        ++spiral_index;
        unit_id = unit_of(v, (uint32_t)player_id, (int32_t)unit_id).ai_group_next;
    }
}

void group_scatter_random_worker(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                 int32_t player_id, int32_t group_index,
                                 uint32_t class_or_radius) {
    (void)own;

    uint32_t centroid_x = 0; // committed llm_strat_ai_group_compute_centroid out-params
    uint32_t centroid_y = 0;
    gc.group_compute_centroid(player_id, group_index, &centroid_x, &centroid_y);

    uint32_t unit_id = v.players[player_id].ai_groups[group_index].head_unit;
    while (unit_id != 0) {
        if (gc.unit_is_order_pending((uint32_t)player_id, unit_id) == 0) {
            int32_t x = 0;
            int32_t y = 0;
            gc.random_point_near(centroid_x, centroid_y, (int32_t)class_or_radius, &x, &y);
            gc.unit_flag_and_move((uint32_t)player_id, (int32_t)unit_id, (uint32_t)x, (uint32_t)y);
        }
        unit_id = unit_of(v, (uint32_t)player_id, (int32_t)unit_id).ai_group_next;
    }
}

void random_point_near(const ai_view &v, const ai_calls &gc, int32_t x, int32_t y, int32_t radius,
                       int32_t *out_x, int32_t *out_y) {
    const double angle    = gc.rand_state_advance(2) * RANDOM_POINT_NEAR_ANGLE_SCALE;
    const double s        = gc.math_fsin_reduce_loop(angle);
    const double c        = gc.math_cos_impl(angle);
    const double radius_f = (double)(uint32_t)radius; // zero-extended, not sign-extended -- see header

    const int32_t dx = (int32_t)std::floor(s * radius_f);
    const int32_t dy = (int32_t)std::floor(c * radius_f);

    *out_x = (int32_t)(((uint32_t)(x + dx)) & *v.map_width_mask);
    *out_y = (int32_t)(((uint32_t)(y + dy)) & *v.map_height_mask);
}

} // namespace detail

void unit_group_assign_by_type(uint32_t player, int32_t group_index, int32_t unit_id,
                               int32_t storage_slot) {
    const ai_state st = state();
    detail::unit_group_assign_by_type(st.read, st.own, live_calls(), player, group_index, unit_id,
                                      storage_slot);
}
void group_rally_formup_worker(int32_t player_id, int32_t group_index) {
    const ai_state st = state();
    detail::group_rally_formup_worker(st.read, st.own, live_calls(), player_id, group_index);
}
void group_scatter_random_worker(int32_t player_id, int32_t group_index, uint32_t class_or_radius) {
    const ai_state st = state();
    detail::group_scatter_random_worker(st.read, st.own, live_calls(), player_id, group_index,
                                        class_or_radius);
}
void random_point_near(int32_t x, int32_t y, int32_t radius, int32_t *out_x, int32_t *out_y) {
    const ai_state st = state();
    detail::random_point_near(st.read, live_calls(), x, y, radius, out_x,
                              out_y);
}


} // namespace mh::ai
