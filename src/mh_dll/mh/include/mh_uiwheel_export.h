#pragma once
//
// D15 -- the mouse wheel scrolls the game's lists. Mechanism and addresses: seams/ui_wheel.cpp; the arithmetic is
// gfx/wheel_list.{h,cpp}.
//
// Three drivers share one reading of the cumulative wheel total (_G_LLM_INPUT_MOUSE_WHEEL_TOTAL), each with its OWN
// baseline (the map zoom keeps its own too):
//   A  menu / dialog list widgets  -- run-before detour on llm_ui_widget_input_tick (0x004b73f9)
//   B  the unit/building info screen (game mode 4), one page per notch -- same hook
//   C  the strategic side-panel lists -- run-before wrapper on llm_ui_scroll_icon_list_draw (0x00415c69), notches
//      from a strat_pre_input pump
// The wheel moves the VIEW only (a list's top row, a page index); the selection is never touched. View state:
// not hashed, not in a save.
//
#ifdef __cplusplus
extern "C" {
#endif

// Read [input] wheel_lists (default 1) from mh_net.ini and arm what can be armed. EN-only. Returns the number of
// drivers armed (0..3).
int MH_UiWheel_Install(void);

// A display-size round trip restarts the game's wheel counter; the next call of each driver takes it as its new
// baseline instead of a wheel delta.
void MH_UiWheel_Reseed(void);

#ifdef __cplusplus
}
#endif
