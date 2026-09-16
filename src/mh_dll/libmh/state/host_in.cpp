// state/host_in.cpp -- the INBOUND (host -> libmh) surface's implementation (LIB-REF-IN).
//
// Three things live here and nothing else:
//   1. the versioned handshake (libmh_in_open), shaped exactly like libmh_set_host_api's: 0 ok,
//      -1 version mismatch, and a REFUSAL KEEPS THE WORKING STATE;
//   2. the runtime dispatch tables + the trap that names an unbound entry, and the walk
//      (libmh_in_unbound) that reports one before a run ever reaches it;
//   3. one body per entry, each an exact positional forward into the SAME live C++ wrapper the
//      hosted promotion seam calls. That sameness is the whole proof strategy: the hosted
//      configuration routes its seams through these entries, so the UI suite and the determinism
//      run exercise the inbound contract rather than a parallel copy of it.
//
// WHAT IS DELIBERATELY NOT HERE. No logic. An entry that computed anything would be a second
// implementation of a body we already own, and the offline oracle would then be proving the
// wrong thing. The only code in an entry body is the guard macro and the forward.
#include "state/host_in.h"

#include <cstdio>
#include <cstring>

#include "ai/ai_state.h"            // ai_say -- the shared trace sink
#include "lockstep/tx_emit_order.h" // the outbound order batch this surface drains
#include "state/host_bind.h"        // libmh_bound_count / libmh_region_count (the open gate)

// The generated half: the order thunks (one per order id, unpacking argv into the committed
// prototype) and ENTRIES[] (name + the address of the C entry, so a derived entry nobody defined
// is a LINK error rather than a runtime surprise). It includes libmh_host_in.h itself.
#include "../../libmh/libmh_host_in_dispatch.gen.h"

namespace {

bool g_open = false;

// The RUNTIME tables. Copies of the generated ones, so the selftest can punch a hole without
// touching generated code and libmh_in_open can put it back.
int32_t (*g_order_thunk[LIBMH_ORD_COUNT])(const int32_t *);
const void *g_entry_impl[LIBMH_IN_ENTRY_COUNT];

int  g_trap_count = 0;
char g_last_trap[128];

void rebuild_tables() {
    for (uint32_t i = 0; i < LIBMH_ORD_COUNT; ++i)
        g_order_thunk[i] = mh::libmh_in::gen::ORDERS[i].thunk;
    for (uint32_t i = 0; i < LIBMH_IN_ENTRY_COUNT; ++i)
        g_entry_impl[i] = mh::libmh_in::gen::ENTRIES[i].impl;
}

void trap(const char *what, const char *name) {
    ++g_trap_count;
    std::snprintf(g_last_trap, sizeof(g_last_trap), "%s", name ? name : "?");
    // The tombstone discipline, in this direction: an unbound entry must be impossible to mistake
    // for a working one that happened to do nothing, so it says its own name.
    mh::ai::ai_say("; [libmh_in] TRAP %s: %s\n", what, g_last_trap);
}

const char *entry_name_of(uint32_t entry_id) {
    return entry_id < LIBMH_IN_ENTRY_COUNT ? mh::libmh_in::gen::ENTRIES[entry_id].name : "?";
}

} // namespace

namespace mh::libmh_in {

// The one gate every entry body passes through. Returns false when the caller must refuse:
// the surface is not open, or this entry's runtime slot is holed. On the way through it emits the
// per-entry one-shot liveness line, the same reason sim_hostreach_promote logs per row -- a run
// can be asked WHICH entries it entered.
bool enter(uint32_t entry_id, bool &fired) {
    if (!g_open) {
        trap("entry called before libmh_in_open", entry_name_of(entry_id));
        return false;
    }
    if (entry_id >= LIBMH_IN_ENTRY_COUNT || g_entry_impl[entry_id] == nullptr) {
        trap("unbound entry", entry_name_of(entry_id));
        return false;
    }
    if (!fired) {
        fired = true;
        mh::ai::ai_say("; [libmh_in] %s call #1\n", entry_name_of(entry_id));
    }
    return true;
}

void test_unbind_entry(uint32_t entry_id) {
    if (entry_id < LIBMH_IN_ENTRY_COUNT) g_entry_impl[entry_id] = nullptr;
}
void test_unbind_order(uint32_t order_id) {
    if (order_id < LIBMH_ORD_COUNT) g_order_thunk[order_id] = nullptr;
}
void test_restore_all() {
    rebuild_tables();
}

const char *last_trap() {
    return g_last_trap;
}
int trap_count() {
    return g_trap_count;
}
void trap_reset() {
    g_trap_count   = 0;
    g_last_trap[0] = '\0';
}

} // namespace mh::libmh_in

// The guard, one line per entry body. Two spellings because the return type varies and a macro
// cannot invent a value for `void`.
#define MH_IN_GUARD(id, closed_result) \
    static bool mh_in_fired_ = false;  \
    if (!::mh::libmh_in::enter(LIBMH_IN_ENTRY_##id, mh_in_fired_)) return closed_result
#define MH_IN_GUARD_V(id)             \
    static bool mh_in_fired_ = false; \
    if (!::mh::libmh_in::enter(LIBMH_IN_ENTRY_##id, mh_in_fired_)) return

// ---- the handshake ---------------------------------------------------------------------------

extern "C" int libmh_in_open(uint32_t in_version) {
    if (in_version != LIBMH_HOST_IN_VERSION) {
        // The libmh_set_host_api shape, arm for arm: a refused open KEEPS whatever was working.
        mh::ai::ai_say("; [libmh_in] REFUSED open: host 0x%08X, lib 0x%08X (the previous open, if "
                       "any, is KEPT)\n",
                       (unsigned)in_version, (unsigned)LIBMH_HOST_IN_VERSION);
        return LIBMH_IN_E_CLOSED;
    }
    // Every entry below reads bound state, so opening before the state ABI is answered for would
    // hand a host a surface aimed at nothing. This is a REFUSAL, not a warning, for the same
    // reason libmh_default_binds refuses standalone rather than returning zeroed bases.
    if (libmh_bound_count() < (int)libmh_region_count()) {
        mh::ai::ai_say("; [libmh_in] REFUSED open: %d of %u regions bound\n", libmh_bound_count(),
                       (unsigned)libmh_region_count());
        return LIBMH_IN_E_STATE;
    }
    rebuild_tables();
    g_open = true;
    mh::ai::ai_say("; [libmh_in] OPEN 0x%08X -- %u entries, %u order ids\n",
                   (unsigned)LIBMH_HOST_IN_VERSION, (unsigned)LIBMH_IN_ENTRY_COUNT,
                   (unsigned)LIBMH_ORD_COUNT);
    return LIBMH_IN_OK;
}

extern "C" int libmh_in_is_open(void) {
    return g_open ? 1 : 0;
}

extern "C" int libmh_in_unbound(void (*on_unbound)(const char *name)) {
    if (!g_open) return LIBMH_IN_E_CLOSED;
    int n = 0;
    for (uint32_t i = 0; i < LIBMH_IN_ENTRY_COUNT; ++i) {
        if (g_entry_impl[i] != nullptr) continue;
        ++n;
        if (on_unbound) on_unbound(mh::libmh_in::gen::ENTRIES[i].name);
    }
    for (uint32_t i = 0; i < LIBMH_ORD_COUNT; ++i) {
        if (g_order_thunk[i] != nullptr) continue;
        ++n;
        if (on_unbound) on_unbound(mh::libmh_in::gen::ORDERS[i].name);
    }
    return n;
}

extern "C" const char *libmh_in_entry_name(uint32_t entry_id) {
    return entry_id < LIBMH_IN_ENTRY_COUNT ? mh::libmh_in::gen::ENTRIES[entry_id].name : nullptr;
}

// ---- commands --------------------------------------------------------------------------------

extern "C" uint32_t libmh_post_event(uint32_t event_code) {
    MH_IN_GUARD(POST_EVENT, 0u);
    return ::mh::sim::game_set_event(event_code);
}

extern "C" int32_t libmh_order_arity(uint32_t order_id) {
    if (order_id >= LIBMH_ORD_COUNT) return -1;
    return (int32_t)mh::libmh_in::gen::ORDERS[order_id].arity;
}

extern "C" const char *libmh_order_name(uint32_t order_id) {
    return order_id < LIBMH_ORD_COUNT ? mh::libmh_in::gen::ORDERS[order_id].name : nullptr;
}

extern "C" int32_t libmh_issue_order(uint32_t order_id, const int32_t *argv, uint32_t argc) {
    MH_IN_GUARD(ISSUE_ORDER, (int32_t)LIBMH_IN_E_CLOSED);
    if (order_id >= LIBMH_ORD_COUNT) return LIBMH_IN_E_ARG;
    const mh::libmh_in::gen::order_row &row = mh::libmh_in::gen::ORDERS[order_id];
    if (argc != row.arity) return LIBMH_IN_E_ARITY;
    if (argv == nullptr && argc != 0) return LIBMH_IN_E_ARG;
    if (g_order_thunk[order_id] == nullptr) {
        trap("unbound order id", row.name);
        return LIBMH_IN_E_UNBOUND;
    }
    // A zero-arity order legitimately passes a null argv; the thunk never reads a[] then.
    static const int32_t k_none[1] = {0};
    return g_order_thunk[order_id](argv ? argv : k_none);
}

extern "C" void libmh_order_ack_voice(void) {
    MH_IN_GUARD_V(ORDER_ACK_VOICE);
    ::mh::orders::issue::group_order_ack_voice();
}

extern "C" uint32_t libmh_drain_outbound_orders(void *buf, size_t cap, size_t *out_bytes) {
    if (out_bytes) *out_bytes = 0;
    MH_IN_GUARD(ISSUE_ORDER, 0u);
    // NOT DERIVED FROM A CALL EDGE -- see the declaration's banner. The original's transport reads
    // the staging batch through the shared .bss; a forked host has no such access, so the batch is
    // handed over here instead. Layout inside the batch is tag(1) + record(0x44); this entry strips
    // the tags so what a host receives is N records of exactly the shape libmh_submit_order
    // accepts, which is the whole point of handing it over at all.
    //
    // IT GUARDED BY THE ORDER ENTRY ON PURPOSE. The drain is not a derived row, so it has no entry
    // id of its own; it takes libmh_issue_order's, which means holing the order surface refuses the
    // drain too. That is the right coupling -- draining a batch whose producer has been holed is
    // not a thing a host should be able to do -- and it gets the closed-gate and the liveness line
    // for free.
    //
    // THE VALIDATION IS NOT DEFENSIVE PADDING. include/packet_buffer.h is explicit that this one
    // region serves THREE roles and that the type arbitrates between none of them: the order batch
    // accumulates, a control message writes ONE record and resets the cursor, and the receive path
    // parses in place with its own local cursor. So "cursor != 0" does NOT by itself mean "N whole
    // tagged order records" -- it means that in the accumulate role and nothing guarantees which
    // role is live at the instant a host calls. Rather than assume, check: the byte count must
    // divide by the record stride and EVERY tag must be ORDER_RECORD_TAG. A batch that fails is
    // refused whole, by name, and left untouched -- handing a host misparsed bytes it would then
    // feed back through libmh_submit_order is precisely how an ABI turns a transport question into
    // a desync.
    const mh::lockstep::order_tx_state st = mh::lockstep::live_order_tx_state();
    if (st.send_buf == nullptr || st.cursor == nullptr) return 0;

    const int32_t used = *st.cursor;
    if (used <= 0) return 0;
    if (used % mh::lockstep::ORDER_RECORD_BYTES != 0) {
        trap("outbound batch is not a whole number of order records", "libmh_drain_outbound_orders");
        return 0;
    }
    const uint32_t n   = (uint32_t)(used / mh::lockstep::ORDER_RECORD_BYTES);
    const size_t   rec = (size_t)mh::lockstep::ORDER_RECORD_BYTES - 1u; // the record without its tag
    for (uint32_t i = 0; i < n; ++i) {
        if (st.send_buf[(size_t)i * mh::lockstep::ORDER_RECORD_BYTES] ==
            mh::lockstep::ORDER_RECORD_TAG)
            continue;
        trap("outbound batch holds a non-order record", "libmh_drain_outbound_orders");
        return 0;
    }
    const size_t need = (size_t)n * rec;
    if (out_bytes) *out_bytes = need;
    // Too small a buffer drains NOTHING: a partial drain would silently lose the tail, and the
    // caller has just been told (via *out_bytes) exactly how much it needs.
    if (buf == nullptr || cap < need) return 0;

    uint8_t *dst = (uint8_t *)buf;
    for (uint32_t i = 0; i < n; ++i) {
        const uint8_t *src = st.send_buf + (size_t)i * mh::lockstep::ORDER_RECORD_BYTES;
        std::memcpy(dst + (size_t)i * rec, src + 1, rec); // +1 skips ORDER_RECORD_TAG
    }
    *st.cursor = 0; // destructive, as declared
    return n;
}

// ---- the read model --------------------------------------------------------------------------

extern "C" void libmh_map_pixel_delta(int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                                      int32_t *out_dx, int32_t *out_dy) {
    MH_IN_GUARD_V(MAP_PIXEL_DELTA);
    ::mh::sim::pixel_delta_wrapped(x1, y1, x2, y2, out_dx, out_dy);
}

extern "C" void libmh_map_tile_delta(int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                                     int32_t *out_dx, int32_t *out_dy) {
    MH_IN_GUARD_V(MAP_TILE_DELTA);
    ::mh::sim::tile_delta_wrapped(x1, y1, x2, y2, out_dx, out_dy);
}

extern "C" int32_t libmh_map_tile_dist(int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    MH_IN_GUARD(MAP_TILE_DIST, (int32_t)0);
    return ::mh::sim::tile_dist_wrapped(x1, y1, x2, y2);
}

extern "C" void libmh_unit_get_coords(uint16_t player, int32_t unit_index, int32_t *out_x,
                                      int32_t *out_y) {
    MH_IN_GUARD_V(UNIT_GET_COORDS);
    ::mh::sim::get_coords(player, unit_index, out_x, out_y);
}

extern "C" int32_t libmh_unit_is_boarding(int32_t state) {
    MH_IN_GUARD(UNIT_IS_BOARDING, (int32_t)0);
    return ::mh::sim::unit_state_is_boarding(state);
}

extern "C" void libmh_bldg_get_coords(uint16_t player, int32_t building_index, int32_t *out_x,
                                      int32_t *out_y) {
    MH_IN_GUARD_V(BLDG_GET_COORDS);
    ::mh::sim::bldg_get_coords(player, building_index, out_x, out_y);
}

extern "C" int32_t libmh_bldg_uses_workers(uint32_t player, int32_t building_index) {
    MH_IN_GUARD(BLDG_USES_WORKERS, (int32_t)0);
    return ::mh::sim::bldg_uses_workers(player, building_index);
}

extern "C" int32_t libmh_bldg_shuttle_slot_is_free(int32_t player, int32_t building_id) {
    MH_IN_GUARD(BLDG_SHUTTLE_SLOT_IS_FREE, (int32_t)0);
    return ::mh::sim::bldg_shuttle_slot_is_free(player, building_id);
}

extern "C" int32_t libmh_bldg_footprint_is_clear(int32_t x, int32_t y, int32_t building_type,
                                                 uint32_t viewer) {
    MH_IN_GUARD(BLDG_FOOTPRINT_IS_CLEAR, (int32_t)0);
    return ::mh::sim::bldg_footprint_is_clear(x, y, building_type, viewer);
}

extern "C" int32_t libmh_bldg_is_network_critical(int32_t player, int32_t b_index) {
    MH_IN_GUARD(BLDG_IS_NETWORK_CRITICAL, (int32_t)0);
    return ::mh::sim::bldg_is_network_critical(player, b_index);
}

extern "C" int32_t libmh_bldg_slot_has_soldiers(int32_t building_index) {
    MH_IN_GUARD(BLDG_SLOT_HAS_SOLDIERS, (int32_t)0);
    return ::mh::sim::first_occupied_unit_slot_has_soldiers(building_index);
}

extern "C" uint32_t libmh_locate_active_port(uint32_t player, int32_t *out_col, int32_t *out_row,
                                             uint32_t *out_port_slot) {
    MH_IN_GUARD(LOCATE_ACTIVE_PORT, 0u);
    return ::mh::sim::locate_active_port(player, out_col, out_row, out_port_slot);
}

extern "C" double libmh_planet_distance(int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    MH_IN_GUARD(PLANET_DISTANCE, 0.0);
    return ::mh::sim::planet_distance(x1, y1, x2, y2);
}

extern "C" double libmh_planet_distance_factor(int32_t src_planet, int32_t dest_planet) {
    MH_IN_GUARD(PLANET_DISTANCE_FACTOR, 0.0);
    return ::mh::sim::planet_distance_factor(src_planet, dest_planet);
}

extern "C" double libmh_prod_transfer_progress(int32_t slot) {
    MH_IN_GUARD(PROD_TRANSFER_PROGRESS, 0.0);
    return ::mh::sim::prod_transfer_progress(slot);
}

// ---- control groups (the LOCAL half) ---------------------------------------------------------

extern "C" int32_t libmh_ctrlgroup_contains(uint32_t unit_id, int32_t count, int32_t group_index) {
    MH_IN_GUARD(CTRLGROUP_CONTAINS, (int32_t)0);
    return ::mh::sim::ctrl_group_contains_unit(unit_id, count, group_index);
}

extern "C" void libmh_ctrlgroup_add_member(int32_t unit_id, int32_t *count_io, int32_t group_index) {
    MH_IN_GUARD_V(CTRLGROUP_ADD_MEMBER);
    ::mh::sim::unit_ctrlgroup_add_member(unit_id, count_io, group_index);
}

extern "C" void libmh_ctrlgroup_remove_member(uint32_t unit_index, int32_t *count_io,
                                              int32_t group_index) {
    MH_IN_GUARD_V(CTRLGROUP_REMOVE_MEMBER);
    ::mh::sim::unit_ctrlgroup_remove_member(unit_index, count_io, group_index);
}

extern "C" void libmh_ctrlgroup_assign(int32_t unit_id, int32_t new_group_id) {
    MH_IN_GUARD_V(CTRLGROUP_ASSIGN);
    ::mh::sim::unit_ctrl_group_assign(unit_id, new_group_id);
}

// ---- building / production commands ----------------------------------------------------------

extern "C" void libmh_bldg_finish_current_order(uint32_t player, uint32_t building_index) {
    MH_IN_GUARD_V(BLDG_FINISH_CURRENT_ORDER);
    ::mh::sim::bldg_finish_current_order(player, building_index);
}

// X-TL-DRAIN (2026-09-12). Boot stage 7's per-building-TYPE defaults sweep. See libmh_host_in.h for
// why this is its own entry rather than a member of libmh_strat_mode_init's sequence: its caller is
// llm_boot_stage_tick, not llm_strat_mode_init, so folding it in would have the host run it at the
// wrong moment.
extern "C" void libmh_bldg_init_all(void) {
    MH_IN_GUARD_V(BLDG_INIT_ALL);
    ::mh::sim::bldg_init_all();
}

extern "C" int32_t libmh_bldg_begin_placement(uint16_t player_idx, int32_t building_idx) {
    MH_IN_GUARD(BLDG_BEGIN_PLACEMENT, (int32_t)0);
    return ::mh::sim::bldg_try_begin_placement(player_idx, building_idx);
}

extern "C" void libmh_storage_purge_dead_docked(int32_t player, int32_t storage_sub_id) {
    MH_IN_GUARD_V(STORAGE_PURGE_DEAD_DOCKED);
    ::mh::sim::storage_purge_dead_docked(player, storage_sub_id);
}

extern "C" void libmh_game_player_set_human(uint8_t player) {
    MH_IN_GUARD_V(GAME_PLAYER_SET_HUMAN);
    ::mh::sim::game_player_set_human(player);
}

extern "C" int32_t libmh_prod_set_transfer_destination(uint32_t player_idx, int32_t prod_slot,
                                                       int32_t dest_planet) {
    MH_IN_GUARD(PROD_SET_TRANSFER_DESTINATION, (int32_t)0);
    return ::mh::sim::prod_set_transfer_destination(player_idx, prod_slot, dest_planet);
}

extern "C" void libmh_prod_shuttle_slot_spawn_arrival(uint32_t slot_index) {
    MH_IN_GUARD_V(PROD_SHUTTLE_SLOT_SPAWN_ARRIVAL);
    ::mh::sim::shuttle_slot_spawn_arrival(slot_index);
}

extern "C" void libmh_progress_recheck_all(void) {
    MH_IN_GUARD_V(PROGRESS_RECHECK_ALL);
    ::mh::sim::recheck_planet_system_all_players();
}

extern "C" int32_t libmh_deploy_starting_squad(void) {
    MH_IN_GUARD(DEPLOY_STARTING_SQUAD, (int32_t)0);
    return ::mh::sim::deploy_starting_squad();
}

extern "C" void libmh_enter_tactical_mission(uint32_t player, uint32_t bldg_idx,
                                             uint32_t param_3) {
    MH_IN_GUARD_V(ENTER_TACTICAL_MISSION);
    ::mh::sim::try_enter_tactical_mission(player, bldg_idx, param_3);
}

// ---- session lifecycle -----------------------------------------------------------------------

extern "C" void libmh_session_globals_reset(uint32_t reset_flag) {
    MH_IN_GUARD_V(SESSION_GLOBALS_RESET);
    // llm_game_init_subsystems @0x004261bb's order, verbatim: the strategic reset then the net one.
    ::mh::sim::session_state_reset(reset_flag);
    ::mh::lockstep::session_globals_reset();
}

extern "C" void libmh_strat_mode_init(void) {
    MH_IN_GUARD_V(STRAT_MODE_INIT);
    // llm_strat_mode_init @0x0045f027's order, verbatim, over the five bodies we own. The twelve
    // calls it interleaves (cursor/blend tables, text pointers, cheats) belong to the fork's host.
    ::mh::sim::tech_tables_reset();
    ::mh::sim::new_game_init();
    ::mh::sim::map_fill_defaults();
    ::mh::sim::pathfinder_init();
    ::mh::sim::player_param_defaults_init();
}

extern "C" void libmh_planet_session_begin(int32_t race, int32_t reset_flag) {
    MH_IN_GUARD_V(PLANET_SESSION_BEGIN);
    ::mh::sim::planet_session_begin(race, reset_flag);
}

extern "C" int32_t libmh_planet_session_begin_multi(const void *cfg_blob) {
    MH_IN_GUARD(PLANET_SESSION_BEGIN_MULTI, (int32_t)0);
    // The blob is opaque EVERYWHERE in this tree (sim_session_begin_multi.h's declared_needs #12):
    // the body reads exactly one byte of it, +0x14, as the strategic RNG seed. The const_cast is
    // the ABI's, not a claim about mutation -- the owned wrapper's parameter is a non-const void*
    // because the original's is, and LIB-CONSTSIG only narrows a signature on evidence.
    return ::mh::sim::session_begin_multi(const_cast<void *>(cfg_blob));
}

extern "C" void libmh_planet_map_session_init(void) {
    MH_IN_GUARD_V(PLANET_MAP_SESSION_INIT);
    ::mh::sim::planet_map_session_init();
}

extern "C" void libmh_planet_transition_finalize(void) {
    MH_IN_GUARD_V(PLANET_TRANSITION_FINALIZE);
    ::mh::sim::planet_transition_finalize();
}

extern "C" int32_t libmh_planet_switch(int32_t planet_index) {
    MH_IN_GUARD(PLANET_SWITCH, (int32_t)0);
    return ::mh::sim::switch_to_planet(planet_index);
}

// ---- the clock -------------------------------------------------------------------------------

extern "C" void libmh_clock_reload_resync(double now) {
    MH_IN_GUARD_V(CLOCK_RELOAD_RESYNC);
    ::mh::lockstep::reload_snapshot_resync_clocks(now);
}

extern "C" void libmh_clock_rebase(double now) {
    MH_IN_GUARD_V(CLOCK_REBASE);
    ::mh::sim::clock_resync_units_and_buildings(now);
}

extern "C" void libmh_clock_resync_and_tick(void) {
    MH_IN_GUARD_V(CLOCK_RESYNC_AND_TICK);
    ::mh::sim::time_resync_and_tick();
}

// ---- load fixups -----------------------------------------------------------------------------

extern "C" void libmh_invasion_alerts_reset(void) {
    MH_IN_GUARD_V(INVASION_ALERTS_RESET);
    ::mh::sim::invasion_alert_reset_all();
}

extern "C" void libmh_ambient_reseed(int32_t planet_index, double now) {
    MH_IN_GUARD_V(AMBIENT_RESEED);
    ::mh::sim::ambient_reseed_planet_event_times(planet_index, now);
}

// ---- net / lockstep --------------------------------------------------------------------------

extern "C" int32_t libmh_player_index_by_side(int32_t side_id) {
    MH_IN_GUARD(PLAYER_INDEX_BY_SIDE, (int32_t)0);
    return ::mh::lockstep::player_by_side_id(side_id);
}

extern "C" uint32_t libmh_player_presence_lost(uint32_t player, uint32_t mode) {
    MH_IN_GUARD(PLAYER_PRESENCE_LOST, 0u);
    return ::mh::sim::player_presence_lost(player, mode);
}
