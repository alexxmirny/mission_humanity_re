// Native storage gestures, compact HUD controls and original-body event adapters.
#pragma once
#include <cstdint>

namespace mh::gfx::panel_rows {
struct hud_widget;
}
namespace mh::ui::storage_controls {
// Reserve these in the relocated panel allocation, never borrow a stock widget/record.
inline constexpr int EXTRA_ENTRIES = 3;
inline constexpr int EXTRA_WIDGETS = 1;
// Called after EVERY icon-table rebuild and widget extension, before publication. entries_count
// excludes the three reserved entries; widgets_count includes the appended Auto widget.
// Existing group callbacks retain their relative list-slot offsets: insertion is at each END.
bool bind_panel_tables(uint8_t *entries, int entries_count, int entries_capacity,
                       int32_t *group_starts, int groups, gfx::panel_rows::hud_widget *widgets,
                       int widgets_count, int auto_widget);
// All call/epilogue guards are checked before any write. Promoted owners suppress their original
// splices individually; their explicit source callbacks must be connected before install.
bool install_original_hooks();
// UI-only requests enter the normal native scheduler/wire path; never apply authority locally.
bool set_auto(uint32_t player, uint32_t building, bool desired);
bool set_rally(uint32_t player, uint32_t building, uint32_t x, uint32_t y);
bool clear_rally(uint32_t player, uint32_t building);
// Original and promoted executors call BEFORE setting state/copying args. This cancels uncommitted
// automatic eligibility for explicit unit commands and snapshots rally for parked manual exits.
uint16_t before_unit_commit(uint32_t player, uint32_t unit, uint16_t code,
                            uint16_t param0, int32_t *args);
} // namespace mh::ui::storage_controls
