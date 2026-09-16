#pragma once
#include <cstdint>

#include "sim/sim_order_enqueue.h" // BUILDING_TYPE_A_MOTHER (0x06) / BUILDING_TYPE_H_MOTHER (0x1a)
#include "sim/sim_state.h"

namespace mh::sim {

// ---- this function's own literal operands with no backing Ghidra enum (rule 17a fallback) -----------
inline constexpr int32_t TEXT_ID_MOTHERSHIP_DESTROYED    = 4; // llm_ui_print_queue_text_id arg
inline constexpr int32_t TEXT_ID_BUILDING_DESTROYED_NAME = 5; // G_TEXT_PTRS[5], the w_sprintf "reason" half

inline constexpr int32_t DEATH_BLAST_TARGET_KIND = 2;    // llm_strat_apply_area_damage's target_kind
inline constexpr double  DEATH_BLAST_DAMAGE      = 10.0; // llm_strat_apply_area_damage's damage
inline constexpr int32_t DEATH_BLAST_RING_COUNT  = 2;    // llm_strat_apply_area_damage's ring_count

// The two fx_anim_spawn calls' param_5 -- an unnamed flag with no backing enum found anywhere in the
// tree (this is the first sim/ai translation to call llm_strat_fx_anim_spawn at all, so there is no
// sibling naming to match). Named only to keep the literal out of the call site per rule 17a; meaning
// genuinely unresolved -- see uncertainties[].
inline constexpr uint32_t FX_ANIM_SPAWN_PARAM5_PRIMARY   = 1u; // first call (Building[].anim[7])
inline constexpr uint32_t FX_ANIM_SPAWN_PARAM5_SECONDARY = 0u; // second call (death_anim_table[...])

// The local-player voice-line sound id: `(player_race==2 ? 0x12 : 0) + 5`, volume 100. No backing enum
// found for the race-dependent offset.
inline constexpr int32_t VOICE_LINE_RACE2_OFFSET  = 0x12;
inline constexpr int32_t VOICE_LINE_BASE_SOUND_ID = 5;
inline constexpr int32_t VOICE_LINE_VOLUME        = 100;

// ---- the outward calls ---------------------------------------------------------------------------
//
// Indirected for the same reason as every other sim/ TU: a direct mh::call:: inside a detail:: body
// reaches into the live game image, which makes the body untestable by net_selftest.exe simtest.
struct bldg_state_destroyed_calls {
    void (*bldg_get_coords)(uint16_t player, int32_t building_index, int32_t *out_x, int32_t *out_y);
    uint32_t (*fx_anim_spawn)(uint32_t x, uint32_t y, uint32_t anim_frame, double elapsed, uint32_t flag);
    int32_t (*rand_below)(int32_t upper_bound);
    void (*apply_area_damage)(int32_t x, int32_t y, int32_t target_kind, double damage, int32_t ring_count,
                              uint32_t owner_filter_zeroed, uint32_t killer_info, int32_t killer_unit_index);
    void (*snd_play_at)(int32_t sound_id, int32_t tile_col, int32_t tile_row); // event record: the
                                                                               // hosted sink runs the
                                                                               // original offscreen_
                                                                               // snd_volume+snd_play
                                                                               // pair at emit
    void (*snd_play)(int32_t sound_id, int32_t volume);                        // fixed-volume voice line only
    void (*ui_print_queue_text_id)(int32_t text_id);
    void (*game_sp_outcome_announce)();
    int32_t (*w_sprintf__vss)(void *dst, const wchar_t *format, const wchar_t *a0, const wchar_t *a1);
    uint32_t (*game_ui_PrintTextMessage)(void *text);
    int32_t (*game_check_players_mothership_alive)();
    int32_t (*invasion_chance_roll)(int32_t building_completed);
    void (*fx_debris_burst)(int32_t tile_col, int32_t tile_row, int32_t energy_max); // event record:
                                                                                     // the hosted sink
                                                                                     // runs offscreen_
                                                                                     // fx_scale -> x87
                                                                                     // intensity ->
                                                                                     // spawn_debris_
                                                                                     // burst at emit
    void (*prod_unbind_planet)(int32_t player, int32_t planet_slot);
    void (*prod_shuttle_slot_release)(int32_t player, int32_t slot);
    void (*bldg_unmap_footprint)(uint16_t player, int32_t building_index); // SIBLING unit, called as original
    uint32_t (*set_event)(uint32_t type);
    void (*unit_purge_unregistered)(uint32_t player);
    uint32_t (*player_presence_lost)(uint32_t player, uint32_t mode);
    uint32_t (*sight_add_circle)(uint32_t player, int32_t x, int32_t y, int32_t building_id, uint8_t sight);
    void (*bldg_notify_ui)(uint16_t player, uint32_t b_index);
};

const bldg_state_destroyed_calls &live_bldg_state_destroyed_calls();

namespace detail {

// llm_strat_bldg_state_destroyed @0x00473194. See the header banner above for the full derivation;
// the .cpp carries the per-line address citation. No parameters -- operates on `v`/`own`'s ambient
// cur_building/cur_player/cur_index/tick_budget alone, matching the original's void(void) signature.
void bldg_state_destroyed(const sim_view &v, sim_store &own, const bldg_state_destroyed_calls &c);

} // namespace detail

// Live wrapper: the logic applied to state() and live_bldg_state_destroyed_calls(). Matches the
// committed prototype (sig_llm_strat_bldg_state_destroyed) exactly.
void bldg_state_destroyed();

namespace detail {
} // namespace detail

} // namespace mh::sim
