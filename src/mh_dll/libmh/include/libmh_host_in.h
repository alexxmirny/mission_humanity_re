/* libmh_host_in.h -- the DESIGNED inbound surface of the libmh host ABI (LIB-REF-IN,
 * the LIB-REF plan section 3.5). HAND-AUTHORED, exactly like libmh_host_events.h and for the
 * same reason: the GENERATED half (libmh_host_in.gen.h -- order ids, entry ids, the version) is
 * derived from the call graph and moves when the tree moves; this envelope is the stable contract
 * a host binds against. C89-includable.
 *
 * WHICH DIRECTION IS WHICH. libmh.h is the replay spine (bind / import / submit_order / sim_step /
 * state_hash / save / load). libmh_host_events.h is OUTBOUND -- libmh tells the host what happened.
 * This file is INBOUND -- the host tells libmh to do something, or asks it a question. Its
 * membership is not a wish list: every entry here serves at least one measured call site where
 * ORIGINAL front-end code today reaches a body libmh owns, and the mapping is adjudicated in
 * tools/data/libmh_inbound_entries.json with `gen_libmh_inbound.py --check` as the gate. 103 such
 * bodies are covered; 36 more are excluded with a written reason.
 *
 * THE SHAPE FOLLOWS libmh_host_events.h's PRECEDENT, deliberately -- that header already carries an
 * inbound command (libmh_submit_screen_answer) and its banner already states the rule: input is
 * PUSHED (R10). So: one computed version constant, a handshake that returns 0 / -1 and KEEPS the
 * working state on refusal, POD in and POD out, and ids rather than addresses.
 *
 * ---- TWO THINGS THAT ARE NOT ACCIDENTS ------------------------------------------------------
 *
 * (1) ORDERS CROSS AS INTENT, NOT AS BYTES. libmh_submit_order takes a wire record because a replay
 * file holds exactly those bytes; it must stay. But it is NOT how a front-end issues an order,
 * because that would put record construction -- the determinism boundary -- in the host, where a
 * binding bug becomes a desync instead of a crash. libmh_issue_order names an intent and libmh
 * builds the 68-byte record itself (orders/order_codec.h stays the single encoding, wire and save).
 *
 * (2) CONTROL GROUPS ARE SPLIT, and the split is the ORIGINAL's, not ours. flash/select already
 * dispatch lockstep orders 0x34/0x35, so they are order ids here. The ROSTER half (add / remove /
 * assign / contains) edits _G_LLM_STRAT_CTRL_GROUPS, which is GROUP-indexed (10 x 0x194) with no
 * player dimension and appears in no lockstep hash column -- so it is local, per-peer state, and
 * replicating it would have two peers clobber one 404-byte slot. Measured 2026-09-11; the
 * full-replication alternative needs a [players][10] re-layout, which breaks save-format byte
 * compatibility (the region is MF_SAVE) and therefore LIB-FORK's permanent guarantee.
 */

#ifndef LIBMH_HOST_IN_H
#define LIBMH_HOST_IN_H

#include <stddef.h>
#include <stdint.h>

#include "libmh_host_in.gen.h" /* LIBMH_HOST_IN_VERSION, LIBMH_ORD_*, LIBMH_IN_ENTRY_* */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- return codes ---------------------------------------------------------------------- */

#define LIBMH_IN_OK        0
#define LIBMH_IN_E_CLOSED  -1 /* libmh_in_open has not succeeded                              */
#define LIBMH_IN_E_ARG     -2 /* a null out-pointer, or an id out of range                     */
#define LIBMH_IN_E_ARITY   -3 /* argc does not match the order's declared arity                */
#define LIBMH_IN_E_STATE   -4 /* not legal in the current session state                        */
#define LIBMH_IN_E_UNBOUND -5 /* the dispatch slot is NULL (after the trap names it)           */

/* ---- the handshake --------------------------------------------------------------------- */

/* Open the inbound surface.
 *   0                  ok
 *   LIBMH_IN_E_CLOSED  version mismatch -- a PREVIOUS successful open is KEPT, exactly as
 *                      libmh_set_host_api keeps a working table on a refused rebind
 *   LIBMH_IN_E_STATE   the state ABI is not fully bound yet (libmh_bound_count() <
 *                      libmh_region_count()); every entry below reads bound state, so opening
 *                      before the bind would hand a host a surface aimed at nothing
 * Every entry in this header returns LIBMH_IN_E_CLOSED (or, where it cannot, is a no-op) until
 * this has returned 0. */
int libmh_in_open(uint32_t in_version);

/* Is the surface open? Diagnostic; a host does not need it. */
int libmh_in_is_open(void);

/* Startup diagnostic, the libmh_host_api_unbound shape exactly: calls on_unbound(name) once per
 * entry whose implementation is not wired (`on_unbound` may be null), and returns the count of
 * such entries, or LIBMH_IN_E_CLOSED if the surface is not open. The arm-time check requires
 * zero -- an unwired entry that quietly did nothing is the failure this walk exists to prevent. */
int libmh_in_unbound(void (*on_unbound)(const char *name));

/* ---- commands -------------------------------------------------------------------------- */

/* Post one game event -- game_SetEvent's 35 front-end call sites, which is every HUD button,
 * panel tick, dialog and mode change in the game.
 *
 * MEASURED PAYLOAD: one argument and no more. `event_code` is a LIBMH_EV_* member; the jump-table
 * domain is 0x00..0x18 INCLUSIVE with a NO-OP HOLE at 0x10, and codes outside that range are
 * no-ops exactly as in the original. The return is the original's own value verbatim -- on the
 * deferral path a ring slot index * 4, otherwise the switch's result -- and is NOT an error code;
 * the surface is already gated by libmh_in_open. */
uint32_t libmh_post_event(uint32_t event_code);

/* game::e::event, the 25 members (the strategic-sim notes). 0x10 has no handler and is a no-op. */
#define LIBMH_EV_PANEL_SHOW_MAP          0x00u
#define LIBMH_EV_PANEL_SHOW_BUILD        0x01u
#define LIBMH_EV_PANEL_SHOW_INFO         0x02u
#define LIBMH_EV_BUILD_TAB_BUILDINGS     0x03u
#define LIBMH_EV_BUILD_TAB_UNITS         0x04u
#define LIBMH_EV_BUILD_TAB_PROJECTS      0x05u
#define LIBMH_EV_INFO_REFRESH            0x06u
#define LIBMH_EV_BUILD_PROJECTS_REFRESH  0x07u
#define LIBMH_EV_BUILD_UNITS_REFRESH     0x08u
#define LIBMH_EV_BUILD_OPEN_AUTOPAGE     0x09u
#define LIBMH_EV_HUD_REDRAW_ALL          0x0au
#define LIBMH_EV_ACTION_DENIED_FEEDBACK  0x0bu
#define LIBMH_EV_PANEL_SHOW_MAP_OBJECTS  0x0cu
#define LIBMH_EV_MAP_SUBMODE1_REFRESH    0x0du
#define LIBMH_EV_MAP_OBJECTS_REFRESH     0x0eu
#define LIBMH_EV_BUILD_BUILDINGS_REFRESH 0x0fu
/* 0x10: no handler -- the jump-table slot aliases the shared tail. Deliberately unnamed. */
#define LIBMH_EV_PANEL_SHOW_INFO_TAB0  0x11u
#define LIBMH_EV_PANEL_SHOW_INFO_TAB1  0x12u
#define LIBMH_EV_CHAT_INPUT_OPEN       0x13u
#define LIBMH_EV_CHAT_INPUT_CLOSE      0x14u
#define LIBMH_EV_BUILD_HOTKEY_TOGGLE   0x15u
#define LIBMH_EV_INFO_HOTKEY_TOGGLE    0x16u
#define LIBMH_EV_MAP_HOTKEY_TOGGLE     0x17u
#define LIBMH_EV_PANEL_REFRESH_CURRENT 0x18u

/* Issue one order by INTENT. `order_id` is a LIBMH_ORD_* member (libmh_host_in.gen.h); `argv`
 * carries that order's committed parameters in declaration order, widened to int32_t. libmh
 * builds the wire record and enqueues it.
 *   >= 0               enqueued
 *   LIBMH_IN_E_ARG     order_id out of range, or argv null with argc != 0
 *   LIBMH_IN_E_ARITY   argc != libmh_order_arity(order_id)
 *   LIBMH_IN_E_UNBOUND the dispatch slot is NULL (the trap has already named it) */
int32_t libmh_issue_order(uint32_t order_id, const int32_t *argv, uint32_t argc);

/* The declared arity of an order id, or -1 -- lets a binding generator emit typed wrappers. */
int32_t libmh_order_arity(uint32_t order_id);

/* The order id's stable name (the owned body it issues), or NULL. */
const char *libmh_order_name(uint32_t order_id);

/* Drain the local peer's newly-issued order records for transmission, in issue order, as wire
 * bytes -- the same encoding libmh_submit_order accepts.
 *
 * NOT DERIVED FROM A CALL EDGE, and recorded as such: the row derivation is a call-graph
 * measurement and is blind to a caller that reaches libmh's state through a REGION instead.
 * llm_net_send_order is exactly that -- it reads the order staging area directly, which works
 * today only because the host and libmh share .bss. At the fork the host's transport has no such
 * access, so without this an order issued locally could never be sent. Draining is destructive.
 * Returns the number of records written; *out_bytes (optional) gets the byte count. */
uint32_t libmh_drain_outbound_orders(void *buf, size_t cap, size_t *out_bytes);

/* ---- the read model -------------------------------------------------------------------- */

/* EVERY GETTER AND COMMAND BELOW IS AN EXACT POSITIONAL MIRROR of the owned body's COMMITTED
 * prototype (tools/data/dll_call_protos.json) -- same order, same widths, same return type,
 * including where that return is a `double`. Nothing is renamed into a tidier shape: the
 * implementation TU forwards straight into the C++ wrapper, so the COMPILER is what proves the
 * mirror, and a "cleaner" signature here would be a translation nobody checks. Where the design
 * sketch guessed a signature and the measurement disagreed, the measurement won -- those are the
 * sign-off deltas in the design doc, each with the body it was measured against. */

/* Map geometry. libmh owns the torus/wrap convention (SIMABI-MAPIO: sim_lt_map_setup_dimensions
 * is ours and re-derives the masks), so a host must ask rather than reimplement the wrap. */
void    libmh_map_pixel_delta(int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                              int32_t *out_dx, int32_t *out_dy);
void    libmh_map_tile_delta(int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                             int32_t *out_dx, int32_t *out_dy);
int32_t libmh_map_tile_dist(int32_t x1, int32_t y1, int32_t x2, int32_t y2);

/* Units. */
void    libmh_unit_get_coords(uint16_t player, int32_t unit_index, int32_t *out_x, int32_t *out_y);
int32_t libmh_unit_is_boarding(int32_t state);

/* Buildings. libmh_bldg_slot_has_soldiers takes the building index ALONE -- the committed
 * prototype has no player parameter, so the design sketch's (player, index) was a guess. */
void     libmh_bldg_get_coords(uint16_t player, int32_t building_index,
                               int32_t *out_x, int32_t *out_y);
int32_t  libmh_bldg_uses_workers(uint32_t player, int32_t building_index);
int32_t  libmh_bldg_shuttle_slot_is_free(int32_t player, int32_t building_id);
int32_t  libmh_bldg_footprint_is_clear(int32_t x, int32_t y, int32_t building_type,
                                       uint32_t viewer);
int32_t  libmh_bldg_is_network_critical(int32_t player, int32_t b_index);
int32_t  libmh_bldg_slot_has_soldiers(int32_t building_index);
uint32_t libmh_locate_active_port(uint32_t player, int32_t *out_col, int32_t *out_row,
                                  uint32_t *out_port_slot);

/* Production / planets. `double` crosses here three times because the originals genuinely return
 * one; libmh's answer to the x87 precision question is a GRANTED guarantee -- PC=53, installed by
 * libmh itself (libmh.h's floating-point contract) -- so a host on another toolchain gets the same
 * bits rather than the same source.
 *
 * libmh_planet_distance is the PIXEL distance between two map points, not a planet-pair lookup:
 * its committed prototype is (x1, y1, x2, y2) -> double. libmh_planet_distance_factor IS the
 * planet-pair one. The two names look like a pair and are not one. */
double libmh_planet_distance(int32_t x1, int32_t y1, int32_t x2, int32_t y2);
double libmh_planet_distance_factor(int32_t src_planet, int32_t dest_planet);
double libmh_prod_transfer_progress(int32_t slot);

/* ---- control groups (the LOCAL half -- see the banner's note (2)) ----------------------- */

/* `count_io` is the original's in/out counter pointer, kept rather than hidden: both bodies are
 * __mh_watcall_ebx_volatile callees whose generated thunk owns the register contract, and the
 * hosted adapters forward positionally into exactly this shape. */
int32_t libmh_ctrlgroup_contains(uint32_t unit_id, int32_t count, int32_t group_index);
void    libmh_ctrlgroup_add_member(int32_t unit_id, int32_t *count_io, int32_t group_index);
void    libmh_ctrlgroup_remove_member(uint32_t unit_index, int32_t *count_io, int32_t group_index);
void    libmh_ctrlgroup_assign(int32_t unit_id, int32_t new_group_id);

/* ---- building / production commands ----------------------------------------------------- */

void    libmh_bldg_finish_current_order(uint32_t player, uint32_t building_index);
int32_t libmh_bldg_begin_placement(uint16_t player_idx, int32_t building_idx);

/* Seed the per-building-TYPE defaults for every defined cfg Building. A BOOT-SEQUENCE entry, not a
 * command: the original is boot stage 7 (llm_strat_bldg_init_all @0x0045a017, called once by
 * llm_boot_stage_tick), and it walks cfg Building[1..99] calling the per-type seeder for each entry
 * whose `type` is not UNDEFINED. Added 2026-09-12 by X-TL-DRAIN, which translated the body -- the
 * inbound edge did not appear, it MOVED UP: llm_strat_bldg_init_defaults was the libmh-owned body
 * before, excluded X-TL because its only caller was still original, and translating that caller
 * turned the pair into one internal call plus one host entry.
 *
 * ITS OWN ENTRY RATHER THAN libmh_strat_mode_init, and the distinction is the caller, not the
 * flavour: the five bodies that entry covers are all called by llm_strat_mode_init @0x0045f027 in
 * one measured sequence, and this one is not -- it runs at program boot, before any mode. Folding it
 * into that entry would have the host call it at the wrong moment. */
void    libmh_bldg_init_all(void);
void    libmh_storage_purge_dead_docked(int32_t player, int32_t storage_sub_id);
void    libmh_game_player_set_human(uint8_t player);
int32_t libmh_prod_set_transfer_destination(uint32_t player_idx, int32_t prod_slot,
                                            int32_t dest_planet);
void    libmh_prod_shuttle_slot_spawn_arrival(uint32_t slot_index);
void    libmh_progress_recheck_all(void);
int32_t libmh_deploy_starting_squad(void);

/* The acknowledgement VOICE, not an order -- it is the sound a group makes when it accepts one,
 * which is why it has its own entry instead of an order id. */
void libmh_order_ack_voice(void);

/* Hand the strategic session over to a tactical mission. The mission itself is FROZEN with its own
 * host table (LIB-IFACE-SPLIT); this is the strategic side's handover, which is ours. */
void libmh_enter_tactical_mission(uint32_t player, uint32_t bldg_idx, uint32_t param_3);

/* ---- session lifecycle ------------------------------------------------------------------ */

/* ---- the two SEQUENCE entries, and what they cost -----------------------------------------
 *
 * The two below are the only entries that cover several owned bodies because ONE ORIGINAL BODY
 * SEQUENCES THEM. That adjacency is the whole merge rule on this surface: an entry covers more
 * than one body only where a single original body runs them in order, never merely because they
 * feel related -- which is why four other rev-1 merges became per-body mirrors on measurement.
 *
 * THEY ARE ALSO THE TWO ENTRIES NO HOSTED RUN REACHES, and that is structural rather than a gap
 * in coverage. Each constituent body keeps its own promotion seam, and a per-row seam cannot call
 * a sequence entry without also executing its siblings -- so these two stay UNROUTED by design
 * (gen_libmh_inbound.py prints them every run as `exception: sequence`). The consequence is
 * recorded rather than glossed: the bodies are exercised by the gate through their own seams, and
 * both entries are proven to link, declare and walk -- but the COMPOSITION, that they call those
 * bodies in the right order, rests on the two disassemblies cited below and on nothing else.
 *
 * CONSIDERED AND DECLINED (2026-09-11): splitting these into per-body mirrors and keeping the
 * sequence entry as their composition would make the composition hosted-provable, at the cost of
 * growing a signed-off surface from 45 entries to 52 for marginal value. The option stands if a
 * fork host ever needs per-body granularity.
 */

/* Reset the per-session globals. ONE entry over two owned bodies (the strategic session reset and
 * the net one) because llm_game_init_subsystems @0x004261bb calls them ADJACENTLY, in that order,
 * passing 0 for the flag. */
void libmh_session_globals_reset(uint32_t reset_flag);

/* The strategic mode's init sequence. ONE entry covering five owned bodies, in this order:
 * tech_tables_reset, new_game_init, map_FillDefaults, pathfinder_init,
 * player_param_defaults_init -- because llm_strat_mode_init @0x0045f027 calls them in exactly that
 * order. It interleaves twelve more calls (cursor/blend tables, text pointers, cheats) that the
 * fork's host owns, which is why they are not in here. See the sequence-entry note above for what
 * this entry's ordering rests on. */
void libmh_strat_mode_init(void);

/* Beginning a session is THREE entries, not one: the original has three entry points with three
 * different callers and three different signatures -- the single-player campaign begin, the
 * multiplayer/skirmish one (carrying the lobby's opaque scenario config blob), and the map-session
 * init the planet loader runs. A merged (planet_index, multi) entry would have been a shape none
 * of the three bodies has. */
void    libmh_planet_session_begin(int32_t race, int32_t reset_flag);
int32_t libmh_planet_session_begin_multi(const void *cfg_blob);
void    libmh_planet_map_session_init(void);

void    libmh_planet_transition_finalize(void);
int32_t libmh_planet_switch(int32_t planet_index);

/* ---- the clock --------------------------------------------------------------------------- */

/* THREE mirrors, not one libmh_clock_advance. The original's per-frame driver,
 * llm_strat_sim_clock_advance @0x0044d238, calls libmh_clock_reload_resync's body unconditionally
 * with the game clock, and libmh_clock_rebase's body ONLY on the >300 s catch-up branch after
 * advancing the clock by the excess -- so the two are not a sequence, and a merged entry would
 * have had libmh reimplement that driver, a body libmh does not own. The third takes no argument
 * at all (it reads the wall clock itself, through the host api) and is reached from seven callers
 * that driver never touches. The fork's host composes them the way the driver does. */
void libmh_clock_reload_resync(double now);
void libmh_clock_rebase(double now);
void libmh_clock_resync_and_tick(void);

/* ---- load fixups --------------------------------------------------------------------------- */

/* Two entries, not one post-load fixup. Both bodies do have a single caller, llm_game_load
 * @0x004475de -- but the alert reset runs early in the load and the ambient reseed runs at the
 * tail, after the file read and map_LoadPlanetFromDisk, so they are not a sequence either. */
void libmh_invasion_alerts_reset(void);
void libmh_ambient_reseed(int32_t planet_index, double now);

/* ---- net / lockstep --------------------------------------------------------------------- */

/* Resolve a wire side id to a player index -- 9 call sites in the original transport. */
int32_t libmh_player_index_by_side(int32_t side_id);

/* Tell libmh a player is gone (drop / timeout / leave). Returns the original's own value. */
uint32_t libmh_player_presence_lost(uint32_t player, uint32_t mode);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LIBMH_HOST_IN_H */
