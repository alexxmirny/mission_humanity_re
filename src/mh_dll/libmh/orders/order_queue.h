//
// orders/order_queue.h -- the order-queue CONTAINER, reimplemented (RI-ORDERS / O2).
//
// The first real reimplementation module, as opposed to the P0 proof seams. Closure analysis and the
// containment proof are in the order-container notes; the operating procedure is the `reimpl-loop`
// skill. Scope is deliberately the container only -- putting records into the three arrays and
// taking them out. The dispatch HANDLERS that interpret an order's payload are the sim and belong
// to RI-SIM.
//
// THE CONTAINER'S INTERFACE IS A PROTOCOL, not a call:
//
//     scratch_reset();                       // all 13 slots := ARG_UNCHANGED
//     scratch_set_field(i, v);               // as many as this order needs
//     enqueue(unit, owner_and_kind, p0, code);   // immediate lane, or
//     pending_enqueue(order);                    // the net-delivered lane
//
// `enqueue` copies all 13 scratch slots into the record's args[]. The scratch buffer is a global,
// not a parameter -- which is why O1 initially concluded the interface was "four scalars in".
//
// LAW 1 APPLIES HARD HERE. The QUEUE is part of the planet SAVE FORMAT (Save/LoadPlanetFromDisk take
// its base address) and the still-original llm_strat_order_queue_dispatch reads it 132 times. The
// array addresses, the 0x44 stride and the field order are FROZEN while either remains. That is why
// this module reads its addresses from addr/mh_addrs.gen.h and its record layout from
// addr/mh_structs.gen.h rather than declaring either locally: a Ghidra-side change to a field offset
// becomes a COMPILE error (static_assert) instead of silent save-file corruption.
//
#pragma once
#include <cstdint>

#include "addr/mh_structs.gen.h"
#include "state/roster_caps.h" // SB-BIND T2: derived per-player row capacities
#include "include/packet_buffer.h"
#include "state/region_owner.h"
#include "state/state_sink.h"

namespace mh::orders {

using order = mh::game::mh_llm_strat_order; // 0x44 bytes, offsets asserted against Ghidra

inline constexpr int32_t QUEUE_CAP     = 300;  // CMP [QUEUE_COUNT],0x12c
inline constexpr int32_t PENDING_CAP   = 1000; // CMP [PENDING_COUNT],0x3e8
inline constexpr int32_t STAGING_CAP   = 300;  // CMP [STAGING_COUNT],0x12c
inline constexpr int     ARG_SLOTS     = 13;   // CMP [i],0xd
inline constexpr int32_t ARG_UNCHANGED = -1;   // the sentinel scratch_reset writes
inline constexpr int32_t MAX_PLAYERS   = 8;    // CMP [player],0x8 in release_due

// llm_strat_player_profile.status_flags bit1 -- "alive / has presence". release_due pumps only
// players with it set (TEST byte ptr [player*0x740 + PLAYERS],0x2 @0x00466577). It is SIM-owned;
// see the field comment in addr/mh_structs.gen.h for who writes it and why a divergence is both a
// pacing change and a lockstep checksum mismatch.
// (the field is the 4-byte E_STRAT_PLAYER_STATUS enum; the original tests its LOW BYTE with
// `TEST byte ptr [...],0x2`, which for bit 1 is the same answer as masking the whole word)
inline constexpr uint32_t PLAYER_ALIVE = 0x02;

// The outbound order batch's flush rule (`cursor + 0x45 >= 0x3f8`, llm_strat_order_schedule
// @0x004664ba) now lives with the buffer it is about: mh::net::packet_buffer::order_would_overflow(),
// over ORDER_RECORD_BYTES and CAPACITY. It used to be two loose constants here (RI-WIRE W2).

// order_dispatch's router test (0x00466000-0x0046601e). `Players` is a SECOND, much smaller player
// table (0x00e587e9, stride 0x34) -- NOT _G_LLM_STRAT_PLAYERS (0x00cff060, stride 0x740). Two
// different arrays, both called "players"; mixing them up would read arbitrary memory.
inline constexpr int32_t SESSION_MP_LOCKSTEP = 3;    // CMP [_G_LLM_GAME_SESSION_MODE],0x3
inline constexpr int32_t PLAYERS_STRIDE      = 0x34; // IMUL EAX,EAX,0x34
inline constexpr uint8_t NET_CONTROLLED      = 0x04; // TEST byte ptr [EAX + 0xe587ef],0x4
inline constexpr int32_t PLAYERS_FLAG_OFF    = 0x06; // 0xe587ef - 0xe587e9: the tested byte's offset

// SB-BIND T2 (2026-09-06): these are the STOCK values and nothing indexes with them any more.
// Production index expressions read the DERIVED capacities off the view/store (`v.caps.units`,
// `caps_.buildings`, ...), which follow whatever the host bound; these survive only as the offline
// fixtures' default and as the documentation of the stock strides. Adding a new production use of
// one is a regression -- it would pin that one site to 100 while every other site followed a
// cap-raised host, which is worse than uniform staleness. See state/roster_caps.h.
inline constexpr int32_t UNITS_PER_PLAYER = mh::state::STOCK_ROSTER_CAPS.units; // units is map::object::unit[8][100] @0x00dd8c48

// apply_and_dequeue's one config read: `Unit[u.unit_proto_id]` at byte +0xec, compared against the
// unit's live state (0x00469d32-0x00469d60). That field is `undefined` in Ghidra -- it has no name
// and its semantics are NOT established -- so this stays a documented raw offset rather than a
// struct field with an invented meaning. Unit is cfg::final::struct::Unit[100], stride 0x23f.
inline constexpr int32_t UNIT_TYPE_STRIDE     = 0x23f;
inline constexpr int32_t UNIT_TYPE_STATE_BYTE = 0xec;

// The order record's args[] are "leave unchanged" at -1; apply_and_dequeue tests each against this
// before copying it into the unit (CMP dword ptr [...],-0x1).
inline constexpr int32_t ARG_UNCHANGED_32 = -1;

// A typed view over the container's shared state. Everything here lives in the game's own .bss at a
// frozen address; this exists so the logic below never spells a raw address or a byte offset, while
// keeping it obvious that these are NOT owned allocations.
struct container_state {
    order                 *queue;                         // [QUEUE_CAP]   -- due now, consumed by the dispatcher
    int32_t               *queue_count;                   //                  live entries in `queue`
    order                 *pending;                       // [PENDING_CAP] -- scheduled, awaiting exec_time
    int32_t               *pending_count;                 //                  live entries in `pending`
    int32_t               *scratch;                       // [ARG_SLOTS]   -- the hidden argument channel
    order                 *staging;                       // [STAGING_CAP] -- proposed this frame, drained by schedule()
    int32_t               *staging_count;                 //                  live entries in `staging`
    double                *horizon;                       // _G_LLM_STRAT_LOCKSTEP_HORIZON -- our requested lockstep horizon
    mh::net::packet_buffer packet;                        // the shared region + its cursor; see include/packet_buffer.h.
                                                          // This path only ever reads the CURSOR (the bytes are appended
                                                          // by gc.send_order(), an original we do not own), so a caller
                                                          // that binds `bytes` to nullptr is legitimate here.
    const mh::game::mh_llm_strat_player_profile *players; // [MAX_PLAYERS], read for PLAYER_ALIVE
    // ---- batch B additions: the router's inputs and the cancel path's target ----
    const int32_t                *session_mode; // _G_LLM_GAME_SESSION_MODE; 3 == MP lockstep
    const uint8_t                *net_players;  // `Players` [MAX_PLAYERS] x PLAYERS_STRIDE, read for NET_CONTROLLED
    const double                 *game_clock;   // _G_LLM_STRAT_GAME_CLOCK
    const double                 *step_size;    // _G_LLM_STRAT_LOCKSTEP_STEP_SIZE
    mh::game::mh_map_object_unit *units;        // [MAX_PLAYERS][UNITS_PER_PLAYER], written by apply_and_dequeue
    const uint8_t                *unit_types;   // `Unit` config prototypes, stride UNIT_TYPE_STRIDE; read-only

    // SB-BIND T2: the per-player ROW CAPACITIES, derived from the sizes the host bound. LAST on
    // purpose -- container_state is AGGREGATE-initialised in member order by both state() and the
    // orderstest fixture, so a member inserted mid-struct silently re-pairs every initializer after
    // it. Stock default, so a fixture that does not name it is unchanged.
    mh::state::roster_caps caps{mh::state::STOCK_ROSTER_CAPS};
};

// The four original functions this container calls OUT to. Indirected for the same reason the state
// is a parameter: it lets orderstest drive schedule()/stage_scheduled() over heap buffers with
// recording stubs. In production these bind to mh::call::* (the generated thunks) -- see live_calls().
//
// WHY THE RECORD IS PASSED AS A POINTER when the original pushes it BY VALUE (`SUB ESP,0x44` +
// `MOVSD.REP` of 17 dwords before each CALL). It is not a shortcut and it does not widen what the
// callee can touch: the generated marshalling thunks (`s_u32_B68`, `s_void_B68_S4` in
// mh_calls.gen.cpp) do that same `sub esp,68` / `rep movsd 17` themselves, so the callee receives
// the caller's stack copy and never sees this pointer. `const order *` is the generated blob-argument
// convention, and by-value semantics are preserved underneath it. (Raised as an aliasing suspicion by
// the reimpl-verify review, 2026-07-27, and settled by reading the thunks -- worth stating here so it
// is settled once.)
struct game_calls {
    void (*integrity_check)(const order *rec, char *tag); // llm_strat_order_integrity_check
    void (*commit_horizon)();                             // llm_net_lockstep_commit_horizon
    int32_t (*send_order)(const order *rec);              // llm_net_send_order (result DISCARDED)
    int32_t (*send_buf_flush)();                          // llm_net_send_buf_flush
    // ---- batch B: apply_and_dequeue's and integrity_check's outward calls ----
    // These three all write `units` (and target_release_ref also `buildings`), which is why
    // apply_and_dequeue's shadow site DECLARES those regions and then lets them run for real --
    // see the note on the shadow bindings in the .cpp.
    void (*unit_set_order_param)(int32_t player, int32_t unit_index, int16_t param);
    void (*target_release_ref)(uint32_t player, int32_t unit_index, uint32_t mode);
    void (*unit_notify_status)(uint32_t player, int32_t unit_index, uint32_t status_code);
    // (llm_teardown_hook_stub was a member here until SIMABI-HOOKS 2026-09-10 -- an empty retail
    // body, so integrity_check's teardown is now the force_return_to_main call alone.)
    void (*force_return_to_main)(); // llm_menu_force_return_to_main
};

// Resolved once from the state region registry (addr/mh_regions.gen.h). Returned by reference so
// call sites read like `st.queue`.
const container_state &state();
const game_calls      &live_calls();

// The two order counts, for consumers OUTSIDE this module -- the turn engine gates release_due and
// schedule on them. Exposed as accessors rather than left to each consumer to bind for itself: a
// region with two independent bindings is a region that can be moved out from under one of them
// (RI-STATE / ST1). These are the only order state anything outside mh::orders needs.
const int32_t *pending_count();
const int32_t *staging_count();

const game_calls &inert_calls();

// The tag pointer stage_scheduled hands to integrity_check: the GAME's own "PropozycjaRozkazu"
// string, not a copy of the text, so the callee sees the same pointer value the original passed.
char *proposal_tag();

// The container's logic, over an EXPLICIT state. The public wrappers below are these applied to
// state(); splitting them costs nothing at runtime (one inlined call) and buys a real unit test:
// `net_selftest.exe orderstest` runs them over heap buffers, with no game and no rig, which is the
// only way the overflow branches get exercised at all -- a 2-peer run cannot reach the immediate
// lane, and no scenario has ever pushed 300 orders into one queue.
namespace detail {
void    scratch_reset(const container_state &st);
void    scratch_set_field(const container_state &st, int32_t index, int32_t value);
int32_t enqueue(const container_state &st, uint16_t unit_index, uint16_t owner_and_kind,
                int16_t param0, uint16_t order_code);
int32_t pending_enqueue(const container_state &st, order *rec);
int32_t stage_scheduled(const container_state &st, const game_calls &gc, char *tag,
                        uint16_t unit_index, uint16_t owner_and_kind, int16_t param0,
                        uint16_t order_code, double exec_time);
int32_t schedule(const container_state &st, const game_calls &gc);
int32_t release_due(const container_state &st, double now);
int32_t dispatch(const container_state &st, const game_calls &gc, char *tag, uint16_t unit_id,
                 uint32_t player, uint16_t op_code, uint16_t arg);
int32_t queue_find_index(const container_state &st, int32_t player, int32_t unit_idx,
                         int32_t kind_tag);
void    queue_apply_and_dequeue(const container_state &st, const game_calls &gc, uint32_t player,
                                int32_t unit_idx, int32_t queue_idx);
void    integrity_check(const container_state &st, const game_calls &gc, const order *rec);
void    emit(const container_state &st, mh::state::state_sink &s);
void    load_state(const container_state &st, mh::state::state_source &src);
} // namespace detail

// ---- the container, in original-behaviour terms -------------------------------------------------
// Each of these is shadow-verifiable against the function it replaces; the binding lives in the .cpp.
// Behaviour notes that a "sensible" reimplementation would get WRONG are on the definitions.

void scratch_reset();                                 // llm_strat_order_scratch_reset
void scratch_set_field(int32_t index, int32_t value); // llm_strat_order_scratch_set_field

// Returns the NEW live count, or 0 if the queue was full (in which case it is EMPTIED -- see the .cpp).
int32_t enqueue(uint16_t unit_index, uint16_t owner_and_kind, int16_t param0, uint16_t order_code);

// Returns 1 on success, 0 if pending was full (also emptied). `rec` is the caller's by-value copy
// and IS MUTATED in place, exactly as the original does -- hence `order *`, not `const order *`:
// the generated signature drops the const for by-value blobs precisely because the callee owns them.
// NOT const, and NOT a spelling accident: this body MASKS FIELDS IN PLACE through `rec`
// (see the &= 0xff group below) before copying the record into the pending array. In the
// thunked call direction that is invisible to the caller -- the marshalling thunk hands us a
// private copy -- which is exactly why mh_calls.gen.h spells the CALL direction `const`. A
// REBIND removes the thunk and calls this body directly, so binding it to that const member
// would mask the caller's own record. LIB-CONSTSIG keeps this row keep_original for that
// reason, not for a generator disagreement.
int32_t pending_enqueue(order *rec);

// llm_strat_order_stage_scheduled -- the SCHEDULED lane's front door. Returns the new staging count,
// or 0 if staging was full. NOTE it REJECTS on overflow; it does NOT empty the array the way the two
// enqueues do. Each of the four ushorts is stored with its LOW BYTE DUPLICATED into both halves.
// The four register parameters are 32-bit, not 16-bit: SIM-READY (2026-08-07) corrected the Ghidra
// prototype from AX:2/DX:2/BX:2/CX:2 to the full EAX/EDX/EBX/ECX the entry sequence spills
// (`MOV dword ptr [EBP-n],reg` @0x00466227-...). That does NOT widen what reaches the record -- the
// body still stores each one's LOW BYTE DUPLICATED -- so the narrowing stays where the original puts
// it, inside detail::stage_scheduled, rather than being hidden in the parameter list.
int32_t stage_scheduled(uint32_t unit_index, uint32_t owner_and_kind, int32_t param0,
                        uint32_t order_code, double exec_time);

// llm_strat_order_schedule -- drains STAGING into PENDING, broadcasting each order, and moves the
// lockstep horizon. Returns the number of bytes flushed by llm_net_send_buf_flush.
int32_t schedule();

// llm_strat_order_release_due -- moves PENDING orders whose exec_time has arrived into QUEUE, per
// live player, deduplicating against what is already queued. Returns 1 iff PENDING ends up empty.
int32_t release_due(double now);

// llm_strat_order_dispatch -- the ROUTER the 63 replicated-lane wrappers call. Picks the scheduled
// lane in MP for a network-controlled player, the immediate lane otherwise, and returns whatever the
// chosen lane returned. Note the argument RENAMING across the call: this function's
// (unit_id, player, op_code, arg) become the record's (unit_index, owner_and_kind, param0, order_code).
int32_t dispatch(uint16_t unit_id, uint32_t player, uint16_t op_code, uint16_t arg);

// llm_strat_order_queue_find_index -- the lookup half of the cancel/replace pair. Returns the QUEUE
// index of the first entry matching (owner nibble, kind nibble, unit index), or -1.
int32_t queue_find_index(int32_t player, int32_t unit_idx, int32_t kind_tag);

// llm_strat_order_queue_apply_and_dequeue -- the apply half: writes the queued order's args into the
// unit, then removes the entry and compacts the queue.
void queue_apply_and_dequeue(uint32_t player, int32_t unit_idx, int32_t queue_idx);

// llm_strat_order_integrity_check -- verifies the byte-duplication encoding on the four identity
// fields and, in MP only, tears down to the main menu when it fails.
void integrity_check(const order *rec, char *tag);

// Where this module's own lines go (the promote/refuse banners). Same shape as the other module
// log sinks;
// the seam layer points it at seam_log so mh/orders needs no dependency on mh/seams.
// ---- ST4: the module IS the owner of its state, so the save and the verdict ask IT -------------
//
// WHAT IS IN THE SAVE FORMAT: the QUEUE (300 x 0x44 = 20400 bytes) and its COUNT, and nothing else.
// SavePlanetToDisk/LoadPlanetFromDisk take exactly those two blocks. `emit` reproduces that byte
// stream in PERSIST mode, so while Law 1 freezes the layout it is byte-identical to the block copy
// it replaces -- which is what lets the delegation land under the existing byte-identity oracle.
//
// LOAD TOUCHES QUEUE AND QUEUE_COUNT ONLY, and that is FIDELITY, not an oversight. Measured
// 2026-07-31: PENDING and STAGING are not reset by the load path at all. The only code that clears
// them is FillDefaults (@0x0045603e, three literal `MOV dword ptr [...],0x0`), whose only callers are
// llm_strat_mode_init and ReadMap_pre -- neither reachable from llm_game_load or LoadPlanetFromDisk.
// So a mid-session load leaves stale SCHEDULED orders in PENDING which will still fire at their
// exec_time against the freshly loaded world. That is the original's behaviour (arguably its latent
// bug); a load_state that "helpfully" cleared the container would silently diverge from it. Asserted
// by orderstest rather than left as a comment.
void emit(mh::state::state_sink &s);
void load_state(mh::state::state_source &src);

// Per-REGION, which is the granularity both consumers actually use: the save driver walks blocks
// (QUEUE and QUEUE_COUNT are two separate file blocks, each with its own header -- emitting them as
// one 20404-byte stream would produce ONE block and would NOT be byte-identical), and the
// determinism hash walks slices. `emit` above is the module-level stream in the file's order; these
// are the pieces. Regions outside this module's ownership are a no-op.
void emit_region(mh::state::region_id rid, mh::state::state_sink &s);
void load_region(mh::state::region_id rid, mh::state::state_source &src);

// Register this module as the owner of its regions with mh::state. Idempotent; safe to call from a
// static initializer or an init seam. Returns the number of regions claimed.
int claim_regions();

void set_logger(void (*fn)(const char *));
void say(const char *fmt, ...);


// PROMOTION (O3): install this module as the LIVE implementation, so the game executes ours instead
// of the original's. One switch for the WHOLE container -- `[promote] orders=1` -- because the unit
// of replacement is a call-tree closure (Law 4), and promoting half of one leaves our code calling
// originals that our other half has already replaced.
//
// Nine hooks, not eleven: `stage_scheduled` and `integrity_check` are reached only from INSIDE the
// container, so once dispatch and enqueue are ours they are ordinary C++ calls and need no live seam.
// That is Law 4's "minimize live seams, not maximize replaced functions" actually paying.
//
// Returns the number of hooks installed (0 = off, the default = the stock path byte for byte, which
// is also the one-flag ROLLBACK). Mutually exclusive with shadow: arming both over one entry would
// stack a replace thunk on a shadow dispatcher, so whichever runs second refuses and says so.
// ---- carrying the harness's ENQUEUE SUPPRESSION across promotion (D18) -------------------------
//
// `[harness] replay_suppress_enqueue=1` exists for the faithful-substitute replay test: neuter
// llm_strat_order_enqueue so the AI keeps running while ONLY injected orders reach the queue. The
// harness implements that by rewriting the original's entry to `xor eax,eax; ret`.
//
// That rewrite and this container's promotion want the same entry, and MH_Harness_Init runs BEFORE
// MH_Seam_Init, so the harness always got there first -- measured: "[export] llm_strat_order_enqueue
// NOT armed -- entry bytes 0000002868C3C031 != expected 0000002868E58955 (wrong build, or already
// hooked)" followed by "[promote] orders: 8/9 seams installed -- PARTIAL, treat this run as invalid".
// So the two facilities were not merely exclusive; using one INVALIDATED the other, and said so in
// language that points at the image.
//
// Suppression is a BEHAVIOUR, not a hook, so unlike the order RECORDER (which rebinds) our body
// simply carries it: when set, the enqueue returns 0 and appends nothing, which is what the neutered
// original did. The harness sets this instead of writing bytes whenever the entry is ours.
//
// WHERE THE CHECK LIVES, and it MOVED on 2026-09-11 (user ruling) because the old placement made the
// documented sentence false. It used to sit in `promoted::enqueue`, the thunk behind the GAME'S entry
// at 0x00466094 -- so it saw only callers that crossed that entry. Two whole populations did not:
// libmh-internal callers, which MH_LIBMH_BIND sends straight to mh::orders::enqueue once the rebind
// row is armed (measured: 715 of 718 rows armed, `llm_strat_order_enqueue -> OURS`); and anything
// routed through our own dispatch, whose immediate lane calls the sink by a plain in-TU call. The
// gate's coverage therefore SHRANK with every promotion, invisibly, and LIB-REF's step-5000 residual
// was the bill: the harness's synth_move workload appended one order per step straight past it.
//
// The check now sits in the SINK, mh::orders::detail::enqueue -- the one point every append goes
// through -- so "only injected orders reach the queue" is true again for the immediate lane. SCOPE:
// the immediate lane is the only appender in a non-lockstep session; the mode-3 lane reaches the
// queue via release_due, which is deliberately NOT suppressed (see the sink's note).
void set_suppress_enqueue(bool on);
bool suppress_enqueue();

int install_promotion(int default_on);

// True once install_promotion() has installed anything -- install_shadow() consults this and refuses.
bool promotion_active();

} // namespace mh::orders
