// Native Mode-like whole-row spinner; data ownership remains in seams/lobby_speed.
#pragma once

namespace mh::ui {

// Rebuild immediately after Mode initialization; Mode is the resolved geometry predecessor.
void  lobby_speed_widget_init(const void *mode);
void *lobby_speed_widget();
void  lobby_speed_widget_tick();
void  lobby_speed_widget_sync_flags();

} // namespace mh::ui
