//
// seams/heat_ui.h -- mods:M1WL v3: what the ImGui "Stream heat" section (gfx/overlay_imgui.cpp) reads and sets.
//
// DISPLAY ONLY, except the one setter. Everything here reads game memory (units, paths) or a DLL-side snapshot; nothing writes sim state, nothing
// consumes RNG, nothing is saved or hashed. The snapshot is filled by the retail splice (move_proto_nudge.cpp rt_unmask) each time the heat rule
// prices a plan; with libmh's twin bound the twin plans instead (a different image) and the "last plan" read-outs stay at their last value --
// the "traffic" map and the tuning controls are unaffected.
//
// The setter goes through the gameplay hook table (config/gameplay.cpp heat_set_live): the same hook a [net] key runs, so there is no second copy
// of the values. It is single-player only: in a lockstep session a local change would desync, so heat_locked() says why and the panel greys out.
//
#pragma once
#include <stdint.h>

namespace mh::heatui {

constexpr int SNAP_TILES = 8192; // = sim/move/stream_heat.h SH_MAX_TILES

// The last plan the heat rule priced.
struct snapshot {
    uint32_t seq    = 0; // plans captured since the process started (0 = none yet)
    uint32_t step   = 0; // the sim step of that plan
    uint32_t player = 0;
    uint32_t unit   = 0; // the planning unit's index
    int      mates  = 0; // groupmates that satisfied the gate
    int      tiles  = 0; // distinct tiles collected
    int      priced = 0; // tiles actually priced (a cost byte of 0 is left alone)
    int      cost   = 0; // sh_params.cost of that plan
    int      scale  = 0;
    int      n      = 0;       // entries below
    uint32_t flat[SNAP_TILES]; // (col << 8) | row
    uint8_t  cnt[SNAP_TILES];  // groupmates walking the tile (0 = left alone)
};

const snapshot &last_plan();
uint32_t        hit_counter(); // stream_heat hits (the carrier's counter, MVH_SH)

// The values in force (what the setter last pushed), for the controls.
struct live_values {
    int on        = 0;
    int cost      = 2;
    int window    = 120;
    int min_mates = 8;
    int scale     = 1;
    int retrace   = 0;  // mods:M1WL v4: re-trace a heated plan charging the tile stepped onto
    int turn      = 0;  // ... with this penalty for a turn of more than 45 degrees
    int scale_cap = 16; // mods:M1WL v5: the mate count at which the scaled cost stops growing
    int margin    = 0;  // mods:M1WL v5: percent; a heated route longer than (100 + margin)% of the unheated one is replaced by it (0 = off)
};
void get_live(live_values *out);
void retrace_counts(uint32_t *plans, uint32_t *changed, uint32_t *refused); // heated plans re-traced / their route replaced / refused (since the process started)
void margin_counts(uint32_t *checks, uint32_t *kept);                       // mods:M1WL v5: heated plans compared with the unheated route / the unheated route written ("margin kept")

// units per tile, every player's live map units (stored ones excluded): counts[(col << 8) | row] = min(n, 255). Returns the unit total.
int traffic_sample(uint8_t *counts);

// The tiles of a unit's CURRENT path from its cursor (up to `max`); 0 when it has none. A display read of the live path slot.
int unit_route(uint32_t player, uint32_t unit, uint32_t *flat, int max);

} // namespace mh::heatui

namespace mh::gameplay {

// Which control a live change addresses.
enum heat_key { HEAT_ON        = 0,
                HEAT_COST      = 1,
                HEAT_WINDOW    = 2,
                HEAT_MIN_MATES = 3,
                HEAT_SCALE     = 4,
                HEAT_RETRACE   = 5,
                HEAT_TURN      = 6,
                HEAT_SCALE_CAP = 7,
                HEAT_MARGIN    = 8 };

// true while a change would desync (a lockstep session is live); *why (if non-null) gets the one-line reason.
bool heat_locked(const char **why);

// Re-run the hook of the key with `value` as an explicit setting (so the improved_movement preset no longer applies to it), publish the fixes and log
// `; [heat] live set cost=.. window=.. mates=.. scale=.. at step S` to mh_net.log. False (nothing changed) when locked or the value did not change.
bool heat_set_live(heat_key which, int value);

} // namespace mh::gameplay
