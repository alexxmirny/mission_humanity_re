//
// orders/issue/issue_state.h -- the ORDER-ISSUE domain's state interface (RI-ORDERS / O4-0).
//
// The domain is the 112 wrappers that write into the sim from OUTSIDE it by putting one order into
// the container. RI-ORDERS owns the container (order_queue.h, promoted); this is the layer above:
// the functions that decide WHICH order, pack its arguments, and hand it over.
//
// WHY THIS FILE EXISTS AT ALL, i.e. what the domain's "state interface" has to be.
//
// A wrapper's whole observable is its EMISSION -- the record it hands the container, and which lane
// it hands it down. Everything else it does is read game state to decide whether, and with what, to
// emit. So the interface splits exactly there:
//
//   `issue_view`  -- the game state a wrapper READS (and, for six of them, writes). Bound from the
//                    region registry, never from a spelled address, for the same reason the container
//                    does it (RI-STATE / ST1: one derivation per region, or two consumers of the same
//                    bytes can disagree about where they are).
//   `order_sink`  -- the FOUR container entry points, indirected through function pointers.
//   `issue_calls` -- every OTHER original this domain calls out to, same convention as the
//                    container's `game_calls`.
//
// THE INDIRECTION ON `order_sink` IS THE WHOLE ORACLE. `net_selftest issuetest` binds a RECORDING
// sink, drives a wrapper with concrete inputs, and compares what arrived against a golden extracted
// from the ORIGINAL's disassembly (order_issue_golden.gen.h). Without the indirection the emission
// is unobservable without a running game, which is precisely why this domain had no oracle and why
// Law 5 forbade promoting any of it. In production the sink binds to mh::orders::* -- OUR container,
// already promoted -- so the indirection costs one inlined call and buys the only proof there is
// that a reimplemented wrapper packs the same order as the original.
//
// EVERY WRAPPER TAKES (view, sink, calls) AS ITS FIRST THREE PARAMETERS, with a public one-line
// overload applying live_view()/live_sink()/live_calls(). Same split, and for the same reason, as
// mh::orders::detail::* vs its public wrappers.
//
// THE PILOT'S SEVEN TAKE ONLY (view, sink) and are left that way deliberately. They were written
// while `issue_calls` was an empty placeholder -- none of them makes an outward call at all -- so a
// third parameter would be a parameter no body could use. The O4A-C sweep (2026-08-28) populated
// `issue_calls` from the whole domain's call-target scan and fixed the three-parameter shape for
// everything after; churning the seven would have rewritten every one of their oracle rows to buy
// uniformity in a file that documents the split anyway. New wrappers take all three, always, even
// when the body uses one -- name the unused ones away (`const issue_view &`).
//
// THE VIEW GROWS PER BATCH, DELIBERATELY. It binds what the translated rows actually read, not every
// region the domain might eventually touch: an unbound member is a compile error at the use site,
// whereas a member bound "just in case" is a silent claim that some row was checked against it. Add a
// member in the slice that first needs it, and say so in that slice's report. The O4A-C sweep
// (2026-08-28) grew it in ONE step because its slice is the whole remaining domain -- the rule is
// unchanged, the slice simply is everything left.
//
// WHERE THE WRAPPER DECLARATIONS LIVE, changed by that sweep. The pilot declared all seven of its
// wrappers here. 52 more would make this file the serialisation point of every translation, so the
// sweep follows the SIM domain's shape instead: each translation unit ships its own
// `orders/issue/issue_<family>.h` declaring its own detail:: + public forms, including only this
// header, and issue_state.cpp includes them to reach their install_shadow_* hooks. The pilot's seven
// declarations stay below rather than being churned. The containment rule is untouched -- a unit
// header may include issue_state.h and nothing else, so a wrapper TU still cannot spell an address.
//
#pragma once
#include <cstdint>

#include "addr/mh_structs.gen.h"
#include "state/roster_caps.h" // SB-BIND T2: derived per-player row capacities

namespace mh::orders::issue {

// map::object::building[8][100], stride 0x111 (RID_BUILDINGS @0x00c3d2a0). The wrappers index it
// as buildings[player * BUILDINGS_PER_PLAYER + index]; the original does the same arithmetic with
// `IMUL EAX,player,0x6aa4` (== 100 * 0x111) plus `IMUL EDX,index,0x111`.
// SB-BIND T2 (2026-09-06): these are the STOCK values and nothing indexes with them any more.
// Production index expressions read the DERIVED capacities off the view/store (`v.caps.units`,
// `caps_.buildings`, ...), which follow whatever the host bound; these survive only as the offline
// fixtures' default and as the documentation of the stock strides. Adding a new production use of
// one is a regression -- it would pin that one site to 100 while every other site followed a
// cap-raised host, which is worse than uniform staleness. See state/roster_caps.h.
inline constexpr int32_t BUILDINGS_PER_PLAYER = mh::state::STOCK_ROSTER_CAPS.buildings;
inline constexpr int32_t UNITS_PER_PLAYER     = mh::state::STOCK_ROSTER_CAPS.units;
inline constexpr int32_t MAX_PLAYERS          = 8;

// The kind nibble every wrapper ORs into the owner byte before handing it over. The container reads
// it back out at dispatch time to pick a lane-specific switch, so it is part of the emission and the
// oracle compares it -- it is NOT decoration on the player id.
//   0x40  building-targeted   0x80  unit-targeted   0x20  unit-targeted, second form
// (Read off the wrappers themselves -- `OR AL,0x40` -- and cross-checked against the dispatcher's
// own routing in the order matrix. Named here rather than left as magic numbers at 68 sites.)
inline constexpr uint32_t KIND_BLDG   = 0x40;
inline constexpr uint32_t KIND_UNIT   = 0x80;
inline constexpr uint32_t KIND_UNIT_2 = 0x20;

// ---- what the wrappers READ (and, for six of them, WRITE) --------------------------------------
//
// GROWN WHOLE FOR THE O4A-C SWEEP (2026-08-28). The pilot's rule was "bind what the translated rows
// actually read, add a member in the slice that first needs it" -- and this slice IS the rest of the
// domain, all 52 remaining rows at once, so every member below is one a mechanical scan of those 52
// listings found referenced. Nothing here is speculative: the scan reads the .asm operand comments,
// and a member no row reads would be a silent claim that some row was checked against it.
//
// CONST-NESS IS THE MIGRATION EVIDENCE, not a style choice. A non-const member below is written by a
// named row and by no other; the manifest's `writes_shared` / `writes_island` columns are the source
// and they agree with the listings.
struct issue_view {
    mh::game::mh_map_object_building *buildings; // [MAX_PLAYERS][BUILDINGS_PER_PLAYER], RID_BUILDINGS

    // SB-BIND T2: the per-player ROW CAPACITIES, derived from the sizes the host bound. Stock
    // default so a hand-built fixture view is unchanged; the binder overwrites it. Index with
    // these, never with the constants above (docs/state-boundary.md D6.3).
    mh::state::roster_caps caps{mh::state::STOCK_ROSTER_CAPS};
    // cfg_final_struct_Building[] (RID_BUILDING @0x00d9ec80, stride 0x842), indexed by a live
    // building's `building_id`. The per-TYPE config table, not the per-instance array above --
    // repair_cycle_start reads BOTH and compares one against the other.
    const mh::game::mh_cfg_final_struct_Building *cfg_buildings;

    // ---- the unit side (batch B) ----
    // map_object_unit[8][100], stride 0xe9 (RID_UNITS @0x00dd8c48). Indexed exactly as `buildings`
    // is, but with the player half scaled by 0x5b04 (== 100 * 0xe9): the unit wrappers do
    // `MOVZX EAX,word [player]` / `IMUL EAX,EAX,0x5b04` / `IMUL EDX,[index],0xe9` / `ADD`.
    // NON-CONST since 2026-08-28 (O4B) and by exactly ONE row, which is what the const-ness rule
    // above requires it to say: llm_strat_ai_group_move_formation_rotating stamps
    // units[p][id].order_notify_status = 1 and .order_status_flags |= 0x40 for every member it moves
    // (issue_group_formation_move.h). Those two fields are order-pipeline state, not unit state at
    // large -- llm_strat_unit_notify_status owns them in the sim (sim/sim_unit_notify.cpp writes both
    // at six sites) and the AI only READS them. No other row in this domain writes through this
    // pointer, and `buildings` above is non-const on the same terms.
    mh::game::mh_map_object_unit *units;
    // cfg_final_struct_Unit[] (RID_UNIT @0x00e4a098, stride 0x23f), indexed by a live unit's
    // `unit_proto_id`. The per-TYPE table: the move wrappers read default op-code bytes out of it to
    // decide which order code to pack.
    const mh::game::mh_cfg_final_struct_Unit *cfg_units;
    // map_object_unit_storage[] (RID_UNIT_STORAGE @0x00c727c0) -- the exit tiles the storage-exit
    // wrappers read.
    const mh::game::mh_map_object_unit_storage *unit_storage;
    // The map dimension masks (RID_GENERAL @0x00e15390): width_mask / height_mask / bh_mask, used for
    // the toroidal wrap in the relative-move arithmetic.
    const mh::game::mh_llm_strat_map_geom *geom;
    // The selection control groups (RID_STRAT_CTRL_GROUPS @0x00b63be0) the group wrappers iterate.
    const mh::game::mh_llm_strat_ctrl_group *ctrl_groups;

    // ---- scalars ----
    const uint16_t *player_side;  // RID_PLAYERSIDE -- the LOCAL player's slot
    const int32_t  *session_mode; // RID_GAME_SESSION_MODE -- == 3 is the multiplayer regime, and
                                  // several wrappers emit a SECOND order only in it
    const int32_t *planet_index;  // RID_G_PLANET_INDEX
    const int32_t *player_race;   // RID_STRAT_PLAYER_RACE -- selects the ack-voice sound triple
    const double  *game_clock;    // RID_STRAT_GAME_CLOCK -- what the ack-voice cooldown compares to
    const uint8_t *key_lalt_held; // RID_KEY_LALT_HELD -- a MODIFIER read, not an input event

    // ---- the AI group-relocation scratch (batch B, 2026-08-28 / O4B) ----
    // The member-id list an AI group task PUBLISHES before calling its mover, and its live count
    // (RID_STRAT_AI_GROUP_UNIT_SCRATCH_LIST @0x00e69ed0, int32[100] / _COUNT @0x00fe5b48). READ-ONLY
    // here: llm_strat_ai_group_move_formation_rotating is a CONSUMER of the list its AI caller filled
    // (libmh/ai/ai_group_task_formation.cpp's harvest_members_into_scratch), which is why an order-issue
    // wrapper reads two AI-owned regions at all. It re-reads the COUNT every iteration -- see the
    // wrapper's header.
    const int32_t *ai_group_scratch_list;
    const int32_t *ai_group_scratch_count;

    // ---- the passable-tile spiral search (batch B, 2026-08-28) ----
    // Read by llm_strat_ai_group_scatter_to_passable_tile, which walks outward from an anchor until
    // it finds a passable tile for each published member. All four are READ-ONLY here.
    // byte[256][256] COLUMN-major, tile index (x << 8) | y (RID_PASSABLE @0x00b64bb0). The scatter
    // spirals while the indexed byte is ZERO, i.e. it stops on the first NON-zero cell.
    const uint8_t *passable;
    // llm_strat_ai_spiral_offset[] (RID_STRAT_AI_TILE_SPIRAL_OFFSETS @0x00fc59b0) -- (dx, dy) SIGNED
    // byte pairs enumerating tiles in rings of increasing radius, built once by
    // llm_strat_ai_spiral_table_init and read-only afterwards. Indexed here as a raw byte pair
    // (`MOVSX .. [ESI*2 + table]` / `[ESI*2 + table + 1]`), so the stride is 2.
    const int8_t *ai_tile_spiral_offsets;
    // The AI scanners' OWN torus wrap masks -- width-1 / height-1, NOT the dimensions
    // (RID_WIDTH_M @0x00fe5b40, RID_HEIGHT_M @0x00fe5b44). DISTINCT from `geom`'s width_mask /
    // height_mask above: mh_addrs.gen.h records these as two different torus masks in the image,
    // not a duplicate, and the scatter reads THESE.
    const uint32_t *width_m;
    const uint32_t *height_m;

    // ---- the ack-voice constants. Registered by this sweep: they are READ-ONLY, so O4-READY's
    // write-set pass could not see them, and a translation may not spell an address. ----
    const double  *ack_voice_cooldown_sec;   // RID_STRAT_ORDER_ACK_VOICE_COOLDOWN_SEC, 2.0
    const int32_t *ack_voice_snd_id_by_race; // RID_STRAT_ORDER_ACK_VOICE_SND_ID_BY_RACE, int[6]

    // ---- the two debug format strings llm_strat_order_set_player_control_mode hands w_sprintf, and
    // the scratch buffer it formats into. The GAME's own pointers, not copies of the text. ----
    const wchar_t *fmt_control_mode_clear; // RID_U___D___D_005011CC
    const wchar_t *fmt_control_mode_set;   // RID_U___D___D_00501250
    void          *text_tmp;               // RID_G_TEXT_TMP -- the 512 B format scratch

    // ---- WRITTEN state (non-const deliberately; see the note above) ----
    // RID_STRAT_ORDER_SEQ_ID_BY_PLAYER, uint8[8]. THE WRITERS OF THIS ARE WHAT SB5 IS WAITING ON;
    // three are batch B rows (group_issue_move_order_confirmed / _deferred, unit_order_exit_storage)
    // and the fourth is the AI domain's llm_strat_ai_commit_attack_order_alt. It is a monotonic
    // per-player stamp, and an UNDECLARED one is the classic `ours = original + 1` divergence --
    // this exact region cost a rig run that way.
    uint8_t *order_seq_id_by_player;
    // RID_PLAYER_CONTROL_MASK, one byte of per-player control bits. Read AND written (AND/OR of a
    // shifted bit) by llm_strat_order_set_player_control_mode, its only accessor in this domain.
    uint8_t *player_control_mask;
    // The three UI hand-off globals llm_strat_bldg_order_depart_confirm_dispatch consumes and CLEARS
    // (RID_STRAT_UI_DEPART_PENDING_BLDG_A / _B, RID_STRAT_UI_PLANET_SEL_ACTION_TARGET).
    int32_t *ui_depart_pending_bldg_a;
    int32_t *ui_depart_pending_bldg_b;
    int32_t *ui_planet_sel_action_target;
    // The ack-voice throttle's own state, written by llm_strat_group_order_ack_voice alone.
    double  *ack_voice_last_play_time;   // RID_STRAT_ORDER_ACK_VOICE_LAST_PLAY_TIME
    int32_t *ack_voice_suppressed_count; // RID_STRAT_ORDER_ACK_VOICE_SUPPRESSED_COUNT
};

// ---- the container, indirected ------------------------------------------------------------------
//
// Signatures are the container's own (order_queue.h), not the original's register contract: by the
// time a wrapper is reimplemented it is C++ calling C++, and the marshalling the original needed is
// the container's promotion seam's problem, not this layer's.
struct order_sink {
    void (*scratch_reset)();                                 // llm_strat_order_scratch_reset
    void (*scratch_set_field)(int32_t index, int32_t value); // llm_strat_order_scratch_set_field
    // llm_strat_order_dispatch -- the REPLICATED lane's router. NOTE the argument renaming the
    // container documents: (unit_id, player, op_code, arg) become the record's
    // (unit_index, owner_and_kind, param0, order_code). The wrappers pass them in that order.
    int32_t (*dispatch)(uint16_t unit_id, uint32_t player, uint16_t op_code, uint16_t arg);
    // llm_strat_order_enqueue -- the IMMEDIATE lane, bypassing the router.
    int32_t (*enqueue)(uint16_t unit_index, uint16_t owner_and_kind, int16_t param0,
                       uint16_t order_code);
};

// ---- everything else this domain calls out to ---------------------------------------------------
//
// Same convention as the container's `game_calls`: one function pointer per original, so the oracle
// can bind recorders and a shadow arm can bind the inert set. Filled out by the O4A-C sweep from a
// mechanical call-target scan of all 52 remaining listings -- these are every non-container, non-CRT
// target the domain reaches, minus `utils_assert_stack_capacity` (a stack probe with no observable
// effect, deliberately not modelled) and minus the wrappers that call each OTHER, which are ours and
// are called directly.
//
// EXACTLY ONE MEMBER IS EFFECTFUL. `snd_play` is `class: effectful` in
// tools/data/orders_issue_effect_classes.json (a device write, verified against the body): under a
// shadow arm it fires TWICE and the state comparison still reads clean. Its two callers here are
// group_order_ack_voice and unit_order_move_confirmed_with_bump. Every other member is a pure query
// or an out-pointer helper -- stubbing THOSE would make a run pass vacuously, so the inert set below
// silences the sound and leaves the rest alone.
struct issue_calls {
    // -- EFFECTFUL --
    void (*snd_play)(int32_t sound_id, int32_t volume); // llm_snd_play @0x00425233

    // -- pure / query --
    int32_t (*rand_below_fx)(uint32_t bound);                                   // @0x00499f84 -- int32 return since the 2026-09-02
                                                                                // re-dump healed the committed prototype (LT1A)
    int32_t (*target_class)(uint32_t owner_and_kind_flag, int32_t roster_slot); // @0x004495f9
    int32_t (*unit_state_is_boarding)(int32_t state);                           // @0x004967ce
    uint32_t (*unit_in_weapon_range)(int32_t a, int32_t b, int32_t c, int32_t d,
                                     int32_t e); // @0x0044965d
    uint8_t (*unit_select_weapon)(uint16_t player, int32_t unit_index,
                                  uint32_t target_mask); // @0x0048ba9e
    int32_t (*storage_type_accepts_unit)(uint32_t building_index,
                                         uint16_t unit_index); // @0x00497b91
    int32_t (*bldg_placement_check_and_preview)(int32_t origin_x, int32_t origin_y,
                                                int32_t building_index); // @0x00453a6d

    // -- out-pointer helpers: they write only through the pointers the caller supplies --
    void (*unit_get_coords)(uint16_t player, int32_t unit_index, int32_t *out_x,
                            int32_t *out_y); // @0x0044b141
    void (*bldg_get_coords)(uint16_t player, int32_t building_index, int32_t *out_x,
                            int32_t *out_y); // @0x00449b8a
    void (*storage_get_approach_tile)(uint16_t a, uint16_t b, uint32_t *out_x, uint32_t *out_y,
                                      uint32_t mode); // @0x0048b37c
    void (*bldg_calc_placement_corner_from_center)(uint16_t unit_index, int32_t center_x,
                                                   int32_t center_y, uint32_t *out_col,
                                                   uint32_t *out_row); // @0x0048d054
    void (*tile_delta_wrapped)(int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t *out_dx,
                               int32_t *out_dy); // @0x004941b9

    // -- writes sim state through the ORIGINAL, and stays original under Law 4 --
    void (*unit_notify_status)(uint32_t player, int32_t unit_index,
                               uint32_t status_code); // @0x004dae0e
    // @0x0046a0d5. THE SINK OF llm_strat_ai_group_move_formation_rotating, and the reason that row
    // has no generated golden case: it is one level ABOVE the container, so
    // gen_order_issue_golden.py -- whose roots are llm_strat_order_dispatch / llm_strat_order_enqueue
    // -- records zero sites for the wrapper and its emission is observed HERE instead, through this
    // pointer, by a hand-authored issuetest recorder.
    // It is a `sim` ledger row (batch C, verified) but binds to the ORIGINAL like every other member
    // of this struct: this domain reaches non-sibling callees through mh::call::, and only the
    // CONTAINER (order_sink) binds to ours.
    // `move_flag` is the committed prototype's name for the 5th (stack) argument; what this caller
    // actually passes is the per-player ORDER SEQUENCE ID, which lands in the move order's args[0xc]
    // and becomes map_object_unit::move_group_id. Name kept as committed rather than renamed here.
    void (*unit_order_move_enqueue)(uint16_t player, int32_t unit_idx, uint32_t x, uint32_t y,
                                    uint32_t move_flag); // @0x0046a0d5

    // -- the debug line. `w_sprintf__vii` is a varargs SHAPE (tools/data/varargs_shapes.json),
    //    measured at this domain's two sites; the base symbol stays uncallable. --
    int32_t (*sprintf_ii)(void *dst, const wchar_t *format, int32_t a0, int32_t a1); // @0x004d0320
};

// Every EFFECTFUL outward call turned into a no-op; the pure ones still run. THE SHADOW ARM MUST USE
// THIS -- mirrors mh::orders::inert_calls(), and the .cpp says which members it changes and why
// stubbing the rest would be worse than not stubbing at all.
const issue_calls &inert_calls();


// O4A verification-debt drain (2026-08-28): the one debt-pilot wrapper that is SIM-rooted and
// therefore RIG-armable. Declared here (not local to issue_bldg_orders.cpp) so install_shadow()
// in issue_state.cpp -- conductor-owned -- can call it.

// The production bindings: the region registry, our promoted container, the generated thunks.
const issue_view  &live_view();
const order_sink  &live_sink();
const issue_calls &live_calls();

// ---- the wrappers -------------------------------------------------------------------------------
//
// Each is declared twice: the explicit-state form the oracle drives, and the public form the game
// calls. Grouped by the batch that owns them.

namespace detail {

// --- O4A, pilot slice (2026-08-27) ---
// Order 0x80/0x80, kind 0x40. Sets the building's field_0x4 bit-2 flag; pairs with deactivate.
void bldg_order_activate(const issue_view &v, const order_sink &s, uint32_t player,
                         uint16_t bldg_idx);
// Order 0x81/0x81, kind 0x40, IMMEDIATE lane -- the `_enqueue` twin of bldg_order_deactivate.
void bldg_order_deactivate_enqueue(const issue_view &v, const order_sink &s, uint32_t player,
                                   uint16_t bldg_idx);
// Order 0x6d/0x6d, kind 0x40, GUARDED on buildings[..].online_state != 0. args[0] = unit_type,
// args[1] = 1 (a hardcoded constant -- the `_count` sibling generalises it).
void bldg_order_production_add(const issue_view &v, const order_sink &s, uint32_t player,
                               int32_t bldg_idx, int32_t unit_type);
// Order 0x6f/0x6f, kind 0x40. A DIFFERENT guard from its sibling above -- `state != 100`, not
// `online_state != 0` -- and only ONE scratch slot, args[0]. The two read as a pair and are not one.
void bldg_order_production_remove(const issue_view &v, const order_sink &s, uint32_t player,
                                  int32_t bldg_idx, int32_t unit_type);
// Order 0xdd/0xdd, kind 0x40. Two scratch slots, written 3 then 2.
void bldg_order_load_resource(const issue_view &v, const order_sink &s, uint32_t player,
                              int32_t bldg_idx, int32_t arg3, int32_t arg2);
// The repair cycle's entry point, and the branchy member of the pilot.
void bldg_order_repair_cycle_start(const issue_view &v, const order_sink &s, uint32_t player,
                                   int32_t bldg_idx);

// --- O4C, pilot slice (2026-08-27) -- see issue_order_core.cpp for why one batch-C row is here ---
// param0 0xf7 / order_code 0xf8 -- the ONLY pilot where the two DIFFER, and the only one that calls
// scratch_reset. Owner is `player & 0xf`, with no kind nibble OR'd in at all.
void order_debug_kill_group(const issue_view &v, const order_sink &s, uint32_t player_id,
                            int32_t damage_amount);

} // namespace detail

void bldg_order_activate(uint32_t player, uint16_t bldg_idx);
void bldg_order_deactivate_enqueue(uint32_t player, uint16_t bldg_idx);
void bldg_order_production_add(uint32_t player, int32_t bldg_idx, int32_t unit_type);
void bldg_order_production_remove(uint32_t player, int32_t bldg_idx, int32_t unit_type);
void bldg_order_load_resource(uint32_t player, int32_t bldg_idx, int32_t arg3, int32_t arg2);
void bldg_order_repair_cycle_start(uint32_t player, int32_t bldg_idx);
void order_debug_kill_group(uint32_t player_id, int32_t damage_amount);

} // namespace mh::orders::issue
