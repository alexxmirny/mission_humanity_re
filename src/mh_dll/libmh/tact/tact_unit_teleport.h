//
// tact/tact_unit_teleport.h -- TACT1A/B: the actual teleport, called by both
// tact_teleport_cmdqueue_jump.cpp (the command-queue op) and llm_tact_teleport_zone_scan_tick
// (frontier, not translated -- scans zones for auto-triggered teleports every frame).
//
//   llm_tact_unit_teleport @0x00432df0 (0x674)
//
// THE LARGEST FUNCTION IN THE SLICE, AND WHY IT IS OFFLINE-ONLY (NOT RIG-ARMABLE): its own body
// (not merely something a caller drags in) calls llm_tact_unit_destroy (-> the roster-refresh
// cascade -> llm_tact_ui_sidebar_roster_refresh, a real framebuffer blit) and llm_tact_fx_spawn
// (-> llm_tact_fx_play_sound, real DirectSound playback) on its "invalid destination" paths. Both
// are TACT-CUT2 SHARED callees tools/data/tact_shared_callees.json classifies `effectful` and are
// UNGATED (the open, non-autonomous TACT-CUT2 item) -- confirming the caller-side notes already on
// record in tact_teleport_cmdqueue_jump.h / tact_unit_cmd_teleport_jump_tick.h. Arming this
// function's entry on the rig would risk a real double-fired sound and a real double framebuffer
// blit under shadow's snapshot/restore, with the state comparison reading clean either way -- the
// exact TWICE-test hazard TACT-CUT2 exists to close but has not. So every outward call this
// function makes is indirected through the `unit_teleport_calls` struct below (same `_calls`
// pattern as its two callers) and proven OFFLINE instead (tact_unit_teleport_selftest.cpp), which
// never executes the real destroy/fx-spawn/vision/camera calls.
//
// THREE TELEPORT MODES, read off _G_LLM_TACT_TELEPORT_TABLE[teleport_id]:
//   - association[0] == 0 (DIRECT): destination is this zone's OWN dest_col[]/dest_row[] table
//     (8 slots). mode==2 (SEQUENCE) advances a forward cursor (field_03) through the slots; mode==1
//     (RANDOM) burns a llm_rand()-derived budget scanning forward through non-empty slots, wrapping
//     at 8; mode==0 always uses slot 0. A resolved (0xff,0xff) pair means "no real destination" --
//     destroy the unit and spawn its death fx instead of moving it. Otherwise a 2x2 block starting
//     at the resolved tile is probed for a `.building`-free corner (Law 2: this reuses the SAME
//     tile_objects.building field teleport_cmdqueue_jump.h already established means "occupied, by
//     a building OR a unit" for this codebase's teleport gate -- not re-derived here).
//   - association[0] != 0 (LINKED): the SAME slot-selection shape (mode 2/1/0) but over the
//     `association[]` array instead, whose value is a teleport-zone ID (not a coordinate) --
//     searched for by scanning _G_LLM_TACT_TELEPORT_TABLE[1..0x3f] for a matching `.id`. No match
//     found -> same destroy+death-fx fallback as the DIRECT path's (0xff,0xff) case. A match whose
//     own `field_28` is already flagged -> silently refuse (no move, no destroy). Otherwise the unit
//     is moved to the matched zone's `start_col`/`start_row` and that zone's `field_28` is stamped 1
//     (a "consumed/occupied" latch -- the DIRECT path never sets this on anything).
//
// THE no_enemy GUARD (@0x00432e4b-0x00432e66) READS BACKWARDS FROM A FIRST-GLANCE PARSE OF THE
// FIELD NAME: it refuses the teleport when `no_enemy==1 AND owner==1`, i.e. owner 1 (this
// subsystem's other established "not owner 0" cases, e.g. camera-centre below, treat owner 0 as the
// human/local side) is exactly the owner EXCLUDED by a "no_enemy"-flagged zone -- so owner==1 reads
// as "the enemy" throughout this function, not owner!=1. Cross-checked against the raw bytes twice
// because the natural first reading (refuse everyone EXCEPT owner 1) is inverted from this.
//
// OUT-OF-DECLARED-BOUNDS `slot` IS A REAL, PRESERVED BEHAVIOUR, NOT A TRANSLATION HAZARD TO CLAMP:
// the mode==2 (SEQUENCE) cursor update computes `slot = field_03 + 1` from a full byte (0..255) and
// indexes dest_col[]/dest_row[]/association[] (nominally 8 elements) with it BEFORE any bound is
// applied -- the only bound is the data-dependent "both zero" / "association==0" reset, which a
// zone with all 8 slots genuinely populated never triggers. The original computes this as a flat
// byte offset from the table base with no array-length check, so `slot` can legitimately read past
// this zone's own 50-byte record into the NEXT one. Reproduced via raw pointer arithmetic
// (`dest_col_at`/`dest_row_at`/`association_at` below) rather than `tz.dest_col[slot]`, which would
// invoke undefined behaviour AND trip ASan's global-buffer-overflow instrument on exactly the same
// access the real game performs safely (the byte offset stays within
// _G_LLM_TACT_TELEPORT_TABLE[66]'s own extent for any `field_03` a real mission ever produces). The
// mode==1 (RANDOM) scan wraps its own cursor at 8 explicitly and never needs this, but uses the same
// helpers for consistency.
//
#pragma once
#include <cstddef>
#include <cstdint>

#include "tact/tact_state.h"

namespace mh::tact {

// Every outward call this function's body makes, indirected for offline testability -- same shape
// as unit_cmd_teleport_jump_tick_calls / teleport_cmdqueue_jump_calls, just a longer list.
struct unit_teleport_calls {
    void (*unit_destroy)(uint32_t unit_idx); // llm_tact_unit_destroy @0x00431d8d
    int32_t (*fx_spawn)(int32_t fx_type, uint8_t owner, int32_t x, int32_t y, int32_t x2, int32_t y2,
                        uint8_t altitude);                                         // llm_tact_fx_spawn @0x0042bdce
    void (*unit_vision_add)(int32_t unit_idx);                                     // llm_tact_unit_vision_add @0x0042e385
    void (*unit_vision_remove)(int32_t unit_idx);                                  // llm_tact_unit_vision_remove @0x0042e53f
    void (*camera_center_on_tile)(int32_t target_col, int32_t target_row);         // @0x0042e889
    int32_t (*rand)();                                                             // llm_rand @0x004da98b
    void (*unit_cmd_advance)(int32_t unit_idx, int32_t cmd_slot_index);            // llm_tact_unit_cmd_advance @0x00431229
    void (*facing_to_delta)(int32_t facing_dir, int32_t *out_dx, int32_t *out_dy); // @0x004311bf
};

const unit_teleport_calls &live_unit_teleport_calls();

namespace detail {

// Raw-byte accessors mirroring the original's flat address arithmetic -- see the header comment on
// out-of-bounds `slot`. `association`/`dest_col`/`dest_row` are declared-adjacent within
// mh_llm_tact_teleport (offsets 0x8/0x18/0x29, static_assert-guarded), so any `idx` a real mission's
// field_03 can produce stays inside _G_LLM_TACT_TELEPORT_TABLE's own 66-entry extent.
inline uint16_t dest_col_at(teleport_zone &tz, int32_t idx) {
    return reinterpret_cast<uint16_t *>(reinterpret_cast<uint8_t *>(&tz) +
                                        offsetof(teleport_zone, dest_col))[idx];
}
inline uint16_t dest_row_at(teleport_zone &tz, int32_t idx) {
    return reinterpret_cast<uint16_t *>(reinterpret_cast<uint8_t *>(&tz) +
                                        offsetof(teleport_zone, dest_row))[idx];
}
inline uint8_t association_at(teleport_zone &tz, int32_t idx) {
    return reinterpret_cast<uint8_t *>(&tz)[offsetof(teleport_zone, association) + idx];
}

// llm_tact_unit_teleport @0x00432df0.
void unit_teleport(tact_store &own, mh::state::mode_planes &planes, const unit_teleport_calls &c,
                   int32_t teleport_id, int32_t unit_id);

} // namespace detail

void unit_teleport(int32_t teleport_id, int32_t unit_id);


} // namespace mh::tact
