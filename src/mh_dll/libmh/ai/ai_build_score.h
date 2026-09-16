//
// ai/ai_build_score.h -- the per-player build-category score cache refresh (RI-AI / AI1B).
//
// One function: recomputes player_data's build-category score cache (fields ai_score_cat_* /
// ai_score_bldg_type_a / ai_score_bldg_type_b, offsets 0x286a8-0x286d4) from the live roster/queue
// state. Feeds llm_strat_ai_plan_construction's and llm_strat_ai_scan_bldg_repair_upgrade's
// decisions (both gated on ai_score_cat_1 != 0, i.e. "scores computed at least once"). Called once
// per llm_strat_ai_player_tick.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

// The logic over an EXPLICIT state, so `net_selftest.exe aitest` can drive it over heap buffers
// with no game and no rig. The wrapper below is this applied to state(); the split costs one
// inlined call.
namespace detail {

// llm_strat_ai_score_build_categories @ 0x004e54cc.
//
// Nine calls to llm_strat_ai_bldg_count_by_category (categories 1, 0x20, 0x21, 0x23, 0x22, 0x10,
// 0x11, 0x12, 0x13 -- IN THAT ORDER, which is not numeric order: 0x23 is stored before 0x22, and
// that is reproduced exactly, not "fixed"), each stored straight into the matching ai_score_cat_*
// field.
//
// Then two race-selected (building-type, count-by-type + queue-pending) pairs, computed identically
// but reading two different race-paired type constants -- the turret pair (race_turret_type) feeds
// ai_score_bldg_type_a, and, after the tenth category call (0x30 -> ai_score_cat_0x30), the mine
// pair (race_mine_type) feeds ai_score_bldg_type_b. The original re-tests is_alien_race a second
// time per pair (once for the count-by-type call's type argument, once for the queue-pending call's)
// rather than caching it across the two calls; nothing writes is_alien_race in between, so a single
// read here is equivalent.
//
// ALL THREE callees -- llm_strat_ai_bldg_count_by_category, llm_strat_bldg_count_by_type and
// llm_strat_ai_bldg_type_queue_has_pending -- stay original (Law 3) and are reached through `gc`,
// i.e. through `ai_calls`, never through `mh::call::` directly. The indirection is not decoration:
// it is what lets `net_selftest.exe aitest` drive this body over heap fixtures with `stub_calls()`,
// and a direct `mh::call::` in a `detail::` function would jump into the game image from a test
// process. The first two had NO COMMITTED PROTOTYPE until 2026-08-02 (EN v203) and were therefore
// uncallable at all; see ghidra_findings 2026-08-02-2103-3.
void score_build_categories(const ai_view &v, const ai_store &own, const ai_calls &gc,
                            int32_t player);

// llm_strat_ai_player_score_tier @ 0x004e5686 (RI-AI batch B, antichain layer 3, 2026-08-06).
//
// Reads the SAME ai_score_cat_* fields score_build_categories above just wrote and folds them into a
// single 0-5 tier via a strictly-ascending short-circuit ladder (each tier's gate is the PREVIOUS
// tier's gate plus one more field), read off the disassembly's five `MOV EDX,N` immediates rather than
// trusted from Ghidra's .c rendering (whose comma-operator side effects group the gating fields one
// off from where the .c's own nesting suggests -- see the .cpp's header banner for the full
// derivation): tier 1 <- 0x10 && 0x20; tier 2 <- + 0x21 && 0x11; tier 3 <- + 0x30; tier 4 <- + 0x12;
// tier 5 <- + 0x13 && 0x23 && 0x22. In particular 0x30 gates tier 3 (not tier 2) and 0x12 gates tier 4
// alone (not bundled with 0x13/0x23) -- both easy to get backwards from the .c's syntactic grouping.
int32_t player_score_tier(const ai_view &v, int32_t player_idx);

// llm_strat_ai_bldg_count_by_category @ 0x004d370a (RI-AI batch B, 2026-08-07). The counter
// score_build_categories above calls nine times per tick. Two-part count over the SAME category:
// built buildings (walking the roster from index 0, per the disassembly -- NOT index 1 like the
// sibling roster walks elsewhere in this cluster; buildings[player][0]'s own building_id happens to
// never match a real building, so this is equivalent to skipping it, but the loop genuinely starts
// at 0) whose cfg Building.ai_build low 12 bits match `category`, PLUS queued AI build-queue entries
// whose status is exactly 1 (kind 1 = construction, per mh_llm_strat_ai_bldg_queue_entry::status's
// own field comment) and whose queued building type has the same category.
int32_t bldg_count_by_category(const ai_view &v, int32_t player, uint32_t category);

} // namespace detail

void    score_build_categories(int32_t player);
int32_t player_score_tier(int32_t player_idx);
int32_t bldg_count_by_category(int32_t player, uint32_t category);

} // namespace mh::ai
