//
// seams/replay_boundary.h -- the seams that NOTE a wholesale state replacement for the replay record's
// segment seeds (replay_boundary.cpp, desync/replay_record.h "SEGMENTS"). Observe only.
//
#pragma once

// Arm the call-site splices (EN only, best-effort; each refusal is logged). After desync::install, and
// only when the replay record is on.
void replay_boundary_install();

// The harness's in-process load (seams/save_triggers.cpp loadgame_now) reaches llm_game_load without the
// menu's call site: it notes the boundary itself, through this.
void replay_boundary_note_save_load(const char *save_name);
