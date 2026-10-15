//
// seams/door_watch.h -- the garage-door watchdog (door_watch.cpp). Observe only.
//
#pragma once
#include <cstdint>

// Read `[desync] door_watch` (default 1) and say whether it is armed. Before the first tick.
void door_watch_install();

// One sim step, with the replay record's match step (0 = no session folder open: nothing to watch).
void door_watch_tick(uint32_t match_step);
