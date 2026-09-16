//
// tact/tact_unit_despawn.h -- TACT1C: removing a unit from the tactical roster.
//
//   llm_tact_unit_despawn @0x00432048 (0xbd)
//   void __watcall llm_tact_unit_despawn(int unit_idx)
//
// PROVEN OFFLINE, not rig-armed. `shadow_region_closure.py` measures a real closure (29 functions,
// 55 regions) whose bulk is pure PRESENTATION scratch reached through its own three UI-refresh
// calls (llm_tact_selection_panel_refresh, llm_tact_active_unit_count_hud_draw,
// llm_tact_ui_draw_player_row_list -- none of the three is one of the 31 TACT-CUT gated walls, and
// about half the closure's globals have no state-registry entry -- same "expensive to declare,
// nothing gained" shape as tact_unit_cmd_teleport_jump_tick.h's closure). None of it is on the
// TACT-CUT2 13-name UNGATED-effectful list (BFS over call_graph_no_crt.json: zero hits), so this is
// not a correctness hazard, just not worth the state-registry investment for a 189-byte function
// whose real, verifiable effects are its own five direct writes plus the FOV update its
// `unit_vision_remove` call makes. So: mock all four outward calls, assert the five direct writes
// (+ that vision_remove was invoked with this unit's index) offline.
//
// DERIVATION (tmp/decomp_tact/llm_tact_unit_despawn_00432048.asm):
//
// 1. @0x0043209d: `unit.status = 0`.
// 2. @0x004320ab: `unit.type += 0x80` (byte add, wraps mod 256) -- NOT an overwrite. Preserved
//    literally: whatever type the slot held (1..0x80 while occupied, per mh_tact_unit_record::type's
//    own "0=empty (guard <=0x80)" comment) is left recoverable in the high range rather than zeroed,
//    so a later read distinguishes "never spawned" (0) from "despawned, was type T" (T+0x80).
// 3. @0x004320b2-0x004320c6: three UI refresh calls, no arguments read from `unit` -- mocked, not
//    modeled (see banner). `llm_tact_ui_draw_player_row_list` is always called with the literal
//    argument -1 (`MOV EAX,0xffffffff` @0x004320bc), never `unit_idx`.
// 4. @0x004320c6-0x004320d4: `tile_objects[unit.pos_col][unit.pos_row].building = 0` -- the word
//    store lands at tile+0x2 (`MOV word ptr [EAX + 0xd1ec82],0x0`; tile base is 0xd1ec80), which
//    is the 16-bit `building` field, NOT the unit[0..1] pair at +0x4. The first shipped version
//    (and its oracle, self-consistently) cleared unit[0..1]; corrected against the raw bytes after
//    llm_tact_unit_spawn's translation showed the twin +0x2 write on the spawn side.
// 5. @0x004320dd-0x004320e6: `passable[unit.pos_col][unit.pos_row] = PASSABLE_DEFAULT` (0x2).
// 6. @0x004320ed: `--*_G_LLM_TACT_UNIT_ACTIVE_COUNT`.
// 7. @0x004320f3-0x004320fb: `llm_tact_unit_vision_remove(unit_idx)` -- mocked (frontier; its own
//    FOV write closure is the same 18-region family llm_tact_unit_rotate_step.h already declares
//    for its own vision_remove/_add calls, but declaring it HERE would still require declaring the
//    presentation calls' closure too, since shadow_region_closure.py measures per SITE not per call
//    -- hence the offline route for the whole function rather than a partial arm).
//
#pragma once
#include <cstdint>

#include "tact/tact_state.h"

namespace mh::tact {

// The four outward calls, indirected for offline testability.
struct unit_despawn_calls {
    void (*selection_panel_refresh)();                     // llm_tact_selection_panel_refresh @0x00434af7
    void (*active_unit_count_hud_draw)();                  // llm_tact_active_unit_count_hud_draw @0x00434dc2
    void (*ui_draw_player_row_list)(int32_t selected_row); // llm_tact_ui_draw_player_row_list @0x004354d3
    void (*unit_vision_remove)(int32_t unit_idx);          // llm_tact_unit_vision_remove @0x0042e53f
};

const unit_despawn_calls &live_unit_despawn_calls();

namespace detail {

// llm_tact_unit_despawn @0x00432048. See the header banner for the full derivation. Takes only
// `own` -- every field this function touches (the unit record, the shared planes, the active-unit
// counter) is read-modify-write through the STORE; nothing here needs the read-only view.
void unit_despawn(tact_store &own, const unit_despawn_calls &c, int32_t unit_idx);

} // namespace detail

void unit_despawn(int32_t unit_idx);


} // namespace mh::tact
