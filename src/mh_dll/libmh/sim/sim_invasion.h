#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- this TU's own literal operands with no backing Ghidra enum (rule 17a fallback) ------------------
inline constexpr int32_t TEXT_ID_INVASION_BEGUN = 0xac; // "invasion begun" -- game_HandleInvasion's own planet arm
inline constexpr int32_t TEXT_ID_INVASION_ON    = 0xad; // "invasion on <planet>" -- the distant-planet arm

// Ghidra's own E_PLANET_STATUS member names (DECLARED NEED 7) -- used as literals pending a real enum.
inline constexpr int32_t PLANET_STATUS_CONQUERED = 2;
inline constexpr int32_t PLANET_STATUS_INVASION  = 4;

// The seven external callees this closure reaches. Indirected for the same reason as every other sim/
// TU's `_calls` struct: a direct mh::call:: inside a detail:: body reaches into the live game image,
// which makes the body untestable by net_selftest.exe simtest. Signatures copied verbatim from
// addr/mh_calls.gen.h, except `w_sprintf__vi` (DECLARED NEED 4, does not exist there yet).
struct invasion_calls {
    int32_t (*rand_below)(int32_t upper_bound); // llm_rand_below @0x00499f49
    int32_t (*w_sprintf__vi)(void *dst, const wchar_t *format,
                             int32_t a0); // DECLARED NEED 4 -- new SIM-VARARGS shape
    int32_t (*w_sprintf__vss)(void *dst, const wchar_t *format, const wchar_t *a0,
                              const wchar_t *a1);                     // existing shape
    uint32_t (*print_text_message)(void *text);                       // game_ui_PrintTextMessage @0x00496508
    int32_t (*spawn_enemy_landing)();                                 // llm_strat_spawn_enemy_landing @0x004998ae
    void (*revoke_invention)(uint16_t player, uint16_t progress_id);  // llm_strat_revoke_invention @0x0044048c
    uint32_t (*player_presence_lost)(uint32_t player, uint32_t mode); // llm_strat_player_presence_lost @0x00498089
    void (*invasion_alert_arm)(int32_t planet, double timestamp);     // llm_strat_invasion_alert_arm @0x0049b49e
};

const invasion_calls &live_invasion_calls();

namespace detail {

// game_HandleInvasion @0x004996c4. See the header banner for the full derivation. `own` (not const
// sim_view alone) because the body writes `planet_status`/`planet_invasion_time` (DECLARED NEEDs 3/--)
// and reads/writes through `text_scratch()`.
int32_t handle_invasion(const sim_view &v, sim_store &own, const invasion_calls &c, int32_t planet,
                        double since_time);

// llm_strat_invasion_chance_roll @0x0049953a. Calls handle_invasion() directly (in-TU), per the batch
// context.
int32_t invasion_chance_roll(const sim_view &v, sim_store &own, const invasion_calls &c,
                             int32_t building_completed);

} // namespace detail

// Public wrappers. Signatures match the committed EXPORT prototypes (mh_export.gen.h's
// sig_game_HandleInvasion / sig_llm_strat_invasion_chance_roll) exactly -- note game_HandleInvasion's
// `planet` and return are uint32_t at this boundary (the committed export type), not the int32_t
// detail:: uses internally; the public wrapper below casts at the seam.
uint32_t handle_invasion(uint32_t planet, double since_time);
int32_t  invasion_chance_roll(int32_t building_completed);

namespace detail {
} // namespace detail

} // namespace mh::sim
