//
// seams/ui_panel_rows.h -- RES7: the side-panel lists fill the panel at window heights above 768.
//
// The layout is gfx/panel_rows.h; the patches and the reasoning are in ui_panel_rows.cpp.
//
#pragma once

namespace mh::video {

// Grows the panel list tables for windows up to `h_max` rows (the tallest size the session can reach: the picker's
// tallest entry or the [video] width/height size; <= 0 means unknown and takes gfx::panel_rows::H_FALLBACK). Arms
// nothing when h_max <= 768, when P6 is not armed ([video] mode_guards=0), when [video] panel_rows=0, or when any
// patch site does not hold the stock bytes. Call once, after the picker list is built. Returns true when armed.
bool install_panel_rows(const char *ini, int h_max);

// A size change happened (seams/video.cpp's post-change hook): the scroll bodies follow the new height and the rows
// past it are hidden. The list row-count sites notice a change themselves too; this only makes it immediate.
void panel_rows_on_size_change();

} // namespace mh::video
