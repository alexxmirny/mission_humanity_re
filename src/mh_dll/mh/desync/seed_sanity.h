//
// desync/seed_sanity.h -- `[desync] seed_sanity` (hotfix rc9.4, default 1): an OFF-THREAD range scan of
// every replay seed (replay_record.h: the step-1 seed and each segment seed after a save load / planet
// transition / tactical return / new game).
//
// WHY. Crash 0x0047908b / 0x004793e1 (tmp/report_triage_2026-10-15/crash_7908b.md): a building's
// pip_level[0] held ~1e8 and retail indexed the 4-entry fire-animation table with it. Who wrote it is
// unknown; both sessions had crossed planet transitions. The seeds are copies of the whole hashed state
// at exactly the boundaries suspected of carrying it in, so scanning each one names the boundary after
// which a bad value first appears -- with no game-thread cost: the seed binder already stages the state
// in one heap buffer for the sink, and instead of freeing it the buffer is handed to a scan thread.
//
// WHAT IS CHECKED (one finding per bad field; live records only -- a building with building_id 0, a unit
// with unit_proto_id 0 is a free slot):
//   buildings[8][100]   pip_level[4] in 0..4, pip_active_count in 0..16, state < 255 and a registered
//                       handler, shuttle_slot < 10.
//   units[8][100]       state < 255 and registered; order < 255; every (ref, index) target pair with a
//                       nonzero ref: owner nibble < 8 and index < 100 (target, target2, passive engage,
//                       AI contact); home_storage_slot < 25, shuttle_slot < 10, path_slot_id < 100 (0xff =
//                       none for all three); selected_weapon <= 4 or 100 (4 occurs in clean saves); ai_group_index 0xffff or < 0x400
//                       (the control-group bits alias it, docs: map_object_unit); ai_group_next/prev < 100.
//   unit_storage[8][25] b_index in 0..99, docked_count in 0..50 and each listed docked unit in 0..99,
//                       door_mutex_unit in 0..99, door_waiter_count in 0..100, occupancy in 0..200.
//   pop_stats[8]        (copied with the seed: not a hash region) no negative counter, idle <= total.
//
// REPORTING. A finding is keyed (region, player, index, field); the first seed of the session that
// carries it logs a `sanity NEW` line naming that seed's segment and boundary, later seeds only count it.
// Every scan logs one summary line with its finding counts and the measured scan time. Lines go to
// mh_net.log and to mh_rec_segments.txt.
//
// NO ADDRESSES HERE (tools/check_sim_addresses.py): the binder passes the regions' offsets inside the
// blob and the two copies (handler bitmaps, pop stats). Record layouts come from the generated structs.
//
#pragma once
#include <cstdint>

namespace mh::desync::sanity {

using log_fn = void (*)(const char *fmt, ...);

// Byte offsets of the scanned regions inside a seed blob (-1 = not in this manifest).
struct layout {
    int64_t buildings    = -1;
    int64_t units        = -1;
    int64_t unit_storage = -1;
};

// Copies the binder takes on the game thread together with the seed (both small, both read-only).
struct extras {
    bool    have_states = false;
    uint8_t unit_state_ok[256]; // 1 = _G_LLM_STRAT_UNIT_STATE_FUNCS[i] is set
    uint8_t bldg_state_ok[256]; // 1 = _G_LLM_STRAT_BLDG_STATE_FUNCS[i] is set
    bool    have_pop = false;
    uint8_t pop[8 * 0x34]; // _G_LLM_STRAT_POP_STATS[8] (llm_strat_pop_stats, stride 0x34)
};

// emit(ctx, key, text): one call per finding. Returns the number of findings.
using emit_fn = void (*)(void *ctx, const char *key, const char *text);
int scan(const uint8_t *blob, uint32_t len, const layout &L, const extras &X, emit_fn emit, void *ctx);

// Install time: [desync] seed_sanity.
void configure(int on, log_fn log);
bool enabled();

// Take ownership of `blob` (GetProcessHeap) and scan it on a background thread; the thread frees it.
// `gen` = the session folder generation (a new one forgets every key seen), `segment`/`label`/`step` name
// the seed, `segments_path` is the file the lines are appended to (may be "").
void scan_async(uint8_t *blob, uint32_t len, const layout &L, const extras &X, unsigned long gen, int segment,
                const char *label, uint32_t step, const char *segments_path);

// Tests: wait until no scan is running. True if idle within `ms`. And the last scan's counts.
bool wait_idle(unsigned ms);
int  last_findings();
int  last_new();

} // namespace mh::desync::sanity
