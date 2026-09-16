#pragma once
#include <cstdint>

#include "tact/tact_state.h"

namespace mh::tact {

// The 8 distinct outward function calls this substep loop makes (`utils_math_trunc` is a 9th real
// call target in the .asm but is UNMARSHALLABLE -- MH_UNAVAILABLE__parameter_storage_not_marshallable
// in addr/mh_calls.gen.h, ST0-in/ST0-out x87-only -- and is reproduced as inline __asm in the .cpp,
// the same posture every other translated TU with this callee already takes). Indirected for offline
// testability (Law 3b) -- mandatory here since proof is OFFLINE.
struct fx_update_projectile_calls {
    double (*time_get_current_time)(); // time_GetCurrentTime @0x00427616

    void (*fx_splash_damage)(int32_t col, int32_t row, int32_t radius_tiles,
                             int32_t damage); // llm_tact_fx_splash_damage @0x004314aa
    int32_t (*fx_spawn)(int32_t fx_type, uint8_t owner, int32_t x, int32_t y, int32_t x2, int32_t y2,
                        uint8_t altitude); // llm_tact_fx_spawn @0x0042bdce
    // ORIGINAL, not the sibling translation mh::tact::facing_to_delta (bit-independence -- two
    // translations must not vouch for each other).
    void (*facing_to_delta)(int32_t facing_dir, int32_t *out_dx,
                            int32_t *out_dy);                        // llm_tact_facing_to_delta @0x004311bf
    void (*unit_set_anim_state)(int32_t building_id, uint8_t state); // llm_tact_unit_set_anim_state @0x00430f03
    void (*unit_refresh_ui_slot)(int32_t building_id);               // llm_tact_unit_refresh_ui_slot @0x00434f98
    int32_t (*unit_enqueue_command)(int32_t unit_id, int32_t op, uint8_t interrupt_flag, int32_t arg0,
                                    uint16_t arg1, uint16_t arg2,
                                    uint16_t arg3); // llm_tact_unit_enqueue_command @0x0042b39d
    double (*sqrt_fn)(double x);                    // llm_sqrt @0x004da9c0
};

const fx_update_projectile_calls &live_fx_update_projectile_calls();

namespace detail {

// llm_tact_fx_update_projectile @0x00431654. See the header banner above for the substep-loop shape,
// the two dead stores, and the two branch asymmetries -- not re-explained per call site below.
void fx_update_projectile(const tact_view &tv, tact_store &own, int32_t fx_index,
                          const fx_update_projectile_calls &c);

} // namespace detail

void fx_update_projectile(int32_t fx_index);


} // namespace mh::tact
