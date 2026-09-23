//
// Determinism replay harness -- see include/mh_harness_export.h and the replay-harness notes.
//
// What it does, each strategic sim step (llm_strat_sim_step @0x0043f512):
//   1. (optionally) force _G_LLM_GAME_SESSION_MODE = 3 so a plain loaded save runs the MP
//      fixed-timestep lockstep path -- the code path multiplayer would use.
//   2. at the configured seed step: either DUMP the full sim-state blob to mh_harness_seed.bin,
//      or INJECT a previously dumped blob back over the arrays (forcing a byte-identical start
//      for the second run of a comparison), depending on ini `seed_mode`.
//   3. FNV-1a-64 hash the sim-state regions (combined + per region) and append a line to the log.
//   4. at the configured stop step: write a final per-region breakdown and (optionally) exit.
//
// The state region set is the measured live strategic sim state (bases/lengths from Ghidra's
// applied array types, 2026-07-09). Cosmetic pools (fx anims, ambient sound) and static config
// tables (building_types) are deliberately EXCLUDED -- they are the "known-cosmetic mask" the
// go/no-go test calls for. tact_units is tactical-mode and excluded. NOTE: projectile_pool is read
// at its stock .bss VA 0x00e1db98 -- run the harness on an exe WITHOUT the pool-relocation patch
// (import-only harness_load patch), else that region reads a stale block.
//
// x87 note: MH forces a fixed FPU control word at init (round-to-nearest, 53-bit) when ini
// `pin_fpu=1`. Both machines in a cross-vendor run must use the same setting; determinism only
// needs consistency, and 53-bit narrows 80-bit transcendental divergence.
//
#include <windows.h>
#include <float.h>
#include <stddef.h> // offsetof -- the mask offsets come from the generated struct mirrors
#include <stdint.h>
#include <intrin.h> // _ReturnAddress -- Sect. 9j rand call-site attribution

#include "include/mh_harness_export.h"
#include "include/mh_core_arm_paths.h" // F4E: the run paths mh.dll composed, copied below
// NOT INCLUDED, DELIBERATELY: mh_harness.dll's own harness_contract.gen.h. This file is compiled by
// TWO projects -- mh_harness.vcxproj, where those tables exist, and mh_nettest.vcxproj, where the
// whole seam set is linked into one offline image and there is no boundary to bind across. Keeping
// the instrument's source free of its own module's plumbing is what lets both keep working: the
// module half lives in mh_harness/mh_harness_dllmain.cpp, and the only thing it tells this file is
// MH_Harness_SetModuleRefused() below.
#include "include/mh_run_context.h"         // MH_RunDir (per-run log folder)
#include "include/mh_uidrive_export.h"      // UI-REC: MH_UIDrive_ActiveScreen -- the journal's screen barrier
#include "state/host_api.h"                 // LIB-ABI: libmh_set_host_api
#include "state/host_bind.h"                // SB-BIND: the state ABI (bind_stock)
#include "state/host_in.h"                  // LIB-REF-IN: libmh_in_open, before any seam installs
#include "hook/host_event_sink.h"           // LIFT-EVQ: bind_host_event_sink (G104 placement)
#include "addr/mh_rebind.gen.h"             // LIB-REBIND R11: load_gates at the named init point
#include "tact/tact_unit_enqueue_command.h" // TACT1-P C4: the driver enters OUR body, not the original
#include "tact/tact_group_issue_order.h"    // TACT1-P C4: the driver enters OUR body, not the original
#include "seams/net_internal.h"             // SHIP_REBIND_DEFAULT -- the R11 gate policy load_gates takes
#include "config/config.h"                  // F2A: the D11 selector that gate policy now derives from
#include "config/ini_read.h"                // TL-HARN4: read_ini_string -- strips a trailing `;comment`
#include "mh_net_export.h"                  // U30(b): MH_Net_Send -- the garbled-frame injector below
#include "mh_net_module.h"                  // mp:X1b: the three snapshot rows + MH_NetSnapshotStatus
#include "addr/mh_addrs.gen.h"              // generated EN VAs (tools/gen_dll_addrs.py)
#include "addr/mh_structs.gen.h"            // generated struct mirrors (llm_strat_player_profile -- D6)
#include "addr/mh_calls.gen.h"              // generated typed callables + __watcall thunks (P0-CALLS)
#include "addr/mh_export.gen.h"             // generated entry VAs + 8-byte arm guards (P0-EXPORT). Used here
                                            // ONLY for llm_game_land_players_on_planet's address and entry
                                            // bytes -- the all-AI hook is a trampoline, not a replacement.
#include "addr/mh_regions.gen.h"            // ST2M: the state region registry + the ordered hash manifest
#include "fp/x87.h"                         // raw_x87_cw / raw_mxcsr -- the FPENV line in the rdump window
#include "sim/libtrans/sim_lt_frame.h"      // set_lt_frame_input_override -- the skip_input_update arm
#include "sim/resid/sim_time_resync.h"
#include "sim/rng_trace.h"        // set_time_resync_pace_disabled -- the skip_pace_hook arm
#include "state/boot_snapshot.h"  // LIB-BOOT: the post-cfg prototype snapshot
#include "state/world_snapshot.h" // LIB-WORLD: the step-0 world blob
#include "state/region_view.h"    // ST6: a slice emits itself into a sink (hash / seed / poke)
#include "desync/desync_watch.h"  // D21: hand the per-step hash to the runtime desync detector
#include "en_guard.h"             // EN-only build gate
#include "hook/hookpoint.h"       // D5: the named hook points -- the harness arms ONLY through these
#include "hook/promoted.h"        // C9/D18: promoted_owner_of -- displaced is not the same as MISMATCHED
#include "tact/tact_journal.h"    // TACT-REC: which seam calls are the PLAYER's
#include "save/save_live.h"       // SV1-P: the in-game save trigger (save_at)
#include "orders/order_queue.h"   // D18: set_suppress_enqueue -- the replay neuter's promoted path

// Per-EVENT temporal trace (net_seams.cpp): sim_step is harness-hooked (not tracer-hookable), so mark
// the step boundary here. No-op unless [trace] temporal=1 + SESSION_MODE==3. id 5 = TEV_SIMSTEP.
extern "C" void MH_Temporal_Event(int id);

#if defined(_M_IX86) // x86-only: inline naked trampolines + absolute exe VAs (mh.exe is 32-bit)

namespace {

// ---- target addresses in mh.exe (generated EN VAs; image base 0x00400000, no ASLR) --------------
constexpr uintptr_t ADDR_SIM_TICK   = mh::addr::llm_strat_sim_tick;         // frame pump; entry 55 89 e5 68 30
constexpr uintptr_t ADDR_SIM_STEP   = mh::addr::llm_strat_sim_step;         // entry: 55 89 e5 68 38 ..
constexpr uintptr_t ADDR_CONSOLE    = mh::addr::llm_debug_console_dispatch; // SHIFT+ENTER
constexpr uintptr_t ADDR_GAME_CLOCK = mh::addr::_G_LLM_STRAT_GAME_CLOCK;    // double
constexpr uintptr_t ADDR_TOTAL_TIME = mh::addr::TOTAL_GAME_TIME;            // time::g::TOTAL_GAME_TIME (double)
// SB-HOSTFREE: a FUNCTION, not a constant -- this region is MOVABLE and a relocating host
// leaves 0xCD at the stock address.
inline uintptr_t ADDR_SIM_INTERVAL() { // double, =0.1
    return mh::state::live_base(mh::state::RID_STRAT_SIM_STEP_INTERVAL);
}
// D5: the TARGETS of the hooks this file arms are GONE FROM HERE. llm_tact_frame (the tactical
// cadence), llm_rand and llm_strat_rng_seed_wallclock_seconds (TACT-PREP / SPCAMP-SEED, whole-body
// pins) and GetCurrentTime (P0-SPDET's wall-clock pin) are rows of the named hook-point table now
// (hook/hookpoint.h), which carries each one's steal width, entry claim and arm guard beside it.
// The three that remain below are the ones this file still READS rather than hooks.

// ---- order pipeline (Phase 2: record/replay). RE'd 2026-07-11 ------------------------------------
constexpr uintptr_t ADDR_ORDER_DISPATCH = mh::addr::llm_strat_order_queue_dispatch; // entry 55 89 e5 68 84
constexpr uintptr_t ADDR_ORDER_ENQUEUE  = mh::addr::llm_strat_order_enqueue;        // SP+AI immediate lane -> QUEUE
// SB-HOSTFREE: FUNCTIONS, not `constexpr uintptr_t`, and the difference is the whole of this item.
// A relocating host moves all six of these order regions into its own arena and poisons the .bss
// they left; a constant baked at compile time keeps pointing at the poison. The harness reading
// 0xCD is worse than the sim doing it, because the harness is the INSTRUMENT -- it would record an
// order stream out of dead memory and the run would look well-formed. (Measured: the first
// relocated soak printed `master_gate=3452816845`, i.e. 0xCDCDCDCD, from the sibling AI_ENABLED
// read below, and still reported PASS.)
inline uintptr_t ADDR_ORDER_QUEUE() { // [300] (llm_strat_order)
    return mh::state::live_base(mh::state::RID_STRAT_ORDER_QUEUE);
}
inline uintptr_t ADDR_ORDER_QCOUNT() { // int
    return mh::state::live_base(mh::state::RID_STRAT_ORDER_QUEUE_COUNT);
}
inline uintptr_t ADDR_ORDER_PENDING() { // [1000] scheduled lockstep cmd buffer
    return mh::state::live_base(mh::state::RID_STRAT_ORDER_PENDING);
}
inline uintptr_t ADDR_ORDER_PCOUNT() { // == the "order_pending" hash region
    return mh::state::live_base(mh::state::RID_STRAT_ORDER_PENDING_COUNT);
}
constexpr int ORDER_SIZE = 0x44; // sizeof(llm_strat_order)
constexpr int ORDER_QCAP = 300;
constexpr int ORDER_PCAP = 1000; // ORDER_PENDING slot count
// SIM1-P clause 8: the THIRD order region, and the one order_log never polled. Same llm_strat_order
// layout as the other two -- the generated manifest gives it 20400 bytes over 300 slots, which is
// exactly ORDER_SIZE (0x44 = 68), so order_dump_array reads it unchanged. Its absence is why the
// order channel could only ever describe two thirds of what the sim had decided.
inline uintptr_t ADDR_ORDER_STAGING() { // [300]
    return mh::state::live_base(mh::state::RID_STRAT_ORDER_STAGING);
}
inline uintptr_t ADDR_ORDER_SCOUNT() { // int
    return mh::state::live_base(mh::state::RID_STRAT_ORDER_STAGING_COUNT);
}
constexpr int       ORDER_SCAP         = 300;
constexpr uintptr_t ADDR_PLAYER_DATA   = mh::addr::game_player_data; // game::player_data[8] base (D11)
constexpr uint32_t  PLAYER_DATA_STRIDE = 0x288fc;                    // per-player
constexpr uint32_t  PD_AI_ENABLED_OFF  = 0x18;                       // player_data[i].ai_enabled
constexpr int       PLAYER_DATA_COUNT  = 8;                          // D11: indexed 0..7 by llm_game_land_players_on_planet

// ---- the all-AI soak (see Config::all_ai) --------------------------------------------------------
// The landing dispatcher is hooked rather than either of its three callers (llm_strat_planet_session_begin,
// llm_strat_session_begin_multi, SwitchToPlanet) because ONE seam then covers every entry into a
// planet -- including the mid-campaign planet switch, which a session-begin hook would miss. D5: its
// address and its 8-byte arm guard live in the hook-point table (point::land_players); this file
// names the POINT and keeps the reasoning.
constexpr uintptr_t ADDR_PLAYERS_PROF = mh::addr::_G_LLM_STRAT_PLAYERS; // llm_strat_player_profile[8]
constexpr int       PLAYER_PROF_COUNT = 8;
// E_STRAT_PLAYER_STATUS bits, from mh_structs.gen.h's status_flags comment.
constexpr uint32_t PS_ENABLED = 0x1u; // slot claimed
constexpr uint32_t PS_ALIVE   = 0x2u; // has presence; cleared by llm_strat_player_presence_lost
constexpr uint32_t PS_HUMAN   = 0x4u; // human-controlled -- the branch input in land_players
constexpr uint32_t PS_AI      = 0x8u; // AI-controlled; gates .DMP base-layout injection

// Fixed-timestep replay: mode 3 (real MP lockstep) is network-gated -- time_tick clamps
// TOTAL_GAME_TIME to the lockstep horizon (DOUBLE_005d558c), which only advances via peer acks, so a
// single process stalls in mode 3. Instead we leave the game in its natural SP mode and pin the
// per-frame clock advance to a constant: TOTAL_GAME_TIME = GAME_CLOCK + interval at sim_tick entry
// makes the mode-1/2 branch run exactly ONE deterministic sim_step (delta=interval) per frame. This
// exercises the SAME llm_strat_sim_step that MP lockstep runs, under the SAME fixed timestep -- the
// property the go/no-go actually needs -- without any lockstep machinery. See the replay-harness notes.

// ---- sim-state region manifest -----------------------------------------------------------------
// GENERATED (ST2M, 2026-07-31). This was a 130-line hand-written table here, mirrored by hand in
// tools/mp_analyze.py's REGION_NAMES with tools/lint_region_mirror.py holding the two level. Both
// now come from tools/data/hash_manifest.json via tools/gen_state_registry.py, which resolves each
// entry to a (registry region, offset, length) SLICE -- so the day a region MOVES, its slices move
// with it instead of hashing whatever is left at the stock .bss address. The adjudication comments
// that used to live in the table live in the JSON and are re-emitted into mh_regions.gen.h; read
// them there.
//
// ORDER IS STILL A WIRE CONTRACT. The per-step `R` line carries one hash per entry in this order and
// mp_analyze labels the columns positionally, so an INSERT still re-labels every later region -- the
// difference is that both ends now read the same list, so they cannot disagree about what it is.
using Region              = mh::state::hash_region;
constexpr auto &REGIONS   = mh::state::HASH_REGIONS;
constexpr int   N_REGIONS = mh::state::HASH_REGION_COUNT;

// Named indices, also generated (mh_regions.gen.h `hash_region_index`). The hand-written IDX_*
// constants they replace carried a standing "APPENDED, not inserted" warning and a runtime
// name-recheck guard in MH_Harness_Init to catch the drift they could not prevent; deriving them
// from the table removes the failure mode rather than watching for it.
constexpr int IDX_RNG   = mh::state::HIDX_RNG_STATE;
constexpr int IDX_CLOCK = mh::state::HIDX_GAME_CLOCK;

// Regions EXCLUDED from the STATE-ONLY hash (confirmed non-durable-state; real desyncs still surface
// in the hashed OUTPUT regions -- buildings/units/productions/etc.). Kept in the combined hash + the
// per-region "R" lines, so they remain visible; just not counted in the desync verdict. WHY each one
// is excluded is recorded at its entry in the generated table.
static inline bool state_excluded(int i) { return REGIONS[i].excluded; }
// D10 (2026-07-27): p0/p1_ai_econ are back IN the verdict. They were excluded as "AI-internal
// bookkeeping that drifts only under host interaction", measured 2026-07-22 -- an assumption that had
// never been tested against a run where the AI was actually doing anything, because until D10 no
// determinism run had an AI in it at all. It now has: with an AI seated at slot 2 and mutating its
// store every single step, a 1200-step ship-paced run had EVERY region agreeing across peers at EVERY
// step except order_pending. Excluding them was costing detection and buying nothing.
// CAVEAT, deliberately not hidden by re-excluding: the original claim was about INTERACTIVE runs. If
// an interactive session ever goes red on ai_econ, that is AI-internal state diverging between peers
// -- a real finding to file (today it would be masked), not a reason to put the exclusion back.

// ---- rng_state: mask the FX channel, hash the other two (D3, 2026-07-27) ------------------------
// The old exclusion dropped the WHOLE region as "the strategic PRNG channel, proven not to feed game
// state (2026-07-09)". Two things were wrong with that. The 2026-07-09 measurement was of the FX
// channel (the replay-harness notes), and the region does not hold one channel -- it holds four
// slots, three of them live, and one of the live ones is the AI's.
//
// SLOT MAP, read out of the EN image rather than assumed (every PRNG entry point indexes
// [slot*4 + 0x603ecc], and EVERY call site passes the slot as a `push imm8` constant, so there is no
// dynamic-slot path):
//   slot 0  strategic  llm_rand_below      -> llm_strat_rng_next(0)         seeded @0x499fe0
//   slot 1  fx         llm_rand_below_fx   -> llm_strat_rng_next(1)         seeded @0x49a013
//   slot 2  AI         llm_rand_below_ai   -> llm_rand_prng_tick_slot(2),
//                                             llm_rand_state_advance(2) x2  seeded @0x4dc685
//   slot 3  UNREACHABLE -- no call site anywhere supplies it.
// (The `fdiv qword ptr [0x603f0c]` in llm_rand_state_advance is the 65535.0 divisor CONSTANT, not
// state. The old 0x40 length stopped one byte short of it by luck.)
//
// So: slot 1 is drawn per RENDERED FRAME (effects), so its draw count is frame-rate-dependent and
// differs between peers for reasons that are not desyncs -- mask it, exactly like the per-frame fog
// bits in tile_objects. Slots 0 and 2 are drawn ONLY from the deterministic sim/AI path, so they MUST
// match; a divergence there is a real desync and a LEADING one (it precedes the state it will
// corrupt). They are now in the state-only hash. Slot 3 is hashed because it is free and pinned at 0.
// The per-step raw dump below still prints all four slots unmasked, so fx drift stays observable.
// (The masked walk itself is mh::state::emit_rng_state, in state/region_view.h -- ST6 moved the
// three masked hashers out of this file and expressed their masks as sink calls.)

// ---- config ([harness] in mh_net.ini, next to the exe) ------------------------------------------
struct Config {
    int seed_step = 1; // step at which to dump/inject the seed blob (0 = never)

    // ---- LIB-BOOT: capture the post-cfg prototype snapshot -----------------------------------
    // 1 = at the first present in _G_LLM_GAME_MODE == 3 (the main menu -- boot stage 9 has just
    // set it, so stages 5..8 have all run and the cfg load is done), write the region-derived
    // snapshot to the run folder and log its hash. One-shot; the game is unperturbed afterwards.
    int boot_snapshot = 0;

    // LIB-WORLD: 1 = capture the STEP-0 WORLD (every bound region) at the first hashed sim step,
    // beside the run's own lockstep hash pair. Written to the run folder as mh_world.bin. Armed
    // with `order_mode=1` this is the paired fixture LIB-REF replays: recording + world + hashes,
    // all from one run so they cannot drift against each other. One-shot.
    int world_capture = 0;

    int seed_mode    = 0; // 0=dump, 1=inject, 2=off
    int stop_step    = 0; // step at which to write final report / exit (0 = run forever)
    int exit_on_stop = 0; // 1 = ExitProcess after the final report
    int fixed_step   = 1; // pin TOTAL_GAME_TIME = GAME_CLOCK + interval each sim_tick (det. replay)
    int pin_fpu      = 1; // pin x87 control word (round-nearest, 53-bit) at init
    // ---- P0-SPDET: pin the WALL CLOCK, not the value derived from it ----------------------------
    // fixed_step pins TOTAL_GAME_TIME, i.e. time_tick's OUTPUT -- which is why it masks time_tick.
    // This pins its INPUT instead: GetCurrentTime (0x00427616, a leaf returning seconds as a double
    // in ST(0)) is whole-body replaced with a counter that advances a fixed dt per sim_tick. Then
    // time_tick's unconditional SP block computes deterministically from deterministic input and its
    // outputs become comparable across two runs -- which neither the hash (pinned/unhashed) nor a
    // shadow site (the second arm would read a DIFFERENT wall clock -- one of the
    // un-shadowable input sources) could otherwise do.
    // ONLY THREE STRATEGIC CALLERS -- llm_strat_time_tick, llm_strat_time_resync_and_tick,
    // llm_strat_session_state_reset. The other 21 are tactical/UI, which a strategic run never
    // reaches. DEFAULT OFF so no archived run's semantics change.
    int pin_wallclock = 0; // 1 = replace GetCurrentTime with the deterministic counter
    // ---- pin_menu_clock: the MENU's ms timer, which pin_wallclock does not reach ----------------
    // pin_wallclock replaces GetCurrentTime (a leaf returning SECONDS as a double, used by the
    // strategic time code). The menu runs off a different primitive entirely: llm_time_get_ticks_ms,
    // the binary's master MILLISECOND timer, from which llm_ui_menu_state_tick samples
    // _G_LLM_UI_MENU_NOW_MS once per frame and the UI derives _G_LLM_UI_ANIM_FRAME_DELTA as the
    // difference between samples.
    //
    // WHY THAT MATTERS FOR A REPLAY, measured 2026-09-07: _G_LLM_UI_MENU_INPUT_LOCK_TIMER is set to
    // 0x32 when a widget activates and llm_ui_widget_input_tick RETURNS EARLY while it is above zero,
    // draining it by that frame delta. So the lock is a WALL-CLOCK duration (~50 ms) while a journal
    // is indexed in PRESENTS: at 60 fps about three presents fall inside it, at 200 fps about ten,
    // uncapped hundreds -- and every replayed record inside the window is swallowed by the early
    // return. The race-select click stopped registering, its screen never reached the content id the
    // next barrier wanted, and the first gameplay order slid from step 78 to 91 to never.
    // Pinning it makes the lock drain a fixed amount PER PRESENT, which is the unit the journal
    // actually has, so menu input stops depending on the frame rate.
    int pin_menu_clock = 0;
    // [harness] tj_trace=N -- log the first N journal records AS THEY ARE INJECTED (idx, seam,
    // recorded frame, interpolated step_due, and the sim step + present they actually landed on),
    // plus the pinned clock at each of the first sim steps. Off by default; it is here because it is
    // what ended a chain of five wrong explanations for a frame-rate-dependent replay. Reading where
    // a record LANDS answered in one run what reasoning about schedules had not answered in a day:
    // records due at steps 40..44 land exactly there at 60 fps and ALL AT STEP 48 uncapped, because
    // the sim's first step happens at present 371 with the pinned clock at 6200 ms capped and at
    // present 417 / 6966 ms uncapped -- and the game catches that 766 ms up as a single burst.
    int tj_trace = 0;
    // [harness] tj_trace_from=I -- the LOW end of that window (default 0). `tj_trace` alone traces
    // records [0, N); with this it traces [I, N). Added 2026-09-07: the divergence under
    // investigation was at record ~36,000 of 37,383, and tracing from 0 to reach it costs 36,000
    // lines of noise for the 40 that matter -- which is enough friction that the instrument does
    // not get used, and the last two bugs here were both found by reading where a record LANDED.
    int tj_trace_from = 0;
    // [harness] tj_ps_log=N -- during a REPLAY, log `; TJPS <present> <step>` every N sim steps.
    // The replay-side counterpart of the recorder's `TJ P` barriers, and the one measurement this
    // harness could not make: the RECORDING's present->step offset is knowable from the journal (a
    // constant 267 for 90% of spcamp-solo, deviating by 1 exactly three times, each beside a dialog
    // barrier), but a REPLAY's was not observable at all -- so "the replay's offset differs under
    // load" could be hypothesised and not tested. 0 = off.
    int tj_ps_log = 0;
    // 1 = replace llm_time_get_ticks_ms with a view of the pinned clock
    int pin_clock_dt_us  = 16667; // per-sim_tick advance in MICROSECONDS (16667 = 60 fps)
    int pin_clock_base_s = 1000;  // starting value in seconds (nonzero: a 0 start makes the first
                                  // frame's dt equal the whole elapsed time, which is not typical)
    // ---- TACT-PREP: the TACTICAL cadence ---------------------------------------------------------
    // Everything above hangs off llm_strat_sim_step. In _G_LLM_GAME_MODE==6 the frame dispatcher
    // calls llm_tact_frame INSTEAD, so sim_step is never entered and the whole apparatus above --
    // step counter, hash line, and the pinned wall clock's advance -- silently stops for the length
    // of a tactical mission. That is the mechanical reason the shipped determinism gate has always
    // been blind to tactical, and it is not a policy exclusion (the tactical-probe work Sect. 2).
    // This is the second cadence: same three jobs, driven by llm_tact_frame, emitted on a `T` line
    // over TACT_HASH_REGIONS[] so nothing about the strategic stream or its goldens changes.
    int tact_hash_step = 0; // emit a `T`/`TR` pair every N tactical frames (0 = off; the hook is
                            // not installed at all, so a strategic run is byte-for-byte unaffected)
    int tact_stop_step = 0; // 0 = run on; else stop logging after this many tactical frames
    // TACT-REC: the SELF-STOP, and it is the tactical oracle's entire wall-clock cost. tact_stop_step
    // ends the LOGGING and nothing else -- a tactical mission has no enemy-wipe end condition, so the
    // process runs until the runner kills it at --tact-wall. The default 400-frame arm therefore costs
    // 40 s of wall clock to produce ~1.6 s of simulation: MEASURED at ~230-250 tactical frames/s
    // headless (6947 and 7116 frames in two 30 s arms), so 400 frames take under two seconds and the
    // other 38 are the game sitting in a mission nobody is watching. This exits after the frame named
    // has been FULLY logged, which turns --tact-wall back into the backstop it reads like.
    // 0 = off (the shipped behaviour, unchanged: no archived run's timing or output moves).
    int tact_exit_at = 0;
    // TACT-REC Sect. 9f: PER-RECORD resolution on the one region that ever diverges. The `TR` line
    // says "tact_units differs", which is 196,596 bytes and 129 records -- true and unactionable.
    // This emits one hash PER RECORD so the offline compare can name the unit INDEX, i.e. the
    // difference between "the roster changed" and "unit 7 changed at frame 410".
    // Cost is why it can simply be on for a whole run instead of needing a window: 129 x 9 chars is
    // ~1.2 KB a frame, ~3.5 MB over 3000 frames. And a window is not available anyway -- the fault is
    // intermittent, so a re-run around a KNOWN divergence frame does not reproduce it.
    int tact_unit_hash = 0; // 1 = emit a `TU` line per hashed frame (0 = not a byte of it runs)
    // Sect. 9f, second level. The TU line names the RECORD; this names the BYTE RANGE inside it.
    // Scoped to a unit RANGE rather than the whole roster because the cost is per unit: 48 chunks of
    // 32 bytes at 9 chars each is ~3.5 KB a frame for eight units, where the whole roster would be
    // ~56 KB. The range that matters is already known -- the two captured divergences named units
    // 2..8 and 4..5, i.e. the PLAYER SQUAD, which spawns into slots 1..squad.
    int tact_detail_lo = 0; // first unit index to chunk-hash (0 = off)
    int tact_detail_hi = 0; // last unit index, inclusive
    // Sect. 9i probe: the GATE fields of llm_tact_unit_owner_tick, per unit per frame.
    // Logged RAW rather than as a computed verdict, deliberately: reproducing the gate logic here
    // would put the thing under investigation into the instrument, and a wrong copy would look like
    // a finding. The offline compare derives the stop index from these columns.
    //   status&8            -> G2, the FIRE bit
    //   op / iflag / mrw    -> G3, the queue-head-busy triple (all three true = blocked)
    // Both gates jump to 0x0043367c -> 0x00433c03, which is the function's EPILOGUE, so the FIRST
    // blocked unit ends the pass for every higher index of that owner.
    int tact_gate_lo = 0; // first unit index to log gate fields for (0 = off)
    int tact_gate_hi = 0; // last unit index, inclusive
    // TACT-REC third level. TU names the record, TD names the 32-byte chunk; this names the FIELD,
    // by hashing a hand-kept table of named windows inside tact_unit_record. It exists because the
    // sim/presentation split the `TS` line asserts has to be MEASURED before it is claimed: a
    // 32-byte chunk straddles cmd_wait_until_time (KEPT by the sim slice) and anim_frame_time
    // (dropped), so TD alone cannot say which side of the line a divergence fell on.
    int tact_field_lo = 0; // first unit index to field-hash (0 = off)
    int tact_field_hi = 0; // last unit index, inclusive
        // ---- TACT-REC: the ORDER JOURNAL ----------------------------------------------------------
    // 1 = observe both player-order seams and log a `TJ` line per PLAYER-caused call. Off by
    // default and free when off: the detours are not installed at all, so a run that does not ask
    // for a journal is byte-for-byte the run it was before this existed.
    // A flat EIP sampler over the game thread. 0 = off, and off costs nothing: no thread is
    // created and no hook is installed. See prof_start().
    int profile_hz       = 0;
    int tact_journal_rec = 0;
    // VERIFY: replay a journal's INPUT and journal the ORDERS the game emits because of it.
    // Neither recording nor replaying quite -- it is the semantic comparison between them, and the
    // only arm that can tell "the replay is deterministic" from "the replay is the same session".
    int tact_journal_verify = 0;
    // Replay: the journal file to issue orders from, resolved next to the exe when relative. Empty
    // = no replay. Recording and replaying at once is refused at arm time -- the recorder would
    // journal the replayer's own injected orders and double every one of them on the next pass.
    char tact_journal[260] = "";
    // Log EVERY call to either seam with its return address, not just the player-caused ones. The
    // instrument that PROVES the site filter rather than assuming it: a headless run has no input,
    // so anything it reports is input-independent by construction.
    int tact_journal_probe = 0;
    // Inject a known cursor move + click pair + keypress at this frame (0 = off). Test scaffolding
    // for the journal's own round trip -- see tji_probe_tick.
    int tact_input_selftest = 0;
    // ---- UI-REC: the same input journal, indexed on PRESENTS instead of tactical frames ---------
    //
    // WHY A SECOND ARM AND NOT A FLAG ON THE FIRST. The input recorder (tji_record) is already
    // mode-agnostic -- it reads the two global 128-slot rings the whole game drains, menu included --
    // and its ONLY tactical dependency is that it stamps each record with g_tact_step. But
    // g_tact_step advances in llm_tact_frame, which is mode 6's per-frame pump: it does not tick once
    // in the menu or in strategic mode, so the tactical arm cannot record the sequence that STARTS a
    // game. The fix is an index that exists in every mode (the present counter) and a hook that runs
    // in every mode (net_lockstep::on_present, which the capture, overlay and UI-drive harnesses
    // already ride through the whole menu walk), not a second recorder.
    //
    // MUTUALLY EXCLUSIVE WITH THE TACTICAL ARM, refused at arm time. Both arms stamp the same
    // journal with the same field, so a session that armed both would interleave two unrelated
    // counters into one file -- and the resulting journal would look well-formed.
    int ui_journal_rec = 0;
    // Dismiss the new-game intro movie + NEWGAME.TXT briefing via the game's own key path
    // (avi_skip_tick), instead of waiting both out before the sim ticks once.
    int skip_intro_avi = 0;
    // Replay: the journal to inject from, resolved like tact_journal. Empty = no replay; recording
    // and replaying at once is refused at arm time for the tactical arm's reason (the recorder cannot
    // tell our injected events from a player's -- they enter through the same ring).
    char ui_journal[260] = "";
    // Which BYTE of the poked region to flip. 0 is the region base, which for tact_units is unit
    // 0's `type` -- inside the sim slice, so the standing RED arm fires BOTH verdicts. Pointing it
    // into the animation window instead is the SECOND red arm: a poke that must move `combined`
    // and must NOT move `sim`. Without it "the sim slice excludes the animation window" is a claim
    // about code nothing watched, which is the exact shape of a vacuous gate.
    int tact_poke_off = 0;
    // The mutation arm. A newly-hashed region set proves nothing until it has been WATCHED TO FAIL,
    // and TACT-PREP's done_when says so explicitly: poke one tactical region mid-run and the two
    // arms must stop agreeing at exactly that frame.
    int tact_poke_at  = 0;  // tactical frame to poke at (0 = never)
    int tact_poke_idx = -1; // which TACT_HASH_REGIONS[] index to poke
    // ---- TACT-SYNTH: the synthetic tactical order workload ---------------------------------------
    // WHY THIS EXISTS, and it is NOT the same reason synth_move exists. The tactical world is
    // already in motion with zero input -- 400 frames of the shipped POZ*.DAT entry produced 400
    // DISTINCT combined hashes (the tactical-probe work 5b), because llm_tact_unit_owner_tick runs
    // the same AI for BOTH sides and every shipped mission gives even the player's squad
    // DEFENSE:GUARD/ATTACK. So this workload is not here to un-idle the world. It is here for
    // COVERAGE: a no-input run reaches only the mission script and the FOV-engage AI, and 10 of the
    // 22 llm_tact_unit_enqueue_command call sites are PLAYER-COMMAND sites that nothing in a
    // headless run ever executes -- group orders, kneel/stand, mines, the 0xa teleport jump, the
    // stance toggles, and llm_tact_group_issue_order's whole body.
    //
    // IT IS A WORKLOAD, NOT A RECORDING. Reproducing a specific human session would need the
    // selection/stance writes llm_tact_frame makes DIRECTLY (see the coverage note on the emitter),
    // and that is a different instrument. This one is the analogue of synth_move: a seeded,
    // reproducible stream of orders issued through the real APIs.
    //
    // TWO ENTRY POINTS, deliberately. Unit-scoped actions call llm_tact_unit_enqueue_command
    // directly (the 6 llm_tact_frame player sites all do exactly that, with no persistent write
    // besides the call). Group-scoped actions call llm_tact_group_issue_order, because calling the
    // funnel underneath it would SKIP the auto-stop, the op-0x46 queue wipe, the op-1
    // already-there confirm and the busy/idle interrupt_flag choice that its body performs -- i.e.
    // it would cover the funnel while leaving the 766-byte closure member untested.
    int tact_synth      = 0;   // 1 = arm the workload (0 = not a byte of it runs)
    int tact_synth_seed = 0;   // pinned by the runner. 0 while armed is a HARD ERROR, same rule as
                               // synth_seed: an unseeded workload is not reproducible evidence.
    int tact_synth_at    = 60; // first ordering frame (let mission load + the first AI passes settle)
    int tact_synth_every = 6;  // re-issue cadence in tactical frames (must be >= 1). 6, not 12:
                               // the rotation is TA_COUNT actions long, so the cadence sets how many
                               // times each capability fires in a run. At 12 over 400 frames each
                               // fired twice, and def_stat -- which advances ONE step per action, as
                               // a real click does -- never reached the 4 that triggers its enqueue.
    int tact_synth_stop  = 0;  // last ordering frame (0 = no limit)
    int tact_synth_units = 4;  // how many player units the group arm selects (>=1)
    int tact_synth_owner = 0;  // the `owner` byte that marks a PLAYER unit; every real selection
                               // path in llm_tact_frame gates status|=1 on owner==0
    // The DIRECT-WRITE arm, separable on purpose. Three player capabilities have NO enqueue opcode
    // at all -- selection itself, weapon choice (active_gun +0x5eb) and the control-group slot
    // (field_0x5f0) -- and one more, the sidebar def_stat cycle, only reaches its enqueue call on a
    // counter's 3->4 transition. A workload that drives ONLY the funnel therefore cannot reach them,
    // which is the caveat this item was opened to answer. Keeping the arm behind its own key means
    // the answer is MEASURED (run with 0, run with 1, compare the coverage line) instead of argued.
    int tact_synth_direct = 1; // 1 = also drive the direct writes the funnel cannot express
    // ---- TACT-PREP: the deterministic rand -------------------------------------------------------
    // llm_rand IS the Watcom CRT rand(), and NO srand is linked anywhere in the binary -- its LCG
    // runs from the thread-data block's static initial value, so the stream is reproducible but
    // process-global and CALL-COUNT-COUPLED: anything that drew earlier shifts every later draw.
    // Seeding is therefore not available and interception is. Tactical is the only consumer that
    // matters (6 of its 7 callers are llm_tact_*), and llm_tact_unit_spawn draws once per unit
    // during mission LOAD, so an un-pinned rand makes even the entry state differ between two runs.
    int pin_rand  = 0;     // 1 = replace llm_rand's whole body with the LCG below
    int rand_seed = 12345; // the replacement's starting state; any nonzero value will do
    // ---- SPCAMP-SEED: the STRATEGIC seed, which pin_rand does NOT reach -------------------------
    // These are two different generators and conflating them cost a whole hypothesis. llm_rand is the
    // Watcom CRT rand() above; the STRATEGIC world draws from llm_strat_rng_next (a 16-bit rotate-mix
    // LCG over _G_LLM_STRAT_RNG_STATE[4]), wrapped as llm_rand_below (channel 0) / _fx (1) / _ai (2).
    // No call-graph edge connects them, so `pin_rand=1` does nothing whatsoever for strategic state.
    //
    // WHAT IS ACTUALLY NONDETERMINISTIC. llm_strat_planet_session_begin seeds channels 0 and 1 from
    // llm_strat_rng_seed_wallclock_seconds() -- time() + localtime(), returning tm.tm_sec, THE REAL
    // WORLD CLOCK SECOND -- and then, still inside its own body, calls llm_game_land_players_on_planet,
    // whose per-player llm_strat_claim_landing_spot draws from channel 0 to pick a landing site. So a
    // campaign started at :17 past the minute lands its players somewhere else than one started at
    // :43, and no amount of input fidelity in a journal replay can make the two agree. That is the
    // divergence-at-step-1 both recorded sessions showed. (pin_wallclock replaces GetCurrentTime; it
    // does not touch time(), so it does not cover this either.)
    //
    // The pin is a whole-body replacement returning a constant, which is the smallest change that
    // makes the roll a function of the seed alone. Everything downstream of the landing was already
    // deterministic: session_begin reseeds channel 0 from _G_LLM_STRAT_RNG_SEED_BYTE straight after
    // landing, and in campaign mode nothing ever writes that byte.
    int pin_strat_seed = 0; // 1 = replace llm_strat_rng_seed_wallclock_seconds with `strat_seed`
    int strat_seed     = 7; // the constant it returns. The real one is 0..59 (a clock second); any
                            // value in that range is a second the game could genuinely have seen.
    // SPCAMP-SEED's instrument, and it is the half that makes the pin PROVABLE rather than asserted:
    // log the RNG channel state either side of landing plus each enabled slot's chosen spot. Without
    // it "the runs agree" and "the landing is constant for some unrelated reason" look identical.
    int land_log = 0;
    // ---- D6: the synthetic moving-unit workload (a determinism gate over an
    // IDLE world). Without this every determinism run compares a world in which nothing happens, so
    // a transient divergence re-converges for free and a CASCADING desync cannot be detected.
    int synth_move = 0;   // 1 = arm the workload
    int synth_seed = 0;   // per-run seed from the runner (os.urandom). 0 while armed is a HARD ERROR:
                          // a fixed destination every run is exactly the vacuity this exists to kill.
    int synth_at    = 60; // first order step (let the session settle first)
    int synth_every = 1;  // re-issue cadence in steps (1 = EVERY step, 0 = issue once).
                          // Every step keeps units[] changing continuously, which is the whole
                          // point: a divergence needs somewhere to go. Cost measured, not assumed
                          // -- measured by run comparison, not estimated.
    // ---- SV1-P: the in-game save trigger. There is no UI route to it: every registered UI scenario
    // enters through NETWORK GAME, and the MP ESC-menu widget arrays carry DIPLOMACY where the SP one
    // carries the save/load widgets, so "Save game" is never on screen in a harness run. This calls
    // the ROOT instead -- the same call game::SaveGame makes, from real mid-match state, and
    // repeatable in a way clicking a menu would not be.
    int save_at    = 0;  // first step at which to save the current planet (0 = never)
    int save_every = 0;  // re-save cadence in steps (0 = once)
    int save_count = 1;  // how many saves in total; the acceptance test wants >= 3
    int save_keep  = 0;  // 1 = copy each save aside as save%02d.dat.<n>. The per-planet
                         // path is FIXED, so a sequence of saves overwrites itself and the
                         // period-2 invariant has nothing to compare.
    int load_at = 0;     // first step at which to LOAD the per-planet file back (0 = never).
                         // With `[promote] load=0` this runs the ORIGINAL
                         // LoadPlanetFromDisk -- which is exactly what "a promoted build's
                         // save loads in an unpromoted build" needs. It REPLACES live
                         // state, so place it deliberately.
    int savegame_at = 0; // step at which to write a whole .sav via game::SaveGame (0 = never).
                         // The CONTAINER root; the per-planet save_at is a different one.
    // Which .sav loadgame_at reads, WITHOUT the extension (the root appends ".sav"). The default
    // pairs with savegame_at's, which is what makes the two a round trip. Overriding it is how the
    // A/B reaches a REAL save: the harness's own save has exactly ONE embedded member, so a run that
    // only ever loads it leaves the multi-member extraction loop untested -- and "1 of 1 members
    // byte-identical" reads just as green as "3 of 3".
    char loadgame_name[64] = "uitest";
    int  loadgame_at       = 0; // step at which to read a whole .sav back via llm_game_load
                                // (0 = never). The container READ root -- and, unlike load_at,
                                // it also re-extracts every embedded per-planet member, so it
                                // REWRITES save%02d.dat. Place it AFTER savegame_at: the two
                                // together are the only in-game route to a container round trip,
                                // there being no UI path to save/load in a network session.
    int load_every = 0;         // re-load cadence in steps (0 = load once)
    int load_count = 1;         // how many loads in total. Interleaving saves and loads is what
                                // exposes the period-2 invariant: every load reverses the region
                                // record order, so save1 and save3 agree and save2 is the reverse.
    // ---- mp:X1b: the live snapshot verbs ---------------------------------------------------------
    //
    // TWO KEYS AND THEY ARE DELIBERATELY ASYMMETRIC, which is unusual here and is the whole design:
    //
    //   snapshot_at=N     the SENDER's verb. At sim step N this peer captures the world (the same
    //                     capture LIB-WORLD's world_capture writes to a file) and hands the blob to
    //                     MH_Net_SnapshotSend. Host-only in every registered scenario, via
    //                     ui_test's --harness-extra-host, because two peers each sending a snapshot
    //                     to the other is not a test of anything.
    //   snapshot_import=1 the RECEIVER's verb, and it is SYMMETRIC on purpose: both peers arm it.
    //                     A peer nothing is sent to polls, gets MH_SNAP_IDLE forever and allocates
    //                     nothing (the module opens its arena on the first CHUNK, not on the first
    //                     poll), so arming it on the sender costs one call per step and removes the
    //                     need for a second asymmetric knob. The peer that IS sent to imports.
    //
    // WHY THE VERBS ARE INI KEYS AND NOT uiscript OPS. A uiscript op would put the trigger on the
    // UI-driver's clock -- "when the walk reaches this line" -- and the thing being captured is a SIM
    // STEP. `snapshot_at=N` fires inside on_sim_step's hash block, from the same `per[]` the R line
    // is written from, so the blob and the hashes it must reproduce describe ONE instant with no
    // window between them. That is world_capture's own argument for its placement and it applies
    // unchanged; a frame-clock trigger would reopen exactly the gap it closes.
    int snapshot_at     = 0; // step at which to capture + send the world (0 = never)
    int snapshot_to     = 1; // which player id to send it to (the host's client is 1)
    int snapshot_import = 0; // poll for an inbound snapshot every step and import it when whole
    int snapshot_log    = 0; // log a `; SNAPSHOT RX` progress line every N steps (0 = off)
    // ---- mp:X3 step one: IMPORT AND HOLD ------------------------------------------------------
    //
    // THIS KNOB EXISTS TO SPLIT ONE MEASUREMENT IN TWO, and it is a diagnostic instrument rather
    // than a feature. X1b left a fault: a peer that imports a live world blob dies with
    // 0xC000041D within about one sim step, 3 runs of 3, and TWO causes were indistinguishable
    // from the outside --
    //
    //   (a) THE REWIND. The import rewinds this peer's world (clock, order queues, rosters) to the
    //       sender's step while the turn engine keeps feeding it inputs for the LIVE step. Every
    //       subsequent sim step then runs an old world under new inputs.
    //   (b) THE RE-DERIVES. libmh_import_world runs seven of them against a session already in
    //       progress -- pathfinder_init REALLOCATES the two heap blocks `general` points at,
    //       pool_reset drains the region pool, and two registrars rewrite the 255-entry unit and
    //       building dispatch tables the match is dispatching through right now.
    //
    // Set `snapshot_hold=1` and the harness STOPS THE SIM at the moment of a successful import:
    // the importing step's own body never runs and the clock is frozen so no further step is ever
    // funded. Nothing about (b) changes -- the seven re-derives all ran. So:
    //
    //   the peer SURVIVES the hold  -> the import itself is safe here and the fault is (a), which
    //                                  is exactly the thing a resync (buffer, import, fast-forward)
    //                                  replaces. The rewind is the bug.
    //   the peer DIES anyway        -> the fault is (b) or the frame/render path walking imported
    //                                  memory, and no amount of input buffering can help; the
    //                                  re-derive ordering has to be fixed first.
    //
    // It holds the SIM only. The frame loop, the renderer, the UI interpreter and the transport
    // all keep running -- which is what makes the negative arm meaningful: a process that survives
    // 60 s of rendering the imported world has had every pointer the renderer walks exercised.
    int snapshot_hold = 0; // 1 = freeze the sim on a successful import (mp:X3 step one)
    // mp:X3. The SUB-DOMAIN trail's cadence, in steps (0 = off). Separate from region_hash_step
    // and deliberately meant to be set to 1: the RD row is nine hashes (~260 bytes) against the R
    // row's 61 (~1.1 KB), so it is affordable EVERY step, which is what makes it a trail rather
    // than a sample. The partition itself is printed once as `; [subdomain]` lines -- see
    // subdomain_map_emit for why the analyzer reads it from the log instead of holding a copy.
    int domain_hash_step = 0;
    int region_hash_step = 0;        // emit an "R <step> <h0>..<hN>" per-region line every N steps (0=off).
                                     // Lets mp_analyze.py pinpoint the exact (step, region) of a cross-peer
                                     // desync without a re-run. Set 1 for MP runs where a desync may appear.
    int order_mode = 0;              // Phase 2 order record/replay: 0=off, 1=record (dispatch->mh_orders.bin),
                                     // 2=replay (mh_orders.bin->queue each step). See the replay-harness notes.
    int replay_ai_off = 0;           // replay only: zero every player's ai_enabled once at start so ONLY the
                                     // recorded orders drive the sim (else the deterministic AI re-issues+doubles).
    int replay_suppress_enqueue = 0; // replay only: neuter the immediate-lane enqueue (return 0, add
                                     // nothing) AT THE SINK -- mh::orders::detail::enqueue, so the
                                     // game's entry AND every rebound libmh-internal caller are
                                     // covered, plus a byte neuter of the original entry when the
                                     // container is not promoted. See MH_Harness_LateArm.
    // [harness] clock_record -- write mh_clock.bin (the per-step game clock, raw doubles) INDEPENDENTLY
    // of the order recorder (LIB-REF-LIVE, 2026-09-11).
    //
    // WHY IT HAD TO BE SPLIT OUT. The clock track was written only under `order_mode == 1`, i.e. only
    // by a run that is also recording an order stream. LIB-REF-LIVE's fixture needs the OPPOSITE
    // combination -- a step-0 world blob plus a clock track and NO order stream, because the whole
    // point of that proof is that the in-sim AI is the sole order source and nothing is injected.
    // Under the old gate the only way to get a clock was to also produce the order stream the item
    // forbids. Measured before the split: an `order_mode=0` all-AI soak run directory contains
    // mh_world.bin and nothing else.
    //
    // `order_mode == 1` IMPLIES IT, which is what keeps every existing recording path byte-identical:
    // the gate below is `order_mode == 1 || clock_record`, so a record run that never heard of this
    // key writes the same clock it always did. Proven rather than asserted -- a 300-step sp_det record
    // before and after the split produced byte-identical mh_clock.bin AND mh_orders.bin.
    int clock_record = 0;
    // [harness] replay_isolate_input -- while a journal replay is armed, suppress the GAME's own
    // input-ring producer (llm_input_wndproc_tap) so the journal is the only writer. Default ON,
    // because a replay with two producers is not a replay of the recorded input; `=0` is the
    // negative arm SPCAMP-FLAKE's done_when needs. Full reasoning at the install site.
    int replay_isolate_input = 1;
    // so the live order source (AI/SP) can't add to the queue -- only the
    // injected recording does -- WHILE the AI keeps running its non-order tick.
    // The faithful-substitute test: AI-on record vs this should be identical.
    int order_log = 0; // 1 = each step where ORDER_PENDING or ORDER_QUEUE is non-empty, dump their
                       // contents (owner/unit/order_code/param0/exec_time) as ";ord" comment lines in
                       // the harness log. Settles what "order_pending" is (the scheduled lockstep cmd
                       // buffer) and WHY its cross-peer count differs: diff host-vs-client ";ord" lines
                       // at the desync step -- same orders/different release phase = benign; different
                       // orders = a real bug. Sampled at the hash point (post release_due, pre body).
    // ---- D3 negative test: deliberately corrupt ONE PRNG slot on ONE peer -----------------------
    // The oracle's own falsification test ("gates can pass vacuously"): a gate
    // that has never been shown to go RED is not evidence. Set these in ONE peer's mh_net.ini
    // (mp_run --harness-extra-host) and the run MUST report a divergence -- immediately in the
    // rng_state region, and then downstream wherever that slot's draws feed. Slot 2 is the AI's, so
    // slot 2 is the interesting one: it proves the AI channel reaches game state.
    int rng_perturb_slot = -1; // -1 = off; 0=strategic 1=fx 2=ai 3=unused
    int rng_perturb_step = 0;  // sim step at which to flip it, once
    // ---- D11 negative test: prove NEW regions are actually hashed AND localized ------------------
    // Flips one byte in every region from index `region_poke_min` upward, on this peer only, at
    // `region_poke_at`. One run then proves coverage + localization for ALL of them at once: every
    // poked region must appear in mp_analyze's diverging-region list, and no other.
    // SAFE ONLY FOR UNCLAIMED STATE. Aimed at player_data slots 2..7 in a 2-peer run, where those
    // slots are unclaimed, zeroed, and read by nothing -- do NOT point it at a live region and expect
    // the game to survive. Every poked region's NAME is logged, so an index drift shows up in the
    // transcript instead of silently poking something else.
    int region_poke_at  = 0;  // sim step at which to poke, once (0 = off)
    int region_poke_min = -1; // lowest REGIONS[] index to poke (-1 = off)
    // POKE EXACTLY ONE REGION (-1 = off, use the min..end range). Added 2026-09-05 for the A/B's
    // go-red arm, which wants to prove the oracle sees a change in ONE module's write region and was
    // getting `min..N_REGIONS` -- 36 regions for `units` at index 20. Poking a range is right for the
    // D11 sweep ("one run proves coverage for ALL of them") and wrong for a per-module red arm, where
    // every extra region is another chance to hit live state and take the process down before the
    // verdict is written. That is exactly what happened: see the poke_off note below.
    int region_poke_only = -1;
    // Byte offset INTO each poked region (default 0 = byte 0, the historical behaviour). D23 needed
    // it: `players` byte 0 is Players[0].race, which the sim reads, whereas name[32] at +0x10 is read
    // by nothing -- so the offset is what lets the poke stay inside the "unclaimed state" rule above
    // while still proving the region is hashed, compared and localized.
    int region_poke_off = 0;
    // ---- THE RAW REGION DUMP (built for SIM-DEEP-DIV, 2026-09-05) --------------------------------
    // Hex-dump one hashed region's bytes at a step window. READ-ONLY, and it changes no verdict --
    // it exists because the per-region hash localizes a divergence to (step, region) and then stops,
    // which is one resolution short of anything actionable. `buildings` is 218400 bytes of
    // map_object_building[8][100] and 212 of each record's 273 bytes are animation, pip and padding,
    // so "the buildings region differs" is compatible with a real sim bug AND with a presentation-only
    // drift. The tactical side already hit this wall and answered it with the per-field `TF` channel
    // (see emit_tact_units_sim); this is the strategic equivalent, done as raw bytes rather than a
    // field table so it needs no per-struct knowledge and works for any region.
    //
    // Diff two arms' `RX` lines offline: the byte offset divides by the record stride to name the
    // record and the remainder names the field. One step of `buildings` is ~437 KB of hex, which is
    // why this is a WINDOW and not a cadence -- dump the step the hash first disagreed at, not the run.
    //
    // CAVEAT for a MASKED region: this dumps what is in memory, while hash_slice() may mask parts of
    // it out (mask_ctrl_group, mask_soldier_anim). For those regions a byte here can differ while the
    // hash agrees. `buildings` is unmasked, so the two coincide.
    int rdump_rid = -1; // REGIONS[] index to dump (-1 = never); mp_analyze.REGION_NAMES order
    int rdump_lo  = 0;  // first sim step to dump at
    int rdump_hi  = 0;  // last sim step to dump at, inclusive
    // ---- AND THE SAME WINDOW OVER AN ARBITRARY ADDRESS (built for SIM-SAVE-DIV, 2026-09-05) -------
    // `rdump_rid` can only reach a HASHED region, and that is exactly the state an oracle can already
    // see. The state that costs days is the state NOTHING hashes: SIM-SAVE-DIV fault 3 is a wrong
    // heading byte in `path_buffers`, which promoted closure code writes and which is in none of the
    // 56 hashed regions -- so it sits there undetected until a unit consumes it, and the first thing
    // any instrument sees is a one-byte symptom in a DIFFERENT region at an unrelated step
    // (a region in no hashed region is invisible until it leaks). Three faithful-verdict investigations later, the question that was
    // still unanswerable was "do the two arms' SOLVER OUTPUTS differ, and at which index" -- and no
    // knob could ask it.
    //
    // So: dump [rdump_addr, rdump_addr + rdump_len) verbatim, at the SAME sample point as the `R` line,
    // in BOTH arms. Same `RX` line shape, with the rid field set to -1 so an address window is never
    // mistaken for a region, plus one `; [rdump] ADDR` line recording the base and length -- a dump
    // whose base is not in its own log is unreadable a day later.
    //
    // READABILITY IS CHECKED, NOT ASSUMED. A region index is safe by construction; a hand-typed
    // address is not, and a diagnostic that faults the run it was added to diagnose is worse than no
    // diagnostic. The range is VirtualQuery'd once and REFUSED loudly if any page of it is not
    // committed and readable, by the same predicate net_seams.cpp's scanner uses.
    int rdump_addr = 0; // absolute address to dump (0 = off); use INSTEAD of rdump_rid
    int rdump_len  = 0; // bytes; capped at RDUMP_ADDR_MAX so a typo cannot fill the disk
    // REGISTRY rid, dumped through live_base() -- the UNHASHED-REGION route, and the reason it
    // exists is a false finding it would have prevented. `rdump_rid` above indexes the HASH table, so
    // it cannot reach an MF_VIEW region at all; the obvious workaround is to look the region's base
    // out of mh_regions.gen.h and use rdump_addr. That is WRONG for any region the AI island
    // relocated out of .bss: the stock VA is abandoned AND POISONED, so the dump comes back 0xCD for
    // its whole length and a cross-arm comparison reports every byte differing -- which reads as a
    // massive finding rather than as an instrument pointed at the wrong memory. Measured 2026-09-11
    // on _G_LLM_STRAT_AI_SITE_CANDIDATES: 65536 of 65536 bytes 'differing', all of them 0xCD.
    // live_base() follows the relocation, which is the whole point of the registry.
    int rdump_regid = -1; // mh::state::region_id (-1 = off); dumped at reach_of(rid) bytes
    // ---- THE RECV FILTER'S NEGATIVE TEST (built for U30(b), 2026-08-29) --------------------------
    // Send ONE frame whose outer tag is outside the game's valid 1..5 set, from this peer, at this
    // step. What it proves is a NEGATIVE, and the negative is the point: MH_Seam_GameRecv drops any
    // datagram whose type byte is not 1..5 before the game sees it (net_seams.cpp, since 2026-07-11 --
    // a leaked LOBBY packet used to reach llm_net_lockstep_dispatch's garbled-stream arm and tear the
    // session down at ~step 31). A filter that has never been watched to eat anything is a filter
    // nobody has tested, so: arm this and read the receiver's log for
    //     ; GameRecv DROP non-lockstep sender=0 len=1 type=0x06
    // with no `on_gameover` and no session teardown. First run, 2026-08-29: exactly that, and the run
    // stayed determinism-clean to step 300.
    // It was built to drive U30(b) -- the garbled arm is the ONLY caller of llm_ui_outcome_dialog that
    // can reach it with SESSION==3 -- and instead proved that path unreachable in this build, which is
    // why the endgame detour is inert. See the g_sync_gameover block in net_lockstep.cpp.
    // Host-only by construction (--harness-extra-host); the frame is one byte.
    int garble_at = 0; // sim step at which to send one unknown-outer-tag frame (0 = off)
    // ---- D10: is the AI actually RUNNING? ---------------------------------------------------------
    // The whole point of D10 is that every green run so far compared a world in which the AI never
    // drew a random number. So an AI-active run must PROVE the AI ran, per-step, rather than be
    // assumed to have -- and "slot 2 non-zero at the stop step" is not that proof: it shows a value,
    // not motion. This emits the master gate, the loop bound, every player's ai_enabled, and the AI
    // PRNG slot on a cadence, so "advancing" is READ off the log instead of argued.
    int ai_probe_step = 0; // emit an "; AIPROBE" line every N steps (0 = off)
    // ---- the SAVE-STATE INDEX probe (2026-08-02) --------------------------------------------------
    // Emit one "; AISTATE" line per player, ONCE, at this step. Built to answer a question the
    // shadow rig kept failing to answer by itself: WHICH SCENARIO exercises a given AI branch. Batch
    // B repeatedly landed T2-not-T1 because the branch carrying a function's point never ran on the
    // rig's two available starts -- plan_unit_training reported roles_max=0 on every one of 600+
    // calls because count_unit_build_sources found no idle production source anywhere on the map.
    // More steps do not fix that; a different SAVE does. So sweep the community saves once, index
    // what state each one actually contains, and pick per function thereafter.
    //
    // ONE-SHOT, not a cadence: this is a property of the loaded save, not a time series, and a
    // per-step version would bury the answer in thousands of duplicate lines.
    int aistate_probe_at = 0; // step at which to emit "; AISTATE" (0 = off)
    // ---- D2: units[].ctrl_group_id ---------------------------------------------------------------
    // mask_ctrl_group is the FIX (see mh::state::emit_units); default on. Settable to 0 so ONE build can run
    // both arms of the A/B -- an unmasked arm that stays green would mean the probe below never fired.
    int mask_ctrl_group = 1;
    // ---- soldiers[].anim_change_count (2026-08-28) ------------------------------------------------
    // mask_soldier_anim is the FIX (see mh::state::emit_soldiers); default on, same A/B shape as
    // mask_ctrl_group above and for the same reason -- one build must be able to run the UNMASKED arm,
    // or nobody can show the mask does anything. The byte is advanced ONLY by the two strategic
    // renderers as they draw, is read by nothing in the sim, and diverges by construction in MP
    // because peers hold different camera positions.
    int mask_soldier_anim = 1;
    // ---- Planets[].bank[] + tlo_index/soldier_sprite_bank_offset (LIFT-TABLE S2, 2026-09-09) ------
    // mask_planets_gfx is the FIX (see mh::state::emit_planets); default on, third instance of the
    // same A/B shape and MANDATORY for the same reason -- the 102 masked bytes per planet record are
    // written by cfg_final_planet_FillBankData and read only by the sprite-bank loaders, and once
    // LIFT-TABLE S3 hands that graphics tail to the host, a mask nobody can run unmasked is a mask
    // nobody can audit. Setting it to 0 restores the unmasked walk of the planets slice.
    int mask_planets_gfx   = 1;
    int synth_ctrlgroup    = 0; // step at which to assign this peer's mothership to a control group (0=off)
    int synth_ctrlgroup_id = 1; // which group (1..9; 0 would be "none" and write nothing)
    // ---- THE GROUP-MOVE WORKLOAD (AI1C read-bridge, 2026-08-07) --------------------------------
    // Drives the machinery that WRITES the six group-move regions the minimap reads
    // (_G_LLM_STRAT_GROUP_CENTROID_X/Y, _GROUP_ROUTE_STEPS, _GROUP_ORDER_OWNER, DAT_0051de38,
    // _GROUP_MEMBERS). Nothing else in the harness reaches them: synth_move orders ONE unit, which
    // takes the plain move path, and synth_ctrlgroup only assigns a ctrl_group_id.
    //
    // WHY THIS IS THE ONLY WAY IN. Those regions are written by llm_strat_group_move_order_commit,
    // whose sole caller is llm_strat_unit_state_group_marshal -- a UNIT-STATE HANDLER (state 0x0a
    // GROUP_MARSHAL) that nothing calls directly. A group move is not a call, it is an ORDER: the
    // issuer sets each member's order and the sim later ticks it into state 0x0a. That is also
    // exactly how the AI reaches this machinery, which is what made the whole family sim-owned
    // rather than AI-owned (the AI closure).
    int synth_groupmove   = 0; // step at which to issue the group move (0 = off)
    int synth_groupmove_x = 0; // destination tile; 0/0 means "offset from the group's own position"
    int synth_groupmove_y = 0;
    int synth_groupmove_n = 4; // how many of this player's units to put in the group (>=2)
    // ---- U32: THE CONQUEST WORKLOAD -- a scripted route to a real END CONDITION ----------------
    //
    // WHY THIS EXISTS. Every determinism run and every match_launch to date has compared a world
    // that could not END. llm_strat_player_presence_lost has never once taken its elimination
    // branch on the rig (tracker U32, measured three ways: match_launch, the determinism workload,
    // and a 2500-step all_ai run all produce ZERO calls), so every endgame clause -- U30 (b),
    // D20 (b)+(c), U19, D21 -- rests on a path nothing exercises. This is the missing stimulus.
    //
    // WHY IT IS NOT all_ai=1, AND WHY THAT DISTINCTION IS THE WHOLE DESIGN. all_ai converts each
    // peer's human slots to AI LOCALLY, at landing, outside the order pipeline: the peers then
    // simulate different worlds by construction (measured 2026-08-29: 2437 of 2500 steps
    // mismatching). Every action below is issued as an ORDER through llm_strat_order_dispatch,
    // which in SESSION_MODE==3 stages it, clamps its exec_time to LOCKSTEP_HORIZON and broadcasts
    // it -- so both peers execute it on the SAME step from the SAME queue. That is what makes an
    // elimination produced here trustworthy enough for the four items above to lean on it.
    //
    // THE _enqueue TWINS ARE FORBIDDEN HERE. Every typed order wrapper exists twice: the plain name
    // routes through order_dispatch (replicated), the _enqueue suffix routes straight to
    // order_enqueue (local, immediate). Calling an _enqueue twin here would reproduce the all_ai
    // failure exactly, and would look identical in the log until the run desynced. Nothing does.
    //
    // HOST-ONLY BY DESIGN. Arm it on ONE peer (--harness-extra-host). A symmetric workload would
    // have both peers building and attacking, which agrees by coincidence; the point is that the
    // PEER does nothing of its own and still ends up dead, which can only happen through the wire.
    //
    // THE SHAPE OF THE MATCH, because the tile arithmetic below is meaningless without it. A fresh
    // H player in MP lands with EXACTLY ONE object: an AIRBORNE H_HELI_MOTHER (cfg Unit 24) and
    // ZERO buildings -- llm_game_land_players_on_planet (0x0045534e) branches on status_flags &
    // HUMAN, and the .DMP base layout is the AI arm of that branch, which never runs for a human.
    // So the first thing this workload does is LAND THE PEER'S MOTHERSHIP: order 0x18 turns cfg
    // Unit 24 into cfg Building 45 (N_human_Matka, 2000 energy) via Unit[24].equivalent, giving the
    // soldiers a STATIONARY GROUND target -- the recipe's "enemy base", which otherwise does not
    // exist. Landing is SAFE for the peer: deploy_to_building calls bldg_construction_complete
    // BEFORE llm_strat_unit_teardown (0x00481a6b, decompile lines 90 then 97), so buildings_alive
    // is already 1 when units_alive hits 0 and presence_lost's OR-gate keeps them alive.
    int conq = 0; // 1 = arm (0 = not a byte of it runs)
    // A RUN LABEL, NOT A SEED, and it deliberately no longer hard-errors on 0. It was copied from
    // synth_move, where the workload picks a RANDOM destination and an unseeded run is genuinely
    // unreproducible. This workload has no randomness of its own: every order is issued at a phase
    // boundary determined by observed state, so the same config produces the same sequence and the
    // seed fed nothing but the banner. Requiring it enforced a reproducibility it did not provide --
    // and the real per-run variation (where an order LANDS, via the lockstep horizon) is not
    // seedable from here at all. Kept because writing the run's identity into its own log is cheap
    // and the runners already pass one.
    int conq_seed = 0;
    int conq_at   = 60; // first action step (let landing + the first ticks settle)
    // Milestones, as offsets from conq_at. Named rather than derived so a run that fails at one of
    // them says WHICH one in the log without arithmetic.
    // THE DEPLOY NEEDS A MOVE IN FRONT OF IT, and that is measured rather than reasoned. The first
    // rig runs issued 0x18 alone at a mothership that was IDLE-SCATTERING (order/state 0x12, goal
    // reshuffling every probe, 32360 scatter records in one run) and it bounced straight back to
    // scatter every time: deploy_approach checks the footprint of the tile IN FRONT of the unit by
    // its heading, and a wandering unit has an arbitrary heading and position, so the check fails
    // silently. The 2026-08-28 human capture shows the real gesture is TWO orders on the same unit:
    //     step 193  unit=1 own|kind=0x0080 param0=0x0010 code=0x000B args[0]=44   <- move
    //     step 202  unit=1 own|kind=0x0080 param0=0x0018 code=0x000B args[0]=44   <- deploy
    // i.e. park it first, then deploy. conq_peer_move_at is that first order. The gap here is far
    // wider than the human's nine steps because their mother was already next to the spot and ours
    // has to stop scattering and walk back.
    // LANDING IS OFF BY DEFAULT, and that is a retreat from realism made on purpose (user,
    // 2026-08-29: "let's just make the game end somehow without realistic scenario"). Two rig runs
    // showed the peer's mothership CANNOT be commanded from outside: it sits in IDLE_SCATTER
    // (order/state 0x12) re-issuing its own wander every few steps -- 32360 scatter records in one
    // 1000-step run -- and it swallowed both the 0x10 move and the 0x18 deploy. Since the buildings
    // never needed it either (the 0xf2 handler has no base precondition), the landing existed only
    // to give the attackers a stationary ground target. It is cheaper to attack the mother where it
    // flies. Set conq_peer_move_at/conq_peer_land_at if a future session wants to retry the deploy.
    // THE DEPLOY WORKS, and the thing that was defeating it was OURS. Two runs' worth of "the
    // mothership sits in IDLE_SCATTER and swallows every order" was wrong: mp_run arms the D6
    // synth_move workload BY DEFAULT (--synth-move defaults to 1), and that workload orders each
    // peer's OWN MOTHERSHIP to a fresh random destination EVERY STEP. It was overwriting the deploy
    // on the step after we issued it, on both peers, with a new random seed per run -- which is also
    // why the match length wandered between 1550 and 2000 steps across six runs while pin_rand and
    // pin_wallclock changed nothing. With synth_move off the chain runs exactly as the RE says:
    //     order 0x10 -> order=16 state=18 goal=(51,21)     the move takes
    //     order 0x18 -> order=24 state=23                  0x18 DEPLOY_APPROACH -> 0x17 DEPLOY_TO_BUILDING
    //                -> primary_mother_unit = 0            the mother is now a BUILDING
    // See the arm guard: conq and synth_move are mutually exclusive and the run refuses rather than
    // quietly fighting itself.
    // BOTH DEFAULT OFF, and the reason is OUR building-attack path, not the game. The deploy itself
    // WORKS (see above): the mother converts to a building with 5000 energy. But in that run the
    // five attackers never fought it -- the probe puts them at (17,49),(13,49),(10,53),(9,41),
    // clustered around OUR OWN base at (15,44) with the target 43 tiles away at (50,19), and every
    // one of them sitting in order 0x10 (a MOVE) with state 0x11. So the 0x1d attack-building order
    // was issued and did not stick, and the target's energy never moved off 5000.
    // DO NOT READ THAT AS "the landed mother is too tough" -- that was the first conclusion and it
    // is not supported. The 0x1d call is UNPROVEN: it may be the weapon argument (we pass
    // ORDER_WEAPON_AUTO), the target encoding, or aircraft breaking off to return home (see
    // llm_strat_unit_state_attack_unit's home_x/home_y arm). Order 0x1e against the SAME mother in
    // the AIR is proven -- four eliminations across four runs. Until 0x1d is debugged the default
    // path attacks the unit; turn these on to reproduce the deploy.
    int conq_peer_move_at = 20;  // +20:  order 0x10 -- move the peer's mother onto its landing tile
    int conq_peer_land_at = 120; // +120: order 0x18 -- LAND it (host-issued)
    int conq_bldg1_at     = 60;  // +60:  academy
    int conq_bldg2_at     = 90;  // +90:  barracks
    int conq_units_at     = 150; // +150: the attackers
    // +400, NOT +200. deploy_to_building descends by one elevation step per tick before it calls
    // construct_finalize, so the building does not exist for a few hundred steps after the order.
    // The first version checked at +200 and reported "the mother did NOT convert" while the mother
    // was still visibly descending -- a false failure that looked exactly like the real one.
    int conq_group_at     = 400; // +400: verify the landing + the attackers, then control-group them
    int conq_attack_at    = 430; // +430: first attack order
    int conq_attack_every = 200; // re-issue cadence, 0 = issue once. An attack order is abandoned
                                 // once the unit's own state machine resettles it; a real player
                                 // clicks again. Drop to 0 if a run shows the first order sticks. // re-issue cadence, 0 = issue once. An attack order is abandoned
                                 // once the unit's own state machine resettles it; a real player
                                 // clicks again. Drop to 0 if a run shows the first order sticks. // re-issue cadence, 0 = issue once. An attack order is abandoned
                                 // once the unit's own state machine resettles it; a real player
                                 // clicks again. Drop to 0 if a run shows the first order sticks.
    // THE FALLBACK for the cross-player deploy. conq_peer_land_at has the HOST issue an order whose
    // owner is the PEER. That is legal as far as anything measurable says -- the wrapper takes
    // `player` as an argument and its only PlayerSide test gates a local bump-sound preview, and the
    // order container has no authority check anywhere -- but it depends on
    // Players[peer].controller_flags bit 2 being set on the host, which is ASSERTED below rather
    // than assumed. If that assertion fires, deploy the key set with --harness-extra (BOTH peers)
    // and set conq_land_self_at instead: each peer then runs only the clause that applies to it and
    // the deploy is issued by the unit's own owner. Same wrapper, same order, no code change.
    int conq_land_self_at = 0; // +offset at which a NON-conquest-host peer lands its OWN mother
                               // (0 = off). Mutually exclusive with conq_peer_land_at.
    // cfg TABLE INDICES -- not cfg_enum_E_BUILDING / E_UNIT_TYPE values. Resolved 2026-08-29 from
    // the decompiled INIT.CFG (68 buildings / 54 units / 27 weapons; src/formats/unpack.py then
    // cfgkit.cfg_decompile). The distinction matters: the academy's TYPE (R_hFABRYKA) is shared with
    // Fabryka1/2/3, so type cannot identify it and index can.
    // OFF BY ONE AGAINST THE DECOMPILED cfg, MEASURED ON THE RIG 2026-08-29 -- three independent
    // confirmations in one run, so this is the mapping and not a coincidence:
    //     passed 27 -> built N_human_Elektrownia   (cfg 26, a REACTOR)
    //     passed 35 -> built N_human_Kopalnia2     (cfg 34, a MEGA-MINE)
    //     passed 25 -> spawned N_human_Matka_h     (cfg 24, a MOTHERSHIP -- five of them)
    // i.e. the game's table index is the cfg YAML index MINUS ONE, so what goes here is cfg + 1.
    // The orders themselves were fine all along; only the identifiers were. Do NOT "fix" these back
    // to the cfg numbers without re-running -- the first version built a power plant and a mine and
    // called them an academy and a barracks, and the log said nothing because the order succeeded.
    int conq_academy  = 28; // -> cfg 27 N_human_Akademia            (type R_hFABRYKA,  energy 500)
    int conq_barracks = 36; // -> cfg 35 N_human_Koszary_Zolnierzy   (type R_hKOSZARYz, energy 1000)
    // THE ATTACKER. Not the recipe's Soldier1 any more, and the reason is the target: the peer's
    // mothership never lands, so it must be killed IN THE AIR. Soldier1 carries only
    // BRON_KARABIN_7mm (ZIEMIA_POWIETRZE, power 3), which is 667 hits against 2000 energy.
    // N_human_Mysliwiec2 -- the tier-2 fighter -- carries BRON_RAKIETA_KOLIBER, target POWIETRZE,
    // power 125, range 10: 16 hits, so five of them is about four volleys. It also flies, so it can
    // reach a wandering target. Every other human unit with an air option tops out at DZIALKO_14mm
    // (power 12). Measured from the decompiled cfg 2026-08-29.
    int conq_soldier    = 34;   // -> cfg 33 N_human_Mysliwiec2 (energy 300, KOLIBER 125 vs air)
    int conq_n_soldiers = 5;    // the recipe's five (1..16). ALSO THE RUN-LENGTH LEVER: five
                                // soldiers at power 3 / long_time 0.2 deal 75 energy per game-second
                                // against the mother building's 2000, i.e. ~27 game-seconds ~ 1330
                                // sim steps of sustained fire. Doubling roughly halves that phase.
    int conq_use_groupmove = 0; // 1 = move the group to the enemy tile instead of per-unit attack
                                // orders. Per-unit is the default because it names its target in the
                                // order payload and reads no local selection state at all.
    int conq_force_kill_at = 0; // BRING-UP / FALLBACK (0 = off). Offset at which to issue order 0xf8
                                // (lethal damage) against every one of the peer's live objects.
                                // Still fully replicated -- the target is named in args[2]/args[3]
                                // and the handler reads nothing local -- so it satisfies U32 (b),
                                // but it is NOT a game. Use it once to prove the end-condition
                                // machinery works end to end, then turn it off.
    // THE WEAPON INDEX for the attack orders. 0xffffffff = let the game auto-select, which is what
    // llm_strat_unit_order_attack_building does when param_5 is -1: it calls
    // llm_strat_unit_select_weapon(player, unit, 1). If that finds nothing valid for the target
    // class it hands back a weapon the order cannot use, and the order silently does nothing --
    // which is a live suspect for why 0x1d against the landed mother never removed a point of
    // energy. Mysliwiec2 carries weapons{0: BRON_KARABIN_JONOWY (ZIEMIA, power 48),
    // 1: BRON_RAKIETA_KOLIBER (POWIETRZE, power 125)}, so 0 is the ground/building gun and 1 is the
    // anti-air one. Force one to take auto-select out of the picture.
    int conq_weapon = -1; // -1 = auto; else the unit's weapon slot index
    // Per-phase watchdog. A phase whose effect never arrives fails LOUDLY and stops the run --
    // waiting forever reads in the log exactly like a scenario that is still working. 900 is
    // generous against the slowest observed phase (the deploy descent, a few hundred steps).
    int conq_phase_timeout = 900;
    int conq_probe_every   = 100; // emit a "; CONQ" census line every N steps (0 = off). Without it a
                                  // run that ends with nobody dead cannot say WHY -- did the peer never
                                  // land, did the soldiers never spawn, did they never arrive, or did
                                  // they arrive and fail to do damage? It also MEASURES the walk, whose
                                  // duration the schedule can only estimate.
    // ---- THE ALL-AI SOAK (2026-08-05) -------------------------------------------------------------
    // Convert every HUMAN player slot to an AI-controlled one, so a solo run is an N-way AI match and
    // the LOCAL slot gets a real AI base too. The coverage lever that `loadgame_at` is for start
    // state, this is for ONGOING activity: the default sp_det world has one AI opposite an inert
    // human, and a long list of AI branches (turret threat rescan, the attack milestones, opponent
    // relations across >2 sides) simply has nothing to act on there.
    //
    // WHERE THE FLIP GOES, AND WHY IT IS NOT `ai_enabled`. llm_game_land_players_on_planet
    // (0x0045534e) loops slots 0..7 and branches on `_G_LLM_STRAT_PLAYERS[i].status_flags & HUMAN`:
    // the AI arm calls llm_strat_claim_landing_spot + llm_strat_spawn_ai_base, the human arm spawns
    // the starting unit inline and calls llm_strat_init_human_player_data (which sets ai_enabled=0).
    // Only spawn_ai_base establishes ai_home_tile_x/y, is_alien_race, the AI.SCR knobs, the build
    // plan and the 5 seed groups. So setting player_data[].ai_enabled=1 AFTER landing -- the obvious
    // move, and the one measured by hand on 2026-08-05 -- yields an AI that ticks with a home tile
    // of (0,0), i.e. every toroidal distance-to-home in the site scanners, the site sorter and the
    // group task handlers is measured from the map origin instead of its base. It runs; it is not
    // the AI. Hence a PRE-LANDING flip of the branch input, which makes the original's own code do
    // the initialisation.
    //
    // BIT 3 IS NOT OPTIONAL. b2 (0x04) is human-controlled and b3 (0x08) is AI-controlled; b3 gates
    // AI base-layout .DMP injection at llm_strat_bldg_completion_dispatch 0x479908, so clearing b2
    // without setting b3 gives an AI whose base never builds from its layout script. The retail
    // net-drop path (llm_net_player_remove 0x49dd6c) writes exactly this pair -- AND 0xfb; OR 0x10;
    // OR 0x08 -- which is why this is a configuration the engine already knows how to be in. It is
    // NOT evidence that a mid-game takeover works: that path runs after landing and never reaches
    // spawn_ai_base, which is the whole difference.
    //
    // DETERMINISM: this changes the world both peers simulate, so under MP it is a SYMMETRIC config
    // knob in the same family as game_speed_pct / sim_step_ms -- set it on both peers or neither.
    int all_ai = 0; // 1 = convert every enabled HUMAN slot to AI before landing
    // Repoint PlayerSide (0xe58354, the index the sim/UI treat as "me") at another slot. -1 = leave
    // it alone, which is the default and the better one: slot 0 still has a claimed landing site, so
    // the camera opens on that AI's base and a visible soak is watchable. Set it to an UNCLAIMED slot
    // (the hand experiment used 7) to detach local selection/ctrl-group state from any AI player.
    // Note synth_move and synth_ctrlgroup both act as PlayerSide -- pointing this at an empty slot
    // makes them no-ops, so the two are warned about at arm time rather than silently combined.
    int all_ai_observer = -1;
    // Watch for match resolution and say so. A soak that keeps stepping past a game-over is
    // measuring a finished world -- the same class of vacuity as `exit_on_stop=0` running past
    // stop_step, and it already bit one 10x/15000-step run whose counters were not comparable with
    // its 1x control for exactly this reason. 0 = off; N = check every N steps.
    int gameover_step = 0;
    int gameover_stop = 1; // 1 = set stop_step to the detection step so the run ends with a report
} g_cfg;

char g_dir[MAX_PATH]; // exe directory, trailing backslash
char g_log_path[MAX_PATH];
char g_ini_path[MAX_PATH];
// Paired paths: *_path = INPUT, next to the exe (where a human/rig drops a banked recording);
// *_out = OUTPUT, in the per-run log folder. See build_paths.
char g_seed_path[MAX_PATH], g_seed_out[MAX_PATH];
char g_boot_snap_out[MAX_PATH];
char g_world_out[MAX_PATH]; // LIB-WORLD: the step-0 world blob, in the run folder
char g_orders_path[MAX_PATH], g_orders_out[MAX_PATH];

// ---- order record/replay state ------------------------------------------------------------------
struct OrderRec {
    uint32_t step;
    uint8_t  order[ORDER_SIZE];
}; // on-disk + in-memory record (0x48 B)
constexpr uint32_t ORDERS_MAGIC = 0x524f484du;          // "MHOR"
HANDLE             g_rec_h      = INVALID_HANDLE_VALUE; // record-mode append handle (lazy)
OrderRec          *g_replay     = nullptr;              // replay buffer (VirtualAlloc)
uint32_t           g_replay_n   = 0;                    // records loaded
uint32_t           g_replay_i   = 0;                    // cursor (records are step-sorted)
bool               g_ai_zeroed  = false;

// ---- clock track (record/replay the per-step game clock) -----------------------------------------
// A real-time (fixed_step=0) recording advances the clock by the variable per-frame delta, NOT a fixed
// 0.1s. So a human recording can't be replayed with fixed_step -- the clocks (and everything timed off
// them) diverge. Fix: record the game clock each step, and on replay PIN TOTAL_GAME_TIME to the recorded
// clock so the sim reproduces the exact per-step deltas. mh_clock.bin = raw doubles, one per step.
char g_clock_path[MAX_PATH], g_clock_out[MAX_PATH];

// F4E: fill the eleven arrays above from mh.dll's single composition. Called as the FIRST statement
// of MH_Harness_Init -- after MH_Core_Arm_Early's build_paths(), which DllMain guarantees by calling
// the two back to back. It replaces a local build_paths() that composed the same strings from the
// process image and MH_RunDir(); the strings are byte-for-byte what they were, and the reason they
// are copied instead of recomposed is that a run folder is a decision and two derivations of a
// decision are two decisions (mh/include/mh_core_arm_paths.h).
//
// SIZE-CHECKED like every other cross-image struct here: mh.dll and mh_harness.dll ship as separate
// files and a player can copy one of them. Refusing leaves the arrays empty, which makes
// harness_enabled() read an empty ini path and answer 0 -- an instrument that cannot find its own
// configuration declines to arm, which is the right direction to fail in.
void copy_arm_paths() {
    const MH_CoreArmPaths *p = MH_Core_ArmPaths();
    if (p == nullptr || p->size != (unsigned)sizeof(MH_CoreArmPaths)) return;
    lstrcpynA(g_dir, p->exe_dir, MAX_PATH);
    lstrcpynA(g_ini_path, p->ini_path, MAX_PATH);
    lstrcpynA(g_log_path, p->log_path, MAX_PATH);
    lstrcpynA(g_seed_path, p->seed_in, MAX_PATH);
    lstrcpynA(g_seed_out, p->seed_out, MAX_PATH);
    lstrcpynA(g_boot_snap_out, p->boot_snap_out, MAX_PATH);
    lstrcpynA(g_world_out, p->world_out, MAX_PATH);
    lstrcpynA(g_orders_path, p->orders_in, MAX_PATH);
    lstrcpynA(g_orders_out, p->orders_out, MAX_PATH);
    lstrcpynA(g_clock_path, p->clock_in, MAX_PATH);
    lstrcpynA(g_clock_out, p->clock_out, MAX_PATH);
}

HANDLE   g_clock_h   = INVALID_HANDLE_VALUE; // record-mode clock append handle
double  *g_clock_trk = nullptr;              // replay clock track (VirtualAlloc)
uint32_t g_clock_n   = 0;

uint32_t g_step      = 0;    // sim step counter (increments before hashing; first hashed step = 1)
bool     g_active    = true; // logging enabled (HSTOP/HGO toggle via console)
bool     g_seed_done = false;

// TACT-PREP: the tactical cadence's own counter. DELIBERATELY SEPARATE from g_step rather than
// sharing it -- the two count different things (sim steps vs tactical frames), they never advance in
// the same frame, and merging them would put a `T` line and an `R` line at the same ordinal while
// meaning different clocks. A tactical excursion inside a longer session therefore has its own
// monotone frame axis, which is what the two-run compare lines up on.
uint32_t g_tact_step      = 0;
bool     g_tact_poke_done = false;
// Set only on the post-mission exit path (on_sim_step), where the tactical roster has already been
// torn down -- it suppresses the final combat census, which would otherwise report the teardown as
// casualties. See tj_report().
bool g_tact_post_mission = false;

// ---- FNV-1a-64 ----------------------------------------------------------------------------------
inline uint64_t fnv1a(const void *p, size_t n, uint64_t h = 1469598103934665603ULL) {
    const uint8_t *b = static_cast<const uint8_t *>(p);
    for (size_t i = 0; i < n; ++i) {
        h ^= b[i];
        h *= 1099511628211ULL;
    }
    return h;
}

// ---- tile_objects: hash only the REPLICATED bytes ----------------------------------------------
// map::tile_object_data is 8 bytes: flags(2) building(2) unit(2) class_owner(1) visibility(1).
// Two of those are FOG, not simulation: llm_map_fog_of_war_recompute (0x00428b11) rewrites
// `visibility` and the top bits of `flags` (it ANDs with 0x3fff then ORs 0x4000) from
// fog_of_war.visible_by_count -- and it runs off the FRAME, not the sim step. So those bytes differ
// between peers purely by frame timing.
//
// MEASURED 2026-07-27: hashing the region flat made all three pairs of a 3-peer run diverge at step 1
// with tile_objects as the ONLY diverging region, while units and soldiers stayed clean. That is a
// timing artifact, not a desync -- so mask the fog rather than either believing it or dropping the
// whole region (the replicated half carries building/unit occupancy, which is exactly what a
// moving-unit workload needs watched).

// ---- units: mask ctrl_group_id, the one LOCAL-PLAYER byte in a replicated record (D2) ----------
// map::object::unit is 0xe9 bytes; +0x2f `ctrl_group_id` is the Ctrl+digit control-group index. It
// diverges across peers BY CONSTRUCTION, and the reason is worth stating exactly, because "only the
// owner writes it" undersells it. Both writers key on PlayerSide (0xe58354), the LOCAL player:
//   llm_strat_unit_ctrlgroup_add_member 0x44945e   units[PlayerSide][unit].ctrl_group_id = group
//   llm_strat_unit_ctrlgroup_leave      0x449372   units[PlayerSide][unit].ctrl_group_id = 0
// leave() is on the deterministic teardown path and is called for ANY unit's death, but indexes the
// LOCAL player's row at the dead unit's index -- so peers legitimately hold different values here.
//
// WHY MASK RATHER THAN REPLICATE. The groups array itself (_G_LLM_STRAT_CTRL_GROUPS 0xb63be0) is
// llm_strat_ctrl_group[10] -- ten groups, NOT per player -- so control groups are structurally
// local-player state and replicating assignment would need a state-layout change (Law 1 forbids that
// while original accessors remain). And the game ALREADY replicates the part it wanted replicated:
// assignment dispatches order code 0x34 (llm_unit_status_bit_set) through llm_strat_order_dispatch.
// Nothing in the sim branches on ctrl_group_id -- its only two consumers,
// llm_strat_unit_count_alive_excluding_ctrl_group (0x44a685) and ..._find_nth_... (0x44adef), are
// dead code. So this is a hash artifact, not a desync: mask the byte, keep the other 232.
// DERIVED FROM THE GENERATED MIRRORS, not hand-typed (2026-07-31). These were `0xe9` / `0x111` /
// `0x2f` written out by hand while addr/mh_structs.gen.h already carried both structs with a
// static_assert on every size and offset -- two derivations of one layout, one machine-checked and
// one not, which is the shape ST2M had just finished removing from the region manifest. It mattered
// here more than usual: the mask below is what keeps ctrl_group_id out of the determinism VERDICT,
// so a Ghidra retype that shifted the field would have silently masked the wrong byte and left the
// gate green. Now it is a compile error. (map::object::building was added to the manifest's `structs`
// list in the same change -- it had no mirror at all, so BLDG_STRIDE was backed by nothing.)
constexpr uint32_t UNIT_STRIDE   = sizeof(mh::game::mh_map_object_unit);
constexpr uint32_t BLDG_STRIDE   = sizeof(mh::game::mh_map_object_building);
constexpr uint32_t UNIT_OFF_CTRL = offsetof(mh::game::mh_map_object_unit, ctrl_group_id);

// ---- rng_state: hash slots 0/2/3, mask slot 1 (fx) ---------------------------------------------
// Rationale + the measured slot map are in the IDX_RNG block above. Same shape as the fog mask: the
// per-FRAME channel is dropped, the per-SIM-STEP ones are hashed.

// ---- tiny WinAPI-only file append (no CRT stdio dependency in the injected context) -------------
// ---- the log writer, and why it is buffered -----------------------------------------------------
//
// It used to open, seek, write and close on EVERY line. Measured cost (tools/tact_profile.py, 3,000
// frames, clock pinned): the tactical sim runs at ~1058 frames/s with the frame hook installed and
// nothing logged, and at ~193 frames/s logging `T`/`TS`/`TR` each frame. That is +3,966 us per
// frame, 81% of the wall clock of every tactical run this rig makes, spent on file handles rather
// than on the game. The hashing is not the cost: 15 regions over ~24 units of 0x5f4 bytes is ~36 KB
// per frame, tens of microseconds.
//
// ONE HANDLE, HELD OPEN, WITH A BUFFER. The constraints are not obvious, so they are stated:
//
//   1. A RUN KILLED BY THE WALL CLOCK MUST STILL LEAVE A READABLE LOG. The arms are routinely
//      killed -- a tactical mission has no end condition -- and TerminateProcess runs no atexit
//      handler and flushes no user-space buffer. So the buffer is bounded and flushed on a timer as
//      well as on size.
//
//      THE TIMER IS NOT A WALL-CLOCK BOUND ON ITS OWN, and the claim that used to stand here ("at
//      most FLUSH_MS of output can be lost, never the run") was FALSE for one measured shape. The
//      timer is only ever consulted by the NEXT append_line, so a run that stops appending keeps
//      whatever is in the buffer for as long as it lives: an ARMED run killed at the MENU writes its
//      whole arm report, never takes a sim step, and leaves mh_harness.log EMPTY -- exactly the run
//      whose log is the only evidence of what armed (fork F4F found it; F4G fixes it). Two things
//      close it, and both are needed because they cover different halves:
//        * the arm reports flush EXPLICITLY at their end (MH_Harness_Init, MH_Harness_LateArm), so
//          everything the instrument decided is on disk before the first frame runs;
//        * log_flush_due() runs off the PRESENT hook, which makes the timer a real bound for the
//          runs that install one. It is not installed in every configuration
//          (MH_Harness_WantsPresentTick), which is why it is the second half and not the first.
//   2. THE FILE MUST STAY READABLE WHILE THE RUN IS LIVE. tools/ watch logs mid-run, so the handle
//      is opened FILE_SHARE_READ and the flush is a real WriteFile, not a cached mapping.
//   3. LINES MUST NOT INTERLEAVE. Several seams log from the game thread; the naked detours can
//      reach here too. A single global buffer with an appending memcpy keeps whole lines contiguous.
//   4. PATH CHANGES REBIND. build_paths() picks the run folder after the first lines are written,
//      and some callers log to other files -- so a write to a DIFFERENT path flushes and reopens
//      rather than silently landing in the wrong file.
constexpr DWORD LOG_BUF      = 1u << 16; // 64 KB: ~250 frames of T/TS/TR
constexpr DWORD LOG_FLUSH_MS = 1000;     // bound the loss from a killed run to one second of output

HANDLE g_log_h = INVALID_HANDLE_VALUE;
char   g_log_path_open[MAX_PATH];
char  *g_log_buf        = nullptr;
DWORD  g_log_used       = 0;
DWORD  g_log_last_flush = 0;

void log_flush() {
    if (g_log_h != INVALID_HANDLE_VALUE && g_log_used) {
        DWORD wrote = 0;
        WriteFile(g_log_h, g_log_buf, g_log_used, &wrote, nullptr);
    }
    g_log_used       = 0;
    g_log_last_flush = GetTickCount();
}

// Constraint 1's other half (fork F4G): flush IF the timer says so, called from somewhere that runs
// whether or not anything is being logged. append_line's own timer check can only fire on the next
// append, so a run that goes quiet holds its buffer until it dies; this is the same test asked by a
// caller that is not an append. Cheap and honest when idle: `g_log_used` is zero for every
// un-instrumented run, so the GetTickCount is not even reached.
void log_flush_due() {
    if (g_log_used && (DWORD)(GetTickCount() - g_log_last_flush) >= LOG_FLUSH_MS) log_flush();
}

void append_line(const char *path, const char *s) {
    if (!g_log_buf) {
        g_log_buf = (char *)VirtualAlloc(nullptr, LOG_BUF, MEM_COMMIT, PAGE_READWRITE);
        if (!g_log_buf) { // no buffer: fall back to the old unbuffered path rather than lose output
            HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE) return;
            SetFilePointer(h, 0, nullptr, FILE_END);
            DWORD wrote = 0;
            WriteFile(h, s, (DWORD)lstrlenA(s), &wrote, nullptr);
            CloseHandle(h);
            return;
        }
    }
    if (g_log_h == INVALID_HANDLE_VALUE || lstrcmpiA(path, g_log_path_open) != 0) {
        log_flush(); // constraint 4: never carry one file's buffered bytes into another
        if (g_log_h != INVALID_HANDLE_VALUE) CloseHandle(g_log_h);
        g_log_h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
        if (g_log_h == INVALID_HANDLE_VALUE) return;
        SetFilePointer(g_log_h, 0, nullptr, FILE_END);
        lstrcpynA(g_log_path_open, path, MAX_PATH);
    }
    const DWORD n = (DWORD)lstrlenA(s);
    if (n >= LOG_BUF) { // a line larger than the buffer: write it straight through, in order
        log_flush();
        DWORD wrote = 0;
        WriteFile(g_log_h, s, n, &wrote, nullptr);
        return;
    }
    if (g_log_used + n > LOG_BUF) log_flush();
    for (DWORD i = 0; i < n; ++i) g_log_buf[g_log_used + i] = s[i];
    g_log_used += n;
    // Constraint 1: bound how much a killed run can lose. GetTickCount wraps every 49 days; the
    // subtraction is unsigned so the wrap is harmless.
    if ((DWORD)(GetTickCount() - g_log_last_flush) >= LOG_FLUSH_MS) log_flush();
}

// ---- THE ARMING SIGNAL (fork F2G, ruling Q6) -----------------------------------------------------
//
// `[harness] enable=1` in mh_net.ini, and nothing else. Default OFF when the key or the whole
// section is absent, so a plain install and every ordinary UI run are unharnessed exactly as before.
// The full history of why it is an explicit key rather than the existence of a file is in
// mh/seams/core_arm.cpp, beside the copy MH_Core_Arm_Early reads.
//
// IT IS ASKED IN BOTH IMAGES SINCE FORK F4E, and that is deliberate rather than a duplication to
// clean up. mh.dll's core arm has to know whether an instrument will run (the relocating state bind
// and the rebind yield both turn on it) and this DLL has to know whether to arm; the answer is one
// GetPrivateProfileInt against a file the OS caches, it is derived from the same ini path mh.dll
// composed and handed us, and it cannot disagree between the two. The alternative -- a host row
// carrying a bool -- would make the harness's own arm gate depend on a bind having succeeded.
//
// READ FRESH FROM THE INI, NOT FROM g_cfg: this is asked before load_config() has run, so g_cfg.*
// is still zero at those call points.
bool harness_enabled() { return GetPrivateProfileIntA("harness", "enable", 0, g_ini_path) != 0; }

// The no-op the skip_input_update arm installs in place of the frame's input WALL.
void harness_input_update_noop() {}

void load_config() {
    g_cfg.seed_step = GetPrivateProfileIntA("harness", "seed_step", g_cfg.seed_step, g_ini_path);

    g_cfg.seed_mode           = GetPrivateProfileIntA("harness", "seed_mode", g_cfg.seed_mode, g_ini_path);
    g_cfg.boot_snapshot       = GetPrivateProfileIntA("harness", "boot_snapshot", g_cfg.boot_snapshot, g_ini_path);
    g_cfg.world_capture       = GetPrivateProfileIntA("harness", "world_capture", g_cfg.world_capture, g_ini_path);
    g_cfg.stop_step           = GetPrivateProfileIntA("harness", "stop_step", g_cfg.stop_step, g_ini_path);
    g_cfg.exit_on_stop        = GetPrivateProfileIntA("harness", "exit_on_stop", g_cfg.exit_on_stop, g_ini_path);
    g_cfg.fixed_step          = GetPrivateProfileIntA("harness", "fixed_step", g_cfg.fixed_step, g_ini_path);
    g_cfg.pin_fpu             = GetPrivateProfileIntA("harness", "pin_fpu", g_cfg.pin_fpu, g_ini_path);
    g_cfg.pin_wallclock       = GetPrivateProfileIntA("harness", "pin_wallclock", g_cfg.pin_wallclock, g_ini_path);
    g_cfg.pin_menu_clock      = GetPrivateProfileIntA("harness", "pin_menu_clock", g_cfg.pin_menu_clock, g_ini_path);
    g_cfg.tj_trace            = GetPrivateProfileIntA("harness", "tj_trace", g_cfg.tj_trace, g_ini_path);
    g_cfg.tj_trace_from       = GetPrivateProfileIntA("harness", "tj_trace_from", g_cfg.tj_trace_from, g_ini_path);
    g_cfg.tj_ps_log           = GetPrivateProfileIntA("harness", "tj_ps_log", g_cfg.tj_ps_log, g_ini_path);
    g_cfg.pin_clock_dt_us     = GetPrivateProfileIntA("harness", "pin_clock_dt_us", g_cfg.pin_clock_dt_us, g_ini_path);
    g_cfg.pin_clock_base_s    = GetPrivateProfileIntA("harness", "pin_clock_base_s", g_cfg.pin_clock_base_s, g_ini_path);
    g_cfg.tact_hash_step      = GetPrivateProfileIntA("harness", "tact_hash_step", g_cfg.tact_hash_step, g_ini_path);
    g_cfg.tact_stop_step      = GetPrivateProfileIntA("harness", "tact_stop_step", g_cfg.tact_stop_step, g_ini_path);
    g_cfg.tact_exit_at        = GetPrivateProfileIntA("harness", "tact_exit_at", g_cfg.tact_exit_at, g_ini_path);
    g_cfg.tact_unit_hash      = GetPrivateProfileIntA("harness", "tact_unit_hash", g_cfg.tact_unit_hash, g_ini_path);
    g_cfg.tact_detail_lo      = GetPrivateProfileIntA("harness", "tact_detail_lo", g_cfg.tact_detail_lo, g_ini_path);
    g_cfg.tact_detail_hi      = GetPrivateProfileIntA("harness", "tact_detail_hi", g_cfg.tact_detail_hi, g_ini_path);
    g_cfg.tact_gate_lo        = GetPrivateProfileIntA("harness", "tact_gate_lo", g_cfg.tact_gate_lo, g_ini_path);
    g_cfg.tact_gate_hi        = GetPrivateProfileIntA("harness", "tact_gate_hi", g_cfg.tact_gate_hi, g_ini_path);
    g_cfg.tact_field_lo       = GetPrivateProfileIntA("harness", "tact_field_lo", g_cfg.tact_field_lo, g_ini_path);
    g_cfg.tact_field_hi       = GetPrivateProfileIntA("harness", "tact_field_hi", g_cfg.tact_field_hi, g_ini_path);
    g_cfg.tact_poke_off       = GetPrivateProfileIntA("harness", "tact_poke_off", g_cfg.tact_poke_off, g_ini_path);
    g_cfg.profile_hz          = GetPrivateProfileIntA("harness", "profile_hz", g_cfg.profile_hz, g_ini_path);
    g_cfg.tact_journal_rec    = GetPrivateProfileIntA("harness", "tact_journal_rec", g_cfg.tact_journal_rec, g_ini_path);
    g_cfg.tact_journal_verify = GetPrivateProfileIntA("harness", "tact_journal_verify", g_cfg.tact_journal_verify, g_ini_path);
    g_cfg.tact_journal_probe  = GetPrivateProfileIntA("harness", "tact_journal_probe", g_cfg.tact_journal_probe, g_ini_path);
    g_cfg.tact_input_selftest = GetPrivateProfileIntA("harness", "tact_input_selftest", g_cfg.tact_input_selftest, g_ini_path);
    // A PATH, so it reads as a string -- the same shape as loadgame_name above, which is the only
    // other string knob in this file.
    mh::config::read_ini_string("harness", "tact_journal", g_cfg.tact_journal, g_cfg.tact_journal, // TL-HARN4
                                sizeof(g_cfg.tact_journal), g_ini_path);
    g_cfg.ui_journal_rec = GetPrivateProfileIntA("harness", "ui_journal_rec", g_cfg.ui_journal_rec, g_ini_path);
    g_cfg.skip_intro_avi = GetPrivateProfileIntA("harness", "skip_intro_avi", g_cfg.skip_intro_avi, g_ini_path);
    mh::config::read_ini_string("harness", "ui_journal", g_cfg.ui_journal, g_cfg.ui_journal, // TL-HARN4
                                sizeof(g_cfg.ui_journal), g_ini_path);
    g_cfg.tact_poke_at      = GetPrivateProfileIntA("harness", "tact_poke_at", g_cfg.tact_poke_at, g_ini_path);
    g_cfg.tact_poke_idx     = GetPrivateProfileIntA("harness", "tact_poke_idx", g_cfg.tact_poke_idx, g_ini_path);
    g_cfg.tact_synth        = GetPrivateProfileIntA("harness", "tact_synth", g_cfg.tact_synth, g_ini_path);
    g_cfg.tact_synth_seed   = GetPrivateProfileIntA("harness", "tact_synth_seed", g_cfg.tact_synth_seed, g_ini_path);
    g_cfg.tact_synth_at     = GetPrivateProfileIntA("harness", "tact_synth_at", g_cfg.tact_synth_at, g_ini_path);
    g_cfg.tact_synth_every  = GetPrivateProfileIntA("harness", "tact_synth_every", g_cfg.tact_synth_every, g_ini_path);
    g_cfg.tact_synth_stop   = GetPrivateProfileIntA("harness", "tact_synth_stop", g_cfg.tact_synth_stop, g_ini_path);
    g_cfg.tact_synth_units  = GetPrivateProfileIntA("harness", "tact_synth_units", g_cfg.tact_synth_units, g_ini_path);
    g_cfg.tact_synth_owner  = GetPrivateProfileIntA("harness", "tact_synth_owner", g_cfg.tact_synth_owner, g_ini_path);
    g_cfg.tact_synth_direct = GetPrivateProfileIntA("harness", "tact_synth_direct", g_cfg.tact_synth_direct, g_ini_path);
    g_cfg.pin_rand          = GetPrivateProfileIntA("harness", "pin_rand", g_cfg.pin_rand, g_ini_path);
    g_cfg.rand_seed         = GetPrivateProfileIntA("harness", "rand_seed", g_cfg.rand_seed, g_ini_path);
    g_cfg.pin_strat_seed    = GetPrivateProfileIntA("harness", "pin_strat_seed", g_cfg.pin_strat_seed, g_ini_path);
    g_cfg.strat_seed        = GetPrivateProfileIntA("harness", "strat_seed", g_cfg.strat_seed, g_ini_path);
    g_cfg.land_log          = GetPrivateProfileIntA("harness", "land_log", g_cfg.land_log, g_ini_path);
    g_cfg.synth_move        = GetPrivateProfileIntA("harness", "synth_move", g_cfg.synth_move, g_ini_path);
    g_cfg.synth_seed        = GetPrivateProfileIntA("harness", "synth_seed", g_cfg.synth_seed, g_ini_path);
    g_cfg.synth_at          = GetPrivateProfileIntA("harness", "synth_at", g_cfg.synth_at, g_ini_path);
    g_cfg.synth_every       = GetPrivateProfileIntA("harness", "synth_every", g_cfg.synth_every, g_ini_path);
    g_cfg.save_at           = GetPrivateProfileIntA("harness", "save_at", g_cfg.save_at, g_ini_path);
    g_cfg.save_every        = GetPrivateProfileIntA("harness", "save_every", g_cfg.save_every, g_ini_path);
    g_cfg.save_count        = GetPrivateProfileIntA("harness", "save_count", g_cfg.save_count, g_ini_path);
    g_cfg.save_keep         = GetPrivateProfileIntA("harness", "save_keep", g_cfg.save_keep, g_ini_path);
    g_cfg.savegame_at       = GetPrivateProfileIntA("harness", "savegame_at", g_cfg.savegame_at, g_ini_path);
    g_cfg.loadgame_at       = GetPrivateProfileIntA("harness", "loadgame_at", g_cfg.loadgame_at, g_ini_path);
    mh::config::read_ini_string("harness", "loadgame_name", g_cfg.loadgame_name, // TL-HARN4
                                g_cfg.loadgame_name, sizeof(g_cfg.loadgame_name), g_ini_path);
    g_cfg.aistate_probe_at =
        GetPrivateProfileIntA("harness", "aistate_probe_at", g_cfg.aistate_probe_at, g_ini_path);
    g_cfg.load_at                 = GetPrivateProfileIntA("harness", "load_at", g_cfg.load_at, g_ini_path);
    g_cfg.load_every              = GetPrivateProfileIntA("harness", "load_every", g_cfg.load_every, g_ini_path);
    g_cfg.load_count              = GetPrivateProfileIntA("harness", "load_count", g_cfg.load_count, g_ini_path);
    g_cfg.snapshot_at             = GetPrivateProfileIntA("harness", "snapshot_at", g_cfg.snapshot_at, g_ini_path);
    g_cfg.snapshot_to             = GetPrivateProfileIntA("harness", "snapshot_to", g_cfg.snapshot_to, g_ini_path);
    g_cfg.snapshot_import         = GetPrivateProfileIntA("harness", "snapshot_import", g_cfg.snapshot_import, g_ini_path);
    g_cfg.snapshot_log            = GetPrivateProfileIntA("harness", "snapshot_log", g_cfg.snapshot_log, g_ini_path);
    g_cfg.snapshot_hold           = GetPrivateProfileIntA("harness", "snapshot_hold", g_cfg.snapshot_hold, g_ini_path);
    g_cfg.region_hash_step        = GetPrivateProfileIntA("harness", "region_hash_step", g_cfg.region_hash_step, g_ini_path);
    g_cfg.domain_hash_step        = GetPrivateProfileIntA("harness", "domain_hash_step", g_cfg.domain_hash_step, g_ini_path);
    g_cfg.order_mode              = GetPrivateProfileIntA("harness", "order_mode", g_cfg.order_mode, g_ini_path);
    g_cfg.replay_ai_off           = GetPrivateProfileIntA("harness", "replay_ai_off", g_cfg.replay_ai_off, g_ini_path);
    g_cfg.replay_suppress_enqueue = GetPrivateProfileIntA("harness", "replay_suppress_enqueue", g_cfg.replay_suppress_enqueue, g_ini_path);
    g_cfg.clock_record            = GetPrivateProfileIntA("harness", "clock_record", g_cfg.clock_record, g_ini_path);
    g_cfg.replay_isolate_input    = GetPrivateProfileIntA("harness", "replay_isolate_input", g_cfg.replay_isolate_input, g_ini_path);
    g_cfg.order_log               = GetPrivateProfileIntA("harness", "order_log", g_cfg.order_log, g_ini_path);
    g_cfg.rng_perturb_slot        = GetPrivateProfileIntA("harness", "rng_perturb_slot", g_cfg.rng_perturb_slot, g_ini_path);
    g_cfg.rng_perturb_step        = GetPrivateProfileIntA("harness", "rng_perturb_step", g_cfg.rng_perturb_step, g_ini_path);
    g_cfg.ai_probe_step           = GetPrivateProfileIntA("harness", "ai_probe_step", g_cfg.ai_probe_step, g_ini_path);
    g_cfg.region_poke_at          = GetPrivateProfileIntA("harness", "region_poke_at", g_cfg.region_poke_at, g_ini_path);
    g_cfg.region_poke_min         = GetPrivateProfileIntA("harness", "region_poke_min", g_cfg.region_poke_min, g_ini_path);
    g_cfg.region_poke_only        = GetPrivateProfileIntA("harness", "region_poke_only", g_cfg.region_poke_only, g_ini_path);
    g_cfg.region_poke_off         = GetPrivateProfileIntA("harness", "region_poke_off", g_cfg.region_poke_off, g_ini_path);
    g_cfg.rdump_rid               = GetPrivateProfileIntA("harness", "rdump_rid", g_cfg.rdump_rid, g_ini_path);
    g_cfg.rdump_lo                = GetPrivateProfileIntA("harness", "rdump_lo", g_cfg.rdump_lo, g_ini_path);
    g_cfg.rdump_hi                = GetPrivateProfileIntA("harness", "rdump_hi", g_cfg.rdump_hi, g_ini_path);
    // GetPrivateProfileIntA parses decimal only, so an address is given as decimal OR as 0x... via the
    // string reader. Accept both: a hex address is how anybody actually writes one, and silently
    // reading 0 from "0x0066a334" would look exactly like "the knob is off".
    {
        char av[32] = {0};
        mh::config::read_ini_string("harness", "rdump_addr", "", av, sizeof(av), g_ini_path); // TL-HARN4
        if (av[0] != '\0') {
            const bool hex = (av[0] == '0' && (av[1] == 'x' || av[1] == 'X'));
            g_cfg.rdump_addr =
                (int)strtoul(hex ? av + 2 : av, nullptr, hex ? 16 : 10); // 0 stays "off"
        }
    }
    g_cfg.rdump_len = GetPrivateProfileIntA("harness", "rdump_len", g_cfg.rdump_len, g_ini_path);
    // skip_input_update: replace the FRAME'S INPUT WALL with a no-op, making the hosted arm
    // structurally equal to the standalone one on the only order-path difference left between them
    // (hosted runs the ORIGINAL llm_strat_input_update @0x00441b88; libref_host binds an empty
    // function). It is an EXPERIMENT ARM, off by default, and it is LOUD when on -- a run that
    // silently dropped the input pump would be a different game, not a controlled comparison.
    // skip_pace_hook: the C4 pacing prelude (adaptive_tick) does not run. Standalone it never runs
    // -- libref_host installs no pacing instrument, so g_pace_hook is null there for the life of the
    // process -- which makes this the second of the two hosted-only code paths on the sim's frame.
    // READ HERE AND NOT IN net_lockstep.cpp, WHICH IS WHERE THE HOOK IS INSTALLED AND WHERE THIS
    // GATE FIRST WENT. The reason has CHANGED and the placement has not, so both halves are worth
    // stating. It was: that TU read `g_ini` (mh_net.ini) while --harness-extra wrote its keys into
    // mh_harness.ini, so the key read 0, the hook installed normally, and the run LOOKED like a
    // clean experiment -- caught only by the arm's own liveness line being absent from the log,
    // because an arm never seen to fire is not an arm. F2G merged the two files, so that particular
    // hole is closed by construction. It stays here anyway because THIS file is where the arm gate
    // lives: a `[harness]` key read from an un-armed run's ini would be a knob acting with no
    // instrument watching it, which is the same failure wearing a different coat.
    if (GetPrivateProfileIntA("harness", "skip_pace_hook", 0, g_ini_path)) {
        mh::sim::set_time_resync_pace_disabled(true);
        append_line(g_log_path,
                    "; [harness] skip_pace_hook=1 -- the C4 pacing prelude is DISABLED this run "
                    "(adaptive_tick does not run). EXPERIMENT ARM, not a shipping configuration.\n");
    }
    // C-prime: arm the RNG draw-sequence trace over the SAME window the rdump uses, so a draw and
    // a byte in this log describe the same steps. Disarmed unless rng_trace=1 is asked for.
    if (GetPrivateProfileIntA("harness", "rng_trace", 0, g_ini_path))
        mh::sim::rng_trace_window((uint32_t)g_cfg.rdump_lo, (uint32_t)g_cfg.rdump_hi);
    if (GetPrivateProfileIntA("harness", "skip_input_update", 0, g_ini_path)) {
        mh::sim::set_lt_frame_input_override(&harness_input_update_noop);
        append_line(g_log_path,
                    "; [harness] skip_input_update=1 -- the frame's input WALL is a NO-OP this run "
                    "(llm_strat_input_update is NOT called). EXPERIMENT ARM, not a shipping "
                    "configuration.\n");
    }
    g_cfg.rdump_regid        = GetPrivateProfileIntA("harness", "rdump_regid", g_cfg.rdump_regid, g_ini_path);
    g_cfg.garble_at          = GetPrivateProfileIntA("harness", "garble_at", g_cfg.garble_at, g_ini_path);
    g_cfg.mask_ctrl_group    = GetPrivateProfileIntA("harness", "mask_ctrl_group", g_cfg.mask_ctrl_group, g_ini_path);
    g_cfg.mask_soldier_anim  = GetPrivateProfileIntA("harness", "mask_soldier_anim", g_cfg.mask_soldier_anim, g_ini_path);
    g_cfg.mask_planets_gfx   = GetPrivateProfileIntA("harness", "mask_planets_gfx", g_cfg.mask_planets_gfx, g_ini_path);
    g_cfg.synth_ctrlgroup    = GetPrivateProfileIntA("harness", "synth_ctrlgroup", g_cfg.synth_ctrlgroup, g_ini_path);
    g_cfg.synth_ctrlgroup_id = GetPrivateProfileIntA("harness", "synth_ctrlgroup_id", g_cfg.synth_ctrlgroup_id, g_ini_path);
    g_cfg.synth_groupmove    = GetPrivateProfileIntA("harness", "synth_groupmove", g_cfg.synth_groupmove, g_ini_path);
    g_cfg.synth_groupmove_x  = GetPrivateProfileIntA("harness", "synth_groupmove_x", g_cfg.synth_groupmove_x, g_ini_path);
    g_cfg.synth_groupmove_y  = GetPrivateProfileIntA("harness", "synth_groupmove_y", g_cfg.synth_groupmove_y, g_ini_path);
    g_cfg.synth_groupmove_n  = GetPrivateProfileIntA("harness", "synth_groupmove_n", g_cfg.synth_groupmove_n, g_ini_path);
    g_cfg.conq               = GetPrivateProfileIntA("harness", "conq", g_cfg.conq, g_ini_path);
    g_cfg.conq_seed          = GetPrivateProfileIntA("harness", "conq_seed", g_cfg.conq_seed, g_ini_path);
    g_cfg.conq_at            = GetPrivateProfileIntA("harness", "conq_at", g_cfg.conq_at, g_ini_path);
    g_cfg.conq_peer_land_at  = GetPrivateProfileIntA("harness", "conq_peer_land_at", g_cfg.conq_peer_land_at, g_ini_path);
    g_cfg.conq_peer_move_at  = GetPrivateProfileIntA("harness", "conq_peer_move_at", g_cfg.conq_peer_move_at, g_ini_path);
    g_cfg.conq_land_self_at  = GetPrivateProfileIntA("harness", "conq_land_self_at", g_cfg.conq_land_self_at, g_ini_path);
    g_cfg.conq_bldg1_at      = GetPrivateProfileIntA("harness", "conq_bldg1_at", g_cfg.conq_bldg1_at, g_ini_path);
    g_cfg.conq_bldg2_at      = GetPrivateProfileIntA("harness", "conq_bldg2_at", g_cfg.conq_bldg2_at, g_ini_path);
    g_cfg.conq_units_at      = GetPrivateProfileIntA("harness", "conq_units_at", g_cfg.conq_units_at, g_ini_path);
    g_cfg.conq_group_at      = GetPrivateProfileIntA("harness", "conq_group_at", g_cfg.conq_group_at, g_ini_path);
    g_cfg.conq_attack_at     = GetPrivateProfileIntA("harness", "conq_attack_at", g_cfg.conq_attack_at, g_ini_path);
    g_cfg.conq_attack_every  = GetPrivateProfileIntA("harness", "conq_attack_every", g_cfg.conq_attack_every, g_ini_path);
    g_cfg.conq_academy       = GetPrivateProfileIntA("harness", "conq_academy", g_cfg.conq_academy, g_ini_path);
    g_cfg.conq_barracks      = GetPrivateProfileIntA("harness", "conq_barracks", g_cfg.conq_barracks, g_ini_path);
    g_cfg.conq_soldier       = GetPrivateProfileIntA("harness", "conq_soldier", g_cfg.conq_soldier, g_ini_path);
    g_cfg.conq_n_soldiers    = GetPrivateProfileIntA("harness", "conq_n_soldiers", g_cfg.conq_n_soldiers, g_ini_path);
    g_cfg.conq_use_groupmove = GetPrivateProfileIntA("harness", "conq_use_groupmove", g_cfg.conq_use_groupmove, g_ini_path);
    g_cfg.conq_force_kill_at = GetPrivateProfileIntA("harness", "conq_force_kill_at", g_cfg.conq_force_kill_at, g_ini_path);
    g_cfg.conq_probe_every   = GetPrivateProfileIntA("harness", "conq_probe_every", g_cfg.conq_probe_every, g_ini_path);
    g_cfg.conq_weapon        = GetPrivateProfileIntA("harness", "conq_weapon", g_cfg.conq_weapon, g_ini_path);
    g_cfg.conq_phase_timeout = GetPrivateProfileIntA("harness", "conq_phase_timeout", g_cfg.conq_phase_timeout, g_ini_path);
    g_cfg.all_ai             = GetPrivateProfileIntA("harness", "all_ai", g_cfg.all_ai, g_ini_path);
    g_cfg.all_ai_observer    = GetPrivateProfileIntA("harness", "all_ai_observer", g_cfg.all_ai_observer, g_ini_path);
    g_cfg.gameover_step      = GetPrivateProfileIntA("harness", "gameover_step", g_cfg.gameover_step, g_ini_path);
    g_cfg.gameover_stop      = GetPrivateProfileIntA("harness", "gameover_stop", g_cfg.gameover_stop, g_ini_path);
}

// ---- seed blob (dump/inject) --------------------------------------------------------------------
// Format: raw concatenation of every region in manifest order. Fixed layout, so dump and inject
// agree by construction as long as the manifest is identical (it is -- same binary).
void seed_dump() {
    HANDLE h = CreateFileA(g_seed_out, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    // ST6 phase 1: through the sink, in PERSIST mode -- everything, nothing masked. Identical bytes
    // to the old flat WriteFile per region, because every slice is still raw and PERSIST is a flat
    // block for all of them; the seed blob's format is unchanged and old blobs stay injectable.
    mh::state::fn_sink sink(
        mh::state::sink_mode::PERSIST,
        [](void *ctx, const void *p, uint32_t n) {
            DWORD wrote = 0;
            WriteFile(*static_cast<HANDLE *>(ctx), p, n, &wrote, nullptr);
        },
        &h);
    for (int i = 0; i < N_REGIONS; ++i) mh::state::emit_slice(i, sink);
    CloseHandle(h);
}

void seed_inject() {
    HANDLE h = CreateFileA(g_seed_path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        append_line(g_log_path, "; seed_inject: seed file missing\n");
        return;
    }
    // ST6 phase 1: the inverse of seed_dump, through state_source. Same bytes, same order, so a blob
    // written by any earlier build still injects.
    mh::state::fn_source src(
        [](void *ctx, void *p, uint32_t n) {
            DWORD got = 0;
            ReadFile(*static_cast<HANDLE *>(ctx), p, n, &got, nullptr);
        },
        &h);
    for (int i = 0; i < N_REGIONS; ++i) mh::state::fill_slice(i, src);
    CloseHandle(h);
}

// ---- LIB-BOOT: the post-cfg prototype snapshot ---------------------------------------------------
//
// WHERE THIS FIRES, AND WHY IT IS THE RIGHT INSTANT. The boot-stage machine runs one stage per frame
// in _G_LLM_GAME_MODE == 1 and stage 9 sets _G_LLM_GAME_MODE = 3 (the game-mode notes). So the FIRST
// present with mode 3 is the first moment at which stage 5's INIT.CFG parse AND stages 6-8 (sprite
// banks, llm_strat_bldg_init_defaults -- which is OURS and writes into `Building` -- and the panel /
// minimap init) have all completed, and no session has begun: the main menu is up. That is exactly
// the "cfg load is done" point LIB-BOOT is defined against, and it needs no new detour -- the
// present tick already exists for the UI journal.
//
// THE ONE THING THIS DOES THAT A PURE READ WOULD NOT. `llm_tutorial_load_script` is a cfg-CLASS
// parse (info\TUTORIAL.TXT out of the .rsr into a table that is read-only afterwards) that the game
// only runs at TUTORIAL START. The snapshot has to carry its output -- it is what closes the second
// of LIB-BOOT's two va_census sites -- so the capture calls it once, here, and PUTS BACK every
// region it writes. The restore set is not hand-listed: BOOT_SNAPSHOT_RESTORE is generated from
// that function's own write closure, so a region it starts writing next year is restored too. The
// two regions it also touches and this does NOT restore are the LZW decompressor's per-call scratch
// (`DATA`, `LZW_UNCOMPRESSED_SIZE`), which the boot cfg load has already run hundreds of blocks
// through -- recorded as `restore: false` with that reason, not silently skipped.
void boot_snapshot_capture() {
    using namespace mh::state;

    // Save what the deferred parse is about to clobber.
    uint32_t save_len = 0;
    for (int i = 0; i < BOOT_SNAPSHOT_RESTORE_COUNT; ++i)
        save_len += BOOT_SNAPSHOT_RESTORE[i].len;
    uint8_t *saved = static_cast<uint8_t *>(
        HeapAlloc(GetProcessHeap(), 0, save_len ? save_len : 1));
    if (saved == nullptr) {
        append_line(g_log_path, "; BOOT SNAPSHOT: HeapAlloc failed for the restore buffer\n");
        return;
    }
    uint32_t off = 0;
    for (int i = 0; i < BOOT_SNAPSHOT_RESTORE_COUNT; ++i) {
        const boot_snapshot_block &b = BOOT_SNAPSHOT_RESTORE[i];
        memcpy(saved + off, reinterpret_cast<const void *>(static_cast<uintptr_t>(live_base(b.rid))), b.len);
        off += b.len;
    }

    mh::call::llm_tutorial_load_script();

    // DID THE CALL ACTUALLY DO ANYTHING? Counted per region, because a restore that puts back bytes
    // nothing changed proves nothing -- it is the vacuous-pass shape, and the oracle refuses a blob
    // whose `restore_changed` is 0 rather than letting it read as a clean check.
    uint32_t changed = 0;
    off              = 0;
    for (int i = 0; i < BOOT_SNAPSHOT_RESTORE_COUNT; ++i) {
        const boot_snapshot_block &b = BOOT_SNAPSHOT_RESTORE[i];
        if (memcmp(reinterpret_cast<const void *>(static_cast<uintptr_t>(live_base(b.rid))),
                   saved + off, b.len) != 0)
            ++changed;
        off += b.len;
    }

    const size_t need = boot::blob_size();
    uint8_t     *blob = static_cast<uint8_t *>(HeapAlloc(GetProcessHeap(), 0, need));
    int          rc   = -100;
    size_t       got  = 0;
    if (blob) rc = boot::capture(blob, need, &got);

    // PUT IT BACK BEFORE ANYTHING ELSE CAN OBSERVE IT -- including a failed capture. The game keeps
    // running after this function returns whatever happened, so the restore is not conditional on
    // success.
    off = 0;
    for (int i = 0; i < BOOT_SNAPSHOT_RESTORE_COUNT; ++i) {
        const boot_snapshot_block &b = BOOT_SNAPSHOT_RESTORE[i];
        memcpy(reinterpret_cast<void *>(static_cast<uintptr_t>(live_base(b.rid))), saved + off,
               b.len);
        off += b.len;
    }

    // ...AND VERIFY IT LANDED. This is the item's proof that the capture leaves the game unperturbed,
    // and it replaces a frame diff that could not carry that weight: the tutorial's running frame is
    // not reproducible under an armed harness even with the capture DISARMED (15.2% vs 15.5% against
    // the same baseline, measured 2026-09-11), so a pixel comparison cannot separate "the capture
    // perturbed the game" from "this frame is unstable here". Bytes can.
    uint32_t mismatch = 0;
    off               = 0;
    for (int i = 0; i < BOOT_SNAPSHOT_RESTORE_COUNT; ++i) {
        const boot_snapshot_block &b = BOOT_SNAPSHOT_RESTORE[i];
        if (memcmp(reinterpret_cast<const void *>(static_cast<uintptr_t>(live_base(b.rid))),
                   saved + off, b.len) != 0)
            ++mismatch;
        off += b.len;
    }
    HeapFree(GetProcessHeap(), 0, saved);

    if (blob && rc == boot::BOOT_OK) {
        boot::blob_header *bh = reinterpret_cast<boot::blob_header *>(blob);
        bh->restore_changed   = changed;
        bh->restore_mismatch  = mismatch;
    }

    char line[400];
    if (rc != boot::BOOT_OK) {
        wsprintfA(line, "; BOOT SNAPSHOT: capture FAILED rc=%d (need %lu bytes)\n", rc,
                  (unsigned long)need);
        append_line(g_log_path, line);
        if (blob) HeapFree(GetProcessHeap(), 0, blob);
        return;
    }

    const boot::blob_header *h = reinterpret_cast<const boot::blob_header *>(blob);
    HANDLE                   f = CreateFileA(g_boot_snap_out, GENERIC_WRITE, 0, nullptr,
                                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD wrote = 0;
        WriteFile(f, blob, (DWORD)got, &wrote, nullptr);
        CloseHandle(f);
    }
    // The hash is logged so a run's snapshot is identifiable from its log alone, and the two
    // text-pointer tallies are logged because they are how the `pointed-into` carry declaration for
    // G_TEXT_BLOCK is CHECKED rather than merely asserted -- a build-time derivation cannot see a
    // pointer written at runtime.
    wsprintfA(line,
              "; BOOT SNAPSHOT: %d blocks, %lu bytes -> %s  hash=%08lX%08lX schema=%08lX"
              "  text_ptrs in_arena=%lu out_arena=%lu  restore changed=%lu mismatch=%lu\n",
              BOOT_SNAPSHOT_BLOCK_COUNT, (unsigned long)got, g_boot_snap_out,
              (unsigned long)(h->base.content_hash >> 32), (unsigned long)(h->base.content_hash & 0xffffffffu),
              (unsigned long)h->base.schema, (unsigned long)h->text_ptrs_in_arena,
              (unsigned long)h->text_ptrs_out_arena, (unsigned long)h->restore_changed,
              (unsigned long)h->restore_mismatch);
    append_line(g_log_path, line);
    // FLUSH IT, because this line is the run's only readable evidence of the capture and a capture
    // run is SHORT. append_line buffers 64 KB and flushes on the next append that is >= 1 s later,
    // so in a menu-only run -- the boot_snapshot UI scenario is exactly that -- nothing else ever
    // appends to mh_harness.log and the line dies in the buffer at process exit. Measured
    // 2026-09-11: the capture lane wrote its 773,036-byte blob and a ZERO-byte mh_harness.log, while
    // an in-game run of the same build (2123 further lines) kept the line. The blob is the artifact,
    // but "which blob did this run make" is the log's job.
    log_flush();
    HeapFree(GetProcessHeap(), 0, blob);
}

// One-shot poll off the present tick. Cheap when disarmed: one predictable branch per present.
void boot_snapshot_tick() {
    static bool done = false;
    if (done || !g_cfg.boot_snapshot) return;
    if (*reinterpret_cast<volatile const uint8_t *>(mh::addr::_G_LLM_GAME_MODE) != 3) return;
    done = true;
    boot_snapshot_capture();
}

// ---- mp:X3: THE SUB-DOMAIN HASH TRAIL -----------------------------------------------------------
//
// THE PROBLEM IT SOLVES. A desync is reported as "the state hash differs at step N", and the only
// thing that localises it is the per-region `R` line -- 61 columns, emitted every
// `region_hash_step` steps because emitting it every step is over a kilobyte per step of log. So
// the trail a reader actually has is coarse in TIME (the R cadence) and expensive to widen. The
// sub-domain fold is the opposite trade: nine numbers, cheap enough to write on EVERY step, naming
// the SUBSYSTEM that diverged and the exact step it first did. The R line then names the region
// inside it, from the next sampled step. Coarse-in-space/fine-in-time and fine-in-space/coarse-in-
// time, and the two together are what make "which subsystem, and when" answerable from one run.
//
// THE MEMBERSHIP IS BY NAME AND IT IS EXPLICIT. A region this table does not recognise lands in
// `other` rather than in a plausible neighbour -- so growing HASH_REGIONS[] makes an unclassified
// region VISIBLE (its hash starts moving in `other`) instead of silently changing the meaning of a
// domain a reader is comparing across runs.
//
// EXCLUDED REGIONS DO NOT FOLD IN. The point of the trail is the desync verdict, and the verdict is
// the state-only hash; folding the peer-local regions in would make `clock` and `lockstep` differ
// on every step of a healthy match, which is the cry-wolf shape D21 already refused once. A domain
// whose every member is excluded prints `-` rather than a hash of nothing.
//
// AND THERE IS NO `pathfinding` DOMAIN, which is a measured absence rather than an omission: NO
// pathfinding state is in the hash manifest at all. The nav-region graph (MAP_REGION_GRID,
// MAP_REGION_BY_INDEX, the pool heads) and the pathfinder's two heap blocks are MF_VIEW -- carried
// by a world blob, re-derived on import, hashed by nothing. `map` below is the closest the trail
// can get and it is a PROXY: tile_objects is the occupancy plane pathing READS, not the
// decomposition pathing WALKS. A desync inside the nav graph names no domain here because it names
// no region either.
enum region_domain {
    RD_UNITS = 0,
    RD_BUILDINGS,
    RD_MAP,
    RD_ORDERS,
    RD_RNG,
    RD_PLAYERS,
    RD_CLOCK,
    RD_LOCKSTEP,
    RD_OTHER,
    RD_COUNT
};

constexpr const char *RD_NAMES[RD_COUNT] = {"units", "buildings", "map", "orders", "rng",
                                            "players", "clock", "lockstep", "other"};

// Exact names first, then the two FAMILIES that are genuinely open-ended (the per-player slices are
// p0_..p7_ and the turn engine's are ls_/peer_). Everything else must be spelled, on purpose.
inline bool rd_name_is(const char *n, const char *lit) { return lstrcmpA(n, lit) == 0; }

inline bool rd_starts(const char *n, const char *pre) {
    while (*pre)
        if (*n++ != *pre++) return false;
    return true;
}

inline int rd_classify(const char *n) {
    if (rd_name_is(n, "units") || rd_name_is(n, "unit_storage") || rd_name_is(n, "soldiers") ||
        rd_name_is(n, "projectile_pool"))
        return RD_UNITS;
    if (rd_name_is(n, "buildings") || rd_name_is(n, "productions") || rd_name_is(n, "mines") ||
        rd_name_is(n, "turrets") || rd_name_is(n, "labs") || rd_name_is(n, "prod_slots"))
        return RD_BUILDINGS;
    if (rd_name_is(n, "tile_objects") || rd_name_is(n, "planets") || rd_name_is(n, "planet_status"))
        return RD_MAP;
    if (rd_starts(n, "order_")) return RD_ORDERS;
    if (rd_name_is(n, "rng_state")) return RD_RNG;
    if (rd_name_is(n, "strat_players") || rd_name_is(n, "players")) return RD_PLAYERS;
    // p0_.. p7_ -- the eight per-player slices, three each.
    if (n[0] == 'p' && n[1] >= '0' && n[1] <= '7' && n[2] == '_') return RD_PLAYERS;
    if (rd_name_is(n, "game_clock") || rd_name_is(n, "current_game_time") ||
        rd_name_is(n, "last_game_time") || rd_name_is(n, "total_game_time") ||
        rd_name_is(n, "sim_step_interval") || rd_name_is(n, "game_time_delta") ||
        rd_name_is(n, "game_speed") || rd_name_is(n, "frame_ring") || rd_name_is(n, "fps_estimate"))
        return RD_CLOCK;
    if (rd_starts(n, "ls_") || rd_starts(n, "peer_")) return RD_LOCKSTEP;
    return RD_OTHER;
}

// THE MAP IS PRINTED, ONCE, AND THAT IS WHAT KEEPS IT SINGLE-OWNER. tools/mp_analyze.py needs the
// same partition to fold the R columns the same way, and a second copy of this table in Python is a
// mirror that goes stale the day a region is appended -- the exact failure lint_region_mirror.py
// exists to watch for on the REGION list itself. So the analyzer READS the partition out of the log
// rather than holding one, and a run whose DLL classified a region differently says so in its own
// file instead of being silently re-folded by the reader.
void subdomain_map_emit() {
    static bool done = false;
    if (done) return;
    done = true;
    for (int d = 0; d < RD_COUNT; ++d) {
        char line[900];
        int  off = wsprintfA(line, "; [subdomain] %s =", RD_NAMES[d]);
        int  n   = 0;
        for (int i = 0; i < N_REGIONS; ++i) {
            if (rd_classify(REGIONS[i].name) != d) continue;
            if (off > (int)sizeof(line) - 48) break; // never write past the buffer -- see sim_hold_now
            off += wsprintfA(line + off, " %s%s", REGIONS[i].name, REGIONS[i].excluded ? "*" : "");
            ++n;
        }
        if (n == 0) off += wsprintfA(line + off, " (none)");
        line[off++] = '\n';
        line[off]   = '\0';
        append_line(g_log_path, line);
    }
    append_line(g_log_path,
                "; [subdomain] a trailing * marks a region EXCLUDED from the state-only verdict; "
                "those do not fold into the RD hashes, and a domain with no unexcluded member "
                "prints -.\n");
}

// One RD row. `per[]` is the caller's freshly computed per-region hash array, so this reads the
// same numbers the R row and the desync sample are built from and cannot describe a different
// instant.
void subdomain_row(const uint64_t *per) {
    uint64_t h[RD_COUNT];
    bool     any[RD_COUNT];
    for (int d = 0; d < RD_COUNT; ++d) {
        h[d]   = 1469598103934665603ULL; // the same FNV-1a-64 basis the state fold uses
        any[d] = false;
    }
    for (int i = 0; i < N_REGIONS; ++i) {
        if (state_excluded(i)) continue;
        const int d = rd_classify(REGIONS[i].name);
        h[d]        = fnv1a(&per[i], sizeof(per[i]), h[d]);
        any[d]      = true;
    }
    // Capacity DERIVED from RD_COUNT, for the reason the R row's R_HDR is derived from N_REGIONS:
    // each column is at most name(9) + '=' + 16 hex + a space = 27, and a table that grows must not
    // require remembering a number somewhere else in the file.
    char rl[32 + RD_COUNT * 28];
    int  off = wsprintfA(rl, "RD %lu", g_step);
    for (int d = 0; d < RD_COUNT; ++d) {
        if (any[d])
            off += wsprintfA(rl + off, " %s=%08lX%08lX", RD_NAMES[d], (unsigned long)(h[d] >> 32),
                             (unsigned long)(h[d] & 0xffffffffu));
        else
            off += wsprintfA(rl + off, " %s=-", RD_NAMES[d]);
    }
    rl[off++] = '\n';
    rl[off]   = '\0';
    append_line(g_log_path, rl);
}

// ---- mp:X3: THE SIM HOLD ------------------------------------------------------------------------
//
// `g_sim_hold` is read by sim_step_detour's naked asm, so it is a `long` and not a `bool` -- the
// same reason net_lockstep.cpp's `g_icon_gate` is (asm cannot read a C++ bool cheaply, and a
// one-byte compare against a type whose representation the standard does not pin is a trap nobody
// needs). NON-ZERO MEANS: llm_strat_sim_step's body must not run.
//
// TWO HALVES, AND BOTH ARE NEEDED. Freezing the clock alone (the F5J fence) stops the NEXT step
// from being funded but cannot stop the step we are already inside -- on_sim_step is the ENTRY
// detour, so its caller is about to run the body the moment we return. The hold therefore also
// makes the detour `ret` instead of falling through. Skipping the body is safe at this exact site
// and only here: llm_strat_sim_step is `void (void)` (__watcall callee, no stack
// arguments), so returning to its caller is the same machine state the body would have left.
long g_sim_hold = 0;
// The hold line's `why` is TRUNCATED at this many characters by a `%.*s`, so a caller cannot
// size the log buffer from the other end of the file. See sim_hold_now for what it cost to
// learn that the other way round.
constexpr int HOLD_WHY_MAX = 320;
void          sim_hold_now(const char *why); // defined next to fence_freeze_clock, whose globals it sets

// ---- LIB-WORLD: the step-0 world capture --------------------------------------------------------
//
// Called from on_sim_step's hash block with the numbers that block just computed -- see the call
// site for why it is there and nowhere else. One-shot, and every refusal is LOUD: a capture that
// silently did not happen is indistinguishable from a run that was never armed, which is the shape
// that makes a missing fixture look like an operator error hours later.
void world_snapshot_capture(uint64_t combined, uint64_t state, uint64_t clock) {
    namespace w      = mh::state::world;
    static bool done = false;
    if (done) return;
    done = true;

    w::capture_params p;
    p.lockstep_combined = combined;
    p.lockstep_state    = state;
    p.game_clock        = clock;
    p.step              = g_step;
    // The masks the hash above was ACTUALLY taken under, read from the same config fields that loop
    // passes to hash_slice. Recorded rather than assumed: a hash re-derived under different masks is
    // a different number, and an oracle that blamed the blob for that would be the wrong diagnosis.
    p.mask_flags = (g_cfg.mask_ctrl_group ? w::MASK_CTRL_GROUP : 0u) |
                   (g_cfg.mask_soldier_anim ? w::MASK_SOLDIER_ANIM : 0u) |
                   (g_cfg.mask_planets_gfx ? w::MASK_PLANETS_GFX : 0u);

    // capture_capacity(), not blob_size(): FORMAT 2 appends the nav trailer after the last block and
    // the trailer scales with the region pool, so blob_size() is now the offset the trailer starts
    // at rather than the total. `got` below is the real length and is what gets written out.
    const size_t need = w::capture_capacity();
    uint8_t     *blob = static_cast<uint8_t *>(HeapAlloc(GetProcessHeap(), 0, need));
    char         line[400];
    if (blob == nullptr) {
        wsprintfA(line, "; WORLD SNAPSHOT: HeapAlloc failed for %lu bytes\n", (unsigned long)need);
        append_line(g_log_path, line);
        log_flush();
        return;
    }

    size_t    got = 0;
    const int rc  = w::capture(blob, need, &got, p);
    if (rc != w::WORLD_OK) {
        wsprintfA(line, "; WORLD SNAPSHOT: capture FAILED rc=%d (need %lu bytes)\n", rc,
                  (unsigned long)need);
        append_line(g_log_path, line);
        log_flush();
        HeapFree(GetProcessHeap(), 0, blob);
        return;
    }

    const w::blob_header *h = reinterpret_cast<const w::blob_header *>(blob);
    HANDLE                f = CreateFileA(g_world_out, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                          FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD wrote = 0;
        WriteFile(f, blob, (DWORD)got, &wrote, nullptr);
        CloseHandle(f);
    }
    // The two lockstep numbers are printed HERE as well as landing in the blob, so the fixture can
    // be checked against the run's own `%lu <clock> <combined> <state>` line -- which the next
    // statement at the call site writes -- rather than only against a field the capture itself
    // chose. Two independent readings of one instant is the whole point of taking them together.
    wsprintfA(line,
              "; WORLD SNAPSHOT: %d blocks, %lu bytes -> %s  content=%08lX%08lX  step=%lu"
              "  lockstep=%08lX%08lX state=%08lX%08lX  masks=%lu  sinkfp=%08lX manifestfp=%08lX"
              "  ptrs head=%lu span=%lu\n",
              mh::state::WORLD_SNAPSHOT_BLOCK_COUNT, (unsigned long)got, g_world_out,
              (unsigned long)(h->base.content_hash >> 32),
              (unsigned long)(h->base.content_hash & 0xffffffffu), (unsigned long)h->step,
              (unsigned long)(h->lockstep_combined >> 32),
              (unsigned long)(h->lockstep_combined & 0xffffffffu),
              (unsigned long)(h->lockstep_state >> 32),
              (unsigned long)(h->lockstep_state & 0xffffffffu), (unsigned long)h->mask_flags,
              (unsigned long)h->hash_sink_fp, (unsigned long)h->hash_manifest_fp,
              (unsigned long)h->region_head_ptrs, (unsigned long)h->registry_span_ptrs);
    append_line(g_log_path, line);
    log_flush(); // same reason as the boot capture's: a short run must not lose its evidence line
    HeapFree(GetProcessHeap(), 0, blob);
}

// ---- mp:X1b: the LIVE snapshot verbs (capture -> send / poll -> import) -------------------------
//
// LIB-WORLD's world_snapshot_capture above writes the same blob to a FILE. These two do the thing a
// file cannot: move it to another mh.exe, in a running match, and put it into that peer's live
// memory. Everything about the format, the chunking, the hashes and the refusals belongs to
// mh_net_udp's pipeline; what is here is the two ends of it -- what to capture, and what to do with
// the bytes that come out.
//
// ---- THE TWO REGION-HASH LINES ARE THE WHOLE ORACLE ----------------------------------------------
//
// `SNAPCAP <step> <h0>..<hN>` on the sender and `SNAPIMP <step> <h0>..<hN>` on the receiver, one
// column per hashed region, same manifest order as the `R` line so mp_analyze reads all three with
// one parser. The claim they let a run make is exact and is checkable by a tool rather than by a
// person: EVERY column of SNAPIMP equals the matching column of SNAPCAP, including `rng_state`.
//
// WHY NOT JUST COMPARE THE `R` LINES. Because the R line is emitted every `region_hash_step` steps
// and the import lands whenever the transfer finishes -- which is 30-60 s of wall clock later, at a
// step nobody chose. Comparing "the client's R line at some step" against "the host's R line at
// step N" would be comparing two different instants and hoping. SNAPCAP is taken AT the capture and
// SNAPIMP immediately after the import returns, so both name the instant they describe.
//
// AND SNAPIMP IS RE-DERIVED, NOT COPIED OUT OF THE BLOB. It is hash_slice() over whatever is bound
// NOW, in this process, after the import has written it -- the same call on_sim_step makes. A line
// echoing numbers the blob carried would prove the blob arrived intact, which channel C and the
// manifest already prove twice over; re-hashing live memory is the only reading that can say the
// import actually landed where the sim will read it.

// One region-hash line, tagged. The capacity arithmetic is DERIVED from N_REGIONS for the reason the
// `R` line's own comment gives at length: this buffer overflowed once when the manifest grew from 23
// regions to 41, and "remember to grow a buffer somewhere else in the file" is not a rule that holds.
void snapshot_region_line(const char *tag, uint32_t step, const uint64_t *per) {
    constexpr int R_HDR = 8 + 10 + 2; // tag + space + up to 10 digits of step + '\n' + NUL
    char          rl[R_HDR + N_REGIONS * 17];
    int           off = wsprintfA(rl, "%s %lu", tag, (unsigned long)step);
    for (int i = 0; i < N_REGIONS; ++i)
        off += wsprintfA(rl + off, " %08X%08X", (unsigned)(per[i] >> 32), (unsigned)per[i]);
    rl[off++] = '\n';
    rl[off]   = '\0';
    append_line(g_log_path, rl);
}

// THE SENDER. Called from on_sim_step's hash block with the `per[]` that block just computed, for
// the same reason world_snapshot_capture is called there: the blob and the hashes it has to
// reproduce must describe ONE instant, and the cheapest way to guarantee that is not to argue it but
// to take both from the same variables one statement apart.
//
// LOUD ON EVERY REFUSAL. A send that silently did not happen is indistinguishable from a run that
// was never armed -- the shape that makes a missing transfer look like an operator error an hour
// later, on a scenario whose whole point is the transfer.
//
// AND IT RETRIES, RATHER THAN BEING ONE-SHOT, for one reason that is not defensive programming:
// `bulk_send_src` refuses when the destination is not an ADMITTED peer yet, and admission is a
// transport fact that a sim-step counter knows nothing about. In practice a launched match has
// already admitted its peer (steps only advance because inputs crossed), so the first attempt is
// expected to take -- but a scenario author who sets `snapshot_at` to the first step of the match
// should get a transfer a few steps later, not a silent nothing that reads as a broken surface.
// Bounded, decimated, and every attempt is logged: an unbounded retry would capture 8 MB per step.
void snapshot_send_now(const uint64_t *per, uint64_t combined, uint64_t state, uint64_t clock) {
    namespace w          = mh::state::world;
    static bool done     = false;
    static int  attempts = 0;
    if (done) return;
    // The first attempt is ON the armed step; the rest are every 25 steps after it, up to 20 tries.
    if (attempts > 0 && ((g_step - (uint32_t)g_cfg.snapshot_at) % 25u) != 0u) return;
    if (++attempts >= 20) done = true;

    char line[420];

    w::capture_params p;
    p.lockstep_combined = combined;
    p.lockstep_state    = state;
    p.game_clock        = clock;
    p.step              = g_step;
    p.mask_flags        = (g_cfg.mask_ctrl_group ? w::MASK_CTRL_GROUP : 0u) |
                   (g_cfg.mask_soldier_anim ? w::MASK_SOLDIER_ANIM : 0u) |
                   (g_cfg.mask_planets_gfx ? w::MASK_PLANETS_GFX : 0u);

    const size_t need = w::capture_capacity();
    uint8_t     *blob = static_cast<uint8_t *>(HeapAlloc(GetProcessHeap(), 0, need));
    if (blob == nullptr) {
        wsprintfA(line, "; SNAPSHOT SEND step=%lu REFUSED -- HeapAlloc failed for %lu bytes\n",
                  (unsigned long)g_step, (unsigned long)need);
        append_line(g_log_path, line);
        log_flush();
        return;
    }
    size_t    got = 0;
    const int rc  = w::capture(blob, need, &got, p);
    if (rc != w::WORLD_OK) {
        wsprintfA(line, "; SNAPSHOT SEND step=%lu REFUSED -- capture rc=%d\n", (unsigned long)g_step,
                  rc);
        append_line(g_log_path, line);
        log_flush();
        HeapFree(GetProcessHeap(), 0, blob);
        return;
    }

    // The hashes BEFORE the send, so the evidence exists even if the transport refuses.
    snapshot_region_line("SNAPCAP", g_step, per);

    // MH_Net_SnapshotSend COPIES (mh_net_module.h's ownership rule), so this blob is ours to free
    // the instant it returns -- which is why the 8 MB does not have to stay on our heap for the 30-60
    // seconds the transfer runs.
    const int sent = MH_Net_SnapshotSend(g_cfg.snapshot_to, blob, (int)got);
    HeapFree(GetProcessHeap(), 0, blob);
    if (sent) done = true; // one transfer per run; the retry above exists only for admission

    MH_NetSnapshotStatus st;
    MH_Net_SnapshotStatus(&st);
    wsprintfA(line,
              "; SNAPSHOT SEND step=%lu dst=%d bytes=%lu rc=%d supported=%d state=%d err=%d"
              "  lockstep=%08lX%08lX state=%08lX%08lX\n",
              (unsigned long)g_step, g_cfg.snapshot_to, (unsigned long)got, sent, st.supported,
              st.state, st.last_err, (unsigned long)(combined >> 32),
              (unsigned long)(combined & 0xffffffffu), (unsigned long)(state >> 32),
              (unsigned long)(state & 0xffffffffu));
    append_line(g_log_path, line);
    log_flush();
}

// THE RECEIVER. Polled every sim step while `snapshot_import` is set. On a peer nothing is sent to
// the per-step cost is one call: MH_Net_SnapshotPoll drains an empty lane and answers MH_SNAP_IDLE,
// and the module does not open its 32 MiB receive arena until a CHUNK lands.
//
// THE ONE COST THAT IS PAID ANYWAY is the destination buffer below -- capture_capacity(), ~8 MB, off
// this process's heap on the first poll, on every armed peer including the sender. It is stated
// rather than avoided because avoiding it would mean a two-call probe-then-deliver protocol on a
// surface whose whole job is to be small, and because this is an INSTRUMENTED run: `[harness]
// snapshot_import` is never set in a shipping game.
void snapshot_poll_now(void) {
    namespace w = mh::state::world;
    // The destination, allocated once. capture_capacity() is exactly the ceiling a capture from THIS
    // build can produce, so a blob that does not fit is a blob from a different build -- which the
    // manifest's own format/root checks have already refused long before this.
    static uint8_t *dst      = nullptr;
    static size_t   dst_cap  = 0;
    static bool     imported = false;
    if (imported) return; // one import per run; a second would be X3's business, not a verb's
    if (dst == nullptr) {
        dst_cap = w::capture_capacity();
        dst     = static_cast<uint8_t *>(HeapAlloc(GetProcessHeap(), 0, dst_cap));
        if (dst == nullptr) {
            g_cfg.snapshot_import = 0; // disarm rather than retry an allocation that failed once
            append_line(g_log_path, "; SNAPSHOT RX DISARMED -- no room for the destination buffer\n");
            log_flush();
            return;
        }
    }

    int       len   = (int)dst_cap;
    int       state = 0;
    const int ready = MH_Net_SnapshotPoll(dst, &len, &state);

    if (!ready) {
        if (g_cfg.snapshot_log > 0 && (g_step % (uint32_t)g_cfg.snapshot_log) == 0 &&
            state != MH_SNAP_IDLE) {
            MH_NetSnapshotStatus st;
            MH_Net_SnapshotStatus(&st);
            char pl[300];
            wsprintfA(pl,
                      "; SNAPSHOT RX step=%lu state=%d verified=%lu/%lu bytes=%lu refused=%d "
                      "err=%d root=%.16s\n",
                      (unsigned long)g_step, state, (unsigned long)st.rx_verified,
                      (unsigned long)st.rx_chunks, (unsigned long)st.rx_len, st.rx_refused,
                      st.last_err, st.root_hex[0] ? st.root_hex : "-");
            append_line(g_log_path, pl);
        }
        return;
    }

    imported = true;

    // The blob's own header, read BEFORE the import so the line below can name what arrived even if
    // the import refuses it.
    const w::blob_header *h         = reinterpret_cast<const w::blob_header *>(dst);
    const unsigned long   blob_step = (unsigned long)h->step;
    const uint64_t        b_comb    = h->lockstep_combined;
    const uint64_t        b_state   = h->lockstep_state;

    // THE FULL RE-DERIVE, NOT world::import() ALONE, and the difference is a crash rather than a
    // subtlety. `world::import()` is the BYTE half by design. A blob carries pointer-valued bytes --
    // `general`'s two pathfinder heap blocks, the nav-region graph, and the two 255-entry state
    // dispatch tables that in a promoted run hold THE CAPTURING PROCESS'S mh.dll addresses. Writing
    // those verbatim into this process and then stepping the sim is a call through a garbage pointer
    // (state/spine.cpp records the measurement: EIP == the fault address, inside the recorder's
    // mh.dll). libmh_import_world is import() plus the seven re-derives that make the imported world
    // belong to THIS process, and it is the only honest entry for a live import.
    // ---- THE SESSION-BEGUN LATCH, AND WHY THIS VERB LIFTS IT ------------------------------------
    //
    // MEASURED FIRST, then reasoned about: the first rig run of this scenario moved 8,186,488 bytes
    // across a real UDP link, delivered them whole and manifest-verified, and libmh_import_world
    // answered -4 -- ERR_SESSION_BEGUN. `world_policy::refuse_import()` returns
    // mh::state::boot::session_begun(), a monotone latch set by the two session-begin bodies
    // (sim_planet_session_begin / sim_session_begin_multi). In a live match it is ALWAYS set, so a
    // live world import is refused by construction, which is exactly the wall mp:X1 said it could
    // not see from inside one process.
    //
    // THE LATCH IS RIGHT FOR THE BLOB IT WAS WRITTEN FOR AND IS THE OPEN QUESTION FOR THIS ONE.
    // world_snapshot.cpp says it shares LIB-BOOT's latch "deliberately rather than duplicated",
    // because "importing a world over live session state is the same hazard". For a BOOT blob it
    // plainly is: that blob carries post-cfg prototype tables which the session has since rewritten,
    // so importing over them mid-session puts boot-time values under live code. A WORLD blob is the
    // other case -- it was CAPTURED mid-session and carries the session's own state, all 829 bound
    // regions of it, which is the whole premise of join-in-progress. Whether the shared latch should
    // grow a per-policy answer is mp:X3's ruling to make, not this item's.
    //
    // SO THIS IS AN INSTRUMENT OVERRIDE, NOT A POLICY CHANGE. The latch is cleared only here, only
    // under `[harness] snapshot_import`, only on the step a verified blob was delivered, and the
    // clearing is LOGGED -- so no run can lift it without saying so, and a reader of a log can tell
    // an import that the policy allowed from one an instrument permitted. `reset_session_latch_for_
    // test` is libmh's own name for this door; it exists because "an arm that proves the refusal
    // fires has to be able to un-fire it", and this is that arm one process further out.
    append_line(g_log_path, "; SNAPSHOT IMPORT LIFTING the session-begun latch (harness override; "
                            "world::import refuses a live session by policy -- mp:X3 owns the "
                            "ruling, this verb only measures what an import would do)\n");
    mh::state::boot::reset_session_latch_for_test();
    const int irc = libmh_import_world(dst, (size_t)len);

    char line[420];
    if (irc != w::WORLD_OK) {
        wsprintfA(line,
                  "; SNAPSHOT IMPORT step=%lu REFUSED rc=%d bytes=%lu blobstep=%lu -- the world is "
                  "UNTOUCHED (import validates completely before its first write)\n",
                  (unsigned long)g_step, irc, (unsigned long)len, blob_step);
        append_line(g_log_path, line);
        log_flush();
        return;
    }

    // THE POST-IMPORT READING. hash_slice over live memory, under the masks this run hashes with --
    // the same three the capture recorded, so a mask mismatch shows up as a whole-line difference
    // rather than as a puzzle.
    uint64_t per[N_REGIONS];
    for (int i = 0; i < N_REGIONS; ++i)
        per[i] = mh::state::hash_slice(i, g_cfg.mask_ctrl_group != 0, g_cfg.mask_soldier_anim != 0,
                                       g_cfg.mask_planets_gfx != 0);
    snapshot_region_line("SNAPIMP", blob_step, per);

    // And the lockstep pair re-folded from the same live memory, which is the blob's own second
    // witness: world::lockstep_hash is the function that must reproduce the capturing peer's two
    // numbers after an import, and the header carries what they were.
    uint64_t now_comb = 0, now_state = 0;
    w::lockstep_hash(h->mask_flags, &now_comb, &now_state);

    MH_NetSnapshotStatus st;
    MH_Net_SnapshotStatus(&st);
    wsprintfA(line,
              "; SNAPSHOT IMPORT step=%lu OK bytes=%lu blobstep=%lu refused=%d root=%.16s\n",
              (unsigned long)g_step, (unsigned long)len, blob_step, st.rx_refused,
              st.root_hex[0] ? st.root_hex : "-");
    append_line(g_log_path, line);
    wsprintfA(line,
              "; SNAPSHOT IMPORT HASHES blob=%08lX%08lX/%08lX%08lX live=%08lX%08lX/%08lX%08lX "
              "match=%d\n",
              (unsigned long)(b_comb >> 32), (unsigned long)(b_comb & 0xffffffffu),
              (unsigned long)(b_state >> 32), (unsigned long)(b_state & 0xffffffffu),
              (unsigned long)(now_comb >> 32), (unsigned long)(now_comb & 0xffffffffu),
              (unsigned long)(now_state >> 32), (unsigned long)(now_state & 0xffffffffu),
              (now_comb == b_comb && now_state == b_state) ? 1 : 0);
    append_line(g_log_path, line);
    log_flush(); // a run that desyncs itself a step later must not lose these two lines

    // mp:X3 STEP ONE -- the hold, LAST, after every witness above is written and flushed. If the
    // process is going to die it dies in the first sim step over the imported world, so the hold's
    // whole value is that the step never happens and the evidence survives.
    if (g_cfg.snapshot_hold)
        sim_hold_now("mp:X3 step one: imported a world blob and STOPPED, to separate the rewind "
                     "(the turn engine feeding live inputs to an older world) from the re-derives "
                     "libmh_import_world just ran against a live session");
}

// ---- order record/replay (Phase 2) --------------------------------------------------------------
// Record: at order_queue_dispatch entry the QUEUE holds exactly the orders about to execute this
// step (post release_due, pre compaction). Snapshot QUEUE[0..COUNT], tag with g_step, append.
void order_record() {
    int n = *reinterpret_cast<const int *>(ADDR_ORDER_QCOUNT());
    if (n <= 0 || n > ORDER_QCAP) return;
    if (g_rec_h == INVALID_HANDLE_VALUE) {
        g_rec_h = CreateFileA(g_orders_out, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (g_rec_h == INVALID_HANDLE_VALUE) {
            g_cfg.order_mode = 0;
            return;
        }
        struct {
            uint32_t magic, ver;
        } hdr = {ORDERS_MAGIC, 1};
        DWORD w;
        WriteFile(g_rec_h, &hdr, sizeof(hdr), &w, nullptr);
    }
    const uint8_t *q = reinterpret_cast<const uint8_t *>(ADDR_ORDER_QUEUE());
    for (int i = 0; i < n; ++i) {
        OrderRec r;
        r.step = g_step;
        memcpy(r.order, q + (size_t)i * ORDER_SIZE, ORDER_SIZE);
        DWORD w;
        WriteFile(g_rec_h, &r, sizeof(r), &w, nullptr);
    }
}

// Replay: load mh_orders.bin into a VirtualAlloc'd, step-sorted array.
void order_replay_load() {
    HANDLE h = CreateFileA(g_orders_path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        append_line(g_log_path, "; order_replay: mh_orders.bin missing\n");
        return;
    }
    DWORD sz = GetFileSize(h, nullptr), got = 0;
    struct {
        uint32_t magic, ver;
    } hdr = {0, 0};
    ReadFile(h, &hdr, sizeof(hdr), &got, nullptr);
    if (got != sizeof(hdr) || hdr.magic != ORDERS_MAGIC || sz < sizeof(hdr)) {
        append_line(g_log_path, "; order_replay: bad mh_orders.bin header\n");
        CloseHandle(h);
        return;
    }
    uint32_t nrec = (sz - sizeof(hdr)) / sizeof(OrderRec);
    g_replay      = static_cast<OrderRec *>(VirtualAlloc(nullptr, (size_t)nrec * sizeof(OrderRec) + 16,
                                                         MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!g_replay) {
        CloseHandle(h);
        return;
    }
    ReadFile(h, g_replay, nrec * sizeof(OrderRec), &got, nullptr);
    g_replay_n = got / sizeof(OrderRec);
    CloseHandle(h);
    char b[128];
    wsprintfA(b, "; order_replay: loaded %lu order records\n", g_replay_n);
    append_line(g_log_path, b);
}

// clock track: append the current step's game clock (8 bytes) during record.
void clock_record(uint64_t clock_bits) {
    if (g_clock_h == INVALID_HANDLE_VALUE) {
        g_clock_h = CreateFileA(g_clock_out, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (g_clock_h == INVALID_HANDLE_VALUE) return;
    }
    DWORD w;
    WriteFile(g_clock_h, &clock_bits, sizeof(clock_bits), &w, nullptr);
}

void clock_load() {
    HANDLE h = CreateFileA(g_clock_path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return; // optional -- absent => fixed_step pin as usual
    DWORD    sz = GetFileSize(h, nullptr), got = 0;
    uint32_t n  = sz / sizeof(double);
    g_clock_trk = static_cast<double *>(VirtualAlloc(nullptr, (size_t)n * sizeof(double) + 16,
                                                     MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!g_clock_trk) {
        CloseHandle(h);
        return;
    }
    ReadFile(h, g_clock_trk, n * sizeof(double), &got, nullptr);
    g_clock_n = got / sizeof(double);
    CloseHandle(h);
    char b[96];
    wsprintfA(b, "; clock_track: loaded %lu step clocks\n", g_clock_n);
    append_line(g_log_path, b);
}

// ---- all-AI: the pre-landing branch flip -------------------------------------------------------
//
// Runs at llm_game_land_players_on_planet ENTRY, i.e. before its slot loop reads status_flags. Every
// enabled HUMAN slot becomes an AI slot, so the loop's own AI arm (claim_landing_spot + spawn_ai_base)
// initialises it exactly the way it initialises a lobby-seated computer player. We do NOT touch
// player_data here -- doing the initialisation ourselves is precisely the mistake this exists to
// avoid; see Config::all_ai.
//
// b1 (ALIVE) IS DELIBERATELY UNTOUCHED. It is sim-owned (it gates sim_step's per-player loop, the
// order pump, and the lockstep commit horizon) and the landing path is not where it is established.
// Writing it here would be reaching into the sim's own bookkeeping to fake a state the sim is about
// to compute.
uint32_t g_allai_converted = 0; // slot bitmask, for the post-landing effectiveness line
bool     g_allai_ran       = false;
bool     g_allai_verified  = false;

// SPCAMP-SEED's instrument. `land_players_detour` stores its EAX argument here before it pushes
// anything, so the log can name the planet whose landing_x/y/spot_index slots to read -- those arrays
// are [32], indexed by planet, and reading the wrong one reports zeros as a result.
// A SEQUENCE, not a one-shot flag: the recorded sessions include a PLANET TRANSITION, so landing
// happens more than once per run and the second one is the interesting one for a campaign that has to
// stay reproducible past the first map.
uint32_t g_land_planet   = 0;
uint32_t g_land_seq      = 0; // ++ at every landing entry
uint32_t g_land_reported = 0; // the seq the post-landing line has already been written for

// Both counters are defined further down (next to the pins they belong to) and read here.
extern uint32_t g_rand_draws;
extern uint32_t g_strat_seed_calls;

void land_log_pre() {
    if (!g_cfg.land_log) return;
    ++g_land_seq;
    const auto *rng = reinterpret_cast<const uint32_t *>(mh::addr::_G_LLM_STRAT_RNG_STATE);
    char        line[224];
    wsprintfA(line,
              "; LAND PRE #%lu planet=%lu rng=%08X/%08X/%08X/%08X crt_draws=%lu seed_calls=%lu\n",
              g_land_seq, g_land_planet, rng[0], rng[1], rng[2], rng[3], g_rand_draws,
              g_strat_seed_calls);
    append_line(g_log_path, line);
}

// Called from the first sim step, i.e. after llm_game_land_players_on_planet has returned. There is no
// exit hook on that function and adding one would be a bigger change than this needs: the landing sites
// are PERSISTENT state (llm_strat_set_landing_site writes them into the player profiles), so reading
// them one step later reads the same numbers.
void land_log_report() {
    if (!g_cfg.land_log || g_land_seq == 0 || g_land_reported == g_land_seq) return;
    g_land_reported  = g_land_seq;
    const auto *rng  = reinterpret_cast<const uint32_t *>(mh::addr::_G_LLM_STRAT_RNG_STATE);
    const auto *prof = reinterpret_cast<const mh::game::mh_llm_strat_player_profile *>(ADDR_PLAYERS_PROF);
    const int   k    = (g_land_planet < 32u) ? (int)g_land_planet : 0;
    char        line[224];
    wsprintfA(line, "; LAND POST #%lu planet=%d rng=%08X/%08X/%08X/%08X crt_draws=%lu seed_calls=%lu\n",
              g_land_reported, k, rng[0], rng[1], rng[2], rng[3], g_rand_draws, g_strat_seed_calls);
    append_line(g_log_path, line);
    // SPCAMP-REC's non-vacuity clause, READ rather than assumed: 1 = the campaign session, 2 = the
    // lobby-created skirmish P0-SPDET already covers, 3 = lockstep MP. A campaign fixture that
    // quietly entered a skirmish would produce a perfectly reproducible run of the wrong thing.
    const unsigned mode = *reinterpret_cast<const uint8_t *>(mh::addr::_G_LLM_GAME_SESSION_MODE);
    wsprintfA(line, "; LAND SESSION_MODE=%u (%s)\n", mode,
              mode == 1   ? "CAMPAIGN"
              : mode == 2 ? "single-player skirmish"
              : mode == 3 ? "lockstep MP"
                          : "UNKNOWN");
    append_line(g_log_path, line);
    // The spot POOL, which is the observable that cannot lie about whether a roll happened:
    // llm_strat_claim_landing_spot stamps status=-2 on the slot it takes, so the claimed set is
    // visible here even if the per-player copy is somewhere this code is not looking.
    struct spot_t {
        int32_t x, y, status;
    };
    const auto *pool = reinterpret_cast<const spot_t *>(mh::addr::_G_LLM_STRAT_LANDING_SPOTS);
    for (int i = 0; i < 16 && pool[i].status != -1; ++i) {
        wsprintfA(line, "; LAND pool[%d] xy=%d,%d status=%d%s\n", i, (int)pool[i].x, (int)pool[i].y,
                  (int)pool[i].status, pool[i].status == -2 ? " CLAIMED" : "");
        append_line(g_log_path, line);
    }
    int seen = 0;
    for (int i = 0; i < PLAYER_PROF_COUNT; ++i) {
        if ((prof[i].status_flags & PS_ENABLED) == 0) continue;
        ++seen;
        // The detour's planet index FIRST, then a scan -- because a zero at [planet] and a landing
        // written to a different planet index look identical, and one of those is a real result.
        int alt = -1;
        for (int p = 0; p < 32; ++p)
            if (prof[i].landing_x[p] || prof[i].landing_y[p] || prof[i].landing_spot_index[p]) {
                alt = p;
                break;
            }
        wsprintfA(line, "; LAND slot=%d flags=%08X spot=%d xy=%d,%d | first-nonzero planet=%d", i,
                  (unsigned)prof[i].status_flags, (int)prof[i].landing_spot_index[k],
                  (int)prof[i].landing_x[k], (int)prof[i].landing_y[k], alt);
        append_line(g_log_path, line);
        if (alt >= 0)
            wsprintfA(line, " spot=%d xy=%d,%d\n", (int)prof[i].landing_spot_index[alt],
                      (int)prof[i].landing_x[alt], (int)prof[i].landing_y[alt]);
        else
            wsprintfA(line, "\n");
        append_line(g_log_path, line);
    }
    if (seen == 0)
        append_line(g_log_path, "; LAND reported NO enabled slot -- the landing this describes did not "
                                "happen, so do not read the PRE/POST rng lines as covering one.\n");
}

void on_land_players() {
    land_log_pre();
    if (!g_cfg.all_ai || g_allai_ran) return;
    g_allai_ran = true;
    auto *prof  = reinterpret_cast<mh::game::mh_llm_strat_player_profile *>(ADDR_PLAYERS_PROF);
    char  line[256];
    int   n = 0;
    for (int i = 0; i < PLAYER_PROF_COUNT; ++i) {
        const uint32_t before = prof[i].status_flags;
        if ((before & PS_ENABLED) == 0 || (before & PS_HUMAN) == 0) continue;
        prof[i].status_flags = (before & ~PS_HUMAN) | PS_AI;
        const uint32_t after = prof[i].status_flags; // READ BACK, do not assume the store landed
        const bool     took  = (after & PS_HUMAN) == 0 && (after & PS_AI) != 0;
        g_allai_converted |= 1u << i;
        ++n;
        // Reports what the bits ARE, not what the store intended. The mutation run that suppressed
        // the write printed "(HUMAN cleared, AI set)" over an unchanged 00000007 -- a line that
        // contradicted the two numbers beside it, which is the shape of a log a reader learns to
        // stop trusting.
        wsprintfA(line, "; ALLAI slot=%d status_flags %08X -> %08X %s\n", i, (unsigned)before,
                  (unsigned)after, took ? "(HUMAN cleared, AI set)" : "*** WRITE DID NOT TAKE ***");
        append_line(g_log_path, line);
    }
    if (n == 0) {
        // Not a no-op to shrug at: it means the run is an ordinary one-AI skirmish wearing the soak's
        // name, and every count read afterwards would describe the wrong world.
        append_line(g_log_path,
                    "; ALLAI armed but converted NOTHING -- no enabled HUMAN slot at landing. "
                    "The run is NOT an all-AI match; do not read its coverage as one.\n");
        return;
    }
    if (g_cfg.all_ai_observer >= 0 && g_cfg.all_ai_observer < PLAYER_PROF_COUNT) {
        auto *side = reinterpret_cast<uint16_t *>(mh::addr::PlayerSide);
        wsprintfA(line, "; ALLAI PlayerSide %d -> %d (observer slot; claimed=%d)\n", (int)*side,
                  g_cfg.all_ai_observer,
                  (prof[g_cfg.all_ai_observer].status_flags & PS_ENABLED) ? 1 : 0);
        append_line(g_log_path, line);
        *side = (uint16_t)g_cfg.all_ai_observer;
        if (g_cfg.synth_move || g_cfg.synth_ctrlgroup)
            append_line(g_log_path,
                        "; ALLAI WARNING: synth_move/synth_ctrlgroup act as PlayerSide, which now "
                        "points at the observer slot -- those workloads will target it, not a "
                        "playing side.\n");
    }
    wsprintfA(line, "; ALLAI converted %d human slot(s) -> AI, mask=%02X\n", n,
              (unsigned)g_allai_converted);
    append_line(g_log_path, line);
}

// The "was the knob EFFECTIVE" line, emitted once from the first sim step after landing. Armed and
// effective look identical in every count the run produces afterwards, so the fields here are the
// ones only spawn_ai_base writes: a converted slot with ai_enabled=1 but home=(0,0) is the exact
// failure the pre-landing placement exists to prevent, and it would otherwise be invisible.
void allai_verify() {
    if (!g_cfg.all_ai || g_allai_verified || g_allai_converted == 0) return;
    g_allai_verified = true;
    const auto *pd   = reinterpret_cast<const mh::game::mh_game_player_data *>(ADDR_PLAYER_DATA);
    char        line[256];
    int         bad = 0;
    for (int i = 0; i < PLAYER_DATA_COUNT; ++i) {
        if ((g_allai_converted & (1u << i)) == 0) continue;
        const auto &P  = pd[i];
        const int   ok = (P.ai_enabled != 0) && (P.ai_home_tile_x != 0 || P.ai_home_tile_y != 0);
        if (!ok) ++bad;
        wsprintfA(line,
                  "; ALLAI VERIFY slot=%d ai_enabled=%d home=(%d,%d) alien=%d plan_len=%lu "
                  "start_units=%d %s\n",
                  i, (int)P.ai_enabled, (int)P.ai_home_tile_x, (int)P.ai_home_tile_y,
                  (int)P.is_alien_race, (unsigned long)(P.ai_build_plan_len_and_flag & 0x7fffffffu),
                  (int)P.ai_start_units_remaining, ok ? "OK" : "NOT-SPAWNED");
        append_line(g_log_path, line);
    }
    if (bad)
        append_line(g_log_path,
                    "; ALLAI VERIFY FAILED: a converted slot has no AI home tile, i.e. it never went "
                    "through llm_strat_spawn_ai_base. Its distance-to-home decisions are measured "
                    "from the map origin -- the run is not usable as AI coverage.\n");
}

// ---- game-over detection ------------------------------------------------------------------------
// "Resolved" = at most one enabled slot still carries ALIVE (b1), which llm_strat_player_presence_lost
// clears when a player's units_alive and buildings_alive both reach 0 on the planet. Reported with the
// per-planet counts so a reader can tell an actual elimination from a b1 write for some other reason.
int g_alive_prev = -1;

void gameover_check() {
    const auto *prof = reinterpret_cast<const mh::game::mh_llm_strat_player_profile *>(ADDR_PLAYERS_PROF);
    // mh::addr:: directly rather than the ADDR_PLANET_IDX alias -- that alias is declared with the
    // synth-workload block further down this file, and this runs above it.
    const unsigned planet = *reinterpret_cast<const uint32_t *>(mh::addr::G_PLANET_INDEX);
    int            alive = 0, last = -1;
    for (int i = 0; i < PLAYER_PROF_COUNT; ++i) {
        const uint32_t f = prof[i].status_flags;
        if ((f & PS_ENABLED) && (f & PS_ALIVE)) {
            ++alive;
            last = i;
        }
    }
    if (alive == g_alive_prev) return;
    char line[256];
    wsprintfA(line, "; GAMEOVER-WATCH step=%lu alive=%d (was %d) last=%d planet=%u\n", g_step, alive,
              g_alive_prev, last, planet);
    append_line(g_log_path, line);
    // Per-player detail on every CHANGE. Without it the trajectory is a bare integer and a run that
    // ends at the outcome dialog leaves no record of WHO went out -- which is the only thing that
    // distinguishes a resolved match from a truncated one after the fact.
    if (planet < 32) {
        for (int i = 0; i < PLAYER_PROF_COUNT; ++i) {
            if ((prof[i].status_flags & PS_ENABLED) == 0) continue;
            wsprintfA(line, ";   p%d sf=%02X units=%d bldgs=%d\n", i,
                      (unsigned)prof[i].status_flags, (int)prof[i].units_alive[planet],
                      (int)prof[i].buildings_alive[planet]);
            append_line(g_log_path, line);
        }
    }
    g_alive_prev = alive;
    if (alive > 1) return;
    if (last >= 0 && planet < 32) {
        wsprintfA(line, "; GAMEOVER survivor=%d units=%d buildings=%d\n", last,
                  (int)prof[last].units_alive[planet], (int)prof[last].buildings_alive[planet]);
        append_line(g_log_path, line);
    }
    append_line(g_log_path, "; GAMEOVER: the match is resolved -- steps past this point simulate a "
                            "finished world and their counters are not comparable with a live run's.\n");
    if (g_cfg.gameover_stop && (g_cfg.stop_step == 0 || g_cfg.stop_step > (int)g_step))
        g_cfg.stop_step = (int)g_step;
}

void zero_ai_enabled_all() {
    for (uint32_t p = 0; p < 8; ++p)
        *reinterpret_cast<int *>(ADDR_PLAYER_DATA + (size_t)p * PLAYER_DATA_STRIDE + PD_AI_ENABLED_OFF) = 0;
}

// ---- the save-state index probe ------------------------------------------------------------------
//
// One "; AISTATE p=<i> ..." line per player slot, emitted once. Fields are chosen to answer "does
// this save exercise branch X", so each is either a LOOP BOUND or a BRANCH INPUT that a shadow arm
// was observed to find empty:
//
//   build_src   nonzero entries in llm_strat_ai_count_unit_build_sources' out-table. THE headline
//               field: plan_unit_training reported roles_max=0 on 600+ calls purely because this was
//               all-zero, leaving pass 3, pass 4, queue_train_unit and the start-units drain
//               unexercised. Computed by CALLING the real helper rather than re-deriving its rule,
//               so the index cannot drift from the function it is meant to feed.
//   prod_busy   production records with a live active_unit_type; prod_idle the rest. The cheaper
//               proxy for the same thing, kept because the two disagreeing is itself informative.
//   qk[0..4]    ai_bldg_queue entries by status NIBBLE -- the dispatch arms of bldg_queue_process,
//               whose rig run took only kinds 1 and 3. qw/qr/qc = the 0x20 waived / 0x40 removed /
//               0x80 committed bits, i.e. the flag paths that never fired.
//   res/mine    resource_spent[] and ai_mine_yield_by_resource[]; the affordability compare and the
//               mine-yield guard in the grant loop both read these.
//   plan/start  build-plan length+cursor (the gate at the top of plan_unit_training) and the two
//               start-unit countdowns.
//
// Reads only. wsprintfA has no %f, so nothing here prints a float.
void emit_aistate() {
    const auto    *pd    = reinterpret_cast<const mh::game::mh_game_player_data *>(ADDR_PLAYER_DATA);
    const auto    *bld   = reinterpret_cast<const mh::game::mh_map_object_building *>(mh::addr::buildings);
    const auto    *prod  = reinterpret_cast<const mh::game::mh_map_object_production *>(mh::addr::productions);
    const uint32_t nplay = *reinterpret_cast<const uint32_t *>(mh::addr::_G_LLM_STRAT_AI_ACTIVE_PLAYER_COUNT);
    static int32_t sources[128]; // the helper fills [0..UNIT.total]; static to keep it off the stack
    for (int p = 0; p < PLAYER_DATA_COUNT; ++p) {
        const auto &P = pd[p];
        const int   ai =
            *reinterpret_cast<const uint32_t *>(ADDR_PLAYER_DATA + (uintptr_t)p * PLAYER_DATA_STRIDE +
                                                  PD_AI_ENABLED_OFF)
                  ? 1
                  : 0;
        // buildings[p][0].index is the player's live building count (same field the AI's own
        // expand gate reads), so it doubles as the loop bound for the type histogram.
        const auto *row     = bld + (size_t)p * 100;
        const int   n_bldg  = row[0].index < 0 ? 0 : (row[0].index > 100 ? 100 : row[0].index);
        int         n_types = 0;
        uint8_t     seen[256];
        for (int i = 0; i < 256; ++i) seen[i] = 0;
        for (int i = 0; i < n_bldg; ++i) {
            const unsigned t = row[i].building_id & 0xffu;
            if (!seen[t]) {
                seen[t] = 1;
                ++n_types;
            }
        }
        int busy = 0, idle = 0;
        for (int j = 0; j < 8; ++j) {
            if (prod[(size_t)p * 8 + j].b_index == 0) continue; // unbound slot, not a building
            (prod[(size_t)p * 8 + j].active_unit_type ? busy : idle)++;
        }
        for (int i = 0; i < 128; ++i) sources[i] = 0;
        mh::call::llm_strat_ai_count_unit_build_sources(p, sources);
        int src = 0, src_max = 0;
        for (int i = 0; i < 128; ++i) {
            if (sources[i] > 0) ++src;
            if (sources[i] > src_max) src_max = sources[i];
        }
        int       qk[5] = {0, 0, 0, 0, 0}, qw = 0, qr = 0, qc = 0;
        const int qn = P.ai_bldg_queue_count < 0 ? 0 : (P.ai_bldg_queue_count > 64 ? 64 : P.ai_bldg_queue_count);
        for (int i = 0; i < qn; ++i) {
            const uint8_t s = P.ai_bldg_queue[i].status;
            if ((s & 0x0f) < 5) ++qk[s & 0x0f];
            if (s & 0x20) ++qw;
            if (s & 0x40) ++qr;
            if (s & 0x80) ++qc;
        }
        char line[640];
        wsprintfA(line,
                  "; AISTATE p=%d ai=%d alien=%d nplay=%lu bldg=%d btypes=%d prod_busy=%d prod_idle=%d "
                  "build_src=%d build_src_max=%d q=%d qk=%d/%d/%d/%d/%d qw=%d qr=%d qc=%d "
                  "res=%d/%d/%d/%d patrolq=%d mine=%d/%d/%d/%d/%d plan_len=%lu plan_cur=%d start_rem=%d "
                  "start_pend=%d recruit=%d targets=%d groups=%d sites=%d\n",
                  p, ai, P.is_alien_race ? 1 : 0, (unsigned long)nplay, n_bldg, n_types, busy, idle,
                  src, src_max, qn, qk[0], qk[1], qk[2], qk[3], qk[4], qw, qr, qc,
                  // `res` is resource ids 1..4. It was 5-wide until 2026-08-03, when what used to be
                  // its slot [0] was split out of resource_spent as the scalar now printed as
                  // `patrolq` -- so a `res` array of length 5 in tools/data/save_index.json is a
                  // pre-reshape row whose FIRST element is the patrol cursor, not a resource.
                  P.resource_spent[0], P.resource_spent[1], P.resource_spent[2], P.resource_spent[3],
                  P.ai_patrol_quadrant_cursor,
                  P.ai_mine_yield_by_resource[0], P.ai_mine_yield_by_resource[1],
                  P.ai_mine_yield_by_resource[2], P.ai_mine_yield_by_resource[3],
                  P.ai_mine_yield_by_resource[4],
                  (unsigned long)(P.ai_build_plan_len_and_flag & 0x7fffffffu), P.ai_build_plan_cursor,
                  P.ai_start_units_remaining, P.ai_start_units_pending_spawn, P.ai_promo_credit,
                  P.ai_target_list_count, P.ai_group_count, P.ai_resource_site_count);
        append_line(g_log_path, line);
    }
}

// Replay inject: called from on_sim_step (ALWAYS runs; dispatch is count-gated so we can't rely on it).
// Overwrite QUEUE with this step's recorded snapshot + set COUNT; sim_step's `if(COUNT!=0)` then fires
// dispatch to execute them. The snapshot already contains deferred re-appearances, so a per-step
// overwrite reproduces the recording exactly.
// order_queue_tail_clear: zero the DEAD slots [count, 300) of the hashed order queue.
//
// THE TAIL-CLEAR (user ruling, 2026-09-11; moved to an UNCONDITIONAL step-level call
// on the coordinator ruling of the same day -- see the call site). `order_queue` is hashed WHOLE -- all 300 slots of
// 0x44 -- while only [0, count) is live, and this injector deliberately leaves the rest alone.
// The residue is therefore process history, and two arms that agree on every live order can
// still disagree on the hash the moment `count` dips below an earlier high-water mark: measured
// between the hosted replay and the standalone at exactly the nine periodic instants, 0 bytes
// differing inside the live prefix and 6 beyond it, re-converging the step the injector next
// covered the stale slot. Zeroing the dead slots makes the hashed bytes a function of the LIVE
// queue alone, which is what the determinism clause always meant.
//
// FROM THE EFFECTIVE COUNT, NOT FROM `n`, and that distinction is load-bearing: the count is set
// ONLY when n > 0 (three lines up), so on a step that injects nothing the previous count -- and
// the records dispatch still owes -- are live. Clearing from `n` would delete them.
//
// CLAMPED, because the count has been MEASURED climbing past the array: it reached >300 around
// step 392 while the earlier append-at-the-count injector was in use (the dispatcher does not
// zero it). An unclamped `QCAP - live` would be negative and the memset would run off the end.
void order_queue_tail_clear() {
    uint8_t *q    = reinterpret_cast<uint8_t *>(ADDR_ORDER_QUEUE());
    int      live = *reinterpret_cast<int *>(ADDR_ORDER_QCOUNT());
    if (live < 0) live = 0;
    if (live > ORDER_QCAP) live = ORDER_QCAP;
    memset(q + (size_t)live * ORDER_SIZE, 0, ((size_t)ORDER_QCAP - (size_t)live) * ORDER_SIZE);
}

// LIB-REF step-5000, count-ledger tag 16: how many records THIS step injected. A global rather than
// a return value because the note is emitted after the UNCONDITIONAL tail-clear (which is not inside
// this function and runs in both arms), so the two halves of the opening balance are read at the
// same point the standalone host reads them.
int g_replay_injected = 0;

// LIB-REF-SOAK: THE INJECTOR'S OWN EVIDENCE, emitted unconditionally at the stop step.
//
// `fixture_replay.py`'s recorder guard refuses a fixture with a `0 -> n` step -- one whose
// top-of-step queue count is 0 in the RECORD while the recorder captured orders at dispatch entry --
// on the reasoning that "dispatch is count-gated and the suppressed enqueue cannot raise it, so its
// orders would silently never execute". That class is REAL and stays refused: an injector that
// APPENDS at the count without setting it leaves a count of 0, dispatch never runs, and the orders
// vanish silently. This injector is not that one (it sets `count = n`), but the guard must not take
// that on trust from a comment -- so the run MEASURES it and says so, and the guard admits `0 -> n`
// only against that measurement.
//
// A READBACK, not a tally of intentions: after writing the count we read the region back and compare.
// That catches a write to the wrong address, an unbound region, and any future injector variant that
// appends -- none of which a "we set it, honest" counter would notice.
int g_inject_arrivals = 0; // steps that injected at least one record
int g_inject_set_ok   = 0; // ... of those, where the count read back as the number injected
int g_inject_no_set   = 0; // ... and where it did not: the append-without-set shape, the real class

void order_replay_inject() {
    if (g_cfg.replay_ai_off && !g_ai_zeroed) {
        zero_ai_enabled_all();
        g_ai_zeroed = true;
    }
    // advance cursor to the first record at/after the current step
    while (g_replay_i < g_replay_n && g_replay[g_replay_i].step < g_step) ++g_replay_i;
    uint8_t *q = reinterpret_cast<uint8_t *>(ADDR_ORDER_QUEUE());
    int      n = 0;
    uint32_t j = g_replay_i;
    while (j < g_replay_n && g_replay[j].step == g_step && n < ORDER_QCAP) {
        memcpy(q + (size_t)n * ORDER_SIZE, g_replay[j].order, ORDER_SIZE);
        ++n;
        ++j;
    }
    if (n > 0) {
        *reinterpret_cast<int *>(ADDR_ORDER_QCOUNT()) = n; // arm the count-gate for dispatch
        ++g_inject_arrivals;
        // The readback -- see g_inject_arrivals' banner for why this is measured and not asserted.
        if (*reinterpret_cast<const int *>(ADDR_ORDER_QCOUNT()) == n)
            ++g_inject_set_ok;
        else
            ++g_inject_no_set;
    }
    g_replay_injected = n;
}

// ---- P0-SPDET: the deterministic wall clock ------------------------------------------------------
// Advanced once per sim_tick (one per frame -- llm_strat_frame calls time_tick and sim_tick as
// SIBLINGS, so time_tick reads the previous frame's value; that is a constant phase offset, not a
// nondeterminism). Read by the naked replacement below, which is the whole body of GetCurrentTime.
double g_pin_now = 0.0;
double g_pin_dt  = 0.0;

// ---- pin_menu_clock: llm_time_get_ticks_ms as a VIEW of the same pinned clock -------------------
//
// ONE CLOCK, TWO UNITS. Deriving the ms timer from g_pin_now rather than giving it a counter of its
// own is the whole design: the menu's milliseconds and the sim's seconds then cannot disagree, and
// pin_clock_dt_us stays the single knob for both. base is subtracted so the timer starts near 0 the
// way a tick count does -- some callers take differences, and a few compare against small constants.
//
// IT MUST WRITE THE GLOBALS, NOT ONLY RETURN. The original publishes _G_LLM_TIME_TICKS_MS (and
// caches the previous sample in _LAST), and callers read that global DIRECTLY as well as through the
// function -- so a replacement that only returned a value would leave every direct reader on a clock
// that had stopped. The diagnostic query counter is kept for the same reason: it still answers "how
// often was the clock asked", which is exactly what it answered before.
//
// What is deliberately NOT called is llm_game_clock_tick_update -- the GetTickCount refresh. That is
// the nondeterminism being replaced, not incidental work.
uint32_t pinned_ticks_ms_value() {
    const double ms = (g_pin_now - (double)g_cfg.pin_clock_base_s) * 1000.0;
    return (uint32_t)(ms < 0.0 ? 0.0 : ms + 0.5);
}

// __watcall, no arguments, returns in EAX -- identical to __cdecl for that shape, which is the same
// reasoning pinned_rand's comment records, so a plain C function is installed rather than a naked
// body. MSVC preserves EBX/ESI/EDI/EBP, which is exactly Watcom's callee-saved set.
uint32_t __cdecl pinned_time_get_ticks_ms();
MH_EXPORT_REPLACE(llm_time_get_ticks_ms, pinned_time_get_ticks_ms)

uint32_t __cdecl pinned_time_get_ticks_ms() {
    const uint32_t ms = pinned_ticks_ms_value();
    ++*(volatile uint32_t *)mh::addr::_G_LLM_TIME_TICK_QUERY_COUNT;
    *(volatile uint32_t *)mh::addr::_G_LLM_TIME_TICKS_MS_LAST = *(volatile uint32_t *)mh::addr::_G_LLM_TIME_TICKS_MS;
    *(volatile uint32_t *)mh::addr::_G_LLM_TIME_TICKS_MS      = ms;
    return ms;
}
// UI-REC: did one of the two IN-GAME cadences advance the pinned clock this frame? Set by
// on_sim_tick and on_tact_frame, read and cleared by MH_Harness_OnPresent.
//
// THE MENU HAS NO CADENCE, which is the whole reason this exists. The pinned clock advances at
// exactly two sites, both of them in-game (llm_strat_sim_tick, llm_tact_frame), so a pinned run
// sitting at the main menu reads the SAME GetCurrentTime() forever. That is survivable for the
// scripted walks -- sp_det.txt gates on UI-state predicates, never on elapsed time -- but a recorded
// session is indexed on presents and replayed against a clock, so a frozen menu clock would make
// every timestamp in the journal's menu half identical and any duration the UI derives from them
// meaningless. The present hook fills that gap and ONLY that gap: it advances the clock when neither
// in-game site did, so an in-game frame is bit-for-bit the frame it was before this existed.
bool g_pin_moved = false;

// GetCurrentTime replacement: no arguments, returns a double in ST(0). Net FPU stack effect +1,
// same as the original. Clobbers nothing else, so it is strictly more register-preserving than the
// body it replaces (which saved eax/ebx/ecx/edx/esi/edi).
// ---- TACT-PREP: the deterministic rand ----------------------------------------------------------
// llm_rand's whole body, replaced. The original is the Watcom CRT rand():
//     seed = seed*0x41C64E6D + 0x3039;  return (seed >> 16) & 0x7fff;
// reading its seed through the per-thread data block. This keeps the ALGORITHM and the RANGE
// identical and only changes WHERE the state lives -- from process-global thread data (which
// anything earlier in the process can advance, and which no srand exists to reset) to a variable
// this file owns and the ini seeds. So a pinned run draws the same sequence from frame 0 regardless
// of what the menus, the intro or a previous mission consumed.
//
// __watcall, no arguments, returns in EAX. A naked body is used rather than an install of a C
// function because the original takes no arguments and returns in EAX under BOTH conventions, so
// there is no marshalling to get wrong.
uint32_t g_rand_state = 0;

// DRAW COUNTER, for the Sect. 9h elimination. The LCG state alone would let two arms be compared, but
// the COUNT says HOW FAR apart they are -- one extra draw and a thousand extra draws are very
// different faults, and a state compare cannot tell them apart.
uint32_t g_rand_draws = 0;

// CALL-SITE ATTRIBUTION for the draws (Sect. 9j). The aggregate counter proved the arms diverge by
// two orders of magnitude; it cannot say WHO is drawing, and the guess that it was one draw per squad
// unit per frame from the wander arm is REFUTED -- that arm is gated by a 2-second interval, which
// over 512 frames of pinned time is ~34 draws, not the ~4122 observed. So bucket by return address
// and let the log name the caller instead of inferring it.
constexpr int RAND_SITES                  = 12;
uint32_t      g_rand_site_pc[RAND_SITES]  = {0};
uint32_t      g_rand_site_cnt[RAND_SITES] = {0};

// The GAME's return address, captured by the naked thunk before it calls in here. _ReturnAddress()
// inside this function is USELESS for attribution: pinned_rand does `call pinned_rand_next`, so it
// returns an address inside pinned_rand itself -- measured, and it reported one single site
// (6D2F8905, an mh.dll address) for every draw in the run.
uint32_t g_rand_caller_pc = 0;

extern "C" uint32_t __cdecl pinned_rand_next() {
    // THE PIN OWNS THIS ENTRY OUTRIGHT since fork F2F. While the deferred-effect seam existed this
    // body opened with `if (!mh::effects::enter_deferred(ADDR_RAND, 0, 0)) return 0;` -- llm_rand is
    // classified `effectful`, its seed is a heap block reached through the CRT's per-thread data
    // pointer, so nothing the shadow snapshot declares can roll a draw back, and asking the seam was
    // what kept the LCG from advancing in BOTH arms of a shadow window. That call ALWAYS FAILED OPEN
    // outside a window and whenever the seam was not installed (the overwhelming majority of runs),
    // which is why deleting it with the gates is behaviour-preserving for every live configuration:
    // there are no windows left to be inside.
    const uint32_t pc = g_rand_caller_pc;
    for (int i = 0; i < RAND_SITES; ++i) {
        if (g_rand_site_pc[i] == pc) {
            ++g_rand_site_cnt[i];
            break;
        }
        if (g_rand_site_pc[i] == 0) {
            g_rand_site_pc[i]  = pc;
            g_rand_site_cnt[i] = 1;
            break;
        }
    }
    ++g_rand_draws;
    g_rand_state = g_rand_state * 0x41C64E6Du + 0x3039u;
    return (g_rand_state >> 16) & 0x7fffu;
}

// ECX AND EDX MUST BE PRESERVED -- this stub impersonates a WATCOM function, and Watcom makes
// EBX/ECX/EDX/ESI/EDI callee-saved. The real llm_rand @0x004da98b honours that literally: its first
// instruction is `PUSH EDX` and its last is `POP EDX`. `pinned_rand_next` is an ordinary __cdecl C
// function, for which EAX/ECX/EDX are VOLATILE, and it calls enter_deferred, walks the attribution
// table and does a 32-bit multiply -- so without these saves the draw returns with EDX destroyed.
//
// ONE CALLER RELIES ON IT AND IT COST A DAY (TACT1-P C4, 2026-09-04).
// llm_tact_unit_update_anim computes the unit base into EDX BEFORE the draw and reads
// anim_frame_time through it AFTER: `IMUL EDX,[EBP-0x28],0x5f4` @0x0042c871 -> `CALL llm_rand`
// @0x0042c878 -> `FADD qword [EDX+0x8260fb]` @0x0042c889 (state 0; state 1 is the same shape at
// 0x0042cc2a/0x0042cc31/0x0042cc42). With EDX clobbered the ORIGINAL body read some other unit's
// timestamp -- unit 0's, whenever the clobber came back 0 -- so its jitter deadline, and therefore
// its advance decision, was wrong on every tactical rig run since pin_rand landed. It surfaced only
// when the promotion of that row made OUR (correct) body run beside it and the capture moved: the
// arm under suspicion was the faithful one. The determinism A/B could never have caught it, since
// both peers were corrupted identically and hashed IDENTICAL.
//
// Every other tact llm_rand caller overwrites EDX immediately (`MOV EDX,EAX` at 0x0042bc0a,
// 0x00433b79, 0x00432f52, 0x00433239, 0x0042efef), which is why exactly one row diverged.
// SPCAMP-SEED: the strategic seed's replacement. A leaf that returns a constant in EAX, so unlike
// pinned_rand it needs no register saves at all -- nothing but EAX is touched, and EAX is the return
// register. `inc` writes EFLAGS, which is fine HERE and only because this is a function ENTRY: a
// Watcom callee's incoming flags are dead. The counter exists so the ARMED line can be followed by
// "and it was actually called N times" -- an armed pin nobody calls and an unarmed pin look identical
// in the resulting hashes, which is the failure mode every other pin in this file logs against.
uint32_t g_strat_seed_value = 7;
uint32_t g_strat_seed_calls = 0;

__declspec(naked) void pinned_strat_seed() {
    __asm {
        inc  dword ptr [g_strat_seed_calls]
        mov  eax, dword ptr [g_strat_seed_value]
        ret
    }
}

__declspec(naked) void pinned_rand() {
    __asm {
        push ecx
        push edx
        push eax // [esp+12] is now the game's return address
        mov  eax, [esp+12]
        mov  g_rand_caller_pc, eax // stash it for pinned_rand_next's call-site attribution
        pop  eax
        call pinned_rand_next // returns in EAX, which is also __watcall's return register
        pop  edx
        pop  ecx
        ret
    }
}

__declspec(naked) void pinned_get_current_time() {
    __asm {
        fld g_pin_now
        ret
    }
}

// LIB-REF-REC: defined next to the stop block that arms it (see on_sim_step's tail). Declared here
// because sim_tick is the PER-FRAME pump and sim_step is not: a stop step whose sim never takes
// another step would otherwise leave the latch unconsumed and the process alive until the runner's
// wall killed it. Whichever of the two fires first is after the stop step's body either way.
void exit_after_body_if_latched();

// ---- F5J: THE SIM-STEP FENCE (mh/include/mh_harness_export.h MH_Harness_StepFence) --------------
//
// Two writes of one value, at the only two instants that can place a capture ON a step:
//
//   on_sim_step, at step N  -- ends the CURRENT frame's catch-up burst with step N. Without this the
//                              frame would keep stepping (llm_strat_sim_tick's loop runs until the
//                              clock catches TOTAL) and the render that follows would carry N+k.
//   on_sim_tick, every frame -- HOLDS it there. time_tick recomputes TOTAL from the wall clock (and
//                              clamps it to the committed horizon) once per frame, so a one-shot
//                              truncation would last exactly one frame -- and the ui-script
//                              interpreter runs ONE step per present, so the `capture` that follows
//                              a satisfied `simstep` fires on the NEXT frame.
//
// Writing TOTAL_GAME_TIME is the same lever `fixed_step` and the clock-track replay two functions
// down already pull; the fence just picks the value that funds zero steps instead of one.
int  g_fence_at  = 0;     // target sim step (0 = no fence armed)
bool g_fence_hit = false; // the target step has run and the sim is being held at it

// TOTAL_GAME_TIME = GAME_CLOCK. Both branches of llm_strat_sim_tick then advance ZERO steps: mode 3
// loops while `clock + interval <= total` (false) and the SP branch runs on `0 < total - clock`
// (false). Unaligned doubles -> memcpy, like every other clock write in this file.
void fence_freeze_clock() {
    double gc;
    memcpy(&gc, reinterpret_cast<const void *>(ADDR_GAME_CLOCK), sizeof(gc));
    memcpy(reinterpret_cast<void *>(ADDR_TOTAL_TIME), &gc, sizeof(gc));
}

// mp:X3. Declared up beside the snapshot verbs (their caller), defined here because it borrows the
// fence's two globals: g_fence_hit makes on_sim_tick re-freeze the clock on EVERY frame, so nothing
// downstream (the mode-3 catch-up loop, the clock-track replay, the fixed pin) can fund another
// step, and g_sim_hold makes the detour skip the body of the step we are already inside.
void sim_hold_now(const char *why) {
    if (g_sim_hold) return; // idempotent: the first hold is the one that gets logged
    g_sim_hold  = 1;
    g_fence_hit = true;
    fence_freeze_clock();
    // THE CAPACITY IS DERIVED, NOT PICKED, and this line has already been the bug it now guards
    // against. The first version was `char hb[300]` against a ~175-character fixed part and a
    // `why` the call site writes as a 197-character sentence: wsprintfA wrote 394 bytes into
    // 300, /GS caught the smashed cookie on return, and the peer died with 0xC0000409 -- INSIDE
    // the very measurement whose whole question is "did the peer die". A stack smash in the
    // instrument is indistinguishable from the fault under investigation, so the size follows
    // the inputs rather than a round number. (The harness's per-region `R` line learned exactly
    // this when D11 took the manifest from 23 regions to 41; see its R_HDR derivation.)
    // `%.319s` AND NOT `%.*s`: wsprintfA is USER32's wvsprintf, whose format subset does NOT
    // include the asterisk for width or precision -- a `*` here would be printed, not consumed,
    // and the truncation that makes the buffer safe would silently not happen. The literal and
    // HOLD_WHY_MAX are tied together by the static_assert below so they cannot drift apart.
    constexpr int HOLD_FIXED = 224; // the format's own text + 10 digits of step + NUL, w/ margin
    static_assert(HOLD_WHY_MAX == 320, "the %.319s literal above must match HOLD_WHY_MAX - 1");
    char hb[HOLD_FIXED + HOLD_WHY_MAX];
    wsprintfA(hb,
              "; SIM HOLD step=%lu -- %.319s. The sim is frozen HERE: this step's body is skipped and "
              "the clock is pinned, so no further sim step is funded. The frame loop, the renderer "
              "and the transport keep running.\n",
              g_step, why);
    append_line(g_log_path, hb);
    log_flush(); // a hold whose reason is a crash under investigation must not die in the buffer
}

// ---- fixed-timestep pin (called from the naked sim_tick detour, BEFORE the mode branch) ---------
// Pin TOTAL_GAME_TIME = GAME_CLOCK + interval so the mode-1/2 branch advances exactly one
// deterministic 0.1s sim_step this frame (delta = interval). Unaligned doubles -> memcpy.
void on_sim_tick() {
    exit_after_body_if_latched(); // LIB-REF-REC: the stop step's body has now run
    // sim_tick ENTRY temporal event. Emitted HERE (not via net_seams' [trace] hooks) for the same
    // reason as sim_step: the harness owns this entry, so install_trace_hooks' prologue guard always
    // skips it ("prologue != frame / already hooked") and TEV_SIMTICK never fired. Must precede every
    // early return below. (2026-07-20)
    MH_Temporal_Event(4 /*TEV_SIMTICK*/);
    // P0-SPDET: advance the pinned wall clock exactly once per frame. Must precede the early returns
    // below for the same reason the temporal event does -- a clock that stops advancing when a
    // clock-track replay is active would freeze time_tick's dt at 0 and silently change the run.
    if (g_cfg.pin_wallclock) {
        g_pin_now += g_pin_dt;
        g_pin_moved = true; // UI-REC: tell the present hook this frame is already accounted for
    }
    // F5J: the fence's HOLD. After the wall-clock pin, which must keep advancing (the present-side
    // cadence and the UI interpreter ride it), and before BOTH clock pins below, either of which
    // would immediately fund another step and let the held frame move again.
    if (g_fence_hit) {
        fence_freeze_clock();
        return;
    }
    // Clock-track replay: pin TOTAL_GAME_TIME to the RECORDED clock for the next step, so the sim
    // reproduces the exact (possibly variable, real-time) per-step deltas of the recording. g_step =
    // steps completed; g_clock_trk[g_step] is the next step's recorded clock (track[0] = step 1). This
    // makes a real-time human recording replay faithfully. Falls through to the fixed pin past the end.
    if (g_clock_trk && g_step < g_clock_n) {
        memcpy(reinterpret_cast<void *>(ADDR_TOTAL_TIME), &g_clock_trk[g_step], sizeof(double));
        return;
    }
    if (!g_cfg.fixed_step) return;
    double gc, iv;
    memcpy(&gc, reinterpret_cast<const void *>(ADDR_GAME_CLOCK), sizeof(gc));
    memcpy(&iv, reinterpret_cast<const void *>(ADDR_SIM_INTERVAL()), sizeof(iv));
    if (!(iv > 0.0)) iv = 0.1; // guard against an unset interval (NaN/0)
    double tot = gc + iv;
    memcpy(reinterpret_cast<void *>(ADDR_TOTAL_TIME), &tot, sizeof(tot));
}

// Dump ORDER_PENDING/ORDER_QUEUE rows as ";ord" comment lines (order_log). One line per order with the
// llm_strat_order identity fields; exec_time (game-seconds double) printed as ms since wsprintfA has no %f.
static void order_dump_array(const char *tag, uintptr_t base, int count, int cap) {
    if (count < 0) count = 0;
    if (count > cap) count = cap; // guard a torn/garbage count against a wild read
    for (int i = 0; i < count; ++i) {
        const uint8_t *o = reinterpret_cast<const uint8_t *>(base + (uintptr_t)i * ORDER_SIZE);
        double         exec;
        memcpy(&exec, o, sizeof(double));                            // +0  scheduled exec game-clock
        unsigned unit = *reinterpret_cast<const uint16_t *>(o + 8);  // +8  unit_index
        unsigned own  = *reinterpret_cast<const uint16_t *>(o + 10); // +10 owner_and_kind (low nibble = player)
        int      p0   = *reinterpret_cast<const int16_t *>(o + 12);  // +12 param0
        unsigned code = *reinterpret_cast<const uint16_t *>(o + 14); // +14 order_code
        char     l[128];
        wsprintfA(l, ";ord   %s%d own=%04X unit=%u code=%02X p0=%d exec=%dms\n",
                  tag, i, own, unit, code, p0, (int)(exec * 1000.0));
        append_line(g_log_path, l);
    }
}

// ---- the per-step work (called from the naked sim_step detour) ----------------------------------
// ---- D6: synthetic moving-unit workload ---------------------------------------------------------
// WHY THIS EXISTS. Every automated determinism run used to drive the peers into the game and then do
// NOTHING -- no orders, no motion, no combat. "ALL PAIRS IDENTICAL" then means the peers agreed about
// a world in which almost nothing happened, and any transient divergence RE-CONVERGES for free
// because there is no ongoing simulation to carry it forward. That makes the gate structurally unable
// to see a CASCADING desync, which is the only kind that matters.
//
// WHY THE SEED MUST COME FROM OUTSIDE THE GAME. A destination drawn from the game's own PRNG would be
// identical on every peer BY CONSTRUCTION and would never exercise the wire. The runner draws it with
// os.urandom and writes the SAME value into every peer's mh_net.ini: the peers agree (so the test
// does not desync itself) but the value is genuinely fresh per run (so a fixed-path fluke cannot hide).
//
// WHY THE MOTHERSHIP. It is a flying unit from step 1, and flying units do not collide with each
// other -- continuous motion with no pathing/collision confound.
//
// WHY landing_x/landing_y AS THE BASE. They are replicated sim state and known-valid map coordinates
// for this planet, so a jittered offset from them is on-map by construction. Absolute coordinates
// would risk an off-map target the game rejects -- a silently no-op workload, i.e. the exact vacuity
// this item exists to remove.
//
// ABI -- verified against two retail call sites (0x0046a8ed, 0x00445184). Do not "simplify" it:
//   llm_strat_unit_order_move: EAX=player EDX=unit_idx EBX=x ECX=y, ONE stack arg = order_seq_id,
//   and it returns RET 4 (CALLEE-cleanup -- the caller must NOT pop). Every retail caller then does
//   seq[player]++ ; if (seq[player] == 0) seq[player]++   (skip 0 on wrap). We replicate that.
// It routes through llm_strat_order_dispatch, which in SESSION_MODE==3 stages+SCHEDULES the order to
// a lockstep-safe future time -- that scheduled/broadcast lane is precisely what is under test.
// llm_strat_unit_order_move_enqueue would bypass it and must not be used here.
constexpr uintptr_t ADDR_PLANET_IDX = mh::addr::G_PLANET_INDEX;
inline uintptr_t    ADDR_ORDER_SEQ() { // SB-HOSTFREE: registry-resolved, see ADDR_ORDER_QUEUE()
    return mh::state::live_base(mh::state::RID_STRAT_ORDER_SEQ_ID_BY_PLAYER);
}
constexpr uintptr_t ADDR_SIDE        = mh::addr::PlayerSide; // ushort, the SIM's player index
constexpr uintptr_t ADDR_PLAYERS_ARR = mh::addr::_G_LLM_STRAT_PLAYERS;
constexpr uintptr_t ADDR_MAP_W       = mh::addr::width;  // strategic map width  (ReadMapFile)
constexpr uintptr_t ADDR_MAP_H       = mh::addr::height; // strategic map height (ReadMapFile)

int      g_saves_done     = 0; // SV1-P save trigger; bounded by cfg.save_count
int      g_loads_done     = 0; // SV1-P-LOAD load trigger; bounded by cfg.load_count
bool     g_synth_armed    = false;
bool     g_synth_failed   = false; // a guard tripped -> the run must not be read as a pass
uint32_t g_synth_check_at = 0;     // step at which to verify the world actually moved
uint64_t g_synth_units_h0 = 0;     // units-region hash captured when the order was issued
// Regions with a NON-DEFAULT hash function or a probe attached. Generated indices (ST2M): these
// were resolved by scanning REGIONS[] for the name at init, because a hardcoded index into a
// hand-appended table was the thing that could silently go stale. Nothing to resolve now.
constexpr int g_idx_units = mh::state::HIDX_UNITS;        // ctrl-group-masked emitter
constexpr int g_idx_tiles = mh::state::HIDX_TILE_OBJECTS; // fog-masked emitter
constexpr int g_idx_bldgs = mh::state::HIDX_BUILDINGS;    // D12 AI-development probe


// A deliberately explicit bit-mixer. NOT the game PRNG -- see the header note above.
inline uint32_t synth_mix(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

// The strategic map is a TORUS, so a destination is just (random mod dimension) -- uniform over the
// whole map with no boundary handling at all.
//
// WHAT THIS REPLACED, and why it mattered (user, 2026-07-27): the first version jittered +/-40 around
// the player's landing site and CLAMPED to a hand-invented [4,250] band. Both were wrong. The band
// was guesswork (the map is not 256 wide, and its real size is right here in `width`/`height`), and
// clamping a torus piles every out-of-range draw onto the boundary. Measured over 42 issued orders:
// x hit the floor 48% of the time, y 40%, either axis 67%, (4,4) came up 9 times, and the whole
// distribution lived in a ~50x55 corner. The seed was random; the DESTINATION was not.
inline uint32_t synth_map_w() { return *reinterpret_cast<const uint32_t *>(ADDR_MAP_W); }
inline uint32_t synth_map_h() { return *reinterpret_cast<const uint32_t *>(ADDR_MAP_H); }

void synth_move_issue() {
    char           msg[240];
    const unsigned side   = *reinterpret_cast<const uint16_t *>(ADDR_SIDE);
    const unsigned planet = *reinterpret_cast<const uint32_t *>(ADDR_PLANET_IDX);
    if (side >= 8 || planet >= 32) {
        wsprintfA(msg, "; SYNTH FAIL step=%lu: side=%u planet=%u out of range\n", g_step, side, planet);
        append_line(g_log_path, msg);
        g_synth_failed = true;
        return;
    }
    const auto *prof =
        reinterpret_cast<const mh::game::mh_llm_strat_player_profile *>(ADDR_PLAYERS_ARR) + side;
    const int unit = prof->primary_mother_unit[planet];
    if (unit == 0) {
        // VACUITY GUARD: no mothership handle means the workload would silently do nothing and the
        // run would look like a clean pass. Fail loudly instead.
        wsprintfA(msg, "; SYNTH FAIL step=%lu: player %u has no primary_mother_unit on planet %u\n",
                  g_step, side, planet);
        append_line(g_log_path, msg);
        g_synth_failed = true;
        return;
    }
    const uint32_t mw = synth_map_w(), mh_ = synth_map_h();
    if (mw < 8 || mw > 256 || mh_ < 8 || mh_ > 256) {
        // GUARD: a bogus dimension would make every destination garbage, and the run would still
        // report a pass. Fail instead of guessing a band (guessing one is what produced the corner
        // pile-up this replaced).
        wsprintfA(msg, "; SYNTH FAIL step=%lu: map dims look wrong (w=%lu h=%lu)\n", g_step,
                  (unsigned long)mw, (unsigned long)mh_);
        append_line(g_log_path, msg);
        g_synth_failed = true;
        return;
    }
    const uint32_t h  = synth_mix(static_cast<uint32_t>(g_cfg.synth_seed) ^ (g_step * 2654435761U) ^
                                  (side * 40503U));
    const int      tx = static_cast<int>(h % mw); // torus: no clamp, uniform over the map
    const int      ty = static_cast<int>(synth_mix(h) % mh_);

    auto          *seqp = reinterpret_cast<uint8_t *>(ADDR_ORDER_SEQ() + side);
    const unsigned seq  = *seqp;
    mh::call::llm_strat_unit_order_move(side, static_cast<int32_t>(unit), static_cast<uint32_t>(tx),
                                        static_cast<uint32_t>(ty), seq);
    if (++(*seqp) == 0) ++(*seqp); // retail post-call idiom (skip 0 on wrap)

    // Logged so the analyzer can assert every peer chose the SAME destination -- a target mismatch
    // would masquerade as a game desync.
    wsprintfA(msg, "; SYNTH step=%lu seed=%d side=%u planet=%u unit=%d -> (%d,%d) map=%lux%lu seq=%u\n",
              g_step, g_cfg.synth_seed, side, planet, unit, tx, ty, (unsigned long)mw,
              (unsigned long)mh_, seq);
    append_line(g_log_path, msg);

    // Arm the did-anything-actually-happen check -- but ONLY when one is not already pending.
    // BUG FIXED 2026-07-27: this used to re-arm on EVERY order, so at the default every-step cadence
    // the target step kept receding and the guard NEVER fired (observed: 2441 orders, 0 "SYNTH ok"
    // lines). A vacuity guard that silently stops running is worse than none -- it reads as evidence.
    // Arming only when idle gives one verification per 50 steps, since verify clears the flag.
    if (g_idx_units >= 0 && g_synth_check_at == 0) {
        // Through the same slice hash the verdict uses, so the before/after pair cannot drift apart.
        g_synth_units_h0 = mh::state::hash_slice(g_idx_units, true);
        g_synth_check_at = g_step + 50;
    }
}

// ---- D2 probe: assign this peer's mothership to a control group, once -----------------------------
// No determinism scenario has ever assigned a control group -- which is exactly why D2's divergence
// was never caught. This is the missing stimulus. It calls the REAL assign function rather than
// poking the byte, so whatever else assignment does (the CTRL_GROUPS bookkeeping, and the replicated
// order 0x34 it dispatches) happens too -- if the mask turned out to be insufficient, this is what
// would reveal it. Drive it on ONE peer (mp_run --harness-extra-host): a symmetric assignment on
// every peer would agree by coincidence and prove nothing.
void synth_ctrlgroup_issue() {
    char           msg[240];
    const unsigned side   = *reinterpret_cast<const uint16_t *>(ADDR_SIDE);
    const unsigned planet = *reinterpret_cast<const uint32_t *>(ADDR_PLANET_IDX);
    if (side >= 8 || planet >= 32) {
        wsprintfA(msg, "; CTRLGRP FAIL step=%lu: side=%u planet=%u out of range\n", g_step, side, planet);
        append_line(g_log_path, msg);
        g_synth_failed = true;
        return;
    }
    const auto *prof =
        reinterpret_cast<const mh::game::mh_llm_strat_player_profile *>(ADDR_PLAYERS_ARR) + side;
    const int unit = prof->primary_mother_unit[planet];
    if (unit == 0) {
        wsprintfA(msg, "; CTRLGRP FAIL step=%lu: player %u has no primary_mother_unit on planet %u\n",
                  g_step, side, planet);
        append_line(g_log_path, msg);
        g_synth_failed = true;
        return;
    }
    const unsigned grp = (unsigned)g_cfg.synth_ctrlgroup_id;
    // Typed through the generated mirror. This function already reached player_profile that way four
    // lines up while hand-offsetting the unit -- the inconsistency was visible inside one body.
    auto *const row = reinterpret_cast<mh::game::mh_map_object_unit *>(mh::state::hash_base(g_idx_units)) +
                      side * 100u;
    uint8_t *const fld    = &row[(unsigned)unit].ctrl_group_id;
    const uint8_t  before = *fld;
    mh::call::llm_strat_unit_ctrl_group_assign((int32_t)unit, (int32_t)grp);
    const uint8_t after = *fld;
    // VACUITY GUARD: if the byte did not actually change, the arm proves nothing about the mask.
    if (after != (uint8_t)grp) {
        wsprintfA(msg, "; CTRLGRP FAIL step=%lu: unit %d ctrl_group_id %u -> %u, wanted %u\n",
                  g_step, unit, before, after, grp);
        append_line(g_log_path, msg);
        g_synth_failed = true;
        return;
    }
    wsprintfA(msg, "; CTRLGRP step=%lu side=%u unit=%d ctrl_group_id %u -> %u (D2 probe)\n",
              g_step, side, unit, before, after);
    append_line(g_log_path, msg);
}

// ---- THE GROUP-MOVE WORKLOAD (AI1C read-bridge evidence, 2026-08-07) --------------------------
//
// Three game calls, in the order a player performs them: assign N units to hotkey group G, "press"
// G (which memcpy's CTRL_GROUPS[G] into CTRL_GROUPS[0], the selection), then issue the group move
// over that selection. See the config block for why nothing shorter reaches the machinery.
//
// THE VACUITY GUARD IS THE POINT OF THE WHOLE THING. A run that issued the order but never reached
// llm_strat_group_move_order_commit would leave the six regions untouched and look exactly like a
// success. So the six bytes-of-interest are snapshotted at issue time and re-read later; if they
// are byte-identical the run FAILS loudly instead of reporting a group move that never happened.
uint64_t g_gm_sig0     = 0;
uint32_t g_gm_check_at = 0;

// The six regions are NOT in the state registry (they are sim-owned scratch, not island data), so
// there is no hash_slice for them -- read the bytes directly. Deliberately a cheap mixing hash: it
// only has to answer "did any of this change", never "how".
uint64_t groupmove_region_sig() {
    static const struct {
        uintptr_t base;
        uint32_t  len;
    } R[] = {
        {0x0051de30u, 4},   // _G_LLM_STRAT_GROUP_CENTROID_X
        {0x0051de34u, 4},   // _G_LLM_STRAT_GROUP_CENTROID_Y
        {0x0051de38u, 4},   // DAT_0051de38 (the adjacent scratch)
        {0x006800acu, 512}, // _G_LLM_STRAT_GROUP_ROUTE_STEPS[256]
        {0x00680ab0u, 4},   // _G_LLM_STRAT_GROUP_ORDER_OWNER
        {0x006804acu, 512}, // _G_LLM_STRAT_GROUP_MEMBERS[256]
    };
    uint64_t h = 1469598103934665603ull;
    for (const auto &r : R)
        for (uint32_t i = 0; i < r.len; ++i) {
            h ^= *reinterpret_cast<const volatile uint8_t *>(r.base + i);
            h *= 1099511628211ull;
        }
    return h;
}

void synth_groupmove_issue() {
    char           msg[260];
    const unsigned side   = *reinterpret_cast<const uint16_t *>(ADDR_SIDE);
    const unsigned planet = *reinterpret_cast<const uint32_t *>(ADDR_PLANET_IDX);
    if (side >= 8 || planet >= 32) {
        wsprintfA(msg, "; GROUPMOVE FAIL step=%lu: side=%u planet=%u out of range\n", g_step, side, planet);
        append_line(g_log_path, msg);
        g_synth_failed = true;
        return;
    }
    if (g_idx_units < 0) {
        wsprintfA(msg, "; GROUPMOVE FAIL step=%lu: no units region index\n", g_step);
        append_line(g_log_path, msg);
        g_synth_failed = true;
        return;
    }
    auto *const row = reinterpret_cast<mh::game::mh_map_object_unit *>(mh::state::hash_base(g_idx_units)) +
                      side * 100u;
    const unsigned grp = (unsigned)g_cfg.synth_ctrlgroup_id;

    // Collect the first N ALIVE units of this player. A group move needs >= 2 members -- a lone unit
    // takes the ordinary move path and never enters GROUP_MARSHAL, which is precisely the reason
    // synth_move (one mothership) cannot produce this evidence.
    int       picked = 0, first_x = 0, first_y = 0;
    unsigned  ids[16];
    const int want = g_cfg.synth_groupmove_n < 2    ? 2
                     : g_cfg.synth_groupmove_n > 16 ? 16
                                                    : g_cfg.synth_groupmove_n;
    for (unsigned u = 1; u < 100u && picked < want; ++u) {
        if (!(row[u].energy > 0.0)) continue;
        ids[picked] = u;
        if (picked == 0) {
            first_x = row[u].x;
            first_y = row[u].y;
        }
        ++picked;
    }
    if (picked < 2) {
        // VACUITY GUARD: fewer than two live units means no GROUP move is possible at all.
        wsprintfA(msg, "; GROUPMOVE FAIL step=%lu: only %d live unit(s) for player %u -- need >= 2\n",
                  g_step, picked, side);
        append_line(g_log_path, msg);
        g_synth_failed = true;
        return;
    }
    for (int i = 0; i < picked; ++i)
        mh::call::llm_strat_unit_ctrl_group_assign((int32_t)ids[i], (int32_t)grp);

    // "Press the hotkey": memcpy CTRL_GROUPS[grp] -> CTRL_GROUPS[0] and select each member. The
    // issuer below reads CTRL_GROUPS[0] and nothing else, so without this it walks an empty list.
    mh::call::llm_unit_ctrl_group_activate((int32_t)grp);
    const int seated = *reinterpret_cast<const volatile int32_t *>(0x00b63be0u);
    if (seated < 2) {
        wsprintfA(msg, "; GROUPMOVE FAIL step=%lu: CTRL_GROUPS[0].count=%d after activate(%u)\n",
                  g_step, seated, grp);
        append_line(g_log_path, msg);
        g_synth_failed = true;
        return;
    }

    // Destination. The issuer SKIPS any member already standing on the target tile, so an offset
    // from the lead unit guarantees at least one member actually gets an order.
    const uint32_t tx = g_cfg.synth_groupmove_x ? (uint32_t)g_cfg.synth_groupmove_x
                                                : (uint32_t)((first_x + 12) & 0x7f);
    const uint32_t ty = g_cfg.synth_groupmove_y ? (uint32_t)g_cfg.synth_groupmove_y
                                                : (uint32_t)((first_y + 12) & 0x3f);
    g_gm_sig0         = groupmove_region_sig();
    g_gm_check_at     = g_step + 60;
    mh::call::llm_strat_group_issue_move_order(tx, ty);
    wsprintfA(msg, "; GROUPMOVE step=%lu side=%u group=%u members=%d seated=%d -> (%lu,%lu)\n",
              g_step, side, grp, picked, seated, (unsigned long)tx, (unsigned long)ty);
    append_line(g_log_path, msg);
}

void synth_groupmove_verify() {
    if (!g_gm_check_at || g_step != g_gm_check_at) return;
    g_gm_check_at = 0;
    char msg[260];
    if (groupmove_region_sig() == g_gm_sig0) {
        // VACUITY GUARD: the six group-move regions are byte-identical 60 steps after the order, so
        // llm_strat_group_move_order_commit never ran and this run is NOT evidence that the minimap's
        // read bridge is live.
        wsprintfA(msg,
                  "; GROUPMOVE FAIL step=%lu: the six group-move regions are unchanged 60 steps"
                  " after the order -- GROUP_MARSHAL never ran\n",
                  g_step);
        g_synth_failed = true;
    } else {
        wsprintfA(msg,
                  "; GROUPMOVE ok step=%lu: the six group-move regions CHANGED after the order"
                  " (centroid/route written; llm_map_minimap_render's inputs are live)\n",
                  g_step);
    }
    append_line(g_log_path, msg);
}

void synth_move_verify() {
    if (!g_synth_check_at || g_step != g_synth_check_at || g_idx_units < 0) return;
    g_synth_check_at = 0;
    const uint64_t h = mh::state::hash_slice(g_idx_units, true);
    char           msg[200];
    if (h == g_synth_units_h0) {
        // VACUITY GUARD: the units region is byte-identical 50 steps after a move order, so nothing
        // moved and this run proves nothing about a world in motion.
        wsprintfA(msg,
                  "; SYNTH FAIL step=%lu: units region unchanged 50 steps after the order --"
                  " the workload did NOT move anything\n",
                  g_step);
        g_synth_failed = true;
    } else {
        wsprintfA(msg,
                  "; SYNTH ok step=%lu: units region changed after the order (world is in motion)\n",
                  g_step);
    }
    append_line(g_log_path, msg);
}

// ---- U32: THE CONQUEST WORKLOAD ------------------------------------------------------------------
// See the config block for WHY. This half is the mechanics.
//
// EVERY GAME CALL BELOW IS A DISPATCH-LANE WRAPPER, checked one by one against the decompile:
//   llm_strat_unit_order_move_confirmed_with_bump 0x0046accf -> dispatch(u, p | 0x80, 0x18, move_op_arg)
//   llm_strat_order_queue_construction_debug      0x0046da00 -> dispatch(0, p,        0xf2, 0xf2)
//   llm_strat_order_create_unit_debug             0x0046d800 -> dispatch(0, p,        0xeb, 0xeb)
//   llm_strat_unit_order_attack_building          0x0046c6c6 -> dispatch(u, p | 0x80, 0x1d, 0x1d)
//   llm_strat_order_debug_kill_group              0x0046fc1e -> dispatch(0, p & 0xf,  0xf7, 0xf8)
// Their _enqueue twins bypass the lockstep lane and MUST NOT be substituted.
//
// WHY move_confirmed_with_bump IS "THE LANDING ORDER", since the name does not say so. The unit lane
// is GENERIC: llm_strat_order_queue_dispatch writes unit.order = param0 and unit.state = order_code,
// then copies args[] into the unit's fields. This wrapper dispatches param0 = 0x18 with goal_x/goal_y
// in args[0]/args[1], so the unit gets order = 0x18 = DEPLOY_APPROACH and state = its ordinary move
// state: it walks to the goal and, on arrival, consults `order` and deploys. Its own plate calls it
// "fixed order 0x18", and its only retail caller (llm_strat_group_issue_move_order_confirmed
// 0x00444fcb) invokes it ONLY for members whose Unit[].equivalent != 0 -- the player's "land here"
// click. The deploy chain is state 0x18 (deploy_approach 0x0048117c) -> state 0x17
// (deploy_to_building 0x00481a6b), which calls llm_bldg_construct_finalize with
// Unit[proto].equivalent. THE TWO SILENT ABORTS in that chain are why the +150 verify exists:
// deploy_approach's llm_bldg_footprint_is_clear runs with viewer = THE PLAYER (not the omniscient 8
// the 0xf2 path gets) and sends the unit to IDLE_SCATTER on failure; and deploy_to_building's own
// failure arm DESTROYS the unit and creates nothing -- which would eliminate the peer BY LANDING and
// read as a false U32(a) pass.

// TWO DIFFERENT PER-PLAYER TABLES, and conflating them is a trap the addr manifest calls out by name.
// _G_LLM_STRAT_PLAYERS (ADDR_PLAYERS_PROF) is llm_strat_player_profile[8], stride 0x740 -- landing_x,
// units_alive, status_flags. game::g::Players is llm_strat_player_desc[8], stride 0x34, and THAT is
// the one llm_strat_order_dispatch tests: `Players[player & 0xf].controller_flags & 4` reads BYTE
// +0x06 bit 2. player_profile has no such field at all.
constexpr uintptr_t ADDR_CONQ_SESSION_MODE = mh::addr::_G_LLM_GAME_SESSION_MODE; // byte; 3 = lockstep
constexpr uintptr_t ADDR_PLAYERS_DESC      = mh::addr::Players;
constexpr unsigned  PLAYER_DESC_STRIDE     = 0x34u;
constexpr unsigned  PLAYER_DESC_FLAGS_OFF  = 0x06u;
constexpr uint8_t   PD_NET_CONTROLLED      = 0x4u; // bit 2 -> order_dispatch takes the REPLICATED lane
constexpr unsigned  CONQ_SESSION_LOCKSTEP  = 3;
constexpr uint32_t  ORDER_WEAPON_AUTO      = 0xffffffffu; // the attack wrappers' "pick one for me"

// The phase machine's state. See conq_issue for WHY milestones are state-keyed rather than
// step-keyed: identical configs put the same order on different steps run to run, because
// order_dispatch clamps exec_time to the live LOCKSTEP_HORIZON.
enum {
    CONQ_MOVE_PEER = 0, // park the peer's mother on its landing tile
    CONQ_LAND_PEER,     // deploy it -- it becomes a BUILDING (the "enemy base")
    CONQ_BUILD1,        // academy
    CONQ_BUILD2,        // barracks
    CONQ_UNITS,         // create the attackers
    CONQ_GROUP,         // adopt them into a control group
    CONQ_ATTACK,        // terminal: attack until the enemy is gone
};
int      g_conq_phase       = CONQ_MOVE_PEER;
uint32_t g_conq_phase_step  = 0;          // step this phase was entered (0 = not yet entered)
bool     g_conq_issued      = false;      // has this phase issued its order yet
int      g_conq_last_energy = 0x7fffffff; // CONQ_ATTACK watchdog: target energy at the last check
int      g_conq_last_count  = 0x7fffffff; // ... and the enemy's live object count
bool     g_conq_armed       = false;
bool     g_conq_failed      = false;   // a guard tripped -> the run must NOT be read as a pass
unsigned g_conq_enemy       = 0xffffu; // resolved once at conq_at
int      g_conq_soldiers[16];          // roster indices of the units we created
int      g_conq_n = 0;

inline uint8_t conq_ctrl_flags(unsigned slot) {
    return *reinterpret_cast<const uint8_t *>(ADDR_PLAYERS_DESC + slot * PLAYER_DESC_STRIDE +
                                              PLAYER_DESC_FLAGS_OFF);
}

// The enemy: the first ENABLED, ALIVE slot that is not us. With Last Question's two lobby slots there
// is exactly one, but resolving it rather than assuming slot 1 is what lets this run on a 4-slot map
// later without quietly ordering an empty profile around.
unsigned conq_find_enemy(unsigned side) {
    const auto *prof = reinterpret_cast<const mh::game::mh_llm_strat_player_profile *>(ADDR_PLAYERS_PROF);
    for (unsigned i = 0; i < 8; ++i) {
        if (i == side) continue;
        const uint32_t f = prof[i].status_flags;
        if ((f & PS_ENABLED) && (f & PS_ALIVE)) return i;
    }
    return 0xffffu;
}

// The peer's first live building, or 0. Walked rather than remembered because the roster index the
// landed mother lands on is the peer roster's business, not ours.
int conq_find_enemy_bldg(unsigned enemy) {
    auto *const b = reinterpret_cast<mh::game::mh_map_object_building *>(mh::state::hash_base(g_idx_bldgs)) +
                    enemy * 100u;
    for (int i = 1; i < 100; ++i)
        if (b[i].building_id != 0 && b[i].state != 2 && b[i].energy > 0.0) return i;
    return 0;
}

// One census line. This is the instrument the whole item exists to build, so it prints exactly the
// numbers a FAILED run needs and not a fifth: who owns what, where our soldiers are, and how much
// energy the target has left. A run that ends with nobody dead and no CONQ lines is indistinguishable
// from one whose orders never left the building.
void conq_probe() {
    char           msg[256];
    const auto    *prof   = reinterpret_cast<const mh::game::mh_llm_strat_player_profile *>(ADDR_PLAYERS_PROF);
    const unsigned side   = *reinterpret_cast<const uint16_t *>(ADDR_SIDE);
    const unsigned planet = *reinterpret_cast<const uint32_t *>(ADDR_PLANET_IDX);
    if (side >= 8 || planet >= 32 || g_conq_enemy >= 8) return;
    const int   tb = conq_find_enemy_bldg(g_conq_enemy);
    auto *const eb = reinterpret_cast<mh::game::mh_map_object_building *>(mh::state::hash_base(g_idx_bldgs)) +
                     g_conq_enemy * 100u;
    wsprintfA(msg, "; CONQ step=%lu me=%u{u=%d b=%d} them=%u{u=%d b=%d} tgt=%d energy=%d motherb=%d\n",
              g_step, side, (int)prof[side].units_alive[planet], (int)prof[side].buildings_alive[planet],
              g_conq_enemy, (int)prof[g_conq_enemy].units_alive[planet],
              (int)prof[g_conq_enemy].buildings_alive[planet], tb, tb ? (int)eb[tb].energy : -1,
              (int)prof[g_conq_enemy].primary_mother_bldg[planet]);
    append_line(g_log_path, msg);
    // THE PEER'S MOTHERSHIP, by state. The 2026-08-29 bring-up failed with "the mother did NOT
    // convert" and the census could not say which of the three causes it was: the order never
    // reached the unit, deploy_approach's footprint check bounced it to IDLE_SCATTER, or the move
    // was zero-length (we aim at the unit's own landing tile) and never produced an arrival
    // transition at all. order/state/xy separates all three on sight.
    {
        auto *const pu =
            reinterpret_cast<mh::game::mh_map_object_unit *>(mh::state::hash_base(g_idx_units)) +
            g_conq_enemy * 100u;
        const int pm = prof[g_conq_enemy].primary_mother_unit[planet];
        if (pm > 0 && pm < 100)
            wsprintfA(msg, ";   peer-mother unit=%d alive=%d at=(%d,%d) order=%u state=%u goal=(%d,%d)\n",
                      pm, (int)(pu[pm].energy > 0.0), (int)pu[pm].x, (int)pu[pm].y,
                      (unsigned)pu[pm].order, (unsigned)pu[pm].state, (int)pu[pm].goal_x,
                      (int)pu[pm].goal_y);
        else
            wsprintfA(msg, ";   peer-mother primary_mother_unit=%d (0 = none/landed)\n", pm);
        append_line(g_log_path, msg);
    }
    // Soldier positions, so "they never arrived" and "they arrived and did nothing" stop looking the
    // same in the log. THIS is what pins the walk duration the schedule can only estimate: step_speed
    // is a game-seconds budget per movement tick and nothing measured says how many ticks make a tile,
    // so the estimate spans ~49 steps to ~1570 for the same 35 tiles.
    auto *const u = reinterpret_cast<mh::game::mh_map_object_unit *>(mh::state::hash_base(g_idx_units)) +
                    side * 100u;
    for (int i = 0; i < g_conq_n; ++i) {
        const int id = g_conq_soldiers[i];
        wsprintfA(msg, ";   s%d unit=%d alive=%d at=(%d,%d) order=%u state=%u\n", i, id,
                  (int)(u[id].energy > 0.0), (int)u[id].x, (int)u[id].y, (unsigned)u[id].order,
                  (unsigned)u[id].state);
        append_line(g_log_path, msg);
    }
}

void conq_issue() {
    char           msg[256];
    const unsigned side   = *reinterpret_cast<const uint16_t *>(ADDR_SIDE);
    const unsigned planet = *reinterpret_cast<const uint32_t *>(ADDR_PLANET_IDX);
    const uint32_t d      = g_step - (uint32_t)g_cfg.conq_at;

    // ---- step 0: resolve, and REFUSE to proceed on anything unresolved -------------------------
    // Every one of these is a condition under which the workload would issue orders that do nothing,
    // and a silently-no-op workload reads in the log exactly like a working one.
    if (d == 0) {
        if (side >= 8 || planet >= 32) {
            wsprintfA(msg, "; CONQ FAIL step=%lu: side=%u planet=%u out of range\n", g_step, side, planet);
            append_line(g_log_path, msg);
            g_conq_failed = true;
            return;
        }
        g_conq_enemy = conq_find_enemy(side);
        if (g_conq_enemy >= 8) {
            append_line(g_log_path, "; CONQ FAIL: no enabled+alive opponent slot -- nothing to eliminate\n");
            g_conq_failed = true;
            return;
        }
        // THE LOCKSTEP-LEGALITY ASSERTION, and it covers BOTH slots. If a flag is clear,
        // order_dispatch takes the IMMEDIATE lane for orders owned by that player and nothing we
        // issue for them is replicated -- the all_ai failure with extra steps. The PEER's flag
        // matters because conq_peer_land_at issues an order whose OWNER is the peer, and
        // order_dispatch keys the lane off the order's owner, not off who called it.
        const unsigned smode   = *reinterpret_cast<const uint8_t *>(ADDR_CONQ_SESSION_MODE);
        const uint8_t  cf_me   = conq_ctrl_flags(side);
        const uint8_t  cf_them = conq_ctrl_flags(g_conq_enemy);
        if (smode != CONQ_SESSION_LOCKSTEP || (cf_me & PD_NET_CONTROLLED) == 0 ||
            ((cf_them & PD_NET_CONTROLLED) == 0 && g_cfg.conq_peer_land_at > 0)) {
            wsprintfA(msg, "; CONQ FAIL step=%lu: session_mode=%u Players[%u].cflags=%02X"
                           " Players[%u].cflags=%02X -- an order would take the LOCAL lane and never"
                           " replicate (see conq_land_self_at for the fallback)\n",
                      g_step, smode, side, (unsigned)cf_me, g_conq_enemy, (unsigned)cf_them);
            append_line(g_log_path, msg);
            g_conq_failed = true;
            return;
        }
        const auto *prof = reinterpret_cast<const mh::game::mh_llm_strat_player_profile *>(ADDR_PLAYERS_PROF);
        wsprintfA(msg, "; CONQ armed step=%lu me=%u@(%d,%d) enemy=%u@(%d,%d) planet=%u seed=%d\n",
                  g_step, side, (int)prof[side].landing_x[planet], (int)prof[side].landing_y[planet],
                  g_conq_enemy, (int)prof[g_conq_enemy].landing_x[planet],
                  (int)prof[g_conq_enemy].landing_y[planet], planet, g_cfg.conq_seed);
        append_line(g_log_path, msg);
        return;
    }

    const auto *prof = reinterpret_cast<const mh::game::mh_llm_strat_player_profile *>(ADDR_PLAYERS_PROF);
    if (g_conq_enemy >= 8 || side >= 8 || planet >= 32) return;
    // THE LANDING SITE, not an absolute constant. Which of Last Question's two spots each peer gets is
    // chosen by llm_rand_below inside llm_strat_claim_landing_spot (the second claimer takes idx^1),
    // so a hardcoded target aims at our OWN base half the time -- a run that looks busy and can never
    // end.
    const int hx = prof[side].landing_x[planet], hy = prof[side].landing_y[planet];
    const int ex = prof[g_conq_enemy].landing_x[planet], ey = prof[g_conq_enemy].landing_y[planet];

    // ---- THE PHASE MACHINE -----------------------------------------------------------------------
    //
    // MILESTONES ARE KEYED OFF OBSERVED STATE, NOT off g_step, and that is a correction with a
    // measurement behind it. The first version fired each step at a fixed offset from conq_at. Two
    // runs at an IDENTICAL config, order streams recorded and diffed on first-sighting step, shared
    // 36 orders of which only 15 landed on the same step -- the academy at 155 vs 154, the five
    // creates at 214 vs 215. The workload ISSUES at a fixed step but the order EXECUTES ~35 steps
    // later, and that execution step moves by one between runs because
    // llm_strat_order_dispatch clamps exec_time to max(GAME_CLOCK + STEP_SIZE, LOCKSTEP_HORIZON) and
    // the live horizon depends on how far ahead the peers were when the client joined. Within a run
    // both peers agree (which is why these runs are determinism-clean); across runs they do not, and
    // a one-step shift at step 154 compounded into 250 steps of match length.
    // Waiting for the PREVIOUS phase's effect removes the compounding: every order is now issued
    // against a world that demonstrably contains its precondition, so a slow horizon delays the run
    // instead of desequencing it. It also kills a whole class of false failure -- the old +200 verify
    // reported "the mother did NOT convert" while the mother was still descending.
    //
    // EVERY PHASE HAS A TIMEOUT. A phase whose effect never arrives must fail LOUDLY and stop the
    // run; silently waiting forever reads in the log exactly like a scenario that is still working.
    const int  want    = g_cfg.conq_n_soldiers < 1 ? 1 : (g_cfg.conq_n_soldiers > 16 ? 16 : g_cfg.conq_n_soldiers);
    const int  my_b    = (int)prof[side].buildings_alive[planet];
    const int  their_b = (int)prof[g_conq_enemy].buildings_alive[planet];
    const bool landing = (g_cfg.conq_peer_land_at > 0 || g_cfg.conq_land_self_at > 0);

    if (g_conq_phase_step == 0) g_conq_phase_step = g_step;
    // THE TERMINAL PHASE MEASURES PROGRESS AS DAMAGE, NOT AS ADVANCEMENT. CONQ_ATTACK never advances
    // -- winning IS its exit -- so a watchdog counting steps-since-the-last-phase-change fires on
    // every successful run. Measured: two runs reached GAMEOVER while the log also said "phase 6 made
    // no progress in 900 steps". Reset the clock whenever the enemy's total object count or the
    // target's energy falls, so the timeout means what it should: a STALEMATE, nothing dying.
    if (g_conq_phase == CONQ_ATTACK) {
        const int   tb_now = conq_find_enemy_bldg(g_conq_enemy);
        auto *const eb_now =
            reinterpret_cast<mh::game::mh_map_object_building *>(mh::state::hash_base(g_idx_bldgs)) +
            g_conq_enemy * 100u;
        const int e_now = tb_now ? (int)eb_now[tb_now].energy : 0;
        const int n_now = (int)prof[g_conq_enemy].units_alive[planet] +
                          (int)prof[g_conq_enemy].buildings_alive[planet];
        if (e_now < g_conq_last_energy || n_now < g_conq_last_count) g_conq_phase_step = g_step;
        g_conq_last_energy = e_now;
        g_conq_last_count  = n_now;
    }
    const uint32_t in_phase = g_step - g_conq_phase_step;
    if ((int)in_phase > g_cfg.conq_phase_timeout) {
        wsprintfA(msg, "; CONQ FAIL step=%lu: phase %d made no progress in %d steps (me b=%d, them b=%d, "
                       "attackers=%d) -- its precondition never arrived\n",
                  g_step, g_conq_phase, g_cfg.conq_phase_timeout, my_b, their_b, g_conq_n);
        append_line(g_log_path, msg);
        g_conq_failed = true;
        return;
    }
    // Advance exactly one phase per step at most, and re-arm the issue flag when we do.
    const int phase_was = g_conq_phase;

    switch (g_conq_phase) {
        case CONQ_MOVE_PEER: {
            // Park the peer's mother on its own landing tile before asking it to deploy.
            // llm_strat_unit_order_move (0x00469fe6), NOT _move_auto (0x0046a56d): the latter's plate
            // calls it "a raw enqueue invoked directly from AI unit-state tick handlers", i.e. the
            // LOCAL lane, which would desync the way the _enqueue twins do.
            if (!landing || g_cfg.conq_peer_move_at == 0) {
                g_conq_phase = CONQ_LAND_PEER;
                break;
            }
            const int m = prof[g_conq_enemy].primary_mother_unit[planet];
            if (!g_conq_issued) {
                if (m == 0) {
                    wsprintfA(msg, "; CONQ FAIL step=%lu: peer %u has no primary_mother_unit on planet %u\n",
                              g_step, g_conq_enemy, planet);
                    append_line(g_log_path, msg);
                    g_conq_failed = true;
                    return;
                }
                mh::call::llm_strat_unit_order_move(g_conq_enemy, m, (uint32_t)ex, (uint32_t)ey, 0);
                wsprintfA(msg, "; CONQ step=%lu order 0x10 MOVE peer %u mother unit=%d -> (%d,%d)\n",
                          g_step, g_conq_enemy, m, ex, ey);
                append_line(g_log_path, msg);
                g_conq_issued = true;
            }
            // Arrived: the mother is standing on the tile we are about to deploy it onto.
            auto *const pu = reinterpret_cast<mh::game::mh_map_object_unit *>(mh::state::hash_base(g_idx_units)) +
                             g_conq_enemy * 100u;
            if (m != 0 && m < 100 && (int)pu[m].x == ex && (int)pu[m].y == ey) g_conq_phase = CONQ_LAND_PEER;
            break;
        }
        case CONQ_LAND_PEER: {
            if (!landing) {
                g_conq_phase = CONQ_BUILD1;
                break;
            }
            if (!g_conq_issued) {
                const int m = prof[g_conq_enemy].primary_mother_unit[planet];
                if (g_cfg.conq_peer_land_at > 0 && m != 0) {
                    mh::call::llm_strat_unit_order_move_confirmed_with_bump(g_conq_enemy, m, (uint32_t)ex,
                                                                            (uint32_t)ey);
                    wsprintfA(msg, "; CONQ step=%lu order 0x18 LAND peer %u mother unit=%d at (%d,%d)\n",
                              g_step, g_conq_enemy, m, ex, ey);
                    append_line(g_log_path, msg);
                } else if (g_cfg.conq_land_self_at > 0 && !g_cfg.conq) {
                    const int sm = prof[side].primary_mother_unit[planet];
                    if (sm != 0)
                        mh::call::llm_strat_unit_order_move_confirmed_with_bump(side, sm, (uint32_t)hx,
                                                                                (uint32_t)hy);
                }
                g_conq_issued = true;
            }
            // The deploy is done when the mother has become a BUILDING. deploy_to_building descends
            // one elevation step per tick before construct_finalize, so this is hundreds of steps.
            if (their_b >= 1) {
                wsprintfA(msg, "; CONQ step=%lu peer %u landed: units=%d buildings=%d motherb=%d\n", g_step,
                          g_conq_enemy, (int)prof[g_conq_enemy].units_alive[planet], their_b,
                          (int)prof[g_conq_enemy].primary_mother_bldg[planet]);
                append_line(g_log_path, msg);
                g_conq_phase = CONQ_BUILD1;
            }
            break;
        }
        case CONQ_BUILD1:
        case CONQ_BUILD2: {
            const bool first_b = (g_conq_phase == CONQ_BUILD1);
            const int  need    = first_b ? 1 : 2;
            if (!g_conq_issued) {
                const int t  = first_b ? g_cfg.conq_academy : g_cfg.conq_barracks;
                const int bx = first_b ? hx + 2 : hx - 3;
                mh::call::llm_strat_order_queue_construction_debug((uint32_t)bx, (uint32_t)(hy + 2),
                                                                   (uint32_t)t, side);
                wsprintfA(msg, "; CONQ step=%lu order 0xf2 %s Building[%d] at (%d,%d)\n", g_step,
                          first_b ? "academy" : "barracks", t, bx, hy + 2);
                append_line(g_log_path, msg);
                g_conq_issued = true;
            }
            if (my_b >= need) g_conq_phase = first_b ? CONQ_BUILD2 : CONQ_UNITS;
            break;
        }
        case CONQ_UNITS: {
            if (!g_conq_issued) {
                for (int i = 0; i < want; ++i)
                    mh::call::llm_strat_order_create_unit_debug((uint32_t)(hx + i - want / 2),
                                                                (uint32_t)(hy + 3),
                                                                (uint32_t)g_cfg.conq_soldier, side);
                wsprintfA(msg, "; CONQ step=%lu order 0xeb x%d attacker Unit[%d] near (%d,%d)\n", g_step,
                          want, g_cfg.conq_soldier, hx, hy + 3);
                append_line(g_log_path, msg);
                g_conq_issued = true;
            }
            // SCAN, not a remembered return value: the creates execute later, so nothing is on the
            // map when we issue them. The stored proto id is the cfg index, one LESS than what the
            // order takes -- accept either and say so if it is neither.
            auto *const u = reinterpret_cast<mh::game::mh_map_object_unit *>(mh::state::hash_base(g_idx_units)) +
                            side * 100u;
            g_conq_n = 0;
            for (unsigned k = 2; k < 100u && g_conq_n < 16; ++k) {
                if (u[k].energy <= 0.0) continue;
                const unsigned pid = (unsigned)u[k].unit_proto_id;
                if (pid == (unsigned)g_cfg.conq_soldier || pid == (unsigned)(g_cfg.conq_soldier - 1))
                    g_conq_soldiers[g_conq_n++] = (int)k;
            }
            if (g_conq_n >= want) g_conq_phase = CONQ_GROUP;
            break;
        }
        case CONQ_GROUP: {
            // "Combine into a squad" as a CONTROL GROUP. The squad merge the game offers is a unit
            // STATE (llm_strat_unit_state_squad_merge 0x0047ea62) that nothing calls, and it would
            // REDUCE the number of attackers, which the damage arithmetic makes the scarce resource.
            for (int i = 0; i < g_conq_n; ++i)
                mh::call::llm_strat_unit_ctrl_group_assign(g_conq_soldiers[i],
                                                           (int32_t)g_cfg.synth_ctrlgroup_id);
            mh::call::llm_unit_ctrl_group_activate((int32_t)g_cfg.synth_ctrlgroup_id);
            wsprintfA(msg, "; CONQ step=%lu grouped %d attacker(s) into ctrl group %d\n", g_step, g_conq_n,
                      g_cfg.synth_ctrlgroup_id);
            append_line(g_log_path, msg);
            g_conq_phase = CONQ_ATTACK;
            break;
        }
        case CONQ_ATTACK: {
            // The terminal phase: it never advances, and its "progress" is the enemy dying, so the
            // timeout above is what stops a stalemate. Re-issued on a cadence because an attack order
            // is abandoned once the unit's own state machine resettles it; a real player clicks again.
            const bool first_a = !g_conq_issued;
            const bool repeat  = g_cfg.conq_attack_every > 0 && in_phase > 0 &&
                                (in_phase % (uint32_t)g_cfg.conq_attack_every) == 0;
            if (!first_a && !repeat) break;
            g_conq_issued = true;
            if (g_conq_n <= 0) break;
            // MIRROR llm_strat_group_issue_attack_order (0x00444894), the gesture a PLAYER performs.
            // Its selector arms call DIFFERENT functions: 0x20 (unit) -> attack_target, 0x40
            // (building) -> attack_building_REPOSITION. The plain attack_building issues the attack
            // without the approach, so the units stand still -- measured, they sat around our own base
            // 43 tiles from the target with its energy frozen. It also selects the weapon rather than
            // assuming one and SKIPS any unit whose select_weapon answer is the sentinel 100.
            const int  tm      = prof[g_conq_enemy].primary_mother_unit[planet];
            const int  tb      = conq_find_enemy_bldg(g_conq_enemy);
            const bool at_bldg = (tb != 0);
            if (!g_cfg.conq_use_groupmove && (at_bldg || tm != 0)) {
                const uint32_t mask = at_bldg ? 1u : 2u;
                const int      tgt  = at_bldg ? tb : tm;
                int            sent = 0, skipped = 0;
                for (int i = 0; i < g_conq_n; ++i) {
                    uint32_t w = (uint32_t)g_cfg.conq_weapon;
                    if (g_cfg.conq_weapon < 0) {
                        w = mh::call::llm_strat_unit_select_weapon((uint16_t)side, g_conq_soldiers[i], mask);
                        if ((w & 0xffu) == 100u) { // retail's "nothing for that target"
                            ++skipped;
                            continue;
                        }
                        w &= 0xffu;
                    }
                    if (at_bldg)
                        mh::call::llm_strat_unit_order_attack_building_reposition(side, g_conq_soldiers[i],
                                                                                  g_conq_enemy, tgt, w);
                    else
                        mh::call::llm_strat_unit_order_attack_target(side, g_conq_soldiers[i], g_conq_enemy,
                                                                     tgt, w);
                    ++sent;
                }
                wsprintfA(msg, "; CONQ step=%lu order %s x%d -> enemy %s %d (mask=%u, %d skipped)\n", g_step,
                          at_bldg ? "0x1d/reposition" : "0x1e", sent,
                          at_bldg ? "building" : "MOTHER unit", tgt, mask, skipped);
            } else {
                mh::call::llm_unit_ctrl_group_activate((int32_t)g_cfg.synth_ctrlgroup_id);
                mh::call::llm_strat_group_issue_move_order((uint32_t)ex, (uint32_t)ey);
                wsprintfA(msg, "; CONQ step=%lu group move -> enemy base (%d,%d)\n", g_step, ex, ey);
            }
            append_line(g_log_path, msg);
            break;
        }
        default: break;
    }

    if (g_conq_phase != phase_was) {
        // A phase boundary re-arms the issue flag and restarts the timeout, and it is logged so the
        // run's shape is readable without correlating step numbers by hand.
        wsprintfA(msg, "; CONQ step=%lu phase %d -> %d (after %lu steps)\n", g_step, phase_was,
                  g_conq_phase, in_phase);
        append_line(g_log_path, msg);
        g_conq_issued     = false;
        g_conq_phase_step = g_step;
    }

    // ---- BRING-UP / FALLBACK: the guaranteed kill ----------------------------------------------
    // Order 0xf8 names its target explicitly and its handler reads nothing local, so it is as
    // replicated as any other order -- a cheat, not a poke, and that distinction is the one U32 (b)
    // turns on. Use it ONCE to show the end-condition machinery works end to end, then turn it off.
    if (g_cfg.conq_force_kill_at > 0 && d == (uint32_t)g_cfg.conq_force_kill_at) {
        auto *const eu = reinterpret_cast<mh::game::mh_map_object_unit *>(mh::state::hash_base(g_idx_units)) +
                         g_conq_enemy * 100u;
        auto *const eb = reinterpret_cast<mh::game::mh_map_object_building *>(mh::state::hash_base(g_idx_bldgs)) +
                         g_conq_enemy * 100u;
        int n = 0;
        for (int k = 1; k < 100; ++k)
            if (eu[k].energy > 0.0) {
                mh::call::llm_strat_order_debug_kill_group(g_conq_enemy | 0x80u, k);
                ++n;
            }
        for (int k = 1; k < 100; ++k)
            if (eb[k].building_id != 0 && eb[k].state != 2) {
                mh::call::llm_strat_order_debug_kill_group(g_conq_enemy | 0x40u, k);
                ++n;
            }
        wsprintfA(msg, "; CONQ step=%lu FORCE-KILL: order 0xf8 x%d against player %u\n", g_step, n,
                  g_conq_enemy);
        append_line(g_log_path, msg);
    }
}

// ---- the tactical roster, shared by the journal and the synthetic workload ----------------------
// Hoisted out of the TACT-SYNTH block on 2026-08-25: the order journal needs the same roster base
// and the same loop bound, and two copies of a roster bound is exactly the pair that drifts.
constexpr uintptr_t ADDR_TACT_UNITS = mh::addr::_G_LLM_TACT_UNITS;
constexpr int       TACT_UNIT_MAX   = 0x80; // llm_tact_frame's own roster loop bound (1 .. 0x80)
constexpr uint8_t   TACT_ANIM_DYING = 0x1f; // the dying animation -- not a live unit

inline mh::game::mh_tact_unit_record *tact_units() {
    return reinterpret_cast<mh::game::mh_tact_unit_record *>(ADDR_TACT_UNITS);
}

// ---- TACT-REC: the ORDER JOURNAL -----------------------------------------------------------------
//
// Record what the PLAYER ordered, keyed to the tactical frame, so a human session can be replayed
// headless. Orders and not input, decided before this was built: a journal of orders replays through
// the game's own command API and survives UI code changing underneath it, where an input journal
// would replay a UI that may drift.
//
// WHICH CALLS ARE THE PLAYER'S, and this is the whole correctness question -- both seams are also
// called by code that runs with NO input, and journalling those would issue every one of them TWICE
// on replay (once by the game, once by the journal). Measured from the EN xref set, 2026-08-25:
//
//   llm_tact_unit_enqueue_command   22 call sites
//     PLAYER  6  llm_tact_frame                          0x429f0e 42a403 42a571 42a58a 42a5b8 42a5d1
//     PLAYER  1  llm_tact_ui_sel_panel_multi_mode_tick    0x436722
//     nested  3  llm_tact_group_issue_order               -- journalled at the GROUP seam instead,
//                                                            or the same order lands twice
//     engine  9  llm_tact_unit_owner_tick                 -- the AI/idle arm
//     engine  1  llm_tact_mission_load                    -- the mission script
//     engine  1  llm_tact_unit_cmd_queue_resubmit_run     -- the REPEAT resubmit
//     engine  1  llm_tact_fx_update_projectile            -- the facing correction
//
//   llm_tact_group_issue_order      13 call sites, and EVERY ONE is a UI function:
//     PLAYER  6  llm_tact_frame
//     PLAYER  7  llm_tact_ui_order_buttons_minimap_tick
//
// THE FILTER IS THE CALLER'S EXTENT, not a list of return addresses. Both are derivable, and the
// extent survives an instruction changing length; a `call+5` table does not. It also happens to be
// one test for both seams, because every player site in both lists is inside one of these two
// ranges and no engine site is. Extents read out of Ghidra (function body max + 1):
// The site rule lives in tact/tact_journal.h so `tacttest` can assert it offline -- that header
// carries the measured xref set behind it. Aliased so the code below reads unchanged.
using mh::tact::tj_is_player;

// D5: the two seam ADDRESSES moved to the hook-point table (point::tact_order_enqueue /
// point::tact_group_order), which is also where the note that made them literals now lives -- they
// are not in mh_addrs.gen.h because they reach C++ through mh_calls.gen.h, and only the detours
// ever needed the raw entry.

void *g_tj_enq_tramp = nullptr;
// SIM1-P clause 6b: the recorder's REBIND TARGET, the fourth of its kind (g_sim_step_promoted,
// g_tact_frame_promoted, g_order_dispatch_promoted are the others). Null = fall through to the
// original, which is right when nothing is promoted; non-null = the generated entry thunk for our
// enqueue body, so the recorder observes AND our code runs. See tj_enqueue_detour's tail.
void *g_tj_enq_promoted = nullptr;
void *g_tj_grp_tramp    = nullptr;

// Captured by the naked thunks before anything else runs. Globals rather than parameters for the
// reason pinned_rand documents: _ReturnAddress() inside the C++ handler names the thunk, not the
// game, and reports one plausible-looking site for every call.
uint32_t g_tj_pc = 0, g_tj_a = 0, g_tj_b = 0, g_tj_c = 0, g_tj_d = 0;
uint32_t g_tj_s1 = 0, g_tj_s2 = 0, g_tj_s3 = 0;

uint32_t g_tj_recorded = 0; // player-caused calls journalled
uint32_t g_tj_facing   = 0; // op-6 cursor-facing echoes dropped (see tj_note)
uint32_t g_tj_engine   = 0; // calls filtered out as input-independent

// The probe's buckets -- same shape as the rand site table, and there for the same reason.
constexpr int TJ_SITES                   = 40;
uint32_t      g_tj_site_pc[TJ_SITES]     = {0};
uint32_t      g_tj_site_cnt[TJ_SITES]    = {0};
uint8_t       g_tj_site_player[TJ_SITES] = {0};

void tj_bucket(uint32_t pc, bool player) {
    for (int i = 0; i < TJ_SITES; ++i) {
        if (g_tj_site_pc[i] == pc) {
            ++g_tj_site_cnt[i];
            return;
        }
        if (g_tj_site_pc[i] == 0) {
            g_tj_site_pc[i]     = pc;
            g_tj_site_cnt[i]    = 1;
            g_tj_site_player[i] = player ? 1u : 0u;
            return;
        }
    }
}

// ---- the replay side ----------------------------------------------------------------------------
//
// One entry per recorded order. `seam` is 'E' (unit enqueue) or 'G' (group order); the argument
// slots mean different things per seam and are stored raw, exactly as observed, so the replay is a
// re-issue and not a re-interpretation.
struct tj_entry {
    uint32_t frame;
    uint8_t  seam;
    uint8_t  iflag;
    uint32_t a0, a1, a2, a3, a4;
    // a5/a6/a7 exist for the mouse record only. It carries eight fields where an order carries
    // five, and the first cut folded dx into `iflag` -- an unsigned BYTE, so every leftward or
    // upward move (dx/dy negative) replayed as a large positive delta, and dy and the wheel were
    // dropped entirely. Widening is 8 bytes per entry; guessing at which deltas matter is not.
    uint32_t a5, a6, a7;
};
// Sized for a human session's INPUT, not its orders. A measured 4.4-minute recording is
// 14,317 records (9,311 mouse events + 4,633 cursor moves + 162 keys + 211 orders/writes) --
// the old 4096 was an ORDER budget and would have dropped two thirds of that on the floor.
//
// RAISED 64Ki -> 256Ki 2026-09-07, because a WHOLE MISSION is the recording we actually want and
// 64Ki does not hold one. The rate above is ~3,250 records/minute, so 64Ki was a ~20-minute ceiling
// and the banked menu-to-landing session already spent 36,068 of it; a run that carries on to an
// eliminated enemy and a planet transition goes past it mid-mission. RECORDING is not what was
// capped -- the recorder appends to a file and never overflows -- so the loss lands entirely on the
// REPLAY side, i.e. on the one journal the whole rung depends on, after it is too late to re-play it.
// 256Ki * 32B = 8 MB, allocated once, only when a journal is armed, and worth it for ~80 minutes.
constexpr int TJ_MAX  = 262144;
tj_entry     *g_tj_ev = nullptr;
// Per-record target SIM STEP, interpolated between the `TJ P` barriers at load (see the block in
// tj_replay_load). 0 = "no anchor, use the present schedule" -- the menu half, before the sim runs.
uint32_t *g_tj_step_due = nullptr;
// Index of the LAST `P` barrier, i.e. the recording's final sim step. Records after it are
// the tail the sim never reaches again -- see the note beside its assignment in tj_load.
int      g_tj_last_p = -1;
int      g_tj_n = 0, g_tj_next = 0;
uint32_t g_tj_issued  = 0;
uint32_t g_tj_skipped = 0; // derived records deliberately NOT injected
// BARRIERS CONSUMED (`S`/`P`). They are records in the file and they are NOT input, so counting
// them against g_tj_issued reports a COMPLETED replay as SHORT: spcamp-solo holds 3,392 `P` plus
// 15 `S`, and a run that issued every one of its 33,976 input records was told it "ended before
// the journal did". A false alarm in the gate's own evidence is worse than no line at all -- the
// next reader either chases it or learns to skip the whole report.
uint32_t g_tj_barriers  = 0;     // `S`/`P` records consumed -- not input, not skipped
bool     g_tj_done      = false; // the journal ran out; the report already fired
bool     g_tj_has_input = false; // the journal carries M/K/C, so the orders in it are DERIVED


// ---- the observers ------------------------------------------------------------------------------
//
// Called from the naked thunks with the seam's arguments already stashed. They log and return; the
// thunk then jumps into the trampoline so the ORIGINAL runs untouched. Nothing here changes the
// game's behaviour -- a recorded run and an unrecorded one differ only in log volume, which is why
// the recording can be trusted as the thing the replay must reproduce.
void tj_note(uint8_t seam) {
    const bool player = tj_is_player(g_tj_pc);
    if (g_cfg.tact_journal_probe) tj_bucket(g_tj_pc, player);
    if (!player) {
        ++g_tj_engine;
        return;
    }
    if (!g_cfg.tact_journal_rec) return;
    // OP 6 IS NOT AN ORDER, and the probe is what proved it. It is the CONTINUOUS CURSOR-FACING
    // echo: llm_tact_frame re-issues it for every selected unit every frame, so a 400-frame run with
    // no player at all journalled 265 of them (measured 2026-08-25, all op 6, from the single site
    // 0x0042a408). Three reasons it is dropped rather than kept:
    //   * VOLUME -- one per selected unit per frame is ~100x the rest of a journal put together,
    //     and a human session is tens of thousands of frames.
    //   * IT IS INPUT, NOT AN ORDER, and orders-not-input was the decision this journal is built on.
    //     Its argument is the cursor's current bearing; nothing discrete happened.
    //   * IT WOULD NOT WORK ANYWAY. op 6 short-circuits to the immediate FACE record rather than the
    //     queue, and the game re-issues it mid-frame AFTER the journal injects at the frame top --
    //     so the replay's own cursor wins and the journalled value is overwritten.
    // THE CONSEQUENCE IS REAL AND IS NOT HIDDEN: a replay's unit facing follows the REPLAY's cursor,
    // not the recording's. Making it faithful needs the replay to SUPPRESS the game's own call at
    // that site so the journalled value is authoritative -- which is a detour that returns without
    // calling through, and is the next piece of work rather than something quietly assumed here.
    if (seam == 'E' && (g_tj_b & 0xffffu) == 6u) {
        ++g_tj_facing;
        return;
    }
    ++g_tj_recorded;
    char l[160];
    if (seam == 'E')
        wsprintfA(l, "TJ E %lu %lu %lu %lu %lu %lu %lu %lu\n", g_tact_step, g_tj_a, g_tj_b,
                  g_tj_c & 0xffu, g_tj_d, g_tj_s1 & 0xffffu, g_tj_s2 & 0xffffu, g_tj_s3 & 0xffffu);
    else
        wsprintfA(l, "TJ G %lu %lu %lu %lu %lu %lu\n", g_tact_step, g_tj_a, g_tj_b, g_tj_c,
                  g_tj_d, g_tj_s1);
    append_line(g_log_path, l);
}

extern "C" void __cdecl tj_note_enqueue() { tj_note('E'); }
extern "C" void __cdecl tj_note_group() { tj_note('G'); }

// The thunks. Register arguments are stashed BEFORE anything is pushed -- MOV to memory does not
// touch EFLAGS, so the PUSHFD below still saves the flags the original body expects. After
// PUSHAD+PUSHFD the original ESP is at ESP+36, so the caller's return address is [ESP+36] and the
// stack arguments follow it.
//
//   enqueue  __watcall                      EAX unit, EDX op, BL iflag, ECX arg0, +4/+8/+12 arg1..3
//   group    __mh_watcall_ecx_ebx_volatile  EAX op,   EDX arg0, EBX arg1, ECX arg2, +4 arg3
__declspec(naked) void tj_enqueue_detour() {
    __asm {
        mov  g_tj_a, eax
        mov  g_tj_b, edx
        mov  g_tj_c, ebx
        mov  g_tj_d, ecx
        pushad
        pushfd
        mov  eax, [esp+36]
        mov  g_tj_pc, eax
        mov  eax, [esp+40]
        mov  g_tj_s1, eax
        mov  eax, [esp+44]
        mov  g_tj_s2, eax
        mov  eax, [esp+48]
        mov  g_tj_s3, eax
        call tj_note_enqueue
        popfd
        popad
                                                                                                                                                                                             // SIM1-P clause 6b (2026-09-05): fall through to OUR body when the tactical promotion asked
                                                                                                                                                                                             // for it, exactly as sim_step_detour and tact_frame_detour do. Before this the recorder's
                                                                                                                                                                                             // fall-through went unconditionally to the ORIGINAL, and because the recorder claims this
                                                                                                                                                                                             // entry EXCLUSIVELY the derived yield disarmed llm_tact_unit_enqueue_command's rebind row --
                                                                                                                                                                                             // so arming the journal recorder silently un-promoted the body it was recording. Measured:
                                                                                                                                                                                             // tact_unit_enqueue_command.cpp fell 80 -> 46 covered lines in poz1-combat and in all four
                                                                                                                                                                                             // journal scenarios, and tact_journal.h rose 25 -> 28 of 28 because the recorder became the
                                                                                                                                                                                             // live path there. That is the shape clause 6b forbids: an instrument may not cost us a
                                                                                                                                                                                             // promotion. The CMP-after-POPFD is safe for the same reason it is safe in sim_step_detour --
                                                                                                                                                                                             // this is a FUNCTION ENTRY and neither the Watcom prologue nor our entry thunk reads incoming
                                                                                                                                                                                             // flags. Tail jump, so the thunk returns straight to the caller.
        cmp  dword ptr [g_tj_enq_promoted], 0
        jne  tj_enqueue_promoted
        jmp  dword ptr [g_tj_enq_tramp] // not promoted: stolen prologue + jmp back to the original
    tj_enqueue_promoted:
        jmp  dword ptr [g_tj_enq_promoted] // the generated entry thunk -> our enqueue body
    }
}

__declspec(naked) void tj_group_detour() {
    __asm {
        mov  g_tj_a, eax
        mov  g_tj_b, edx
        mov  g_tj_c, ebx
        mov  g_tj_d, ecx
        pushad
        pushfd
        mov  eax, [esp+36]
        mov  g_tj_pc, eax
        mov  eax, [esp+40]
        mov  g_tj_s1, eax
        call tj_note_group
        popfd
        popad
        jmp  dword ptr [g_tj_grp_tramp]
    }
}

// ---- the journal file ---------------------------------------------------------------------------
//
// One line per order, the same shape the recorder logs, so the file a human commits is the file the
// DLL reads and a diff between them is meaningful. Parsed by hand rather than with sscanf: this file
// takes no CRT dependency it does not already have, and the grammar is eight unsigned integers.
//
//   E <frame> <unit> <op> <iflag> <arg0> <arg1> <arg2> <arg3>
//   G <frame> <op> <arg0> <arg1> <arg2> <arg3>
//
// Anything else -- blank lines, `#` comments, the `;` banner Python writes -- is skipped, so the
// journal can carry its own provenance header without a second format.

void tj_load(const char *path) {
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        char m[400];
        wsprintfA(m, "; TJ REPLAY NOT ARMED: cannot open %s\n", path);
        append_line(g_log_path, m);
        return;
    }
    const DWORD sz  = GetFileSize(h, nullptr);
    char       *buf = (char *)VirtualAlloc(nullptr, sz + 1, MEM_COMMIT, PAGE_READWRITE);
    DWORD       got = 0;
    if (!buf || !ReadFile(h, buf, sz, &got, nullptr)) {
        CloseHandle(h);
        append_line(g_log_path, "; TJ REPLAY NOT ARMED: read failed\n");
        return;
    }
    CloseHandle(h);
    g_tj_ev = (tj_entry *)VirtualAlloc(nullptr, sizeof(tj_entry) * TJ_MAX, MEM_COMMIT,
                                       PAGE_READWRITE);
    if (!g_tj_ev) return;
    const char *p = buf, *end = buf + got;
    uint32_t    dropped = 0;
    while (p < end) {
        const char *eol = p;
        while (eol < end && *eol != '\n') ++eol;
        const char  kind = *p;
        const char *q    = p + 1;
        // 'P' BELONGS IN THIS LIST AND WAS MISSING UNTIL 2026-09-07. Everything else about the step
        // barrier was already here -- the `need` arm below, the store arm, and the consume case in
        // the injector -- so the recorder wrote `TJ P` records, the file carried them, and the loader
        // dropped every one on the floor without even counting it (the overflow branch below omitted
        // 'P' too). The failure mode is the worst available: the mechanism looks present at both ends
        // and does nothing in the middle, so the fix for rung 10000 would have measured as "the step
        // barriers did not help" on a freshly recorded journal.
        if ((kind == 'E' || kind == 'G' || kind == 'D' || kind == 'M' || kind == 'K' ||
             kind == 'C' || kind == 'S' || kind == 'P') &&
            g_tj_n < TJ_MAX) {
            tj_entry e{};
            e.seam          = (uint8_t)kind;
            const int need  = (kind == 'E')   ? 8
                              : (kind == 'D') ? 4
                              : (kind == 'M') ? 9
                              : (kind == 'K') ? 4
                              : (kind == 'C') ? 3
                              : (kind == 'S') ? 6
                              : (kind == 'P') ? 2 // idx + sim step
                                              : 6;
            uint32_t  v[9]  = {0};
            int       got_n = 0;
            for (; got_n < need; ++got_n) {
                q = mh::tact::tj_scan_u32(q, eol, &v[got_n]);
                if (!q) break;
            }
            // 'S' HAS GAINED A FIELD TWICE AND OLD JOURNALS MUST STILL LOAD. A recorded session is a
            // human's time -- the first real one was 25,709 records of it -- so a format addition that
            // silently rejected every existing journal would be the wrong trade. Three vintages now:
            //   4 fields  pre-signature      -> replayed on the container VA alone (weakest)
            //   5 fields  geometry signature -> replayed on that (better, but see below)
            //   6 fields  + CONTENT id       -> replayed on that (MH_UIDrive_ScreenId)
            // The geometry signature is not merely weaker than the content id, it is WRONG on any
            // screen that animates: it hashes resolved centers, so a screen with one moving child
            // hashes differently every present and the recorded number names an instant. Journals of
            // vintage 5 keep replaying exactly as they did; they cannot be upgraded in place, because
            // the id they lack was never recorded.
            const bool legacy_s = kind == 'S' && (got_n == 4 || got_n == 5);
            if (got_n == need || legacy_s) {
                e.frame = v[0];
                if (kind == 'E') {
                    e.a0                  = v[1];
                    e.a1                  = v[2];
                    e.iflag               = (uint8_t)v[3];
                    e.a2                  = v[4];
                    e.a3                  = v[5];
                    e.a4                  = v[6];
                    g_tj_ev[g_tj_n].frame = e.frame;
                    g_tj_ev[g_tj_n].seam  = e.seam;
                    g_tj_ev[g_tj_n].iflag = e.iflag;
                    g_tj_ev[g_tj_n].a0    = v[1];
                    g_tj_ev[g_tj_n].a1    = v[2];
                    g_tj_ev[g_tj_n].a2    = v[4];
                    g_tj_ev[g_tj_n].a3    = v[5];
                    g_tj_ev[g_tj_n].a4    = v[6];
                    // v[7] is arg3; stash it in the spare slot the group form does not use.
                    g_tj_ev[g_tj_n].a4 = (v[6] & 0xffffu) | ((v[7] & 0xffffu) << 16);
                } else if (kind == 'M') {
                    // M <frame> <type> <buttons> <x> <y> <dx> <dy> <wheel> <ts>
                    g_tj_ev[g_tj_n].frame = e.frame;
                    g_tj_ev[g_tj_n].seam  = e.seam;
                    g_tj_ev[g_tj_n].a0    = v[1]; // event_type
                    g_tj_ev[g_tj_n].a1    = v[2]; // buttons
                    g_tj_ev[g_tj_n].a2    = v[3]; // x
                    g_tj_ev[g_tj_n].a3    = v[4]; // y
                    g_tj_ev[g_tj_n].a5    = v[5]; // dx -- signed, printed as %ld, re-read as u32
                    g_tj_ev[g_tj_n].a6    = v[6]; // dy
                    g_tj_ev[g_tj_n].a7    = v[7]; // wheel_delta
                    g_tj_ev[g_tj_n].a4    = v[8]; // timestamp, replayed VERBATIM
                } else if (kind == 'K') {
                    g_tj_ev[g_tj_n].frame = e.frame;
                    g_tj_ev[g_tj_n].seam  = e.seam;
                    g_tj_ev[g_tj_n].a0    = v[1];
                    g_tj_ev[g_tj_n].a1    = v[2];
                    g_tj_ev[g_tj_n].a2    = v[3];
                } else if (kind == 'C') {
                    g_tj_ev[g_tj_n].frame = e.frame;
                    g_tj_ev[g_tj_n].seam  = e.seam;
                    g_tj_ev[g_tj_n].a0    = v[1];
                    g_tj_ev[g_tj_n].a1    = v[2];
                } else if (kind == 'S') {
                    // UI-REC: a SCREEN BARRIER, not an event. See the replay loop for what it does.
                    g_tj_ev[g_tj_n].frame = e.frame;
                    g_tj_ev[g_tj_n].seam  = e.seam;
                    g_tj_ev[g_tj_n].a0    = v[1]; // the active widget-list VA at record time
                    g_tj_ev[g_tj_n].a1    = v[2]; // pinned clock at record time, microseconds hi
                    g_tj_ev[g_tj_n].a2    = v[3]; // ... and lo
                    // The identity a replay matches on, best first. a4 = the CONTENT id, a3 = the
                    // geometry signature, and 0 in both means a pre-signature journal whose barrier
                    // falls back to the container VA (weakest -- every screen with no dialog open
                    // shares one value).
                    g_tj_ev[g_tj_n].a3 = (got_n >= 5) ? v[4] : 0u;
                    g_tj_ev[g_tj_n].a4 = (got_n >= 6) ? v[5] : 0u;
                } else if (kind == 'P') {
                    g_tj_ev[g_tj_n].frame = e.frame;
                    g_tj_ev[g_tj_n].seam  = e.seam;
                    g_tj_ev[g_tj_n].a0    = v[1]; // the sim step the recording was on
                } else if (kind == 'D') {
                    g_tj_ev[g_tj_n].frame = e.frame;
                    g_tj_ev[g_tj_n].seam  = e.seam;
                    g_tj_ev[g_tj_n].a0    = v[1]; // unit
                    g_tj_ev[g_tj_n].a1    = v[2]; // field id
                    g_tj_ev[g_tj_n].a2    = v[3]; // value
                } else {
                    g_tj_ev[g_tj_n].frame = e.frame;
                    g_tj_ev[g_tj_n].seam  = e.seam;
                    g_tj_ev[g_tj_n].a0    = v[1];
                    g_tj_ev[g_tj_n].a1    = v[2];
                    g_tj_ev[g_tj_n].a2    = v[3];
                    g_tj_ev[g_tj_n].a3    = v[4];
                    g_tj_ev[g_tj_n].a4    = v[5];
                }
                ++g_tj_n;
            } else {
                ++dropped;
            }
        } else if (kind == 'E' || kind == 'G' || kind == 'D' || kind == 'M' || kind == 'K' ||
                   kind == 'C' || kind == 'S' || kind == 'P') {
            ++dropped; // past TJ_MAX
        }
        p = (eol < end) ? eol + 1 : end;
    }
    // ---- STEP-SCHEDULE THE IN-GAME INPUT, by interpolating between the step barriers -------------
    //
    // A `TJ P` barrier fixes the queue at every TJ_SYNC_STEPS steps; BETWEEN two of them the records
    // were still due on the replay's own PRESENT count, and that is the residual cadence bug. Measured
    // on spcamp-mission at the 10000 rung: `order_queue` and `order_queue_count` diverged at step 96
    // and again at 104 -- 8 apart, the barrier cadence -- and the divergence was a PHASE SHIFT, not a
    // different value (the original arm's step 97 hash equalled the ship arm's step 96). One arm
    // enqueued a click one sim step before the other because the two ran at different frame rates
    // between the same pair of barriers.
    //
    // WHAT MAKES THIS DERIVABLE FROM AN ALREADY-RECORDED JOURNAL: in game the recorder runs 1 present
    // to 1 sim step (spcamp-mission: 2,553 of 2,877 barrier intervals are exactly (8 presents, 8
    // steps), and every common pair is (n,n)). So a record's present index IS a step index, and the
    // barriers either side of it give the exact anchors. Linear interpolation between them is exact
    // in the 1:1 case and stays correct where it is not -- ACROSS A PAUSE ESPECIALLY: through the
    // briefing and the planet transition presents advance while the sim does not, so `ns == bs` and
    // every record in that interval maps to the same step and is due at once, which is what the
    // recording did. A ratio assumption would have mis-scheduled exactly that stretch by 104 steps.
    //
    // Records BEFORE the first barrier are the whole menu walk, where there is no sim to schedule
    // against; they keep the present schedule, marked by step_due 0. Records after the last barrier
    // extrapolate 1:1, which is the measured ratio.
    g_tj_step_due = (uint32_t *)VirtualAlloc(nullptr, sizeof(uint32_t) * TJ_MAX, MEM_COMMIT,
                                             PAGE_READWRITE);
    if (g_tj_step_due) {
        int prev = -1; // index of the barrier on the left
        for (int i = 0; i < g_tj_n; ++i) {
            if (g_tj_ev[i].seam != 'P') continue;
            if (prev >= 0) {
                const int64_t bp = g_tj_ev[prev].frame, bs = g_tj_ev[prev].a0;
                const int64_t np = g_tj_ev[i].frame, ns = g_tj_ev[i].a0;
                const int64_t dp = np - bp;
                for (int j = prev + 1; j < i; ++j) {
                    const int64_t f  = g_tj_ev[j].frame;
                    const int64_t s  = (dp > 0) ? bs + ((f - bp) * (ns - bs)) / dp : ns;
                    g_tj_step_due[j] = (uint32_t)(s < bs ? bs : (s > ns ? ns : s));
                }
            }
            g_tj_step_due[i] = g_tj_ev[i].a0; // a barrier is its own anchor
            prev             = i;
        }
        if (prev >= 0) { // past the last barrier: the measured 1:1 ratio
            const int64_t bp = g_tj_ev[prev].frame, bs = g_tj_ev[prev].a0;
            for (int j = prev + 1; j < g_tj_n; ++j)
                g_tj_step_due[j] = (uint32_t)(bs + ((int64_t)g_tj_ev[j].frame - bp));
        }
        // ---- AND THE TAIL HAS NO SIM TO ANCHOR TO, WHICH THE EXTRAPOLATION ABOVE ASSUMES IT DOES --
        //
        // The ratio is right; the assumption that the steps it predicts will ever HAPPEN is not. A
        // `P` barrier is only emitted while the sim is running, so the last one names the recording's
        // FINAL step by construction -- and a session that ends by opening the menu (this fixture
        // does; the user stopped the sim that way) leaves 243 present-indexed records behind it whose
        // extrapolated `step_due` is past that final step. `g_step + 1 >= step_due` can then never
        // become true and the queue deadlocks with the journal unexhausted.
        //
        // MEASURED, and only because the stuck-head report was added to ask: record 37140 of 37383,
        // seam=M frame=30316, extrapolated to step 30050, while the sim was frozen at 30042 in
        // game_mode 3. The run spent 98.4% of its wall time (1.79M frames) presenting that menu until
        // the runner killed it -- and BOTH stall hatches stayed silent, because neither a `P` nor an
        // `S` barrier was at the head; an ordinary mouse record was.
        //
        // This is the same rule the leading menu walk already gets, and the comment above states it:
        // "there is no sim to schedule against; they keep the present schedule". The tail is that
        // situation at the other end. It is NOT enough to zero step_due here, though -- a recording
        // that ends while the sim is still running wants the step schedule's one-step drain lead for
        // its last records. So the schedule is kept and the PRESENT gate is added as an ALTERNATIVE
        // for the tail only (see tj_due): in-sim the step gate still fires first and nothing changes;
        // once the sim has stopped for good, presents release the tail instead of nothing doing so.
        g_tj_last_p = prev;
    }
    for (int i = 0; i < g_tj_n; ++i) {
        const uint8_t k = g_tj_ev[i].seam;
        if (k == 'M' || k == 'K' || k == 'C') {
            g_tj_has_input = true;
            break;
        }
    }
    char m[300];
    if (g_tj_has_input)
        append_line(g_log_path,
                    "; TJ REPLAY: journal carries INPUT -- its orders and direct writes are"
                    " DERIVED and will NOT be injected (they are the expectation)\n");
    wsprintfA(m, "; TJ REPLAY ARMED: %d order(s) from %s%s\n", g_tj_n, path,
              dropped ? " (MALFORMED/OVERFLOW LINES DROPPED -- the replay is INCOMPLETE)" : "");
    append_line(g_log_path, m);
    if (dropped) {
        wsprintfA(m, "; TJ REPLAY DROPPED %lu line(s)\n", dropped);
        append_line(g_log_path, m);
    }
}


// ---- the three player actions that are NOT orders -----------------------------------------------
//
// An order journal alone is LOSSY, and this is the part that closes the gap. Three things a player
// does have no opcode at all -- the UI writes the unit record directly, so nothing passes through
// either seam:
//
//   active_gun     +0x5eb  the gun swap                (`active_gun ^= 1`)
//   squad_group_id +0x5f0  control-group assign/reset  (sidebar icon, or 0xff to clear)
//   def_stat       +0x006  the sidebar def_stat cycle  (its 3->4 step ALSO enqueues, and that half
//                          is journalled at the seam -- this is the other half)
//
// WHY A DELTA WATCH RATHER THAN MORE DETOURS. The writes are scattered across several UI functions
// (llm_tact_ui_sel_panel_multi_mode_tick at 0x436336, llm_tact_sidebar_dispatch at 0x435adc and
// 0x435b01, and the gun swap), and a detour per write site is a list that goes stale the moment one
// moves. Watching the FIELDS instead is indifferent to where the write came from, which is the
// property that matters for a journal that has to survive the code changing.
//
// OWNER 0 ONLY. These are the player's own units, and the restriction is what keeps an engine-side
// write to some other side's record out of the journal -- a value we would then re-apply on replay
// on top of the engine's own write.
//
// THE ONE-FRAME QUESTION, stated because it is the design's only real assumption. The write happens
// mid-frame N inside the UI phase; the watch sees it at the TOP of frame N+1 and records it there,
// and the replay applies it at the top of frame N+1 too. So at every frame boundary -- which is
// where the hash is taken -- the recording and the replay agree. What differs is the intra-frame
// effect: in the recording the rest of frame N ran with the new value, in the replay it did not.
// That is NOT assumed to be harmless; it is exactly what the replay-vs-recording TS comparison
// measures, and a systematic one-frame skew shows up there as a divergence rather than as silence.
// Offsets and field ids come from tact/tact_journal.h, so the watch and the offline test cannot
// disagree about which byte is which.
constexpr int      TJD_F_COUNT           = mh::tact::TJ_F_COUNT;
constexpr uint32_t TJD_OFF[TJD_F_COUNT]  = {mh::tact::TJ_DIRECT_OFF[0], mh::tact::TJ_DIRECT_OFF[1],
                                            mh::tact::TJ_DIRECT_OFF[2], mh::tact::TJ_DIRECT_OFF[3]};
constexpr uint8_t  TJD_MASK[TJD_F_COUNT] = {mh::tact::TJ_DIRECT_MASK[0], mh::tact::TJ_DIRECT_MASK[1],
                                            mh::tact::TJ_DIRECT_MASK[2], mh::tact::TJ_DIRECT_MASK[3]};

uint8_t  g_tjd_shadow[TACT_UNIT_MAX + 1][TJD_F_COUNT] = {};
uint8_t  g_tjd_primed                                 = 0;
uint32_t g_tjd_recorded                               = 0;

// Watch the three fields and journal any change on an owner-0 unit. Returns nothing; the shadow is
// refreshed either way, so a run that is not recording still leaves the watch consistent if
// recording is turned on later in the same process (it cannot be, today -- but a shadow that is
// only correct in one mode is a trap waiting for the next knob).
void tj_watch_direct() {
    auto *u = tact_units();
    for (int i = 1; i <= TACT_UNIT_MAX; ++i) {
        const uint8_t *rec = reinterpret_cast<const uint8_t *>(&u[i]);
        const bool     own = rec[1] == 0 && rec[0] != 0; // owner 0, occupied slot
        for (int f = 0; f < TJD_F_COUNT; ++f) {
            const uint8_t now = (uint8_t)(rec[TJD_OFF[f]] & TJD_MASK[f]);
            if (g_tjd_primed && own && now != g_tjd_shadow[i][f] && g_cfg.tact_journal_rec) {
                char l[128];
                wsprintfA(l, "TJ D %lu %d %d %u\n", g_tact_step, i, f, (unsigned)now);
                append_line(g_log_path, l);
                ++g_tjd_recorded;
            }
            g_tjd_shadow[i][f] = now;
        }
    }
    g_tjd_primed = 1;
}


// ---- the ORDER watch: the same trick, applied to the command queue ------------------------------
//
// The rationale, the three limits and the geometry all live in tact/tact_journal.h so they can be
// asserted offline; this is only the loop. In one line: E/G are trampolines on the ORIGINAL order
// entries and therefore go blind the moment our own bodies call their own siblings, so the order
// stream is watched as STATE instead -- which is what makes it survive any promotion or rebind.
constexpr int TJQ_TOTAL = mh::tact::TJ_Q_TOTAL;
constexpr int TJQ_W     = (int)mh::tact::TJ_Q_STRIDE;

uint8_t  g_tjq_shadow[TACT_UNIT_MAX + 1][TJQ_TOTAL][TJQ_W] = {};
uint32_t g_tjq_recorded                                    = 0;

// The layout the watch reads has to be the layout the rest of the DLL compiles against, or the
// journal would describe a record shape nothing else believes in.
static_assert(offsetof(mh::game::mh_tact_unit_record, cmd_queue) == mh::tact::TJ_Q_BASE,
              "TJ_Q_BASE drift vs the generated unit record");
static_assert(sizeof(mh::game::mh_llm_tact_unit_cmd_entry) == mh::tact::TJ_Q_STRIDE,
              "TJ_Q_STRIDE drift vs the generated command entry");
static_assert(sizeof(mh::game::mh_tact_unit_record::cmd_queue) / mh::tact::TJ_Q_STRIDE ==
                  mh::tact::TJ_Q_SLOTS,
              "TJ_Q_SLOTS drift vs the generated command queue");

// THE FIRST FRAME REPORTS EVERYTHING, and that is the opposite of what tj_watch_direct() does.
// That watch primes silently so arming it later cannot emit a unit's standing field values as if
// they had just been set. Here the standing queue IS the interesting content: llm_tact_mission_load
// enqueues every scripted unit's whole command list BEFORE the first llm_tact_frame runs, so a
// watch that primes on frame 1 adopts the entire mission script as baseline and reports only what
// happens afterwards.
//
// MEASURED (2026-09-05): with priming, a full POZ3 replay recorded 15,428 orders of which every one
// was op 1 (MOVE) or 0x40 (REPEAT) -- the re-submissions -- and ZERO teleports, on the only mission
// in the game that has any. The 20 `TELE` commands were all loaded before frame 1 and were sitting
// in the shadow. The shadow is zero-initialised, so simply not suppressing the first diff makes the
// mission's own order load the first thing the journal contains, which is also what a differential
// wants: if our llm_tact_mission_load ever loaded a script differently, that belongs in the diff.
// EVERY OWNER, NOT JUST OWNER 0 -- and the difference between this watch and the direct-write one
// above is exactly why. tj_watch_direct() restricts to owner 0 because its records are REPLAYED:
// re-applying an enemy unit's recorded field value would stamp the recording's engine state over the
// replay's own. A `Q` record is never injected into anything; it is a COMPARISON channel, so the
// restriction bought nothing there and cost real coverage.
//
// MEASURED, not theorised (2026-09-05). POZ3 is the game's ONLY mission with teleports, and reading
// its script settles where the interesting ops live: all 20 `TELE`, all 7 `WALK` and 11 of 18 `RUN`
// sit under the `; obcy` (aliens) block; `; nasi` (ours) has ZERO of any of them. With the owner-0
// gate those three opcodes were structurally unrecordable -- a human could play POZ3 perfectly and
// journal none of them, which is exactly what the first POZ3 recording did (533 orders, 0 teleports).
// Widening also makes the differential STRONGER for free: a divergence in the AI's own order stream
// between the two arms is now visible instead of silent.
void tj_watch_queue() {
    auto *u = tact_units();
    for (int i = 1; i <= TACT_UNIT_MAX; ++i) {
        const uint8_t *rec = reinterpret_cast<const uint8_t *>(&u[i]);
        const bool     own = rec[0] != 0; // occupied slot, ANY owner -- see above
        for (int s = 0; s < TJQ_TOTAL; ++s) {
            const uint8_t *e  = rec + mh::tact::tj_q_off(s);
            uint8_t       *sh = g_tjq_shadow[i][s];
            // THE FAST PATH, and it is the reason this watch is affordable at all: 130 slots x 128
            // units is 16,640 records a frame, and in a real mission nearly every one of them is
            // permanently empty. Empty-and-was-empty needs neither the compare nor the copy, and
            // skipping it cannot lose a record -- any later append has op != 0 against a shadow
            // whose op is 0, which always compares different.
            if (mh::tact::tj_q_op(e) == 0u && mh::tact::tj_q_op(sh) == 0u) continue;
            if (own && g_cfg.tact_journal_rec && mh::tact::tj_q_record(sh, e)) {
                char l[160];
                wsprintfA(l, "TJ Q %lu %d %d %u %u %u %u %u %u\n", g_tact_step, i, s,
                          (unsigned)e[0], (unsigned)mh::tact::tj_q_op(e),
                          (unsigned)mh::tact::tj_q_arg(e, 0), (unsigned)mh::tact::tj_q_arg(e, 1),
                          (unsigned)mh::tact::tj_q_arg(e, 2), (unsigned)mh::tact::tj_q_arg(e, 3));
                append_line(g_log_path, l);
                ++g_tjq_recorded;
            }
            for (int b = 0; b < TJQ_W; ++b) sh[b] = e[b];
        }
    }
}

// Apply one recorded direct write. The replay's counterpart of the watch above.
void tj_apply_direct(uint32_t unit, uint32_t field, uint32_t value) {
    if (unit < 1 || unit > (uint32_t)TACT_UNIT_MAX || field >= TJD_F_COUNT) return;
    auto         *u     = tact_units();
    uint8_t      *rec   = reinterpret_cast<uint8_t *>(&u[unit]);
    const uint8_t m     = TJD_MASK[field];
    rec[TJD_OFF[field]] = (uint8_t)((rec[TJD_OFF[field]] & ~m) | ((uint8_t)value & m));
}


// ---- the sampling profiler ----------------------------------------------------------------------
//
// Answers "where does the wall clock go" with samples rather than with subtraction. The
// differential table in tools/tact_profile.py prices a KNOB by turning it off; this prices an
// ADDRESS, which is what is needed once the question stops being "which feature" and becomes
// "which code".
//
// FLAT, NOT A STACK WALK, and that is a decision rather than a shortcut. mh.exe is Watcom-built
// with frame-pointer omission, so walking EBP would produce plausible, wrong callers -- and a wrong
// stack is worse than no stack, because it is attributed with confidence. The module split alone
// settles the current question: the oracle's hashing lives in mh.dll, the game lives in mh.exe.
//
// A HISTOGRAM, NOT A LOG. Writing a line per sample would make the profiler the thing being
// profiled (the log writer was itself a suspect here). Samples land in a fixed open-addressed table
// and are dumped once, at the end.
constexpr uint32_t PROF_SLOTS = 4096;

struct prof_slot {
    uint32_t pc;
    uint32_t hits;
};

prof_slot    *g_prof        = nullptr;
HANDLE        g_prof_thread = nullptr;
HANDLE        g_prof_target = nullptr;
volatile LONG g_prof_stop   = 0;
uint32_t      g_prof_total = 0, g_prof_lost = 0;
uintptr_t     g_prof_exe_lo = 0, g_prof_exe_hi = 0;
uintptr_t     g_prof_dll_lo = 0, g_prof_dll_hi = 0;
uint32_t      g_prof_in_exe = 0, g_prof_in_dll = 0, g_prof_other = 0;

void prof_add(uint32_t pc) {
    ++g_prof_total;
    if (pc >= g_prof_dll_lo && pc < g_prof_dll_hi)
        ++g_prof_in_dll;
    else if (pc >= g_prof_exe_lo && pc < g_prof_exe_hi)
        ++g_prof_in_exe;
    else
        ++g_prof_other;
    // Open addressing with linear probing. A full table drops the sample and SAYS SO rather than
    // evicting: a profile that silently forgets its tail reports a flat distribution as a peaked one.
    uint32_t i = (pc * 2654435761u) % PROF_SLOTS;
    for (uint32_t n = 0; n < PROF_SLOTS; ++n) {
        if (g_prof[i].hits == 0) {
            g_prof[i].pc   = pc;
            g_prof[i].hits = 1;
            return;
        }
        if (g_prof[i].pc == pc) {
            ++g_prof[i].hits;
            return;
        }
        i = (i + 1) % PROF_SLOTS;
    }
    ++g_prof_lost;
}

DWORD WINAPI prof_thread(LPVOID) {
    const DWORD period = (DWORD)(1000 / (g_cfg.profile_hz > 0 ? g_cfg.profile_hz : 1));
    while (!InterlockedCompareExchange(&g_prof_stop, 0, 0)) {
        Sleep(period ? period : 1);
        if (SuspendThread(g_prof_target) == (DWORD)-1) continue;
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(g_prof_target, &ctx)) prof_add((uint32_t)ctx.Eip);
        ResumeThread(g_prof_target);
    }
    return 0;
}

// A fingerprint of the LIVE hash implementation: FNV-ish over a fixed vector, emitted once per
// run. Anything that changes what `hash_sink` computes changes this, with nobody having to remember
// to bump anything -- which is the point, because the thing being guarded against is precisely
// somebody not remembering. Consumers that store hashes for a later run (tools/test_ui.py
// soak_golden) record it beside the data and refuse to compare across a mismatch.
void hash_fingerprint_report() {
    // The vector spans the cases the block form can get wrong: a partial tail, a whole block, and a
    // length that is neither. If the tail handling changes, this moves.
    static const uint8_t VEC[21] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
                                    0x10, 0x11, 0x12, 0x13, 0x14};
    mh::state::hash_sink a(mh::state::sink_mode::VERDICT);
    a.raw(VEC, sizeof(VEC));
    // Fed a SECOND time in a different split, so the fingerprint also pins split-invariance: a
    // change that broke it would move this value rather than pass unnoticed.
    mh::state::hash_sink b(mh::state::sink_mode::VERDICT);
    b.raw(VEC, 3);
    b.raw(VEC + 3, 8);
    b.raw(VEC + 11, 10);
    char m[160];
    wsprintfA(m, "; HASH FINGERPRINT %08X%08X split=%s\n", (uint32_t)(a.finish() >> 32),
              (uint32_t)(a.finish() & 0xffffffffu),
              a.finish() == b.finish() ? "ok" : "BROKEN");
    append_line(g_log_path, m);
}

void prof_start() {
    if (g_cfg.profile_hz <= 0) return;
    g_prof = (prof_slot *)VirtualAlloc(nullptr, sizeof(prof_slot) * PROF_SLOTS, MEM_COMMIT,
                                       PAGE_READWRITE);
    if (!g_prof) return;
    // Module extents, so a sample can be attributed without any symbol file. The sizes come from
    // the loaded headers rather than from a constant -- mh.dll is rebuilt constantly and .bss was
    // extended once already.
    // Module extents WITHOUT psapi: the PE headers are already mapped, so SizeOfImage is a couple
    // of pointer hops from the base. That avoids adding a link dependency to the injected DLL for
    // an opt-in diagnostic -- a profiler that changes what gets loaded into the game is a profiler
    // that changes what it measures.
    auto extent = [](HMODULE h, uintptr_t &lo, uintptr_t &hi) {
        if (!h) return;
        const auto *dos = (const IMAGE_DOS_HEADER *)h;
        const auto *nt  = (const IMAGE_NT_HEADERS *)((const uint8_t *)h + dos->e_lfanew);
        lo              = (uintptr_t)h;
        hi              = lo + nt->OptionalHeader.SizeOfImage;
    };
    extent(GetModuleHandleA(nullptr), g_prof_exe_lo, g_prof_exe_hi);
    extent(GetModuleHandleA("mh.dll"), g_prof_dll_lo, g_prof_dll_hi);
    // A REAL handle to the sampled thread, duplicated: GetCurrentThread() is a pseudo-handle that
    // means "whoever asks", so the sampler thread would suspend ITSELF and the process would stop.
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_prof_target,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, 0);
    if (!g_prof_target) return;
    g_prof_thread = CreateThread(nullptr, 0, prof_thread, nullptr, 0, nullptr);
    char m[200];
    wsprintfA(m, "; PROF armed at %d Hz -- exe %08X..%08X dll %08X..%08X\n", g_cfg.profile_hz,
              (uint32_t)g_prof_exe_lo, (uint32_t)g_prof_exe_hi, (uint32_t)g_prof_dll_lo,
              (uint32_t)g_prof_dll_hi);
    append_line(g_log_path, m);
}

void prof_report() {
    if (!g_prof || !g_prof_total) return;
    InterlockedExchange(&g_prof_stop, 1);
    if (g_prof_thread) WaitForSingleObject(g_prof_thread, 2000);
    char m[220];
    wsprintfA(m, "; PROF %lu sample(s): mh.exe %lu (%lu%%), mh.dll %lu (%lu%%), other %lu%s\n",
              g_prof_total, g_prof_in_exe, g_prof_in_exe * 100 / g_prof_total, g_prof_in_dll,
              g_prof_in_dll * 100 / g_prof_total, g_prof_other,
              g_prof_lost ? " -- TABLE FULL, tail dropped" : "");
    append_line(g_log_path, m);
    // The hot addresses, relative to their module so they can be looked up directly: an mh.exe
    // offset IS the Ghidra VA for this binary, and an mh.dll offset resolves through its map file.
    for (uint32_t pass = 0; pass < 40; ++pass) {
        uint32_t best = 0, bi = PROF_SLOTS;
        for (uint32_t i = 0; i < PROF_SLOTS; ++i)
            if (g_prof[i].hits > best) {
                best = g_prof[i].hits;
                bi   = i;
            }
        if (bi == PROF_SLOTS || !best) break;
        const uint32_t pc = g_prof[bi].pc;
        const char    *mod =
            (pc >= g_prof_dll_lo && pc < g_prof_dll_hi)
                   ? "mh.dll"
                   : ((pc >= g_prof_exe_lo && pc < g_prof_exe_hi) ? "mh.exe" : "?");
        const uint32_t rel = (pc >= g_prof_dll_lo && pc < g_prof_dll_hi)
                                 ? (uint32_t)(pc - g_prof_dll_lo)
                                 : ((pc >= g_prof_exe_lo && pc < g_prof_exe_hi)
                                        ? (uint32_t)(pc - g_prof_exe_lo)
                                        : pc);
        wsprintfA(m, "; PROF %-7s +%08X  %5lu  %3lu.%01lu%%\n", mod, rel, best,
                  best * 100 / g_prof_total, (best * 1000 / g_prof_total) % 10);
        append_line(g_log_path, m);
        g_prof[bi].hits = 0; // consumed
    }
}

// ---- TACT-REC: the INPUT journal, and why it supersedes enumerating UI writes -------------------
//
// The order journal was built on a decision -- record ORDERS, not input -- and that decision was
// revised on evidence, not preference. Two rounds of it:
//
//   round 1  three player actions have NO OPCODE (active_gun, squad_group_id, def_stat): the UI
//            writes the record directly. Journalled by watching those three fields.
//   round 2  a real 63,563-frame session replayed to ZERO combat, because SELECTION -- `status`
//            bit 0, which llm_tact_group_issue_order qualifies its whole loop on -- is also a
//            direct UI write, and was not among the three.
//
// The pattern is the problem, not either miss. "Everything the UI writes directly" is an OPEN set:
// each round found another member, and nothing bounds the next one. Enumerating an unbounded set
// and calling it complete is how both rounds happened.
//
// INPUT IS A CLOSED INTERFACE. Every player action enters through two 128-slot rings that the game
// itself drains -- llm_input_mouse_event[128] and llm_input_key_event[128], both stride 0x38,
// producer index advanced &0x7f. Record what enters those, replay it, and EVERY downstream UI write
// reproduces through the game's own code: selection, gun swap, control groups, def_stat, facing and
// the orders themselves. Nothing has to be enumerated, because nothing is being reproduced -- it is
// being re-caused.
//
// This is proven machinery, not a new idea: seams/ui_drive.cpp already injects into both rings with
// no OS input at all, and the whole UI regression suite runs on it.
//
// THE ORDER RECORDS STAY, and their role changes from mechanism to ASSERTION. Recording input AND
// orders means a replay can be checked semantically: re-cause the input, then compare the orders the
// game emitted against the ones it emitted when a human did it. That is a far stronger statement
// than "the hashes match", and it is what would have caught round 2 on the first replay.
//
// THE ONE-FRAME CONVENTION, stated because it is the design's only sleight of hand. Events are
// produced mid-frame and this hook runs at the frame TOP, so what it sees at the top of frame N was
// produced during N-1. It records them as frame N and replays them at frame N -- so every event is
// uniformly one frame later than it originally was. Uniform, not skewed: the sequence and the
// spacing are preserved, which is what the game consumes. Replay-vs-replay (clause 2) is unaffected;
// a replay-vs-RECORDING hash comparison would be offset by one frame, and is not the test.
constexpr uintptr_t TJI_M_EVENTS = mh::addr::_G_LLM_INPUT_MOUSE_EVENTS;
constexpr uintptr_t TJI_M_WRITE  = mh::addr::_G_LLM_INPUT_MOUSE_WRITE_IDX;
constexpr uintptr_t TJI_M_READ   = mh::addr::_G_LLM_INPUT_MOUSE_READ_IDX;
constexpr uintptr_t TJI_K_EVENTS = mh::addr::_G_LLM_INPUT_KEY_EVENTS;
constexpr uintptr_t TJI_K_WRITE  = mh::addr::_G_LLM_INPUT_KEY_WRITE_IDX;
constexpr uintptr_t TJI_K_READ   = mh::addr::_G_LLM_INPUT_KEY_READ_IDX;
constexpr uintptr_t TJI_CUR_X    = mh::addr::_G_LLM_INPUT_MOUSE_LAST_X;
constexpr uintptr_t TJI_CUR_Y    = mh::addr::_G_LLM_INPUT_MOUSE_LAST_Y;

// UI-REC: THE JOURNAL'S STEP INDEX, and the one symbol that used to make this recorder tactical.
//
// Every record is stamped with this and every replay compares against it, so it is the only thing
// that decides which cadence a journal is indexed on. The tactical hook sets it to g_tact_step; the
// present hook sets it to the present counter. Neither reads the other's, and the two arms are
// refused together at arm time -- a file carrying both counters would be well-formed and wrong.
uint32_t g_tj_idx  = 0; // what the next record is stamped with / replayed against
uint32_t g_ui_step = 0; // presents seen since the UI arm was armed (the UI journal's own index)

// Does the PRESENT-indexed arm own the journal this run? Read by both hooks to decide which cadence
// stamps g_tj_idx, and by the present hook to decide whether it does anything at all -- which is why
// it is the config and not `g_tj_ev`: a recording run has no loaded journal but still owns the index.
inline bool ui_journal_armed() { return g_cfg.ui_journal_rec != 0 || g_cfg.ui_journal[0] != 0; }

uint32_t g_tji_mw = 0, g_tji_kw = 0; // last-seen producer indices
uint8_t  g_tji_primed = 0;
int32_t  g_tji_cx = -1, g_tji_cy = -1;
uint32_t g_tji_m = 0, g_tji_k = 0, g_tji_c = 0; // recorded counts
uint32_t g_tji_s   = 0;                         // UI-REC: screen barriers recorded
uint32_t g_tji_scr = 0;                         // last screen VA seen by the recorder
uint32_t g_tji_btn = 0;                         // buttons of the last mouse event drained (0 = none)
uint32_t g_tji_sig = 0;                         // content signature of the last barrier emitted
// UI-REC / SPCAMP-REC: THE STEP BARRIER. A screen barrier synchronises the menu half; nothing
// synchronised the IN-GAME half, and that is what caps the deep rungs. Once a game is live the
// journal is still a PRESENT schedule while the world advances on SIM STEPS, and the two ratios are
// not the same in a headless replay as they were in a played session -- so an in-game click drifts by
// a step or two, and a step or two is a different game. Measured 2026-09-07: two arms of one journal
// agreed on `state` for 200 and for 2000 steps and diverged at 3264, with the last screen barrier
// released two sim steps apart between them. `TJ P <idx> <step>` records where the sim WAS, and the
// replay holds the queue until it is there. Emitted every TJ_SYNC_STEPS rather than per step: one per
// step would roughly double a 36k-record journal to re-sync ~8x more often than the drift needs.
uint32_t           g_tji_p         = 0; // step barriers recorded
uint32_t           g_tji_last_sync = 0; // step of the last one
constexpr uint32_t TJ_SYNC_STEPS   = 8;
// How long a replay waits at one screen barrier before proceeding anyway. Wall time, not presents --
// a headless replay runs several times the rate a played session did, so a present budget would mean
// a different amount of patience in every run. 5s is far beyond any menu transition measured here.
// THE BUDGET IS FOR A STALL, NOT FOR A SCREEN LOAD, and that distinction is the whole fix. It used
// to be time-since-the-barrier-started, which forces a replay that is still MAKING PROGRESS towards
// the screen it wants -- measured 2026-09-07: with dgVoodoo's frame cap lifted (--fps-limit 0) the
// barrier before the landing click forced every run, `live` still on the previous screen, while the
// same journal at 60 fps matched it and never forced. Everything downstream then read as a game-logic
// bug (the first gameplay right-click issuing nothing, the first order sliding from step 78 to 90).
// Now it is time-since-the-live-screen-LAST-CHANGED: a run walking through screens keeps its budget,
// and only a run where nothing is happening at all spends it. Bounded exactly as before -- a screen
// that never arrives still forces, still reports, still counts.
constexpr uint32_t TJ_BARRIER_MS   = 5000;
uint32_t           g_tj_hold_since = 0; // GetTickCount at the last observed CHANGE in the live screen
uint32_t           g_tj_hold_live  = 0; // the live id that was current then
// G146: WHY the barrier is holding, because the two reasons must be treated oppositely.
//   live != want   -- waiting for real work (a load, a screen the game has not built yet). That work
//                     takes WALL time, so the presents it costs scale with the frame rate, and the
//                     pinned clock must NOT advance through them or the rate leaks into the sim.
//   live == want   -- waiting out the settle dwell only. That dwell is now measured in the PINNED
//     && !settled    clock (ui_drive settle_now), so freezing the clock here would stop the very
//                     counter the wait is waiting on: an unconditional freeze DEADLOCKS.
// Set every held present, cleared when the barrier matches or is forced.
bool     g_tj_hold_identity = false;
uint32_t g_tj_forced        = 0; // barriers the replay proceeded past unmatched
// UI-REC idle watchdog. `stop_step` is the only end condition a strategic run has, and it fires from
// INSIDE on_sim_step -- so a replay that ends up somewhere the strategic sim does not step can never
// reach it. That is not a corner case for a game-start journal: the recording may end at a planet
// transition, in a menu, or anywhere else outside the sim, and then the run presents frames forever
// while the step counter stands still. Measured: 24 minutes of a 30-minute budget spent after the
// journal was 99.7% issued, with the harness log not growing by a single byte.
uint32_t           g_tj_last_step_ms = 0;     // GetTickCount at the last sim step
constexpr uint32_t TJ_IDLE_MS        = 20000; // journal done + this long without a step = the run is over
uint32_t           g_tj_waits        = 0;     // UI-REC: presents a replay spent held at a barrier
uint32_t           g_tj_held_at      = 0;     // which barrier the "holding" line was last logged for
// UI-REC: how far the replay's clock has drifted from the recording's, in presents. Set at every
// barrier that releases late; added to every later record's frame so the recorded SPACING survives a
// resynchronisation. Signed and 64-bit: a replay can run ahead of the recording as well as behind.
int64_t  g_tj_shift  = 0;
uint32_t g_tji_inj_m = 0, g_tji_inj_k = 0;   // injected counts
uint32_t g_tji_drop_m = 0, g_tji_drop_k = 0; // injections LOST to a full ring -- see TJDROP
// The mouse-ring producer census (see WHOSE EVENT IS IN THE RING, at the scan itself). File-scope
// rather than function-static so tj_report can print them: a zero foreign count is only evidence if
// the scan walked slots at all, so all three numbers are reported together.
uint32_t g_tj_slots_seen = 0, g_tj_slots_ours = 0, g_tj_foreign_n = 0;

// ---- SPCAMP-FLAKE: WHO RUNS A DIFFERENT NUMBER OF TIMES? ----------------------------------------
//
// The flake is one strategic order enqueued in one run and not in another, under CPU load, on input
// PROVEN to land on the same sim step in every run (the present->step offset is a constant 2 in
// every run measured, divergent ones included). Every diverging order so far is issued from the
// UI/input path: llm_strat_ui_storage_bldg_panel (a panel DRAW) and llm_strat_input_update. So the
// question is no longer "when does input arrive" but "how many times does the UI path RUN", and that
// is a count nothing in this harness could observe.
//
// Two entry counters, sampled on the TJPS row beside the ring cursors and the pinned clock, so ONE
// batch of runs discriminates five hypotheses instead of one:
//   iu/pn  -- the two issuers' entry counts. Constant across runs + a divergent order => a PREDICATE
//             inside one of them reads something unpinned. Varying => the frame path is the thing.
//   kw/mw  -- the input rings' PRODUCER cursors. If these differ, events are entering the rings that
//             the journal did not put there, i.e. real OS input is leaking into a replay.
//   kr/mr  -- the CONSUMER cursors. These move when a consumer runs, so they separate "more events"
//             from "more draining".
//   tk     -- the PINNED ms clock. If it differs at the same step, the pin has a hole and that is
//             the answer on its own -- the cheapest of the five and the one nothing has checked.
//   cx/cy  -- the cursor the hit-tests read.
// The deferred-event ring's defer/q are GONE from the row: measured 0 in every sample of every run
// (measured by the UI suite), so they cost width and can no longer say anything. `pend` stays -- it is the
// latch the panel tests-and-clears, and it is the one that was observed to differ.
//
// Gated on [harness] tj_ps_log, so a run that is not being probed carries no extra entry detour.
// D5: both entries are hook-point rows now (point::ui_input_update / point::ui_storage_panel).
void             *g_probe_iu_tramp = nullptr;
void             *g_probe_pn_tramp = nullptr;
volatile uint32_t g_probe_iu       = 0; // llm_strat_input_update entries
volatile uint32_t g_probe_pn       = 0; // llm_strat_ui_storage_bldg_panel entries

// THE REPLAY'S INPUT-PRODUCER SUPPRESSOR -- see replay_isolate_input at the arming site. All four of
// llm_input_wndproc_tap's arguments arrive in registers (__mh_watcall_ecx_ebx_volatile: EAX/EDX/EBX/
// ECX, mh_calls.gen.h), so there is no stack frame to unwind and no argument bytes to pop: a bare
// RET is a complete and correct whole-body replacement.
__declspec(naked) void wndproc_tap_suppressed() {
    __asm { ret }
}

// Run-before-and-continue, and nothing else: INC a counter in memory and fall through to the stolen
// prologue. Every register is preserved; only EFLAGS is clobbered, which is dead at a call boundary.
__declspec(naked) void probe_iu_detour() {
    __asm {
        inc dword ptr [g_probe_iu]
        jmp dword ptr [g_probe_iu_tramp]
    }
}
__declspec(naked) void probe_pn_detour() {
    __asm {
        inc dword ptr [g_probe_pn]
        jmp dword ptr [g_probe_pn_tramp]
    }
}

// ---- [harness] skip_intro_avi: dismiss the new-game intro movie + briefing ----------------------
//
// llm_menu_race_select_cb -> llm_game_boot_init(Intro_NewGameH.avi, "info\NEWGAME.TXT", 0x28, 1)
// plays a movie and then a briefing before the sim ticks once -- ~128 s of a VISIBLE replay, measured
// per game_mode. A player ends both with one SPACE. This presses it.
//
// IT IS THE GAME'S OWN DISMISS. llm_ui_menu_async_tick (0x004c240c) ends the screen when
// llm_ui_modal_key_pump reports an event carrying ASCII 0x20/0x0a/0x1b, and falls into
// llm_ui_avi_close_return_to_menu -- which tears down codecs and surfaces AND calls DAT_006445f7, the
// armed proceed action. The dismiss is the step that ENTERS THE GAME, so nothing here reimplements it.
//
// IT HANGS OFF THE MOVIE TICK, NOT THE PRESENT HOOK, and that was the real bug behind five failed
// attempts. llm_ui_menu_async_tick drives these screens through its own DirectDraw surface calls and
// never reaches llm_gfx_present_flip, so MH_Harness_OnPresent does not run for the whole movie --
// measured with the readout below: the seam fired at presents 1 and 362 of a run whose movie is
// minutes long, so every keystroke it "sent" was sent while no intro was up. video.cpp already had a
// trampoline on that tick for the no_window keeper; MH_Harness_WantsMovieTick arms the same one.
// Result: the visible menu phase went 128.2 s -> 6.6 s.
//
// WHY INJECTING THE KEY IS NOT ENOUGH ON ITS OWN, the other half of this seam, and it cost
// four measured attempts that moved the clock by 0.0 s. The pump does not
// dispatch what it dequeues. It LATCHES -- one event into KEYREC, `pending` bit set -- and PUBLISHES
// into the pair the caller reads only on the branch it takes when
// STEP_NEXT_MS >= STEP_DEADLINE_MS. Until that accumulator says so the key sits latched and every
// later injection is dropped on the closed latch. So we supply BOTH halves of one keystroke: the
// event, and the segment end that makes the pump publish it. In one call the pump then dequeues
// (setting `pending` itself), takes the publish branch, writes ASCII 0x20, and returns 1 -- and the
// caller closes. Forcing the accumulator is not a hack around the pump; it is the same edge a real
// keystroke reaches by waiting.
//
// TWO GATES, both learned by getting them wrong:
//   1. the async callback must be llm_ui_menu_async_tick -- installed by llm_game_boot_init and
//      cleared by the close, so it is true for this screen and no other menu; and
//   2. _G_LLM_BOOT_MODE_FLAG must be 1. That callback ALSO drives the boot LOGO.AVI, where the flag
//      is 0 and the pump's caller jumps past every dismiss branch. Injecting there dismisses nothing
//      and desynchronises a replay from a recording whose barriers were banked with the logo
//      playing -- measured at 129 s of forced barriers against 14.3 s untouched.
void tji_inject_key(uint32_t scancode, uint32_t type, uint32_t ts); // defined below, with the injectors

constexpr uint32_t TJ_AVI_SKIP_EVERY = 8;  // presents between attempts
constexpr uint32_t TJ_AVI_SKIP_MAX   = 64; // bounded: a gate that never clears cannot spin forever
uint32_t           g_avi_skip_sent   = 0;
uint32_t           g_avi_skip_last   = 0;
uint32_t           g_avi_skip_frame  = 0; // its own present counter: runs with or without a journal

void avi_skip_tick() {
    if (!g_cfg.skip_intro_avi) return;
    const uint32_t  now = ++g_avi_skip_frame;
    const uintptr_t cb  = *(volatile uintptr_t *)mh::addr::_G_LLM_UI_MENU_ASYNC_CALLBACK;
    const bool      up  = (cb == mh::addr::llm_ui_menu_async_tick) &&
                    (*(volatile int32_t *)mh::addr::_G_LLM_BOOT_MODE_FLAG == 1);
    if (!up) {
        g_avi_skip_sent = 0; // rearm: a campaign shows this screen more than once
        return;
    }
    if (g_avi_skip_sent >= TJ_AVI_SKIP_MAX) return;
    if (g_avi_skip_sent && now - g_avi_skip_last < TJ_AVI_SKIP_EVERY) return;
    g_avi_skip_last = now;
    // 0x39 = SPACE in the set-1 codes the ring carries. THE STAMP IS THE GAME'S CLOCK, NOT
    // GetTickCount: a real event's timestamp is `GetTickCount() - _G_LLM_INPUT_TIME_EPOCH` (see the
    // ring structs), which is exactly what _G_LLM_TIME_TICKS_MS holds, and every OTHER injected event
    // replays its RECORDED stamp for the reason tji_inject_mouse states -- the consumer compares
    // timestamps. A raw GetTickCount here was the one wall-clock value left in an otherwise pinned
    // input stream. Reading the published global rather than CALLING llm_time_get_ticks_ms keeps it
    // side-effect-free (no query-counter bump, no _LAST shift); under pin_menu_clock that global IS
    // the pinned clock, and without it, the real one -- so this is correct either way.
    tji_inject_key(0x39u, 0x100u, *(volatile uint32_t *)mh::addr::_G_LLM_TIME_TICKS_MS);
    // ...and end the pump's current segment, so the very next call publishes it instead of counting
    // out 150 ms steps first. NEXT == DEADLINE is the publish branch's own condition, not a poke past
    // it: `if (NEXT < DEADLINE) {catch-up} else {publish}`.
    *(volatile uint32_t *)mh::addr::_G_LLM_UI_MODAL_STEP_NEXT_MS =
        *(volatile uint32_t *)mh::addr::_G_LLM_UI_MODAL_STEP_DEADLINE_MS;
    // KEPT, deliberately: this readout is what turned five failed attempts into the answer. It showed
    // the seam firing at presents 1 and 362 of a run whose movie is minutes long -- i.e. the keystroke
    // was never the problem, the hook point was.
    ++g_avi_skip_sent;
    if (g_avi_skip_sent <= 4) {
        char m[220];
        wsprintfA(m,
                  "; AVI-SKIP #%lu present=%lu ascii=%04X sc=%08X next=%08X dead=%08X now=%08X"
                  " dlg0=%02X\n",
                  g_avi_skip_sent, now,
                  (unsigned)*(volatile uint16_t *)mh::addr::_G_LLM_UI_MODAL_KEY_ASCII,
                  *(volatile uint32_t *)mh::addr::_G_LLM_UI_MODAL_KEY_SCANCODE,
                  *(volatile uint32_t *)mh::addr::_G_LLM_UI_MODAL_STEP_NEXT_MS,
                  *(volatile uint32_t *)mh::addr::_G_LLM_UI_MODAL_STEP_DEADLINE_MS,
                  *(volatile uint32_t *)mh::addr::_G_LLM_UI_MENU_NOW_MS,
                  (unsigned)*(volatile uint8_t *)mh::addr::_G_LLM_DLG_STATE_FLAGS);
        append_line(g_log_path, m);
    }
}

// Journal every event the rings gained since the previous frame, plus the cursor when it moves.
// Reading the SLOTS rather than hooking a producer is deliberate: it captures events from BOTH
// producers (the DirectInput poll and the wndproc tap are mutually exclusive but which one runs is a
// property of the machine, not of the session) and from ui_drive's own injections, with one rule.
// ---- WHOSE EVENT IS IT? The recorder must not journal the harness's own injections -------------
//
// tji_record walks the game's ring WRITE CURSORS, so it cannot tell a human's event from one we put
// there ourselves -- it sees the slots, not the source. That was harmless while nothing injected
// during a recording, and stopped being harmless when skip_intro_avi became the default: avi_skip_tick
// puts a synthetic SPACE in the key ring, the recorder would journal it, and a replay would then
// deliver that SPACE TWICE (the live skip, plus the recorded one) at a screen where the second press
// goes somewhere the recording never went.
//
// One bit per ring slot, and the ring is exactly 128 entries, so the whole thing is 4 words. Set on
// injection, tested-and-cleared as the recorder walks past. Slot reuse cannot leak a stale bit: a slot
// is only rewritten after the cursor has wrapped past it, and the recorder clears the bit on the way.
uint32_t g_tji_own_m[4] = {0, 0, 0, 0}; // mouse-ring slots this harness wrote
uint32_t g_tji_own_k[4] = {0, 0, 0, 0}; // key-ring slots this harness wrote

inline void tji_own_set(uint32_t *mask, uint32_t slot) { mask[(slot >> 5) & 3] |= 1u << (slot & 31); }
inline bool tji_own_take(uint32_t *mask, uint32_t slot) {
    const uint32_t bit = 1u << (slot & 31);
    uint32_t      &w   = mask[(slot >> 5) & 3];
    const bool     own = (w & bit) != 0;
    w &= ~bit;
    return own;
}

void tji_record() {
    // UI-REC: THE SCREEN BARRIER, emitted before this present's events so a replay reaches the screen
    // before it clicks on it. Only the present-indexed arm records these -- a tactical mission has
    // one screen for its whole life, so a barrier there would be a constant.
    //
    // THIS IS THE FINDING THAT MADE THE FEATURE WORK, and it was measured rather than reasoned: the
    // first round trip recorded the scripted menu walk correctly, replayed all 22 records at the
    // recorded present indices, and never started a game. A present index is not a clock for the
    // menu. How many presents pass before a screen is up depends on asynchronous resource loading,
    // so "click at present 5603" replays into whatever screen present 5603 happens to be, which is
    // not the one the click was aimed at. It is the same fact the script grammar encodes by banning
    // frame/time waits and gating everything on UI state -- rediscovered from the other direction.
    //
    // NOT WHILE A BUTTON IS HELD. A menu item that opens its screen on the mouse-DOWN changes the
    // screen mid-click, so the barrier would be journalled between the press and the release -- and
    // on replay the barrier then waits for a screen that only the release can produce, while the
    // release waits behind the barrier. That is a permanent hang, and the "no timeout" rule that is
    // right everywhere else is what makes it permanent. Measured on a real 25,709-record session.
    // A press and its release are ONE GESTURE; nothing may be scheduled between them. Deferring
    // costs a present: the barrier is emitted on the next one, after the release is journalled.
    //
    // AND ONLY ONCE THE SCREEN HAS SETTLED -- but settled in its IDENTITY, not in its geometry, and
    // that correction is SPCAMP-SYNC. The first version recorded at geometric stillness, which is the
    // right rule for a click and the wrong one for a barrier: the campaign race picker has a child
    // that walks (500,22) -> (320,34) over about eight seconds, so on that screen the geometry NEVER
    // settles and the recorder emits no barrier at all -- while a replay's barrier, which also
    // requires geometric stillness, can never be confirmed and is FORCED by the timeout every single
    // time. That is the residue the content signature left unexplained: 13 of 37 forced on session 2,
    // all of them on animated screens. Identity stillness is true the moment the CONTENT stops
    // changing, which is the thing a barrier is actually waiting for.
    if (ui_journal_armed() && g_cfg.ui_journal_rec && !g_tji_btn && MH_UIDrive_ScreenIdSettled()) {
        const uint32_t scr = MH_UIDrive_ActiveScreen();
        if (scr != g_tji_scr || MH_UIDrive_ScreenId() != g_tji_sig) {
            g_tji_scr = scr;
            g_tji_sig = MH_UIDrive_ScreenId();
            // THE BARRIER CARRIES THE CLOCK, and that is what makes the round trip comparable rather
            // than merely correct. The pinned clock advances per PRESENT in the menu, so a replay
            // that reaches a screen in a different number of presents than the recording did arrives
            // there with a different clock -- and carries that offset into the game forever. The
            // first round trip measured exactly that: the sim state matched on all 200 steps while
            // every wall-clock-derived region differed on all 200. Re-pinning the clock at each
            // barrier is the same device the tactical arm's clock track uses for the same reason.
            //
            // Microseconds as a 64-bit pair: the journal's scanner reads u32 fields, and a session
            // long enough to matter overflows a single one at ~72 minutes.
            const uint64_t us = (uint64_t)(g_pin_now * 1000000.0 + 0.5);
            char           sb[160];
            // Field 5 is the geometry signature and field 6 is the CONTENT id; the replay matches on
            // the LAST one present. Both are written because a journal outlives the theory that
            // produced it: field 5 is what every banked session carries, and keeping it in the new
            // format means a journal recorded today can still be read by the older matcher and, more
            // usefully, that a diff of the two columns shows directly how much of a screen's hash
            // motion was geometry. The container VA (field 2) stays for the same reason -- it costs
            // nothing and reads well in a log -- but it identifies nothing on its own.
            wsprintfA(sb, "TJ S %lu %lu %lu %lu %lu %lu\n", g_tj_idx, (unsigned long)scr,
                      (unsigned long)(us >> 32), (unsigned long)(us & 0xffffffffu),
                      (unsigned long)MH_UIDrive_ScreenSig(), (unsigned long)MH_UIDrive_ScreenId());
            append_line(g_log_path, sb);
            ++g_tji_s;
        }
    }
    // The step barrier (see TJ_SYNC_STEPS). Gated on `!g_tji_btn` for a WEAKER version of the screen
    // barrier's reason. It cannot deadlock -- the sim advances whether or not the release is issued,
    // so the hold always ends -- but a hold between a press and its release LENGTHENS the click, and
    // in-game that is the difference between a click and a drag (the selection box). A barrier is
    // free to slip one present; a gesture is not free to change shape.
    if (ui_journal_armed() && g_cfg.ui_journal_rec && !g_tji_btn &&
        g_step >= g_tji_last_sync + TJ_SYNC_STEPS) {
        g_tji_last_sync = g_step;
        char pb[80];
        wsprintfA(pb, "TJ P %lu %lu\n", g_tj_idx, g_step);
        append_line(g_log_path, pb);
        ++g_tji_p;
    }
    const uint32_t mw = *(volatile uint32_t *)TJI_M_WRITE;
    const uint32_t kw = *(volatile uint32_t *)TJI_K_WRITE;
    if (!g_tji_primed) {
        g_tji_primed = 1;
        g_tji_mw     = mw;
        g_tji_kw     = kw;
        return;
    }
    char l[200];
    for (uint32_t i = g_tji_mw; i != mw; i = (i + 1) & 0x7f) {
        if (tji_own_take(g_tji_own_m, i)) continue; // the harness put this here, not a human
        const auto *e = (const mh::game::mh_llm_input_mouse_event *)(TJI_M_EVENTS +
                                                                     (size_t)i * 0x38u);
        wsprintfA(l, "TJ M %lu %lu %lu %ld %ld %ld %ld %ld %lu\n", g_tj_idx,
                  (unsigned long)e->event_type, (unsigned long)e->buttons, (long)e->x, (long)e->y,
                  (long)e->dx, (long)e->dy, (long)e->wheel_delta, (unsigned long)e->timestamp);
        append_line(g_log_path, l);
        // Whether a button is DOWN, read off the event rather than inferred from the type encoding
        // (which button, and which constant means release, both stop mattering). The screen-barrier
        // block above defers while this is set -- see it for the deadlock that requires it.
        g_tji_btn = e->buttons;
        ++g_tji_m;
    }
    for (uint32_t i = g_tji_kw; i != kw; i = (i + 1) & 0x7f) {
        if (tji_own_take(g_tji_own_k, i)) continue; // e.g. avi_skip_tick's SPACE -- see tji_own_set
        const auto *e = (const mh::game::mh_llm_input_key_event *)(TJI_K_EVENTS +
                                                                   (size_t)i * 0x38u);
        wsprintfA(l, "TJ K %lu %lu %lu %lu\n", g_tj_idx, (unsigned long)e->scancode,
                  (unsigned long)e->event_type, (unsigned long)e->timestamp);
        append_line(g_log_path, l);
        ++g_tji_k;
    }
    g_tji_mw = mw;
    g_tji_kw = kw;

    const int32_t cx = *(volatile int32_t *)TJI_CUR_X;
    const int32_t cy = *(volatile int32_t *)TJI_CUR_Y;
    if (cx != g_tji_cx || cy != g_tji_cy) {
        g_tji_cx = cx;
        g_tji_cy = cy;
        wsprintfA(l, "TJ C %lu %ld %ld\n", g_tj_idx, (long)cx, (long)cy);
        append_line(g_log_path, l);
        ++g_tji_c;
    }
}

// Inject one recorded mouse event, the same way ui_drive does -- including the RECORDED timestamp
// rather than a fresh one, because the consumer compares timestamps (double-click intervals), and a
// re-stamped event would replay a different gesture from the one recorded.
void tji_inject_mouse(uint32_t type, uint32_t buttons, int32_t x, int32_t y, int32_t dx, int32_t dy,
                      int32_t wheel, uint32_t ts) {
    const uint32_t w    = *(volatile uint32_t *)TJI_M_WRITE;
    const uint32_t next = (w + 1) & 0x7f;
    // A FULL RING DROPS THE RECORD, AND IT USED TO DO IT IN SILENCE. This line's comment claimed the
    // drop was "counted below"; nothing counted it. The record cursor still advances, so the run
    // reports the record as ISSUED and the game never sees it -- an input that vanishes with no
    // trace anywhere, which is precisely the shape of the residues being chased at steps 29,069 and
    // 29,531. The ring is 128 entries and is drained once per frame, so a present that injects a
    // burst larger than that (the catch-up after a barrier releases) silently loses the tail.
    // Loud and bounded: this must never be a number nobody reads.
    if (next == *(volatile uint32_t *)TJI_M_READ) {
        if (++g_tji_drop_m <= 16) {
            char db[168];
            wsprintfA(db,
                      "; TJDROP mouse #%lu -- RING FULL at step %lu present %lu (record %lu):"
                      " type=%lu btn=%lu x=%ld y=%ld. THE INPUT IS LOST.\n",
                      g_tji_drop_m, g_step, g_tj_idx, g_tj_next, type, buttons, (long)x, (long)y);
            append_line(g_log_path, db);
        }
        return;
    }
    auto *e        = (mh::game::mh_llm_input_mouse_event *)(TJI_M_EVENTS + (size_t)w * 0x38u);
    e->buttons     = buttons;
    e->event_type  = type;
    e->dx          = dx;
    e->dy          = dy;
    e->wheel_delta = wheel;
    e->x           = (uint32_t)x;
    e->y           = (uint32_t)y;
    e->wheel_total = 0;
    e->timestamp   = ts;
    tji_own_set(g_tji_own_m, w); // ours -- the recorder must skip this slot
    *(volatile uint32_t *)TJI_M_WRITE = next;
    ++g_tji_inj_m;
}

// ---- UI-REC: THE KEYSTATE ARRAY IS A SECOND INPUT CHANNEL, and the ring alone does not reach it ---
//
// ui_drive.cpp's enqueue_key says the keystate array is "deliberately left alone" because "every
// consumer we need reads the event ring". That was true of the consumers ui_drive drives; it is FALSE
// for a journal replay. llm_strat_input_update's squad branch tests _G_LLM_KEY_RSHIFT_HELD /
// _G_LLM_KEY_LSHIFT_HELD -- bytes INSIDE _G_LLM_INPUT_KEYSTATE, not ring events:
//
//     if (dblclick_window || (RSHIFT_HELD & 1) || (LSHIFT_HELD & 1))
//         llm_strat_group_issue_move_order(...)          // squad merge, order 0x33
//     else
//         llm_strat_group_issue_move_order_deferred(...) // plain move, order 0x0A
//
// So a shift-modified click replays as an UNMODIFIED one: the journal records the shift press
// correctly (`K <present> 54 256`), the ring gets it, and the flag the branch reads stays zero.
// MEASURED on the user's campaign: order 0x33 appears 11 times in the recording and 0 times in every
// replay, and the user confirmed the input was shift+click rather than a double-click.
//
// The transform is llm_input_wndproc_tap's own, read off its raw WM_KEYDOWN/UP path, including the
// EXTENDED-key split -- which the journal can reconstruct because the same flag is mirrored into the
// event's own type as 0x200:
//     down:  KEYSTATE[sc] |= extended ? 10 : 5      up: KEYSTATE[sc] &= extended ? 0xfd : 0xfe
void tji_keystate_apply(uint32_t scancode, uint32_t type) {
    volatile uint8_t *ks  = (volatile uint8_t *)mh::addr::_G_LLM_INPUT_KEYSTATE;
    const uint32_t    sc  = scancode & 0x7fu; // the wndproc masks the lParam field the same way
    const bool        ext = (type & 0x200u) != 0;
    if (type & 0x100u)
        ks[sc] = (uint8_t)(ks[sc] | (ext ? 10u : 5u));
    else if (type & 0x80u)
        ks[sc] = (uint8_t)(ks[sc] & (ext ? 0xfdu : 0xfeu));
}

// WHICH KEYS THE JOURNAL BELIEVES ARE DOWN, so they can be RE-ASSERTED every present.
//
// A single write is not enough and the reason is the one ui_drive's comment gives: when a DirectInput
// keyboard device is present, llm_input_wndproc_tap delegates to llm_input_di_keyboard_poll and that
// poll re-latches the whole array from the REAL device -- where nothing is held during a headless
// replay. Re-asserting at present-flip puts the value back after any such latch and before the next
// frame's llm_strat_input_update reads it, which is the same one-frame convention the ring injection
// already uses. Cleared by the recorded key-up, so a held key cannot get stuck.
uint8_t g_tj_key_down[128] = {0};

void tji_keystate_reassert() {
    volatile uint8_t *ks = (volatile uint8_t *)mh::addr::_G_LLM_INPUT_KEYSTATE;
    for (uint32_t sc = 0; sc < 128; ++sc)
        if (g_tj_key_down[sc]) ks[sc] = (uint8_t)(ks[sc] | g_tj_key_down[sc]);
}

void tji_inject_key(uint32_t scancode, uint32_t type, uint32_t ts) {
    const uint32_t w    = *(volatile uint32_t *)TJI_K_WRITE;
    const uint32_t next = (w + 1) & 0x7f;
    if (next == *(volatile uint32_t *)TJI_K_READ) { // full -- see tji_inject_mouse's TJDROP
        if (++g_tji_drop_k <= 16) {
            char db[168];
            wsprintfA(db,
                      "; TJDROP key #%lu -- RING FULL at step %lu present %lu (record %lu):"
                      " sc=%lu type=%lu. THE INPUT IS LOST.\n",
                      g_tji_drop_k, g_step, g_tj_idx, g_tj_next, scancode, type);
            append_line(g_log_path, db);
        }
        return;
    }
    auto *e       = (mh::game::mh_llm_input_key_event *)(TJI_K_EVENTS + (size_t)w * 0x38u);
    e->scancode   = scancode;
    e->event_type = type;
    e->timestamp  = ts;
    tji_own_set(g_tji_own_k, w); // ours -- the recorder must skip this slot
    *(volatile uint32_t *)TJI_K_WRITE = next;
    // The OTHER half of a keystroke -- see tji_keystate_apply. Only while REPLAYING a journal: during
    // a live session the real wndproc already maintains this array, and writing it again would fight
    // the device.
    if (g_tj_ev) {
        tji_keystate_apply(scancode, type);
        const uint32_t sc = scancode & 0x7fu;
        if (type & 0x100u) g_tj_key_down[sc] = (uint8_t)((type & 0x200u) ? 10u : 5u);
        else if (type & 0x80u) g_tj_key_down[sc] = 0;
    }
    ++g_tji_inj_k;
}

void tji_set_cursor(int32_t x, int32_t y) {
    *(volatile int32_t *)TJI_CUR_X = x;
    *(volatile int32_t *)TJI_CUR_Y = y;
}

// A SCRIPTED INPUT PROBE, and it exists to test the journal rather than the game. Without it the
// record->replay round trip can only be exercised by a human playing, which is exactly the loop this
// whole feature is trying to make cheap: two play sessions were already spent finding gaps that a
// self-contained test would have caught. It injects a known cursor move, a left down/up pair and a
// keypress at a known frame; the recorder must capture them and a replay must reproduce them
// EXACTLY, which is a statement about the rings and the frame keying, not about game semantics.
void tji_probe_tick() {
    if (!g_cfg.tact_input_selftest) return;
    const uint32_t at = (uint32_t)g_cfg.tact_input_selftest;
    if (g_tact_step == at) {
        tji_set_cursor(160, 120);
        tji_inject_mouse(1u /*EV_MOVE*/, 0u, 160, 120, 0, 0, 0, 0x11110000u);
    } else if (g_tact_step == at + 2) {
        tji_inject_mouse(2u /*EV_LDOWN*/, 1u, 160, 120, 0, 0, 0, 0x22220000u);
    } else if (g_tact_step == at + 4) {
        tji_inject_mouse(3u /*EV_LUP*/, 0u, 160, 120, 0, 0, 0, 0x33330000u);
        tji_inject_key(0x10u /*Q*/, 0x100u, 0x44440000u);
    }
}

// ---- TACT-REC clause 3: the combat report -------------------------------------------------------
//
// "The replay verdict prints units_lost_total and the squad-survivor set, and both show a real
// engagement -- a journal that reached no combat CANNOT pass this clause."
//
// NOTHING COMPUTED THIS BEFORE. `units_lost_total` exists but is the STRATEGIC per-planet cumulative
// counter (llm_strat_player_profile), not a per-excursion tally, and it is not reset at mission
// start -- reporting it would be reporting the campaign. What a tactical excursion needs is its own
// roster, counted at the start and again at the end, so this takes a baseline at the first hashed
// frame and diffs it.
//
// It is emitted for EVERY tactical run, not only a replay: a determinism arm that reached no combat
// is worth knowing about too, and it is one line.
struct tj_roster {
    uint16_t live[8]; // by owner
    uint16_t total;
};

tj_roster g_tj_base  = {};
uint8_t   g_tj_based = 0;
uint16_t  g_tj_last  = 0; // last reported live total -- the census is emitted when it MOVES

tj_roster tj_census() {
    tj_roster r{};
    auto     *u = tact_units();
    for (int i = 1; i <= TACT_UNIT_MAX; ++i) {
        const uint8_t *rec = reinterpret_cast<const uint8_t *>(&u[i]);
        if (rec[0] == 0) continue;                                        // empty slot
        if (u[i].hp == 0 || u[i].anim_state == TACT_ANIM_DYING) continue; // dead / dying
        ++r.total;
        ++r.live[rec[1] & 7u];
    }
    return r;
}

// The squad-survivor SET, not just its size: which slots of the player's squad are still standing.
// A count cannot distinguish "lost three" from "lost three and gained three", and the set can.
void tj_combat_report() {
    const tj_roster now = tj_census();
    char            l[400];
    int             off = wsprintfA(l, "; TACT COMBAT frames=%lu total %u->%u", g_tact_step,
                                    (unsigned)g_tj_base.total, (unsigned)now.total);
    for (int o = 0; o < 4; ++o)
        if (g_tj_base.live[o] || now.live[o])
            off += wsprintfA(l + off, " owner%d %u->%u(-%u)", o, (unsigned)g_tj_base.live[o],
                             (unsigned)now.live[o],
                             (unsigned)(g_tj_base.live[o] > now.live[o]
                                            ? g_tj_base.live[o] - now.live[o]
                                            : 0));
    unsigned lost = 0;
    for (int o = 0; o < 8; ++o)
        if (g_tj_base.live[o] > now.live[o]) lost += (unsigned)(g_tj_base.live[o] - now.live[o]);
    off += wsprintfA(l + off, " units_lost_total=%u\n", lost);
    append_line(g_log_path, l);

    // The survivor set, owner 0. Slot indices, so a reader can tell WHICH squad members lived --
    // and so a replay's set can be compared against the recording's rather than only its size.
    static char sl[64 + TACT_UNIT_MAX * 5];
    int         soff = wsprintfA(sl, "; TACT SURVIVORS owner0");
    auto       *u    = tact_units();
    int         n    = 0;
    for (int i = 1; i <= TACT_UNIT_MAX; ++i) {
        const uint8_t *rec = reinterpret_cast<const uint8_t *>(&u[i]);
        if (rec[0] == 0 || (rec[1] & 7u) != 0) continue;
        if (u[i].hp == 0 || u[i].anim_state == TACT_ANIM_DYING) continue;
        soff += wsprintfA(sl + soff, " %d", i);
        ++n;
    }
    if (!n) soff += wsprintfA(sl + soff, " (none)");
    sl[soff++] = '\n';
    sl[soff]   = '\0';
    append_line(g_log_path, sl);
}

// Issue every order recorded for this frame. Called from on_tact_frame BEFORE the hash and before
// the game's own body, which is the same ordering rule the poke arm and TACT-SYNTH follow: the order
// must land before this frame's hash, or the frame the log names is one ahead of the frame it
// changed. Entries are in file order and the file is frame-ordered, so this is a cursor, not a scan.
void tj_report();

void tj_replay_frame() {
    // In VERIFY mode the derived records are NOT injected -- only the input is. That is the whole
    // experiment. An order (E/G) and a direct write (D) are both CONSEQUENCES of input: if the input
    // journal is complete, the game re-emits them by itself, and injecting them as well would double
    // every order. If it is not complete, the difference is exactly what is missing -- named, per
    // record, instead of inferred from a hash that moved.
    // INPUT-ONLY IS NOT A MODE, IT IS WHAT A JOURNAL WITH INPUT MEANS. If the journal carries
    // M/K/C records then its E/G/D records are CONSEQUENCES of them -- the game re-emits every one
    // unaided (the tactical-probe work 9p: 211 of 211, zero shift). Injecting them as well issues
    // each order twice: once from the replayed click that caused it, once from the journal. That is
    // not a subtle corruption -- it fights a different battle and ends the mission early, which is
    // exactly how this was found.
    const bool input_only = g_tj_has_input;
    // `<=`, NOT `==`, and this is a deadlock fix rather than a nicety. The cursor advances only
    // past records it issues, so a single record whose frame is BEHIND the current step -- one
    // out-of-order line anywhere in the file -- would never match, and every record after it would
    // wait behind it forever. The run does not fail: it plays on happily, issuing nothing, and
    // reports a frame count that looks complete. The negative arm found this by shifting a record
    // without re-sorting the file and stalling at 9,400 of 14,317, which is exactly the failure a
    // human-edited journal would produce. A late record is issued at once; the journal cannot jam.
    // THE SCHEDULE IS THE SIM STEP WHEREVER THE JOURNAL GIVES ONE. Present count is the fallback,
    // and it is only ever right where there is no sim to measure against (the menu). An `S` barrier
    // keeps the present schedule even in game: its release is decided by the screen, and gating its
    // EVALUATION on a step the sim may never reach -- because the click that advances the sim is
    // behind this very barrier -- is the deadlock shape this file has met twice already.
    // ---- THE DRAIN LATENCY, and why the step gate fires a step EARLY ----------------------------
    // Injection and consumption are not the same frame. This hook runs at present-flip, i.e. AFTER
    // the frame's input tick has already drained the ring, so a record injected here is consumed by
    // the NEXT frame. A RECORDED event is not symmetric with that: it entered the ring during the
    // frame the recorder stamps it with, and was drained by that same frame's input tick. So the
    // recorder's "frame N" means CONSUMED ON N, while injecting on N means consumed on N+1.
    //
    // The ordering note above ui_journal_present claims both directions carry the same one-frame
    // offset. They do not, and this is the correction. MEASURED against the recording's own hash
    // stream (the comparison the rung ladder cannot make -- both its arms are replays, so they
    // diverge from the recording identically and every rung stays green): the first TEN orders were
    // byte-identical in content and every one of them landed exactly ONE SIM STEP LATE.
    //
    //     recording  77  85  86  86  87  87  88  88  89  89
    //     replay     78  86  87  87  88  88  89  89  90  90
    //
    // With fixed_step=1 the sim runs one step per frame, so one frame of drain latency IS one step.
    // Firing at `due - 1` puts the injection on the frame BEFORE, and the drain then lands on `due`.
    //
    // ONLY THE STEP PATH IS CORRECTED. The present path below keeps its plain comparison because a
    // uniform +-1 there has no lasting consequence -- every screen barrier re-anchors g_tj_shift, so
    // a menu record's absolute present is re-derived at the next barrier anyway. A step is the SIM's
    // own clock, anchored to recorded steps by the `P` barriers, so an offset there is a real error
    // that nothing downstream absorbs. Correcting a path the evidence does not cover would be a
    // guess, and this file has already paid for two of those.
    auto tj_due = [&](int i) -> bool {
        // ---- A `P` IS REACHABLE THE MOMENT THE QUEUE GETS TO IT (2026-09-08) -------------------
        //
        // It is a STEP barrier. Its wait is the hold in the loop body (`g_step + 1 < a0`), which is
        // denominated in the sim's own clock and is correct. What was wrong was its EVALUATION: it
        // fell through to the present comparison below, so a barrier recorded at present F was not
        // even LOOKED AT until `F + g_tj_shift` presents had passed -- and everything queued behind
        // it waited with it, past its own step.
        //
        // THE ONE-PRESENT `lead` BELOW WAS THIS SAME BUG, MEASURED AT SHIFT 1 AND PATCHED AT SHIFT 1.
        // The note under it says as much ("a barrier recorded at present F is not even looked at
        // until the replay reaches F"). It is not a constant: `g_tj_shift` is re-anchored to
        // `g_tj_idx + 1 - frame` every time a barrier is consumed, so a barrier evaluated late
        // ENLARGES the shift, which defers the next barrier further -- a feedback loop. MEASURED at
        // the residue this fixture had left, steps 29,069..29,531 of spcamp-solo, `--ui-tj-trace`:
        //
        //     idx=35868  M  frame=29333  due=29068  ->  step 29067   (the lead, working)
        //     idx=35870  P  frame=29334  a0 =29069                   <- barrier
        //     idx=35871  M  frame=29334  due=29069  ->  step 29070   (TWO steps late)
        //     idx=35872  M  frame=29340  due=29075  ->  step 29074   (the lead again)
        //
        // and the shift across that window grows 4 -> 5 -> 6. The record either side of the barrier
        // obeys the gate and the one behind it does not, which is the barrier deciding, not the gate.
        // At shift 1 that cost one step and the `lead` hid it; at shift 4 it costs two and nothing
        // does. This is the same defect the comment below already describes, and the same defect the
        // loop body's own MEASURED note describes -- fixed at the cause this time rather than at the
        // magnitude it happened to have.
        //
        // Returning true here does NOT proceed past the barrier: the loop body still holds on the
        // step, still counts `g_tj_waits`, still rebases on release, and is still bounded by the
        // stall hatch. All that changes is that the hold now begins when the queue reaches the
        // barrier instead of when the presents do, so the release lands on the STEP it names.
        if (g_tj_ev[i].seam == 'P') return true;
        if (g_tj_step_due && g_tj_step_due[i] && g_tj_ev[i].seam != 'S') {
            if (g_step + 1 >= g_tj_step_due[i]) return true;
            // THE TAIL'S SECOND GATE. Past the last `P` the extrapolated step may never arrive (the
            // recording ended by opening the menu, which stops the sim), so presents release it
            // instead. In-sim this is dead code: the step gate above fires a step EARLIER than this
            // one, exactly as the drain lead intends. See tj_load's note on g_tj_last_p.
            if (g_tj_last_p >= 0 && i > g_tj_last_p)
                return (int64_t)g_tj_ev[i].frame + g_tj_shift <= (int64_t)g_tj_idx;
            return false;
        }
        // THE `lead` IS NOW DEAD FOR 'P' and is kept only so the expression reads the same for the
        // seams that still take this path ('S' and any record without a step_due). See the block
        // above: 'P' never reaches this comparison any more, because gating a step barrier on a
        // present count was the defect rather than the fix.
        //
        // THE REBASE STILL ADDS THE LEAD BACK, and still must. `g_tj_shift` is re-anchored to
        // `g_tj_idx + 1 - frame` when a barrier is consumed; the barrier is released one step (and
        // so, at fixed_step, one present) before the step it names, exactly as the injection gate
        // above releases records at `due - 1`. The `+1` in the body compensates for that lead, not
        // for this comparison, so it survives the change unaltered.
        const int64_t lead = (g_tj_ev[i].seam == 'P') ? 1 : 0;
        return (int64_t)g_tj_ev[i].frame + g_tj_shift <= (int64_t)g_tj_idx + lead;
    };
    while (g_tj_next < g_tj_n && tj_due(g_tj_next)) {
        // UI-REC: THE SCREEN BARRIER. A journal's frame numbers are an ORDERING, not a schedule --
        // the menu's readiness is decided by asynchronous loading, not by a present count -- so the
        // head record is held here until the game is actually on the screen the recording was on.
        // Everything behind it waits, which is the point: the records after a barrier are the clicks
        // aimed at that screen.
        //
        // THE STEP BARRIER is its in-game twin and needs none of the identity machinery: the sim step
        // IS the world's clock, so "hold until the sim is where the recording was" is exact. It is
        // also UNBOUNDED on purpose, unlike the screen barrier -- a screen may never arrive, but the
        // step count only stops when the sim does, and a run whose sim has stopped is over anyway
        // (stop_step / the idle watchdog end it). Rebases like a screen barrier, so the records behind
        // it keep their recorded spacing relative to the sync point rather than to the run's start.
        if (g_tj_ev[g_tj_next].seam == 'P') {
            // ONE STEP OF LEAD, for the same reason tj_due carries it -- see THE DRAIN LATENCY above.
            // A record injected on frame N is consumed on N+1, so a record due at step S must be
            // injected at S-1; tj_due does that. But a `P` barrier sits BETWEEN records, and one that
            // released only at S held everything behind it until S, so those records drained at S+1
            // and arrived a step late -- with tj_due's correction intact and invisible, because the
            // barrier, not the gate, was deciding.
            //
            // MEASURED on spcamp 20260907_201620, where nine of ten early orders landed on the
            // recorded step and ONE did not:
            //     1366  C  due=695  ->  step 694      (tj_due's lead, working)
            //     1367  P 964 697                     <- barrier
            //     1368  P 972 705                     <- barrier: held to 705
            //     1369  M  due=705  ->  step 705      (should have been 704; drained at 706)
            //     1372  M  due=711  ->  step 710      (no barrier in front; correct again)
            // The two records either side of the barrier obey the lead and the ones behind it do not,
            // which is the barrier's release condition and nothing else. A sporadic +1 rather than a
            // systematic one is the signature of a gate that is right being overruled by one that is
            // not.
            if (g_step + 1 < g_tj_ev[g_tj_next].a0) {
                // ---- THE STALL HATCH: a `P` barrier waits for the SIM, and the sim can stop -----
                // This hold used to be UNBOUNDED on purpose, reasoned as "the step count only stops
                // when the sim does, and a run whose sim has stopped is over anyway (stop_step / the
                // idle watchdog end it)". That reasoning has a hole, and MEASURED 2026-09-07 the hole
                // is a hard deadlock: the idle watchdog is gated on `g_tj_done`, so it cannot fire
                // while records remain -- and a sim that stops BEFORE the journal is exhausted leaves
                // this barrier waiting forever. The all-original arm of spcamp-solo sat presenting
                // frames at step 29,999 with the sim halted and 44 recorded steps still ahead of it,
                // burning the runner's whole wall budget and then being KILLED, which loses the
                // buffered log and every number in it.
                //
                // WHAT BOUNDS IT: no sim step for TJ_IDLE_MS while this barrier holds. Not a present
                // count -- headless runs present hundreds of times a second and a legitimate in-game
                // pause would trip it. The predicate is exactly the one the idle exit already uses
                // for "the sim has stopped"; all that changes is that it no longer requires the
                // journal to be finished first.
                //
                // AND YES, IT IS WALL CLOCK, deliberately, and it is NOT the G146 trap. G146 was
                // about wall time deciding WHEN INPUT LANDS, which feeds the simulated world. This
                // decides nothing about the world: it fires only when the run is already broken, and
                // turns a hang into a reported force -- the same role SPCAMP-SYNC's scope protects
                // for the `S` barrier's hatch ("a bounded, reported force is what turned a hang into
                // a verdict, and it stays whatever the identity becomes").
                if (g_tj_last_step_ms && (GetTickCount() - g_tj_last_step_ms) >= TJ_IDLE_MS) {
                    char sb[200];
                    wsprintfA(sb,
                              "; TJ STEPS: FORCED past step barrier at record %d (want step %lu, "
                              "sim stuck at %lu for %ums) -- the sim stopped with journal left\n",
                              g_tj_next, (unsigned long)g_tj_ev[g_tj_next].a0, g_step, TJ_IDLE_MS);
                    append_line(g_log_path, sb);
                    ++g_tj_forced;
                    g_tj_shift = (int64_t)g_tj_idx + 1 - (int64_t)g_tj_ev[g_tj_next].frame;
                    ++g_tj_next;
                    ++g_tj_barriers;
                    continue;
                }
                ++g_tj_waits;
                break;
            }
            // +1: the lead in tj_due let this barrier be consumed a present early, and the schedule
            // must be anchored to where the barrier BELONGS, not to where we reached it. See the
            // drift note on `lead` above.
            g_tj_shift = (int64_t)g_tj_idx + 1 - (int64_t)g_tj_ev[g_tj_next].frame;
            ++g_tj_next;
            ++g_tj_barriers;
            continue;
        }
        if (g_tj_ev[g_tj_next].seam == 'S') {
            // SETTLED, not merely present. The screen VA changes when the panel is CREATED, which is
            // before it has finished sliding in -- so releasing on the VA alone puts the click into a
            // moving panel whenever the replay runs at a different frame rate than the recording
            // (measured: the walk reached a different screen on each attempt, further along when
            // frame capture happened to slow it down). ui_drive's own predicate is a wall-clock
            // stillness test for exactly this reason; the barrier uses the same one.
            // MATCH ON THE CONTENT SIGNATURE when the journal carries one. The container VA is the
            // legacy path and it is genuinely weak: `*ACTIVE_DIALOG ?: *MENU_LIST` gives every
            // no-dialog screen the same value, so on a real session 10 of 17 barriers could not be
            // confirmed and two arms replaying one journal played different games.
            //
            // THREE IDENTITIES, BEST FIRST, decided by what the journal recorded (SPCAMP-SYNC):
            //   a4  the CONTENT id      -- widget count + per-widget value/disp_idx/flags/label
            //   a3  the geometry sig    -- resolved centers; correct only on a screen that holds still
            //   a0  the container VA    -- the legacy path, and genuinely weak
            // The geometry rung is kept only so journals recorded before the content id still replay
            // exactly as they did. It is not a lesser version of the same idea: on a screen with any
            // moving decoration it hashes a MOMENT, so both `live == want` and the settle test below
            // are false forever and the barrier is forced by the timeout every time.
            const uint32_t id_want  = g_tj_ev[g_tj_next].a4;
            const bool     have_sig = g_tj_ev[g_tj_next].a3 != 0;
            const uint32_t live     = id_want    ? MH_UIDrive_ScreenId()
                                      : have_sig ? MH_UIDrive_ScreenSig()
                                                 : MH_UIDrive_ActiveScreen();
            const uint32_t want     = id_want    ? id_want
                                      : have_sig ? g_tj_ev[g_tj_next].a3
                                                 : g_tj_ev[g_tj_next].a0;
            const bool     matched  = live == want && (id_want ? MH_UIDrive_ScreenIdSettled() != 0
                                                               : MH_UIDrive_ScreenSettled() != 0);
            // BOUNDED, and the earlier "no timeout, deliberately" was wrong -- not about hiding
            // failures, but about what this identity can promise. active_list() is
            // `*ACTIVE_DIALOG ?: *MENU_LIST`, which is the CONTAINER, not the screen: several menu
            // screens share one list, and a dismissed dialog's pointer can still be live for a few
            // frames. Measured on a real session at the new-game -> race-picker step: the replay was
            // ON the race picker (captured the frame to be sure) while this comparison still read the
            // dialog, so an unbounded barrier hung a replay that was doing exactly the right thing.
            //
            // So: wait, then proceed and SAY SO. A forced barrier is reported per occurrence and
            // counted in the verdict -- if proceeding was wrong the hashes diverge and the count says
            // where to look, which is strictly more than a hang says. The bound is WALL time for the
            // reason settle_ms is: presents are not a clock. It is measured from the last CHANGE in
            // the live screen rather than from the barrier's start -- see TJ_BARRIER_MS.
            if (!matched && live != g_tj_hold_live) { // progress: a different screen is up now
                g_tj_hold_live  = live;
                g_tj_hold_since = GetTickCount();
            }
            if (!matched && g_tj_hold_since && (GetTickCount() - g_tj_hold_since) >= TJ_BARRIER_MS) {
                char fb[180];
                wsprintfA(fb,
                          "; TJ SCREENS: FORCED past record %lu (frame %lu) after %ums -- wanted"
                          " %08X, live %08X. Proceeding; a divergence after this names the cause.\n",
                          g_tj_next, g_tj_ev[g_tj_next].frame, TJ_BARRIER_MS,
                          want, live);
                append_line(g_log_path, fb);
                // WHAT THE REPLAY IS ACTUALLY LOOKING AT. Two hashes that disagree say a barrier
                // failed and nothing about why; the widget list says which screen the replay is on
                // and what is different about it. This is the only moment that information exists,
                // and it goes to mh_uidrive.log beside the script's own dumps.
                MH_UIDrive_DumpWidgets();
                log_flush();
                ++g_tj_forced;
                g_tj_hold_since    = 0;
                g_tj_hold_live     = 0;
                g_tj_hold_identity = false;
                g_tj_shift         = (int64_t)g_tj_idx - (int64_t)g_tj_ev[g_tj_next].frame;
                ++g_tj_next;
                continue;
            }
            if (!matched) {
                g_tj_hold_identity = (live != want); // see the flag's own comment
                if (!g_tj_hold_since) {
                    g_tj_hold_since = GetTickCount();
                    g_tj_hold_live  = live;
                }
                // SAY IT ONCE, AND FLUSH IT. The log is buffered and only flushed at a report, so a
                // replay that stalls at a barrier is killed by the runner's wall clock with an EMPTY
                // log -- which reads exactly like "the arm never armed" and cost two rounds of
                // diagnosis. One line per barrier, forced out, so a stall always names itself.
                if (!g_tj_held_at || g_tj_held_at != g_tj_next) {
                    g_tj_held_at = g_tj_next;
                    char hb[160];
                    wsprintfA(hb,
                              "; TJ SCREENS: holding at record %lu (frame %lu) -- want screen %08X,"
                              " live %08X\n",
                              g_tj_next, g_tj_ev[g_tj_next].frame, want, live);
                    append_line(g_log_path, hb);
                    log_flush();
                }
                ++g_tj_waits;
                break;
            }
            // REBASE, so a barrier costs synchronisation and not spacing. Without this, everything
            // queued behind a barrier fires in the single present that released it -- the clicks
            // aimed at one screen would arrive as a burst instead of at the intervals the recording
            // has, and a second click can land before the first one's screen has rebuilt (the widget
            // list is rebuilt on activation; that is the documented reason scripts gate between
            // clicks). Shifting the whole remaining journal by the barrier's own lateness preserves
            // every later interval exactly as recorded.
            g_tj_hold_since    = 0; // matched: the next barrier starts its own patience budget
            g_tj_hold_live     = 0;
            g_tj_hold_identity = false;
            g_tj_shift         = (int64_t)g_tj_idx - (int64_t)g_tj_ev[g_tj_next].frame;
            // THE CLOCK IS RECORDED HERE AND DELIBERATELY NOT APPLIED -- see the barrier's own
            // comment in tji_record for what it is, and this for why re-pinning it was reverted.
            //
            // Re-pinning g_pin_now to the recorded value at each barrier looks like the obvious fix
            // for the one thing this round trip does not reproduce (the wall-clock regions). It was
            // tried and MEASURED, and it breaks the replay outright: a replay that reaches a screen
            // in FEWER presents than the recording did arrives with a clock ahead of the recorded
            // one, so the re-pin moves GetCurrentTime BACKWARDS -- and the menu's own timers compute
            // now-minus-last, so the walk stops completing at all. It turned a run whose sim state
            // was identical on all 200 steps into one that never reached a live game.
            //
            // The value is still journalled because it costs nothing and any real fix needs it. The
            // shape a fix has to have: correct the clock ONCE, at a boundary both runs agree on
            // (session start), rather than repeatedly at points where either run may be ahead.
            ++g_tj_next;
            ++g_tj_barriers;
            continue;
        }
        const int       e_idx = g_tj_next;
        const tj_entry &e     = g_tj_ev[g_tj_next++];
        // tj_trace: where does each early record ACTUALLY land? (see the config field)
        if (g_cfg.tj_trace && e_idx >= g_cfg.tj_trace_from && e_idx < g_cfg.tj_trace) {
            char tm[160];
            wsprintfA(tm, "; TJTRACE idx=%d seam=%c frame=%lu due=%lu step=%lu idx_now=%lu shift=%d\n",
                      e_idx,
                      (char)e.seam, e.frame, g_tj_step_due ? g_tj_step_due[e_idx] : 0u, g_step,
                      g_tj_idx, (int)g_tj_shift);
            append_line(g_log_path, tm);
        }
        if (input_only && (e.seam == 'E' || e.seam == 'G' || e.seam == 'D')) {
            ++g_tj_skipped;
            continue;
        }
        if (e.seam == 'E')
            mh::tact::unit_enqueue_command((int32_t)e.a0, (int32_t)e.a1, e.iflag,
                                           (int32_t)e.a2, (uint16_t)e.a3,
                                           (uint16_t)(e.a4 & 0xffffu),
                                           (uint16_t)(e.a4 >> 16));
        else if (e.seam == 'D')
            tj_apply_direct(e.a0, e.a1, e.a2); // a direct write: unit, field id, value
        else if (e.seam == 'M')
            tji_inject_mouse(e.a0, e.a1, (int32_t)e.a2, (int32_t)e.a3, (int32_t)e.a5, (int32_t)e.a6,
                             (int32_t)e.a7, e.a4);
        else if (e.seam == 'K')
            tji_inject_key(e.a0, e.a1, e.a2);
        else if (e.seam == 'C')
            tji_set_cursor((int32_t)e.a0, (int32_t)e.a1);
        else
            mh::tact::group_issue_order((int32_t)e.a0, e.a1, e.a2, e.a3, e.a4);
        ++g_tj_issued;
    }
    // THE REPORT BELONGS AT THE END OF THE JOURNAL, not only at tact_stop_step or exit_at. A replay
    // of a human session is longer than any wall a probe was sized for, so the run is routinely
    // killed after it has done everything the journal asked -- and a verdict that reads the missing
    // report as "killed before it could report" then fails a replay that actually completed. Fire
    // once, the moment the last record is consumed.
    if (g_tj_ev && !g_tj_done && g_tj_next >= g_tj_n) {
        g_tj_done = true;
        char l[200];
        wsprintfA(l, "; TJ REPLAY COMPLETE: journal exhausted at frame %lu\n", g_tj_idx);
        append_line(g_log_path, l);
        tj_report();
        // UI-REC: a game-start journal has no other end condition. The tactical arm is bounded by
        // tact_exit_at (frames) and the strategic one by stop_step (sim steps), but a journal that
        // walks the MENU may never reach a single sim step -- so a replay of one would sit at
        // whatever screen it ended on until the runner's wall-clock timeout, and report a TIMEOUT
        // for a replay that did everything asked of it. stop_step, when set, still wins: that is the
        // A/B shape, where the journal starts a game and the steps AFTER it are the comparison.
        if (ui_journal_armed() && g_cfg.stop_step == 0) {
            append_line(g_log_path,
                        "; UI-REC EXIT: journal exhausted and stop_step=0 -- nothing further to "
                        "measure\n");
            log_flush();
            TerminateProcess(GetCurrentProcess(), 0);
        }
    }
    // Entries for a frame the run never reached are not silently forgotten -- the verdict reports
    // issued vs loaded, and a short replay is exactly how a journal can look green having done
    // almost nothing.
}

// ---- UI-REC: the present-cadence arm of the input journal ---------------------------------------
//
// The same recorder and the same replayer as the tactical arm, driven from a hook that runs in EVERY
// game mode. What it does NOT do is as deliberate as what it does: no tji_probe_tick, no
// tj_watch_direct, no tj_watch_queue and no census. All four read mh_llm_tact_unit_record -- the
// tactical roster -- which does not exist outside mode 6, so running them here would sweep whatever
// happens to be at that address in the menu and journal it as gameplay.
//
// So a UI journal carries M/K/C only. That is not a reduced version of the tactical journal, it is
// the whole of the design the tactical one arrived at: input is a CLOSED interface, and everything
// downstream is re-caused rather than reproduced (see the TACT-REC block above). The tactical arm
// keeps its E/G/D records because they are the ASSERTION half -- a semantic check that the replay
// re-emitted what the human's session did. The equivalent assertion for a game-start recording is
// the strategic per-step hash stream, which the harness already emits.
//
// ORDERING: this runs at present-flip, i.e. at the END of the frame, so an event recorded here
// entered the ring during the frame that just rendered, and an event injected here is drained by the
// NEXT frame. Both directions carry the same one-frame offset, so the sequence and spacing a replay
// reproduces are the recording's -- the same uniform-shift argument the tactical arm's one-frame
// convention rests on, applied at the other end of the frame.
void ui_journal_present() {
    if (!ui_journal_armed()) return;
    g_tj_idx = ++g_ui_step;
    // The menu's missing clock cadence -- and ONLY the menu's. In-game one of the two site advanced
    // already this frame, so this is a no-op there and an in-game frame is unchanged.
    //
    // ---- G146: A PRESENT SPENT WAITING FOR WORK IS NOT A PRESENT THE RECORDING HAD ---------------
    // A barrier holds for one of two reasons and only ONE of them belongs in this clock.
    //
    // `live != want` -- the game has not built the screen yet: a load, a transition, real work that
    // takes WALL time. The presents that costs scale with the frame rate, so letting the clock run
    // through them injects a rate-dependent lump into a counter the sim reads. That is the leak, and
    // g_tj_hold_identity is exactly this case.
    //
    // `live == want && !settled` -- only the settle dwell is outstanding. That dwell is now measured
    // in THIS clock (ui_drive settle_now, armed beside pin_menu_clock), so it is already a fixed
    // number of presents at any frame rate -- and freezing here would stop the counter the wait is
    // waiting on, i.e. deadlock. It must keep advancing.
    //
    // tj_replay_frame ALREADY corrects the input schedule for a hold -- g_tj_shift rebases the whole
    // remaining journal by the barrier's lateness. This is that same correction applied to the clock,
    // which is the half the rebase's own comment records as deliberately left undone. Withholding an
    // advance can only ever make the clock LATER, never earlier, so the failure that killed the
    // reverted re-pin (a run arriving early moved GetCurrentTime BACKWARDS, and the menu's
    // now-minus-last timers stopped completing) cannot arise here.
    //
    // The gate reads the hold state as of the previous present, so the frame a barrier is FIRST seen
    // unsatisfied still advances -- one present per barrier, in both arms alike, a constant rather
    // than a rate dependence. In-game is untouched: there g_pin_moved is already set by on_sim_tick,
    // so this whole line is a no-op whether a barrier is held or not.
    if (g_cfg.pin_wallclock && !g_pin_moved && !g_tj_hold_identity) g_pin_now += g_pin_dt;
    g_pin_moved = false;
    // TJPS: the present/step pair, sampled on the SIM's cadence so it lines up one-for-one with the
    // recorder's own `TJ P` rows and the two can be subtracted directly. Sampled BEFORE
    // tj_replay_frame, so the pair describes the state this frame's injections are about to see.
    if (g_cfg.tj_ps_log > 0 && g_tj_ev) {
        static uint32_t s_last_ps = 0xffffffffu;
        if (g_step != s_last_ps && (g_step % (uint32_t)g_cfg.tj_ps_log) == 0) {
            s_last_ps = g_step;
            // WHAT THE ROW CARRIES, and why each field is on it: see the probe block beside
            // g_probe_iu. `pend` is the panel refresh latch llm_strat_ui_storage_bldg_panel
            // tests-and-clears -- the flag that gates the order issuing, and the one already
            // observed to differ between runs. The deferred-event ring's defer/q left the row once
            // they had been measured at 0/never-moving in every sample of every run.
            const uint32_t ui_pend =
                *(volatile uint32_t *)mh::addr::_G_LLM_STRAT_UI_BLDG_PANEL_REFRESH_PENDING;
            char psb[256];
            wsprintfA(psb,
                      "; TJPS %lu %lu iu=%lu pn=%lu kw=%lu kr=%lu mw=%lu mr=%lu tk=%lu cx=%ld"
                      " cy=%ld pend=%lu fs=%lu fo=%lu\n",
                      g_tj_idx, g_step, g_probe_iu, g_probe_pn,
                      *(volatile uint32_t *)mh::addr::_G_LLM_INPUT_KEY_WRITE_IDX,
                      *(volatile uint32_t *)mh::addr::_G_LLM_INPUT_KEY_READ_IDX,
                      *(volatile uint32_t *)mh::addr::_G_LLM_INPUT_MOUSE_WRITE_IDX,
                      *(volatile uint32_t *)mh::addr::_G_LLM_INPUT_MOUSE_READ_IDX,
                      *(volatile uint32_t *)mh::addr::_G_LLM_TIME_TICKS_MS,
                      *(volatile int32_t *)mh::addr::_G_LLM_CURSOR_X,
                      *(volatile int32_t *)mh::addr::_G_LLM_CURSOR_Y, ui_pend, g_tj_slots_seen,
                      g_tj_foreign_n);
            append_line(g_log_path, psb);
        }
    }
    // ---- WHOSE EVENT IS IN THE RING? The producer census, and it names the flake ------------------
    //
    // MEASURED 2026-09-08: a divergent run's mouse-ring WRITE cursor runs AHEAD of a clean run's --
    // 66 -> 68 at step 13 while the clean run stayed at 66 -- and the game cursor jumps from the
    // journal's (322,136) to (0,479) in the same step. Two events entered that ring which the
    // journal did not put there. `llm_input_wndproc_tap` is the ring's SOLE producer entry and it
    // runs on EVERY window message, while the frame (and therefore the drain) runs once per
    // WM_PAINT -- so what lands in a given frame's drain is decided by the OS message schedule,
    // which is exactly the thing machine load moves.
    //
    // This turns that inference into an identification. The recorder already marks every slot the
    // harness itself wrote (g_tji_own_m, for the opposite purpose -- so a recording does not
    // journal our own injections), and nothing clears those bits during a replay because the
    // recorder is not running. So a slot that appeared since the last present and is NOT ours came
    // from the game's own producer, and dumping it prints the foreign event's whole content --
    // including its timestamp, which is real GetTickCount-derived time rather than a replayed one.
    // Bounded per present so a storm cannot fill the log.
    if (g_cfg.tj_ps_log > 0 && g_tj_ev) {
        static uint32_t s_seen_mw = 0xffffffffu;
        const uint32_t  mw        = *(volatile uint32_t *)mh::addr::_G_LLM_INPUT_MOUSE_WRITE_IDX;
        if (s_seen_mw == 0xffffffffu) s_seen_mw = mw;
        for (uint32_t n = 0; s_seen_mw != mw && n < 16; ++n) {
            const uint32_t slot = s_seen_mw;
            s_seen_mw           = (s_seen_mw + 1) & 0x7f;
            ++g_tj_slots_seen;
            // THE POSITIVE CONTROL. "0 foreign events" is only evidence if the scan was looking at
            // anything, and a scan that classifies every slot as ours looks identical to one whose
            // ownership test is inverted or whose cursor never moves. So both halves are counted and
            // both are reported: ours + foreign must add up to the slots walked, and the total must
            // be in the thousands for a 9,000-step replay.
            if (g_tji_own_m[(slot >> 5) & 3] & (1u << (slot & 31))) {
                ++g_tj_slots_ours;
                continue;
            }
            if (++g_tj_foreign_n > 64) break; // bounded: the first 64 are the evidence
            const auto *e = (const mh::game::mh_llm_input_mouse_event *)(TJI_M_EVENTS +
                                                                         (size_t)slot * 0x38u);
            char        fb[220];
            wsprintfA(fb,
                      "; TJFOREIGN #%lu step=%lu present=%lu slot=%lu type=%lu btn=%lu x=%ld y=%ld"
                      " dx=%ld dy=%ld ts=%lu\n",
                      g_tj_foreign_n, g_step, g_tj_idx, slot, e->event_type, e->buttons,
                      (long)(int32_t)e->x, (long)(int32_t)e->y, (long)e->dx, (long)e->dy,
                      e->timestamp);
            append_line(g_log_path, fb);
        }
    }
    if (g_tj_ev) tj_replay_frame();
    // Hold what the recording held. See tji_keystate_reassert: a DirectInput keyboard poll re-latches
    // the keystate array from a device on which nothing is pressed, so a key the journal says is down
    // has to be re-asserted rather than written once.
    if (g_tj_ev) tji_keystate_reassert();
    // The recorder is refused alongside a replay at arm time, so these two never both fire.
    if (g_cfg.ui_journal_rec) tji_record();

    // ---- WHY IS THE QUEUE NOT MOVING? The stuck-head report ---------------------------------------
    //
    // MEASURED 2026-09-08: an A/B/C arm did ~3 minutes of real work and then sat for TWELVE MINUTES
    // presenting menu frames (98.4% of its wall time in game_mode 3, 1.79M frames) until the runner's
    // wall clock killed it. The recording ends with the player opening the menu, which stops the sim
    // -- so the replay reaches a `P` barrier naming a step the sim will never take again. Both hatches
    // that exist for exactly this (`TJ STEPS: FORCED`, `TJ SCREENS: FORCED`) logged NOTHING, and the
    // idle exit below never fired, so the run was diagnosed only by comparing file mtimes between two
    // logs. That is not a diagnosis a harness should make its operator perform.
    //
    // So: while the journal is NOT exhausted and the cursor has not moved for TJ_IDLE_MS, say so, and
    // say what the head record is and what it is waiting for. Bounded and rate-limited -- this is a
    // report about a run that is already stuck, and a stuck run has plenty of presents to spare.
    if (g_tj_ev && !g_tj_done) {
        static uint32_t s_stuck_next = 0xffffffffu, s_stuck_since = 0, s_stuck_said = 0;
        const uint32_t  now = GetTickCount();
        if (s_stuck_next != g_tj_next) { // the queue moved -- rearm
            s_stuck_next  = g_tj_next;
            s_stuck_since = now;
        } else if (s_stuck_since && (now - s_stuck_since) >= TJ_IDLE_MS && s_stuck_said < 8) {
            ++s_stuck_said;
            s_stuck_since = now; // one line per TJ_IDLE_MS, not one per present
            const auto &h = g_tj_ev[g_tj_next];
            char        sb[256];
            wsprintfA(sb,
                      "; TJ STUCK #%lu: record %lu of %lu (seam=%c frame=%lu a0=%lu) has not moved"
                      " for %ums. step=%lu present=%lu shift=%ld mode=%u -- the journal is NOT"
                      " exhausted and nothing is being issued.\n",
                      s_stuck_said, g_tj_next, g_tj_n, (char)h.seam, h.frame, h.a0, TJ_IDLE_MS,
                      g_step, g_tj_idx, (long)g_tj_shift,
                      (unsigned)*(volatile uint8_t *)mh::addr::_G_LLM_GAME_MODE);
            append_line(g_log_path, sb);
        }
    }

    // THE IDLE EXIT. Only once the journal is exhausted, so it can never cut a replay short: while
    // records remain, a quiet sim is just a menu the recording was sitting in. After that, a sim that
    // has not stepped for TJ_IDLE_MS means the run has produced everything it is going to, and
    // waiting for a `stop_step` that cannot arrive only costs the runner's whole wall budget --
    // AND LOSES THE LOG, because the buffered log is flushed at reports and a killed process writes
    // none of them. Exiting here reports first, which is the difference between a diagnosable run
    // and an empty file.
    if (g_tj_ev && g_tj_done && g_tj_last_step_ms &&
        (GetTickCount() - g_tj_last_step_ms) >= TJ_IDLE_MS) {
        char b[200];
        wsprintfA(b,
                  "; UI-REC EXIT: journal exhausted and no sim step for %ums (last step %lu) --"
                  " the replay ended outside the strategic sim, so stop_step can never fire\n",
                  TJ_IDLE_MS, g_step);
        append_line(g_log_path, b);
        // No once-latch needed (and g_tj_verdict_done is not declared this early): the process is
        // terminated on the next line, so nothing can report twice.
        tj_report();
        log_flush();
        TerminateProcess(GetCurrentProcess(), 0);
    }
}

// ---- TACT-SYNTH: the synthetic tactical order workload -------------------------------------------
//
// The instrument the coverage caveat asked for. The config block at the top of this file carries WHY
// it is a workload rather than a recording and why it has two entry points; this is what it does.
//
// WHAT IT DRIVES, stated precisely, because a coverage claim that is not measured is decoration.
// There are 22 llm_tact_unit_enqueue_command call sites. 12 are input-independent -- the mission
// script (llm_tact_mission_load), its REPEAT resubmit, the projectile facing correction and 9 in
// llm_tact_unit_owner_tick -- and they already run with zero input. The other 10 are PLAYER-command
// sites, split across three functions:
//   * llm_tact_frame x6            -- op 9 (the M key: arm a mine), op 6 (continuous cursor-facing),
//                                     and two STOP+KNEEL / STOP+STAND pairs fired by right-clicking
//                                     a hovered, already-selected, own unit.
//   * llm_tact_group_issue_order x3 -- the auto-stop, and the busy/idle interrupt_flag pair.
//   * llm_tact_ui_sel_panel_multi_mode_tick x1 -- the sidebar def_stat cycle's 3->4 transition.
// Sites 1-9 write NO persistent state besides their enqueue call (their only extra writes are
// frame-local UI arbitration flags such as _G_LLM_TACT_CLICK_ACTION_TAKEN), so issuing the same
// arguments through the same API reproduces their effect exactly. Site 10 does not fit that shape --
// see the direct arm below.
//
// THE STALL TRAP, and it is why there is a whitelist rather than a random op draw. Enqueue validates
// almost nothing: an op with no case in llm_tact_unit_weapons_tick's dispatch (0xc-0x1b, 0x20-0x3f,
// 0x41-0x7e) is accepted, returns 1, and then sits at the queue head FOREVER, because nothing ever
// calls cmd_advance for it -- the unit is permanently wedged and the run still looks green. Every op
// this workload issues has a proven tick-side handler (the weapon-FX notes, the op table).
//
// WHY interrupt_flag IS ALWAYS 0. It is the polarity every player-input call site uses, and it is
// the only one that cannot be refused: enqueue's head-busy gate rejects an interrupt_flag=1 call
// outright while the unit's head command is a live player-issued one. A workload half of whose
// orders were silently refused would be reporting its own scheduling, not the game's behaviour.
constexpr uintptr_t ADDR_TACT_MAP_W = mh::addr::_G_LLM_TACT_MAP_WIDTH_CACHE;
constexpr uintptr_t ADDR_TACT_MAP_H = mh::addr::_G_LLM_TACT_MAP_HEIGHT_CACHE;

// The actions, in the fixed rotation the workload cycles through. A ROTATION rather than a seeded
// draw over the same set: the seed varies the ARGUMENTS (which unit, which tile, which direction),
// but which capabilities get exercised must not be left to a die roll, or a short run reports
// coverage it merely happened to reach.
//
// THE ORDER IS LEVEL DESIGN, not tidiness, and one arrangement of it was measurably wrong. STAND
// (llm_tact_frame sites 5+6) can only be issued to a unit in anim_state 3, and a unit only reaches
// 3 after llm_tact_unit_kneel_tick has advanced `progress` past 15 -- sixteen uninterrupted frames
// after the KNEEL lands. The first arrangement put GROUP_STOP one slot after KNEEL, so every kneel
// was cancelled six frames in and op 5 was issued ZERO times in a 400-frame run while the coverage
// table happily listed "stand" among the actions taken. So KNEEL is now followed only by actions
// that do NOT touch the command queue -- stance toggles and the three direct writes -- and STAND
// comes four slots later, 24 frames at the default cadence. The interrupting group orders sit after
// it. (The AI cannot cancel it either: owner_tick issues with interrupt_flag=1, which enqueue's
// head-busy gate refuses while a player-issued command is at the head.)
enum tact_action {
    TA_GROUP_MOVE = 0, // llm_tact_group_issue_order(1)    -- the primary player order
    TA_FACE,           // enqueue op 6                     -- llm_tact_frame site 2 (cursor facing)
    TA_GROUP_ATTACK,   // llm_tact_group_issue_order(2)    -- left-click attack; op 2 short-circuits
    TA_KNEEL,          // enqueue op 0x7f + 4              -- llm_tact_frame sites 3+4
    TA_STANCE_ON,      // enqueue op 0x1f                  -- stance toggle (direct-write op)
    TA_GUNSWAP,        // DIRECT: active_gun ^= 1 -- no opcode exists for this
    TA_CTRLGROUP,      // DIRECT: the +0x5f0 squad slot -- no opcode exists for this either
    TA_STAND,          // enqueue op 0x7f + 5              -- llm_tact_frame sites 5+6
    TA_GROUP_STOP,     // llm_tact_group_issue_order(0x7f) -- the sidebar STOP button
    TA_MINE,           // enqueue op 9                     -- llm_tact_frame site 1 (the M key)
    TA_TELEPORT,       // enqueue op 0xa                   -- the teleport jump (see the note below)
    TA_STANCE_OFF,     // enqueue op 0x1d                  -- the other half of the toggle pair
    TA_DEFSTAT,        // DIRECT: the sidebar def_stat cycle + its 3->4 enqueue (site 10)
    TA_RUNMODE,        // llm_tact_group_issue_order(0x40) then (0x47) -- the sidebar RUN button,
                       // which issues the PAIR; 0x47's forward scan is what reads 0x40's marker, so
                       // issuing either alone would exercise half a mechanism.
    TA_GROUP_RESET,    // llm_tact_group_issue_order(0x46) -- the sidebar RESET button (queue wipe)
    TA_COUNT
};
const char *const TACT_ACTION_NAMES[TA_COUNT] = {
    "group_move", "face", "group_atk", "kneel", "stance_on", "gunswap", "ctrlgroup",
    "stand", "group_stop", "mine", "teleport", "stance_off", "defstat", "runmode",
    "group_reset"};
// Which actions belong to the DIRECT arm -- the ones the funnel cannot express. Kept as data so the
// summary line can report funnel and direct coverage separately without a second bookkeeping path.
inline bool tact_action_is_direct(int a) {
    return a == TA_DEFSTAT || a == TA_GUNSWAP || a == TA_CTRLGROUP;
}

// The ops this workload can put on the wire, in a fixed order so the summary tally is positional and
// machine-readable. Every one has a tick-side handler.
constexpr int      TACT_OP_COUNT           = 13;
constexpr uint16_t TACT_OPS[TACT_OP_COUNT] = {1, 2, 4, 5, 6, 9, 0xa,
                                              0x1d, 0x1f, 0x40, 0x46, 0x47, 0x7f};

bool g_ts_failed   = false; // a guard tripped -> the run must not be read as a pass
bool g_ts_selected = false; // the group arm's population has been chosen
bool g_ts_summed   = false; // the summary line is emitted exactly once
int  g_ts_sel[16]  = {0};   // the selected unit ids -- the GROUP arm's population
int  g_ts_sel_n    = 0;
// Two units held OUT of the group, and this is not tidiness either. A group order reaches every
// selected unit, so anything long-running issued to a selected unit gets cancelled by the next
// group action. Two ops are long-running:
//   * KNEEL needs sixteen uninterrupted llm_tact_unit_kneel_tick advances before anim_state reaches
//     3, which is the ONLY state from which STAND (llm_tact_frame sites 5+6) may be issued. With the
//     stance pair inside the group, op 5 was issued zero times over 400 frames -- twice, under two
//     different rotation orders -- because a group STOP always landed first.
//   * MINE parks its unit in anim_state 4 (llm_tact_unit_mine_arm_tick is the sole writer of that
//     value) and, in this headless entry, never leaves it. A unit spending the run mid-arm is a
//     unit no stance action can ever use.
// A player can perfectly well kneel one soldier and mine with another while a squad manoeuvres, so
// dedicating a unit to each is faithful, not a workaround.
int      g_ts_stance_unit = 0; // KNEEL/STAND subject, outside the selection
int      g_ts_mine_unit   = 0; // MINE subject, outside the selection
uint32_t g_ts_orders      = 0; // enqueue / group calls issued
uint32_t g_ts_effective   = 0; // ... that measurably changed the target's command state
uint32_t g_ts_rejected    = 0; // ... that enqueue refused outright (returned 0)
uint32_t g_ts_inert       = 0; // ... accepted (returned 1) but changed nothing -- the silent
                               // no-op class enqueue's boolean return cannot report
uint32_t g_ts_direct  = 0;     // direct-arm writes that changed something
uint32_t g_ts_actions = 0;     // firing frames
uint32_t g_ts_skipped = 0;     // firing frames whose action found no unit in the right state and so
                               // issued NOTHING. Counted rather than left implicit: without it the gap
                               // between `actions` and `orders` reads as a bug in the tally, and a
                               // capability that never fires because its precondition never held is
                               // indistinguishable from one that fired and did nothing.
uint32_t g_ts_op_tally[TACT_OP_COUNT] = {0};
uint32_t g_ts_act_tally[TA_COUNT]     = {0};

// A unit that exists and is not mid-death. `type` is the roster occupancy byte (0 = empty slot) and
// anim_state 0x1f is the dying animation; ordering either would be a silently-ignored call, i.e. the
// inert class this instrument exists to COUNT rather than to manufacture.
inline bool tact_unit_live(const mh::game::mh_tact_unit_record &u) {
    return u.type != 0 && u.hp > 0 && u.anim_state != TACT_ANIM_DYING;
}

// The command-state signature of one unit: the bytes an order is supposed to move, and NOT the
// per-frame timers. Two disjoint, timer-free ranges -- [0x02,0x0b) is status/pos/anim_state/
// def_stat/facing, and [0x53,0x5f4) is cmd_index + the 128-entry queue + the two immediate records
// (ATTACK/AIM at +0x5d4, FACE/TURN at +0x5df) + move_aborted_op + active_gun + ammo + the squad
// slot at +0x5f0. Folding in the doubles at +0x0b/+0x13/+0x1b would make the before/after test pass
// unconditionally, because those advance every frame -- and a vacuity guard that is always green is
// the exact failure this file keeps having to design around. Safe to include fields a tick also
// writes (ammo): the two snapshots straddle one synchronous call with no frame boundary between
// them, so any difference is ours.
uint64_t tact_cmd_sig(int idx) {
    const auto *base = reinterpret_cast<const uint8_t *>(&tact_units()[idx]);
    uint64_t    h    = fnv1a(base + 0x02, 0x0b - 0x02, 1469598103934665603ULL);
    return fnv1a(base + 0x53, 0x5f4 - 0x53, h);
}

// One order through the funnel, with its own before/after verdict. Returns enqueue's boolean.
int tact_synth_enqueue(int unit, uint16_t op, int32_t a0, uint16_t a1, uint16_t a2, uint16_t a3) {
    const uint64_t before = tact_cmd_sig(unit);
    const int      rc     = mh::tact::unit_enqueue_command(unit, op, 0, a0, a1, a2, a3);
    const uint64_t after  = tact_cmd_sig(unit);
    ++g_ts_orders;
    for (int i = 0; i < TACT_OP_COUNT; ++i)
        if (TACT_OPS[i] == op) ++g_ts_op_tally[i];
    if (rc == 0) ++g_ts_rejected;
    else if (after == before) ++g_ts_inert;
    else ++g_ts_effective;
    return rc;
}

// One group order. The whole selected set is the subject, so the verdict is over the set: at least
// one member must have changed, or the call did nothing and counting it as coverage would be false.
void tact_synth_group(uint16_t op, uint32_t a0, uint32_t a1, uint16_t a2, uint16_t a3) {
    uint64_t before = 1469598103934665603ULL, after = 1469598103934665603ULL;
    for (int i = 0; i < g_ts_sel_n; ++i) {
        const uint64_t s = tact_cmd_sig(g_ts_sel[i]);
        before           = fnv1a(&s, sizeof(s), before);
    }
    mh::tact::group_issue_order(op, a0, a1, a2, a3);
    for (int i = 0; i < g_ts_sel_n; ++i) {
        const uint64_t s = tact_cmd_sig(g_ts_sel[i]);
        after            = fnv1a(&s, sizeof(s), after);
    }
    ++g_ts_orders;
    for (int i = 0; i < TACT_OP_COUNT; ++i)
        if (TACT_OPS[i] == op) ++g_ts_op_tally[i];
    // llm_tact_group_issue_order returns its loop counter, not a status -- there is no rejected
    // class here, only effective vs inert.
    if (after == before) ++g_ts_inert;
    else ++g_ts_effective;
}

// Pick the group arm's population and mark it selected. status bit 0 IS the selection flag
// (llm_tact_group_issue_order's loop qualifies on `status & 1`), and every real selection path in
// llm_tact_frame gates that same write on owner == 0 -- so this is the write the game makes, not a
// back door around it. Re-asserted every firing frame: idempotent, and it survives anything that
// clears it.
void tact_synth_select() {
    auto     *u    = tact_units();
    const int want = g_cfg.tact_synth_units < 1
                         ? 1
                         : (g_cfg.tact_synth_units > 16 ? 16 : g_cfg.tact_synth_units);
    if (!g_ts_selected) {
        for (int i = 1; i < TACT_UNIT_MAX && g_ts_sel_n < want; ++i)
            if (tact_unit_live(u[i]) && u[i].owner == (uint8_t)g_cfg.tact_synth_owner)
                g_ts_sel[g_ts_sel_n++] = i;
        g_ts_selected = true;
        // The two out-of-group subjects, taken from the units the selection did not claim. Falling
        // back to a selected one when the squad is too small is deliberate: a smaller squad should
        // degrade the coverage (and say so in the skip lines) rather than disable the action.
        for (int i = 1; i < TACT_UNIT_MAX; ++i) {
            if (!tact_unit_live(u[i]) || u[i].owner != (uint8_t)g_cfg.tact_synth_owner) continue;
            bool taken = false;
            for (int k = 0; k < g_ts_sel_n; ++k) taken = taken || g_ts_sel[k] == i;
            if (taken) continue;
            if (!g_ts_stance_unit) g_ts_stance_unit = i;
            else if (!g_ts_mine_unit) g_ts_mine_unit = i;
        }
        if (!g_ts_stance_unit) g_ts_stance_unit = g_ts_sel_n ? g_ts_sel[g_ts_sel_n - 1] : 0;
        if (!g_ts_mine_unit) g_ts_mine_unit = g_ts_stance_unit;
        int live = 0, mine = 0;
        for (int i = 1; i < TACT_UNIT_MAX; ++i) {
            if (!tact_unit_live(u[i])) continue;
            ++live;
            if (u[i].owner == (uint8_t)g_cfg.tact_synth_owner) ++mine;
        }
        char msg[240];
        wsprintfA(msg,
                  "; TSYNTH roster frame=%lu live=%d owner%d=%d selected=%d first=%d stance_unit=%d "
                  "mine_unit=%d\n",
                  g_tact_step, live, g_cfg.tact_synth_owner, mine, g_ts_sel_n,
                  g_ts_sel_n ? g_ts_sel[0] : -1, g_ts_stance_unit, g_ts_mine_unit);
        append_line(g_log_path, msg);
        if (g_ts_sel_n == 0) {
            // VACUITY GUARD: with no player unit every order below would target nothing and the run
            // would still report a clean pass over a workload that never fired.
            append_line(g_log_path, "; TSYNTH FAIL: no live unit with the configured owner -- the "
                                    "workload has nothing to order\n");
            g_ts_failed = true;
        }
    }
    for (int i = 0; i < g_ts_sel_n; ++i) u[g_ts_sel[i]].status |= 1;
}

// The DIRECT arm. Three capabilities have no opcode at all and one only reaches its opcode through a
// counter, so these reproduce the player's effect by writing the same bytes llm_tact_frame and the
// sidebar tick write. Each one measures itself the same way the funnel arm does.
void tact_synth_direct_action(int act, int unit, uint32_t h) {
    auto          *u      = tact_units();
    const uint64_t before = tact_cmd_sig(unit);
    switch (act) {
        case TA_DEFSTAT:
            // llm_tact_ui_sel_panel_multi_mode_tick @0x004366da: def_stat cycles 0->1->2->3->4->0 on
            // each click of the stance icon, and ONLY the 3->4 transition also fires enqueue op 4. That
            // conditional is the whole reason site 10 cannot be reached by calling the funnel: the
            // funnel call is downstream of a counter the funnel does not own.
            u[unit].def_stat = (uint8_t)(u[unit].def_stat >= 4 ? 0 : u[unit].def_stat + 1);
            if (u[unit].def_stat == 4) tact_synth_enqueue(unit, 4, 0, 0, 0, 0);
            break;
        case TA_GUNSWAP:
            // llm_tact_frame @0x00429ea1 (scancode 0x35, every selected unit) and the sidebar weapon
            // icon @0x0043680f both do exactly this. There is no weapon-selection opcode.
            u[unit].active_gun ^= 1;
            break;
        case TA_CTRLGROUP:
            // llm_tact_ui_sel_panel_multi_mode_tick @0x00436336 assigns the squad slot; the digit keys
            // 2..9 in llm_tact_frame recall by it. Also opcode-less. Slots 2..9 mirror the real keys.
            // Addressed by raw offset on purpose: Ghidra has no field at +0x5f0 yet, so the generated
            // mirror models it as `_pad_0x5f0[4]`, and writing through a member named "pad" would read
            // as a bug. When the field is named in the DB this becomes u[unit].squad_slot.
            reinterpret_cast<uint8_t *>(&u[unit])[0x5f0] = (uint8_t)(2 + h % 8);
            break;
        default:
            return;
    }
    if (tact_cmd_sig(unit) != before) ++g_ts_direct;
}

void tact_synth_issue() {
    auto *u = tact_units();
    tact_synth_select();
    if (g_ts_sel_n == 0) return;

    const uint32_t h   = synth_mix((uint32_t)g_cfg.tact_synth_seed ^ (g_tact_step * 2654435761U));
    const int      act = (int)(g_ts_actions % (uint32_t)TA_COUNT);
    ++g_ts_actions;
    ++g_ts_act_tally[act];
    if (tact_action_is_direct(act) && !g_cfg.tact_synth_direct) return;

    // The map bounds come from the caches llm_tact_map_compute_bounds writes, never from a
    // hand-invented band -- the strategic workload learned that the hard way, where a clamped guess
    // piled 67% of its destinations onto the map boundary.
    const uint32_t mw  = *reinterpret_cast<const uint32_t *>(ADDR_TACT_MAP_W);
    const uint32_t mh_ = *reinterpret_cast<const uint32_t *>(ADDR_TACT_MAP_H);
    if (mw < 4 || mw > 128 || mh_ < 4 || mh_ > 128) {
        char msg[160];
        wsprintfA(msg, "; TSYNTH FAIL frame=%lu: tactical map bounds look wrong (w=%lu h=%lu)\n",
                  g_tact_step, (unsigned long)mw, (unsigned long)mh_);
        append_line(g_log_path, msg);
        g_ts_failed = true;
        return;
    }
    int            unit = g_ts_sel[h % (uint32_t)g_ts_sel_n];
    const uint32_t col  = 1 + synth_mix(h) % (mw > 2 ? mw - 2 : 1);
    const uint32_t row  = 1 + synth_mix(h ^ 0x9e3779b9U) % (mh_ > 2 ? mh_ - 2 : 1);

    // KNEEL and STAND are the two actions with a STATE precondition (llm_tact_frame issues STOP+KNEEL
    // only at anim_state 0/1 and STOP+STAND only at 3), so they pick a unit that SATISFIES it rather
    // than taking the seeded one and falling through. That is also what the player does -- you
    // right-click the unit that is standing, or the one that is kneeling. Picking blind made STAND
    // fire zero times in a 400-frame run, because the seeded unit was never mid-kneel at that
    // instant: a capability reported as covered while its op tally read 0.
    if (act == TA_MINE) unit = g_ts_mine_unit;
    if (act == TA_KNEEL || act == TA_STAND) {
        const uint8_t wantmin = act == TA_KNEEL ? 0 : 3;
        const uint8_t wantmax = act == TA_KNEEL ? 1 : 3;
        int           found   = -1;
        // The dedicated stance subject first; the selection only as a fallback, so the pair still
        // has somewhere to go on a squad too small to spare a unit.
        const int cand0 = g_ts_stance_unit;
        if (cand0 && u[cand0].anim_state >= wantmin && u[cand0].anim_state <= wantmax) found = cand0;
        for (int i = 0; found < 0 && i < g_ts_sel_n; ++i) {
            const int c = g_ts_sel[(h + (uint32_t)i) % (uint32_t)g_ts_sel_n];
            if (u[c].anim_state >= wantmin && u[c].anim_state <= wantmax) {
                found = c;
                break;
            }
        }
        if (found < 0) {
            // The skip line names the states that WERE present, not just the one that was wanted.
            // "no unit was kneeling" and "every unit was mid-transition" are different findings, and
            // a bare skip count cannot tell them apart -- which is exactly how a capability reported
            // as covered ends up with a zero op tally nobody can explain.
            ++g_ts_skipped;
            char sm[256];
            int  o = wsprintfA(sm, "; TSYNTH frame=%lu act=%-11s SKIPPED: want anim_state %u..%u, "
                                    "selection has",
                               g_tact_step, TACT_ACTION_NAMES[act], (unsigned)wantmin,
                               (unsigned)wantmax);
            o += wsprintfA(sm + o, " %d*:%u", g_ts_stance_unit,
                           (unsigned)u[g_ts_stance_unit].anim_state);
            for (int i = 0; i < g_ts_sel_n; ++i)
                o += wsprintfA(sm + o, " %d:%u", g_ts_sel[i], (unsigned)u[g_ts_sel[i]].anim_state);
            sm[o++] = '\n';
            sm[o]   = '\0';
            append_line(g_log_path, sm);
            return;
        }
        unit = found;
    }

    switch (act) {
        case TA_GROUP_MOVE: tact_synth_group(1, col, row, 0, 0); break;
        case TA_GROUP_STOP: tact_synth_group(0x7f, 0, 0, 0, 0); break;
        case TA_GROUP_RESET: tact_synth_group(0x46, 0, 0, 0, 0); break;
        case TA_RUNMODE:
            // The sidebar RUN button issues the PAIR (llm_tact_ui_order_buttons_minimap_tick): 0x40
            // drops the resubmit marker and 0x47 then scans forward from the head counting the block
            // it will repeat, writing the count back into the head entry's arg0. Issuing 0x40 alone
            // would leave a marker nothing measures, and 0x47 alone would scan an absent one.
            tact_synth_group(0x40, 0, 0, 0, 0);
            tact_synth_group(0x47, 0, 0, 0, 0);
            break;
        case TA_GROUP_ATTACK:
            // op 2 short-circuits inside enqueue to the immediate ATTACK/AIM record at +0x5d4. The aim
            // point is in PIXEL scale, computed the way llm_tact_unit_owner_tick computes it for the AI
            // (tile*0x20+0x10 across, tile*0x18+0xc down).
            tact_synth_group(2, 0, 0, (uint16_t)(col * 0x20 + 0x10), (uint16_t)(row * 0x18 + 0xc));
            break;
        case TA_FACE:
            // op 6's direction MUST be 1..0x18. Outside that range enqueue returns 1 and writes nothing
            // -- one of the silent no-ops the boolean return cannot report -- so the range is enforced
            // here rather than discovered as an inert count.
            tact_synth_enqueue(unit, 6, (int32_t)(1 + h % 24), 0, 0, 0);
            break;
        case TA_KNEEL:
            // The pair the site issues, in the site's order: STOP first, then the stance command.
            // The eligible unit was chosen above.
            tact_synth_enqueue(unit, 0x7f, 0, 0, 0, 0);
            tact_synth_enqueue(unit, 4, 0, 0, 0, 0);
            break;
        case TA_STAND:
            tact_synth_enqueue(unit, 0x7f, 0, 0, 0, 0);
            tact_synth_enqueue(unit, 5, 0, 0, 0, 0);
            break;
        case TA_STANCE_ON: tact_synth_enqueue(unit, 0x1f, 0, 0, 0, 0); break;
        case TA_STANCE_OFF: tact_synth_enqueue(unit, 0x1d, 0, 0, 0, 0); break;
        case TA_MINE:
            // op 9 is the one op with a real REJECTION path: a global blast cooldown
            // (_G_LLM_TACT_MINE_BLAST_TIME_END) makes enqueue return 0. Rejections are counted, not
            // failed -- refusing under cooldown IS the behaviour under test.
            tact_synth_enqueue(unit, 9, 0, 0, 0, 0);
            break;
        case TA_TELEPORT:
            // NOTE, and it corrects this item's own premise: no PLAYER path passes op 0xa. All 22 call
            // sites that issue it are mission-script (llm_tact_mission_load) or the REPEAT resubmit, so
            // this action covers the OPCODE from a non-script origin, not a player gesture.
            tact_synth_enqueue(unit, 0xa, (int32_t)col, (uint16_t)row, 0, 0);
            break;
        default: tact_synth_direct_action(act, unit, h); break;
    }
    char msg[240];
    wsprintfA(msg,
              "; TSYNTH frame=%lu act=%-11s unit=%d tile=(%lu,%lu) orders=%lu eff=%lu rej=%lu "
              "inert=%lu direct=%lu\n",
              g_tact_step, TACT_ACTION_NAMES[act], unit, (unsigned long)col, (unsigned long)row,
              (unsigned long)g_ts_orders, (unsigned long)g_ts_effective,
              (unsigned long)g_ts_rejected, (unsigned long)g_ts_inert, (unsigned long)g_ts_direct);
    append_line(g_log_path, msg);
}

// The verdict line. Emitted once, at the stop frame, and it is what the runner's shape rules read --
// a zero-order run must not be able to pass as coverage, which means the count has to be IN the log
// rather than inferred from the runner's intent. (read the arming state back OUT
// of the DLL's own banner; never trust what the runner meant to pass.)
void tact_synth_summary() {
    if (g_ts_summed) return;
    g_ts_summed = true;
    char line[760];
    int  off = wsprintfA(line,
                         "; TSYNTH SUMMARY frames=%lu actions=%lu skipped=%lu orders=%lu "
                          "effective=%lu rejected=%lu inert=%lu direct=%lu ops=",
                         g_tact_step, (unsigned long)g_ts_actions, (unsigned long)g_ts_skipped,
                         (unsigned long)g_ts_orders, (unsigned long)g_ts_effective,
                         (unsigned long)g_ts_rejected, (unsigned long)g_ts_inert,
                         (unsigned long)g_ts_direct);
    for (int i = 0; i < TACT_OP_COUNT; ++i)
        off += wsprintfA(line + off, "%s%X:%lu", i ? "," : "", (unsigned)TACT_OPS[i],
                         (unsigned long)g_ts_op_tally[i]);
    off += wsprintfA(line + off, " acts=");
    for (int i = 0; i < TA_COUNT; ++i)
        off += wsprintfA(line + off, "%s%s:%lu", i ? "," : "", TACT_ACTION_NAMES[i],
                         (unsigned long)g_ts_act_tally[i]);
    line[off++] = '\n';
    line[off]   = '\0';
    append_line(g_log_path, line);
    if (g_ts_orders == 0 || g_ts_effective == 0) {
        append_line(g_log_path, "; TSYNTH FAIL: the workload issued no order that changed anything "
                                "-- this run is NOT coverage evidence\n");
        g_ts_failed = true;
    }
    append_line(g_log_path, g_ts_failed ? "; TSYNTH VERDICT: FAILED\n" : "; TSYNTH VERDICT: ok\n");
}

// ---- TACT-PREP: the tactical cadence ------------------------------------------------------------
//
// Runs at the entry of llm_tact_frame, i.e. once per rendered tactical frame. It is NOT a sim step:
// tactical has no fixed-timestep pump, the frame IS the tick. That is why the `T` line carries a
// frame ordinal and no game clock -- _G_LLM_STRAT_GAME_CLOCK is frozen for the whole excursion
// (nothing advances it in mode 6), so printing it would be a column of the same number.
//
// Three jobs, the same three on_sim_step does for strategic:
//   1. advance the pinned wall clock. Without this it FREEZES in tactical rather than merely being
//      deterministic -- llm_tact_unit_spawn reads it four times per unit during mission load, and a
//      frozen clock gives every unit identical animation phase, which is deterministic but not the
//      behaviour the original has. (the tactical-probe work Sect. 3.)
//   2. emit the per-frame hash over TACT_HASH_REGIONS[].
//   3. the mutation arm, so the oracle can be shown to go RED.
bool g_tj_verdict_done = false; // TACT-REC: tj_report() fires once per tactical run

// What the journal actually did. Every number here is a COUNT OF WORK DONE, not of work requested:
// "armed" and "issued 0 of 300" are the two states a broken replay presents as, and they are
// indistinguishable from success unless the run says which.
void tj_report() {
    // A final stamp at the stop, on top of the per-change stream above -- the two are not
    // redundant: this one is the census at the boundary the verdict is about.
    //
    // NOT on the post-mission exit, and this is load-bearing rather than tidiness. That path runs
    // AFTER the mission tore its roster down, so a census there reads `total 24->1 owner0 8->0(-8)
    // units_lost_total=23` -- every unit "lost" to despawn. test_ui.tact_combat() keeps the LAST
    // TACT COMBAT line, so emitting one here would overwrite the real outcome (frames=15360,
    // units_lost_total=10) with teardown noise, and every arm that compares units_lost/survivors
    // against the recording would be reading it. The meaningful boundary is the last census taken
    // while the mission was live, which the per-change stream already wrote.
    if (g_tj_based && !g_tact_post_mission) tj_combat_report();
    if (!g_cfg.tact_journal_rec && !g_tj_ev && !g_cfg.tact_journal_probe) return;
    char l[320];
    if (g_cfg.tact_journal_rec) {
        wsprintfA(l,
                  "; TJ RECORD: %lu order(s) + %lu direct write(s) journalled over %lu frame(s); "
                  "%lu engine call(s) filtered out; %lu op-6 cursor-facing echo(es) dropped "
                  "(NOT journalled -- a replay's facing follows the REPLAY's cursor)\n",
                  g_tj_recorded, g_tjd_recorded, g_tact_step, g_tj_engine, g_tj_facing);
        append_line(g_log_path, l);
        // The order WATCH, reported separately from the order DETOURS on purpose: they measure the
        // same events through instruments with different blind spots, and the gap between the two
        // numbers is itself the reading. Detours see calls that cross the original entries; the
        // watch sees queue slots that changed. Equal-ish means nothing was promoted out from under
        // the detours; watch >> detours means our own bodies are issuing orders the detours miss.
        wsprintfA(l, "; TJ QUEUE: %lu order(s) observed by the hook-free queue watch\n",
                  g_tjq_recorded);
        append_line(g_log_path, l);
        wsprintfA(l,
                  "; TJ INPUT: %lu mouse + %lu key event(s) + %lu cursor move(s) journalled\n",
                  g_tji_m, g_tji_k, g_tji_c);
        append_line(g_log_path, l);
        if (g_tji_p) {
            wsprintfA(l, "; TJ STEPS: %lu step barrier(s) journalled (every %u sim steps)\n", g_tji_p,
                      TJ_SYNC_STEPS);
            append_line(g_log_path, l);
        }
        if (g_tji_s) {
            wsprintfA(l, "; TJ SCREENS: %lu screen barrier(s) journalled\n", g_tji_s);
            append_line(g_log_path, l);
        }
    }
    if (g_tj_ev && (g_tj_waits || g_tj_shift || g_tj_forced)) {
        // WHERE A REPLAY GOT STUCK, named. `held` counts presents spent waiting at a barrier and
        // `shift` is how far the replay's clock ended up from the recording's -- a large held with
        // records left over is a replay that never reached a screen, which is a different failure
        // from one that reached it and diverged.
        wsprintfA(l,
                  "; TJ SCREENS: held %lu present(s) at barriers, %lu FORCED, final shift %ld"
                  " present(s)\n",
                  g_tj_waits, g_tj_forced, (long)g_tj_shift);
        append_line(g_log_path, l);
    }
    if (g_tj_ev) {
        wsprintfA(l, "; TJ REPLAY INPUT: injected %lu mouse + %lu key event(s); DROPPED %lu + %lu\n",
                  g_tji_inj_m, g_tji_inj_k, g_tji_drop_m, g_tji_drop_k);
        append_line(g_log_path, l);
        if (g_cfg.tj_ps_log > 0) {
            wsprintfA(l,
                      "; TJ RING CENSUS: %lu mouse slot(s) walked, %lu ours, %lu FOREIGN"
                      " (a producer other than this journal wrote them)\n",
                      g_tj_slots_seen, g_tj_slots_ours, g_tj_foreign_n);
            append_line(g_log_path, l);
        }
        // In VERIFY mode a "short" issue count is EXPECTED and not a fault: the derived records
        // (E/G/D) are skipped on purpose, so issued + skipped is what must reach the load count --
        // and so are the `S`/`P` BARRIERS, which are records in the file and are not input.
        const uint32_t accounted = g_tj_issued + g_tj_skipped + g_tj_barriers;
        wsprintfA(l,
                  "; TJ REPLAY: issued %lu input record(s) + %lu barrier(s) of %d loaded%s\n",
                  g_tj_issued, g_tj_barriers, g_tj_n,
                  (accounted == (uint32_t)g_tj_n)
                      ? ""
                      : " -- SHORT: the run ended before the journal did, so this replay covered"
                        " only part of the recorded session");
        append_line(g_log_path, l);
        if (g_tj_skipped) {
            wsprintfA(l, "; TJ REPLAY WITHHELD %lu derived record(s) -- input-only replay\n",
                      g_tj_skipped);
            append_line(g_log_path, l);
        }
    }
    if (g_cfg.tact_journal_probe) {
        for (int i = 0; i < TJ_SITES && g_tj_site_pc[i]; ++i) {
            wsprintfA(l, "; TJ SITE %08X %-6s %lu\n", g_tj_site_pc[i],
                      g_tj_site_player[i] ? "PLAYER" : "engine", g_tj_site_cnt[i]);
            append_line(g_log_path, l);
        }
    }
}

static bool g_tact_verdict_done = false; // TACT-RIG: report_all() fires once per tactical run

void on_tact_frame() {
    if (!g_active) return;
    ++g_tact_step;

    // TACT-REC's self-stop. Checked HERE, at the top of frame N+1, rather than after the hash at the
    // bottom of frame N -- because every path below this point can `return` early (hashing off, or
    // past tact_stop_step), and an exit placed after them would simply never fire in exactly the
    // configuration that wants it. At this instant frames 1..tact_exit_at are fully logged, TR line
    // included, so the log the runner reads is complete.
    if (g_cfg.tact_exit_at && g_tact_step > (uint32_t)g_cfg.tact_exit_at) {
        char b[128];
        wsprintfA(b, "; TACT EXIT after frame %d ([harness] tact_exit_at) -- run complete\n",
                  g_cfg.tact_exit_at);
        append_line(g_log_path, b);
        // TACT-RIG (2026-08-25): the armed sites' FINAL VERDICT, here as well as at the strategic
        // stop step. It was ONLY at the strategic one, and that made a tactical shadow run
        // structurally unreadable: mode 6 dispatches to llm_tact_frame and never calls
        // llm_strat_sim_step, so `g_step` never advances, the `stop_step` branch in on_sim_step()
        // never runs, and report_all() never fired. migration_sweep uses the presence of this block
        // as the completion signal, so every tactical site would have come back INCOMPLETE -- a
        // verdict about the runner, indistinguishable from a site that genuinely never ran.
        //
        // Before TerminateProcess, not after: the self-stop deliberately reproduces the old
        // kill-at-wall exactly (see below), and a killed process writes nothing on the way out.
        //
        // Latched, because tact_stop_step (which fires at frame N) and tact_exit_at (frame N+1) are
        // set to the SAME value by every runner in the tree -- so without the latch a normal run
        // would emit TWO verdict blocks and the sweep's per-site call counts would be ambiguous
        // about which one it read.
        if (!g_tj_verdict_done) {
            g_tj_verdict_done = true;
            tj_report();
        }
        // TerminateProcess, NOT ExitProcess, and the difference is measurable. ExitProcess runs CRT
        // atexit handlers and every loaded DLL's DLL_PROCESS_DETACH; the runner's previous behaviour
        // -- kill at --tact-wall -- ran none of that. A back-to-back A/B at 3000 frames DIVERGED
        // under ExitProcess and was IDENTICAL both with the old kill and with one lane per arm, so
        // the self-stop must reproduce the kill exactly rather than merely stop earlier.
        // FLUSH FIRST. TerminateProcess discards user-space buffers, and the log writer is
        // buffered (see append_line) -- without this the run's last second of output, including
        // the EXIT line the runner keys completion off, dies with the process.
        prof_report();
        log_flush();
        TerminateProcess(GetCurrentProcess(), 0);
    }

    // Sect. 9g: THE ENTRY STAMP. g_pin_now is advanced by BOTH cadences -- on_sim_step (line ~1108)
    // and here -- and `--tactical` reaches mode 6 by first doing a `--load`, i.e. by running the
    // STRATEGIC game at menu-idle for however many frames the load happens to take. So the pinned
    // clock has already advanced by an arm-dependent amount before the first tactical frame, and
    // llm_tact_unit_spawn stamps FOUR clock reads per unit during mission load -- into
    // anim_frame_time / anim_cycle_time / cmd_wait_until_time, which are exactly the bytes Sect. 9f
    // found diverging. If these two numbers differ between arms, the units start with different
    // animation phase and every later divergence follows from it.
    // Printed once, unconditionally when the cadence is armed: the ABSENCE of a matching pair in the
    // two logs is itself the finding, so it must not be behind a knob nobody set.
    // MEASURED 2026-08-24 AND THE ENTRY CANDIDATE IS DEAD: both arms print
    // `pin_now=1000.000000 strat_steps=0`. The `--load` phase runs ZERO strategic sim steps before
    // the verb fires, so the pinned clock is still at its base and every mission-load spawn stamp is
    // taken at the SAME instant in both arms. Kept, and made PERIODIC (every 512 frames), because the
    // remaining candidates -- a frame the hook did not count, or an advance skipped by the !g_active
    // early-return -- would show as pin_now DRIFTING apart at the same frame index, which a single
    // sample at entry cannot see.
    if ((g_tact_step & 511) == 1) {
        // These per-frame diagnostic buffers are STATIC, not stack: on_tact_frame is a DETOUR on the
        // game's own thread, the game is a 2001 binary that manages its own stack (utils_assert_stack_
        // capacity is all over it), and with ~4 KB of them on the stack one arm died after frame 1.
        // Single-threaded by construction -- one tactical frame pump, no reentrancy.
        static char eb[192];
        // Sect. 9h: the two cheap eliminations ride on this same line rather than getting their own,
        // so one hunt tests both and all three columns are read at the same frame index.
        //   rand=<draws>/<state> -- pin_rand replaces llm_rand's body with a harness-owned LCG. If
        //     the DRAW COUNT differs at frame N the RNG sequence position has diverged, which is
        //     upstream of the timestamps and would perturb several units at once (the wander arm of
        //     llm_tact_unit_owner_tick is llm_rand-driven). Count matching but STATE differing would
        //     instead mean something draws through a path pin_rand did not replace.
        //   fpu=<control word> -- these fields are doubles compared by x87 ops. pin_fpu is applied at
        //     install time; whether the control word still holds INSIDE the excursion is exactly what
        //     was never checked, so read it HERE rather than trusting the arm-time value.
        uint16_t cw = 0;
#if defined(_M_IX86)
        __asm { fnstcw cw }
#endif
        wsprintfA(eb,
                  "; TACT CLOCK pin_now=%d.%06u strat_steps=%lu tact_frame=%lu rand=%lu/%08X "
                  "fpu=%04X (pin=%d)\n",
                  (int)g_pin_now,
                  (unsigned)((g_pin_now - (double)(int)g_pin_now) * 1000000.0 + 0.5), g_step,
                  g_tact_step, g_rand_draws, g_rand_state, (unsigned)cw, g_cfg.pin_wallclock);
        append_line(g_log_path, eb);
        // Sect. 9j: WHO is drawing. One line, only the sites that actually fired.
        static char rb[64 + RAND_SITES * 24];
        int         roff = wsprintfA(rb, "; TACT RAND f=%lu", g_tact_step);
        for (int i = 0; i < RAND_SITES && g_rand_site_pc[i]; ++i)
            roff += wsprintfA(rb + roff, " %08X:%lu", g_rand_site_pc[i], g_rand_site_cnt[i]);
        rb[roff++] = '\n';
        rb[roff]   = '\0';
        append_line(g_log_path, rb);
    }

    // Job 1. Guarded on pin_wallclock like the strategic advance, and by the same dt: the two
    // cadences never run in the same frame (mode 6 dispatches to exactly one of them), so the clock
    // advances once per frame either way and no double-advance is possible.
    if (g_cfg.pin_wallclock) {
        g_pin_now += g_pin_dt;
        g_pin_moved = true; // UI-REC: see MH_Harness_OnPresent
    }

    // Job 1a -- TACT-REC. The journal, before everything else this hook does and before the game's
    // own frame body (the detour is a run-before shape), so a replayed order lands exactly where the
    // recorded one did: ahead of this frame's hash, and ahead of the ticks that consume the queue.
    // The same ordering rule the poke arm and TACT-SYNTH follow, for the same reason.
    //
    // UI-REC: the whole journal block below is SKIPPED when the present-indexed arm owns the journal.
    // A tactical excursion inside a UI-journal session would otherwise re-stamp g_tj_idx with
    // g_tact_step and interleave two counters into one file. The two arms are also refused together
    // at arm time; this is the second half of that guard, for the case where the tactical CADENCE is
    // armed for its hashes while the journal belongs to the UI arm.
    if (!ui_journal_armed()) {
        g_tj_idx = g_tact_step;
        if (g_tj_ev) tj_replay_frame();
        // The direct-write watch runs on BOTH sides. Recording, it journals what the UI wrote during
        // the previous frame; replaying, it costs a roster sweep and journals nothing
        // (tact_journal_rec is refused alongside a replay at arm time). Running it in both modes
        // keeps the shadow honest.
        tji_probe_tick();
        // NOT in verify mode: the rings there contain what WE injected, so recording them would
        // journal our own replay back at us and call the echo a match.
        if (g_cfg.tact_journal_rec && !g_cfg.tact_journal_verify) tji_record();
        if (g_cfg.tact_journal_rec || g_tj_ev) tj_watch_direct();
        // The ORDER watch, same both-sides rule and same reason. Unlike the E/G detours this one is
        // hook-free, so it keeps seeing orders when a promoted body issues them to its own sibling --
        // which is exactly the blind spot that forced --tact-equiv's order diff to be advisory.
        if (g_cfg.tact_journal_rec || g_tj_ev) tj_watch_queue();
    }
    // The combat baseline, once, at the first frame this hook sees. Taken here rather than at arm
    // time because the mission's units are spawned by llm_tact_mission_start, which has already run
    // by the first llm_tact_frame but had NOT when the ini was parsed.
    if (!g_tj_based) {
        g_tj_based = 1;
        g_tj_base  = tj_census();
        g_tj_last  = g_tj_base.total;
        tj_combat_report(); // frame 1, so even a run that dies immediately has one census
    }
    // THE CENSUS MUST NOT DEPEND ON THE RUN ENDING TIDILY, and that is a lesson paid for: a
    // 63,563-frame human recording produced NO combat line at all, because --tact-record sets
    // tact_stop_step=0 and --tact-play sets tact_exit_at=0, so neither verdict path ever fired --
    // the human closed the window and the process just went away. The session HAD fought (555
    // weapon-scatter draws, a group ATTACK as its first order); the instrument simply never said so,
    // and the runner then told the user to re-record a perfectly good session.
    //
    // So it is emitted on CHANGE -- which makes it a combat timeline rather than a single number --
    // with a heartbeat so a last line always exists however the run ends.
    else {
        const tj_roster now = tj_census();
        if (now.total != g_tj_last || (g_tact_step & 511u) == 0u) {
            g_tj_last = now.total;
            tj_combat_report();
        }
    }

    // Job 1b -- TACT-SYNTH. BEFORE every hash gate, for two reasons. (a) The order must land before
    // this frame's hash, or the frame the log names is one frame ahead of the frame the order
    // changed -- the same ordering rule the poke arm and synth_move follow. (b) The workload is
    // deliberately independent of tact_hash_step: arming an oracle and arming a workload are
    // separate decisions, and coupling them would make "hash every 4th frame" silently quarter the
    // order rate.
    if (g_cfg.tact_synth && g_tact_step >= (uint32_t)g_cfg.tact_synth_at &&
        (!g_cfg.tact_synth_stop || g_tact_step <= (uint32_t)g_cfg.tact_synth_stop)) {
        const int every = g_cfg.tact_synth_every < 1 ? 1 : g_cfg.tact_synth_every;
        if (((g_tact_step - (uint32_t)g_cfg.tact_synth_at) % (uint32_t)every) == 0) tact_synth_issue();
    }
    // The verdict, once, at the frame the run stops logging at -- so it is in the log the runner
    // reads rather than at a process teardown a killed arm never reaches.
    if (g_cfg.tact_synth && g_cfg.tact_stop_step && g_tact_step == (uint32_t)g_cfg.tact_stop_step)
        tact_synth_summary();

    // TACT-REC: the journal's own verdict, at the same boundary. It reports what it DID, not what
    // it was given -- loaded vs issued, and the recorder's player/engine split -- because a journal
    // that armed and issued nothing looks exactly like a journal that replayed perfectly.
    if (g_cfg.tact_stop_step && g_tact_step == (uint32_t)g_cfg.tact_stop_step && !g_tj_verdict_done) {
        g_tj_verdict_done = true;
        tj_report();
    }

    // TACT-RIG: and the SHADOW verdict at the same boundary, for the configuration that stops
    // logging without exiting (tact_stop_step set, tact_exit_at not). Guarded by its own latch
    // rather than by the frame test alone, so a run that sets both does not emit it twice -- two
    // FINAL VERDICT blocks would make the sweep's per-site call counts ambiguous.

    if (g_cfg.tact_hash_step <= 0) return;
    if (g_cfg.tact_stop_step && g_tact_step > (uint32_t)g_cfg.tact_stop_step) return;

    // Job 3, BEFORE the hash, so the frame named in the ini is the frame the divergence is reported
    // at -- the same ordering rule the region_poke and rng_perturb arms follow.
    if (!g_tact_poke_done && g_cfg.tact_poke_at > 0 && g_cfg.tact_poke_idx >= 0 &&
        g_cfg.tact_poke_idx < mh::state::TACT_HASH_REGION_COUNT &&
        g_tact_step == (uint32_t)g_cfg.tact_poke_at) {
        g_tact_poke_done = true;
        // Clamped rather than trusted: a tact_poke_off past the region's end would write into
        // whatever follows it in .bss, and the run would then report a divergence it caused
        // somewhere the oracle is not even watching.
        const uint32_t plen = mh::state::TACT_HASH_REGIONS[g_cfg.tact_poke_idx].len;
        uint32_t       poff = (uint32_t)(g_cfg.tact_poke_off < 0 ? 0 : g_cfg.tact_poke_off);
        if (poff >= plen) poff = 0;
        uint8_t *b = mh::state::tact_mutate_target(g_cfg.tact_poke_idx) + poff;
        *b ^= 0xA5;
        char pl[192];
        wsprintfA(pl, "; TACT POKE frame=%lu idx=%d %-18s @%08X+%u -> %02X (the RED arm)\n",
                  g_tact_step, g_cfg.tact_poke_idx,
                  mh::state::TACT_HASH_REGIONS[g_cfg.tact_poke_idx].name,
                  (unsigned)mh::state::tact_hash_base(g_cfg.tact_poke_idx), poff, (unsigned)*b);
        append_line(g_log_path, pl);
    }

    if ((g_tact_step % (uint32_t)g_cfg.tact_hash_step) != 0) return;

    // Job 2. TWO verdicts from one pass (TACT-REC, 2026-08-25):
    //
    //   T  -- the combined hash over every slice, byte-for-byte what it has always been. The alias
    //         slice is `excluded` and therefore not folded in, so historical `T` values stay
    //         comparable across this change.
    //   TS -- the same fold with tact_units replaced by tact_units_sim, i.e. the roster WITHOUT the
    //         render-written animation window (state/region_view.h emit_tact_units_sim).
    //
    // Both are emitted always, not behind a knob. The PAIR is the verdict -- a run that logged only
    // one of them could not tell presentation drift from a divergence the simulation owns, and that
    // distinction is the entire reason the second hash exists.
    // BOTH LINES IN ONE append_line, and this is not tidiness -- it is the difference between
    // adding a verdict and perturbing the thing being measured. append_line does a full
    // CreateFile / SetFilePointer / WriteFile / CloseHandle per CALL, so emitting `TS` separately
    // would DOUBLE this hook's per-frame syscall count on a fault that is known to be
    // timing-sensitive (the tactical-probe work 9k: "the instrument may be suppressing the fault").
    // The log is byte-identical either way -- two lines, same order; only the open count changes.
    uint64_t       per[mh::state::TACT_HASH_REGION_COUNT];
    const auto     v        = mh::state::tact_hash_all(per);
    const uint64_t combined = v.combined;
    // Sized from the format, not rounded: two lines of ("T"|"TS") + ' ' + up to 10 digits + ' ' +
    // 16 hex + newline = 29 + 30, plus the NUL. 96 was the single-line size and still fits, but a
    // derived number is what stops the next added line from being a silent overrun.
    char line[2 * 32 + 8];
    int  loff = wsprintfA(line, "T %lu %08X%08X\n", g_tact_step,
                          (unsigned)(combined >> 32), (unsigned)combined);
    wsprintfA(line + loff, "TS %lu %08X%08X\n", g_tact_step, (unsigned)(v.sim >> 32),
              (unsigned)v.sim);
    append_line(g_log_path, line);

    // The per-region line, always emitted next to the combined one rather than on its own cadence
    // knob: a tactical run is a few thousand frames, not the tens of thousands a strategic soak is,
    // and the whole point of the set is localising WHICH region diverged. Capacity derived from the
    // table, never a round number -- growing REGIONS[] once overflowed a hand-sized buffer here and
    // killed both peers one step into every run.
    constexpr int TR_HDR = 3 + 10 + 2; // "TR " + up to 10 digits + '\n' + NUL
    char          rl[TR_HDR + mh::state::TACT_HASH_REGION_COUNT * 17];
    int           off = wsprintfA(rl, "TR %lu", g_tact_step);
    for (int i = 0; i < mh::state::TACT_HASH_REGION_COUNT; ++i)
        off += wsprintfA(rl + off, " %08X%08X", (unsigned)(per[i] >> 32), (unsigned)per[i]);
    rl[off++] = '\n';
    rl[off]   = '\0';
    append_line(g_log_path, rl);

    // Sect. 9f: one hash per tact_unit_record, on the SAME cadence as the TR line so the two are
    // comparable frame for frame. Hashed over the WHOLE record, deliberately not the command-state
    // subset tact_unit_sig() uses -- that one excludes the per-frame timers, and here the timers are
    // exactly the sort of thing that might be diverging.
    if (g_cfg.tact_unit_hash) {
        constexpr int    TU_N      = 129;   // tact_unit_record[129]
        constexpr size_t TU_STRIDE = 0x5f4; // 129 * 0x5f4 == 196,596 == the tact_units region
        // tact_hash_base, not the stock address: every consumer of a region has to follow a
        // rebase or the instrument reads bytes nothing writes -- the "gates can pass vacuously"
        // trap, and here it would corrupt the evidence rather than the game.
        const auto *ubase = reinterpret_cast<const uint8_t *>(
            static_cast<uintptr_t>(mh::state::tact_hash_base(mh::state::TIDX_TACT_UNITS)));
        static char ul[16 + TU_N * 10];
        int         uoff = wsprintfA(ul, "TU %lu", g_tact_step);
        for (int i = 0; i < TU_N; ++i) {
            const uint64_t h =
                fnv1a(ubase + (size_t)i * TU_STRIDE, TU_STRIDE, 1469598103934665603ULL);
            // 32 bits is plenty to LOCALISE -- the 64-bit TR line already decided that something
            // differs -- and halving the width halves a 7 MB two-arm log.
            uoff += wsprintfA(ul + uoff, " %08X", (unsigned)(h ^ (h >> 32)));
        }
        ul[uoff++] = '\n';
        ul[uoff]   = '\0';
        append_line(g_log_path, ul);
    }

    // Second level: chunk hashes INSIDE a unit record, so a divergence names a byte offset and
    // therefore a FIELD (cross-reference tact_unit_record in docs/structs.md). One line per unit so a
    // single index can be grepped out of a large log.
    if (g_cfg.tact_detail_hi >= g_cfg.tact_detail_lo && g_cfg.tact_detail_hi > 0) {
        constexpr size_t TU_STRIDE = 0x5f4;
        constexpr size_t CHUNK     = 32; // 48 chunks, the last one short
        constexpr int    NCHUNK    = (int)((TU_STRIDE + CHUNK - 1) / CHUNK);
        const auto      *dbase     = reinterpret_cast<const uint8_t *>(
            static_cast<uintptr_t>(mh::state::tact_hash_base(mh::state::TIDX_TACT_UNITS)));
        for (int u = g_cfg.tact_detail_lo; u <= g_cfg.tact_detail_hi && u < 129; ++u) {
            if (u < 0) continue;
            static char dl[24 + NCHUNK * 10];
            int         doff = wsprintfA(dl, "TD %lu %d", g_tact_step, u);
            for (int c = 0; c < NCHUNK; ++c) {
                const size_t   off = (size_t)c * CHUNK;
                const size_t   len = (off + CHUNK <= TU_STRIDE) ? CHUNK : (TU_STRIDE - off);
                const uint64_t h =
                    fnv1a(dbase + (size_t)u * TU_STRIDE + off, len, 1469598103934665603ULL);
                doff += wsprintfA(dl + doff, " %08X", (unsigned)(h ^ (h >> 32)));
            }
            dl[doff++] = '\n';
            dl[doff]   = '\0';
            append_line(g_log_path, dl);
        }
    }

    // Sect. 9i probe: llm_tact_unit_owner_tick's gate fields, one line per frame for a unit range.
    // Field offsets are the ones the gates themselves read, taken from their instructions:
    //   type   +0x00  (CMP byte[unit+0x8260d0],0     @0x0043360b)   G1
    //   owner  +0x01  (MOVZX EAX,[unit+0x8260d1]     @0x004335f8)   G1
    //   status +0x02  (TEST byte[unit+0x8260d2],8    @0x00433631)   G2, bit 3
    //   cmd_index      +0x53, queue base +0x54, entry stride 0x0b
    //     iflag  entry+0x00 (CMP byte[+0x826124],0   @0x0043365e)   G3
    //     op     entry+0x01 (CMP word[+0x826125],0   @0x00433647)   G3, a WORD
    //   move_retry_wait +0x4d (CMP word[+0x82611d],0 @0x00433670)   G3
    //
    // 9k added four ANIMATION columns and one OCCUPANCY column, and the occupancy one is why this
    // probe still earns its place after the gates were exonerated. `update_anim` is reached only
    // from llm_tact_render_view's viewport tile scan, which visits a unit only while
    // tile_objects[pos_col][pos_row].building still names it -- so a unit can stop animating
    // permanently with every gate field bit-identical between the arms, which is exactly what the
    // captured pairs show. The two candidate mechanisms are told apart by these columns alone:
    //
    //   a frozen at 2/3/4 while q stays 1  -> a stuck animation state
    //   q goes 0 while a stays 0/1         -> occupancy loss, i.e. SIM state -- and then the
    //                                         "just mask the animation fields" idea is dead
    //
    //   a anim_state +0x05   d def_stat +0x06   x frame_index +0x33   p progress +0x44
    //   c/r pos_col/pos_row +0x03/+0x04
    //   q  the occupancy SELF-CHECK: tile_objects[pos_col][pos_row].building == u
    if (g_cfg.tact_gate_hi >= g_cfg.tact_gate_lo && g_cfg.tact_gate_hi > 0) {
        constexpr size_t TU_STRIDE = 0x5f4;
        constexpr size_t Q_BASE    = 0x54;
        constexpr size_t Q_STRIDE  = 0x0b;
        const auto      *gbase     = reinterpret_cast<const uint8_t *>(
            static_cast<uintptr_t>(mh::state::tact_hash_base(mh::state::TIDX_TACT_UNITS)));
        // Column-major, flat, (col << 8) | row -- the same formula mh::state::mode_planes and
        // mh::tact::tile_at use. Read through tact_hash_base so it follows a rebase like every
        // other consumer instead of freezing the stock address.
        const auto *tob = reinterpret_cast<const mh::game::mh_map_tile_object_data *>(
            static_cast<uintptr_t>(mh::state::tact_hash_base(mh::state::TIDX_TILE_OBJECTS)));
        // THE PER-RECORD BUDGET IS DERIVED FROM THE FORMAT STRING, not chosen. Growing this line
        // by columns while leaving a hand-picked multiplier alone is exactly how this file has
        // overflowed a log buffer twice, and the previous value survived the
        // last two columns by luck: the true worst case is 77 bytes against 96 of slack.
        //
        //   literals " %d:t,o,s,i,op,f,w,a,d,x,p,c,r,q"                              30
        //   %d unit index (0..128)                                                    3
        //   t/o/a/d/x/p/c/r  eight bytes as %u                                    8 * 3 = 24
        //   s  %02X of a byte                                                          2
        //   i  cmd_index, a byte as %u                                                 3
        //   op a live uint16_t as %u                                                   5
        //   f  a byte as %u                                                            3
        //   w  an int16_t widened to int -- "-32768"                                   6
        //   q  0 or 1                                                                  1
        //                                                                     total   77
        constexpr int GROUP  = 96; // > 77, with the arithmetic above for the next person to extend
        constexpr int TG_HDR = 32; // "TG " + up to 10 digits + newline + NUL, rounded up
        static char   gl[TG_HDR + 33 * GROUP];
        int           goff = wsprintfA(gl, "TG %lu", g_tact_step);
        // Stop one GROUP short of the end so the LAST write cannot be the one that overruns: the
        // guard is checked BEFORE a write whose length it does not know.
        for (int u = g_cfg.tact_gate_lo;
             u <= g_cfg.tact_gate_hi && u < 129 && goff < TG_HDR + 32 * GROUP; ++u) {
            if (u < 0) continue;
            const uint8_t *rec = gbase + (size_t)u * TU_STRIDE;
            const unsigned ci  = rec[0x53];
            const uint8_t *ent = rec + Q_BASE + (size_t)(ci & 0x7f) * Q_STRIDE;
            const unsigned col = rec[0x03], row = rec[0x04];
            const unsigned occ = tob[(col << 8) | row].building;
            goff += wsprintfA(gl + goff,
                              " %d:t%u,o%u,s%02X,i%u,op%u,f%u,w%d,a%u,d%u,x%u,p%u,c%u,r%u,q%u", u,
                              (unsigned)rec[0], (unsigned)rec[1], (unsigned)rec[2], ci,
                              (unsigned)(*(const uint16_t *)(ent + 1)), (unsigned)ent[0],
                              (int)*(const int16_t *)(rec + 0x4d), (unsigned)rec[0x05],
                              (unsigned)rec[0x06], (unsigned)rec[0x33], (unsigned)rec[0x44], col,
                              row, occ == (unsigned)u ? 1u : 0u);
        }
        gl[goff++] = '\n';
        gl[goff]   = '\0';
        append_line(g_log_path, gl);
    }

    // Third level: one hash per NAMED FIELD WINDOW of tact_unit_record, so an offline diff of two
    // arms attributes a divergence to a field rather than to a 32-byte chunk. The table is the
    // resolution TD cannot reach and the `TS` verdict depends on: chunk 1 (0x20..0x3F) straddles
    // cmd_wait_until_time, which the sim slice KEEPS, and anim_frame_time, which it drops -- so
    // "chunk 1 differs" is compatible with both a presentation-only drift and a real one.
    //
    // The windows are the struct's own, taken from mh_tact_unit_record in mh_structs.gen.h. Five
    // are marked `local` here purely as documentation of what emit_tact_units_sim() drops; this
    // line always emits ALL of them, because the whole point is to see the excluded ones move.
    if (g_cfg.tact_field_hi >= g_cfg.tact_field_lo && g_cfg.tact_field_hi > 0) {
        struct field {
            const char *name;
            uint16_t    off;
            uint16_t    len;
        };
        // Not the whole record -- the 1408-byte command queue is one window, since a queue
        // divergence is interesting as a fact and TD already localises inside it.
        static constexpr field F[] = {
            {"type", 0x00, 1},
            {"owner", 0x01, 1},
            {"status", 0x02, 1},
            {"pos", 0x03, 2},
            {"anim_state", 0x05, 1},
            {"def_stat", 0x06, 1},
            {"vision", 0x07, 3},
            {"facing_dir", 0x0a, 1},
            {"move_timer", 0x0b, 8},
            {"weapon_timer", 0x13, 8},
            {"wander_time", 0x1b, 8},
            {"cmd_wait", 0x23, 8},
            // ---- the five the sim slice marks local() -------------------------------------
            {"anim_frame_t", 0x2b, 8},
            {"frame_index", 0x33, 1},
            {"anim_cycle_t", 0x34, 8},
            {"frame_interval", 0x3c, 8},
            // -------------------------------------------------------------------------------
            {"progress", 0x44, 1},
            {"sprite_id", 0x45, 2},
            {"path_slot", 0x47, 2},
            {"path_step", 0x49, 2},
            {"hp", 0x4b, 2},
            {"retry_wait", 0x4d, 2},
            {"retry_att", 0x4f, 2},
            {"stuck_cd", 0x51, 2},
            {"cmd_index", 0x53, 1},
            {"cmd_queue", 0x54, 0x580},
            {"tail", 0x5d4, 0x20},
        };
        constexpr int    NF         = (int)(sizeof(F) / sizeof(F[0]));
        constexpr size_t TU_STRIDE2 = 0x5f4;
        const auto      *fbase      = reinterpret_cast<const uint8_t *>(
            static_cast<uintptr_t>(mh::state::tact_hash_base(mh::state::TIDX_TACT_UNITS)));
        for (int u = g_cfg.tact_field_lo; u <= g_cfg.tact_field_hi && u < 129; ++u) {
            if (u < 0) continue;
            static char fl[24 + NF * 10];
            int         foff = wsprintfA(fl, "TF %lu %d", g_tact_step, u);
            for (int k = 0; k < NF; ++k) {
                const uint64_t h = fnv1a(fbase + (size_t)u * TU_STRIDE2 + F[k].off, F[k].len,
                                         1469598103934665603ULL);
                foff += wsprintfA(fl + foff, " %08X", (unsigned)(h ^ (h >> 32)));
            }
            fl[foff++] = '\n';
            fl[foff]   = '\0';
            append_line(g_log_path, fl);
        }
        // The header, once, so the offline differ names fields without a second copy of the table.
        static bool hdr_done = false;
        if (!hdr_done) {
            hdr_done = true;
            static char hb[64 + NF * 18];
            int         hoff = wsprintfA(hb, "; TF FIELDS");
            for (int k = 0; k < NF; ++k) hoff += wsprintfA(hb + hoff, " %s", F[k].name);
            hb[hoff++] = '\n';
            hb[hoff]   = '\0';
            append_line(g_log_path, hb);
        }
    }
}

// ---- rdump's two helpers (see the rdump_* config block) -----------------------------------------
// The hex emitter is SHARED by the region window and the address window so the two cannot drift into
// different line formats -- an offline differ reads one shape or it reads neither.
constexpr uint32_t RDUMP_ADDR_MAX = 1u << 20; // 1 MB; the largest hashed region is 512 KB

void rdump_emit(const uint8_t *p, uint32_t n, int rid_field) {
    static const char HEX[] = "0123456789ABCDEF";
    // 64 bytes per line: the header is at most 26 chars, the payload exactly 128, so 200 has margin
    // without the size being a number anybody has to remember somewhere else.
    for (uint32_t off = 0; off < n; off += 64) {
        const uint32_t m = (n - off) < 64u ? (n - off) : 64u;
        char           b[200];
        int            k = wsprintfA(b, "RX %lu %d %lu ", g_step, rid_field, off);
        for (uint32_t j = 0; j < m; ++j) {
            b[k++] = HEX[p[off + j] >> 4];
            b[k++] = HEX[p[off + j] & 0x0f];
        }
        b[k++] = '\n';
        b[k]   = '\0';
        append_line(g_log_path, b);
    }
}

// Every page of [addr, addr+len) committed and readable. Same predicate as net_seams.cpp's scanner --
// PAGE_GUARD excluded, because touching a guard page is precisely the fault this check exists to avoid.
bool range_is_readable(uintptr_t addr, uint32_t len) {
    MEMORY_BASIC_INFORMATION mbi;
    for (uintptr_t a = addr; a < addr + len;) {
        if (!VirtualQuery((void *)a, &mbi, sizeof(mbi))) return false;
        const DWORD rp = mbi.Protect & 0xff;
        const bool  ok = mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) &&
                        (rp == PAGE_READONLY || rp == PAGE_READWRITE || rp == PAGE_WRITECOPY ||
                         rp == PAGE_EXECUTE_READ || rp == PAGE_EXECUTE_READWRITE ||
                         rp == PAGE_EXECUTE_WRITECOPY);
        if (!ok) return false;
        const uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (next <= a) return false; // no forward progress -- refuse rather than spin
        a = next;
    }
    return true;
}

// LIB-REF-REC: the deferred-exit latch. Set by the stop block at the END of this function (see the
// long note there); consumed at the TOP of the next on_sim_step or on_sim_tick, both of which are
// provably after the stop step's sim body has run -- which is what lets order_record() snapshot the
// exit step's queue at the same instant as every other step's.
bool g_exit_after_body = false;

void exit_after_body_if_latched() {
    if (!g_exit_after_body) return;
    g_exit_after_body = false; // one-shot: prof_report() must not re-enter this on a nested path
    prof_report();
    log_flush(); // prof_report() appends after the stop block's own flush
    ExitProcess(0);
}

void on_sim_step() {
    exit_after_body_if_latched();        // LIB-REF-REC: the stop step's body has now run
    g_tj_last_step_ms = GetTickCount();  // UI-REC idle watchdog -- see ui_journal_present
    if (g_cfg.tj_trace && g_step < 12) { // the clock + journal cursor at each early step
        char tm[160];
        wsprintfA(tm, "; TJSTEP step=%lu pin_now_ms=%lu tj_idx=%lu next=%d\n", g_step + 1,
                  (unsigned long)((g_pin_now - (double)g_cfg.pin_clock_base_s) * 1000.0),
                  g_tj_idx,
                  g_tj_next);
        append_line(g_log_path, tm);
    }
    land_log_report(); // SPCAMP-SEED: one-shot, the first step after landing
    if (!g_active) return;
    MH_Temporal_Event(5 /*TEV_SIMSTEP*/); // step boundary (pre-body: GAME_CLOCK still = (n-1)*interval)
    ++g_step;
    // C-prime: the draws that follow belong to THIS step. Set unconditionally -- the trace gates
    // itself on its window, and a step counter that only advanced when armed would be a second,
    // divergent counter.
    mh::sim::rng_trace_set_step(g_step);

    // F5J: THE FENCE TRIPS HERE, at the ENTRY of the target step -- i.e. after sim_tick's loop has
    // already charged the clock for it and before its body runs. Freezing TOTAL at the clock now
    // makes the loop's next test false, so this step is the LAST of the burst and the render that
    // closes the frame carries exactly N. (Tripping it from the present hook instead would read the
    // burst's result, which is the overshoot the whole fence exists to remove.)
    if (g_fence_at > 0 && !g_fence_hit && g_step >= (uint32_t)g_fence_at) {
        g_fence_hit = true;
        fence_freeze_clock();
        char fb[192];
        wsprintfA(fb,
                  "; [simfence] HELD at step %lu (target %d) -- this frame's catch-up burst ends "
                  "here, and every present from now renders this exact sim state\n",
                  g_step, g_fence_at);
        append_line(g_log_path, fb);
        // FLUSHED, and this is not tidiness. The fence is the LAST sim event of the run by
        // construction -- the script captures and ends, and the runner kills the process -- so
        // nothing appends after it and the <=64 KB buffer dies unwritten. Measured on the first F5J
        // run: the HOST's HELD line survived (its log had later traffic) and the CLIENT's did not,
        // which is precisely the peer whose fence a reader needs to confirm. Same reasoning as the
        // stop block's own flush, one event over.
        log_flush();
    }

    // THE TACTICAL SELF-STOP'S MISSING HALF (2026-09-04). tact_exit_at fires from on_tact_frame, on
    // `g_tact_step > tact_exit_at` -- and g_tact_step only advances while mode 6 is dispatching to
    // llm_tact_frame. When the MISSION ENDS EARLY the game leaves mode 6, that counter freezes below
    // the target, and the condition can never become true again: the run then lives until
    // --tact-wall kills it. Measured on the poz1_combat journal -- combat resolves at frame 15360,
    // tactical hashing stops at T 15810 of a requested 16031, and the process burned another ~4
    // minutes emitting STRATEGIC hashes nobody reads. That is also why --tact-suite's four replay
    // arms all reported "KILLED before it could report": not an undersized wall, a stop condition
    // that could not fire. Every tactical journal arm paid it, every run.
    //
    // This hook is the exact complement, and the block at tact_exit_at already documents the mirror
    // trap in the other direction (mode 6 never calls llm_strat_sim_step, so the strategic stop_step
    // branch never runs during a mission). Turn that symmetry around: reaching HERE with
    // g_tact_step > 0 means a tactical mission ran and has now ended, so nothing further will be
    // measured and the run is complete.
    //
    // Gated on tact_exit_at, so only the tactical runners can trigger it -- --tact-play sets 0 and
    // is untouched. Verdicts use the same latches as the tactical path (a run that stopped both
    // ways must not report twice), and the exit reproduces it exactly: reports, prof, flush, then
    // TerminateProcess rather than ExitProcess -- see that block for why the difference is
    // measurable rather than cosmetic.
    if (g_cfg.tact_exit_at && g_tact_step > 0 && g_tact_step <= (uint32_t)g_cfg.tact_exit_at) {
        char b[160];
        wsprintfA(b,
                  "; TACT EXIT after frame %lu -- the mission ENDED before tact_exit_at (%d); the "
                  "tactical cadence has stopped, so the run is complete\n",
                  g_tact_step, g_cfg.tact_exit_at);
        append_line(g_log_path, b);
        g_tact_post_mission = true; // suppress the teardown census -- see tj_report()
        if (!g_tj_verdict_done) {
            g_tj_verdict_done = true;
            tj_report();
        }
        prof_report();
        log_flush();
        TerminateProcess(GetCurrentProcess(), 0);
    }

    // seed dump/inject at the configured step, once
    if (!g_seed_done && g_cfg.seed_step && g_step == (uint32_t)g_cfg.seed_step && g_cfg.seed_mode != 2) {
        if (g_cfg.seed_mode == 0) seed_dump();
        else seed_inject();
        g_seed_done = true;
    }

    // D6: synthetic moving-unit workload (see the block above). Issued BEFORE the hash so the order
    // is staged on the same step on every peer; it takes effect later via the scheduled lockstep lane.
    if (g_synth_armed && !g_synth_failed && g_cfg.synth_at > 0 && g_step >= (uint32_t)g_cfg.synth_at) {
        const uint32_t d = g_step - (uint32_t)g_cfg.synth_at;
        if (d == 0 || (g_cfg.synth_every > 0 && (d % (uint32_t)g_cfg.synth_every) == 0))
            synth_move_issue();
    }
    synth_move_verify();

    // SV1-P: the in-game save trigger. Issued at the step boundary, before the hash, so a save never
    // straddles a sim body. The planet is the CURRENT one -- the same index game::SaveGame's member
    // predicate would select -- and the mode is 3, which is what both real callers pass.
    if (g_cfg.save_at > 0 && g_saves_done < g_cfg.save_count && g_step >= (uint32_t)g_cfg.save_at) {
        const uint32_t d = g_step - (uint32_t)g_cfg.save_at;
        if (d == 0 || (g_cfg.save_every > 0 && (d % (uint32_t)g_cfg.save_every) == 0)) {
            const int32_t  planet = *reinterpret_cast<const int32_t *>(ADDR_PLANET_IDX);
            const unsigned rc     = mh::save::save_planet_now(planet, 3);
            ++g_saves_done;
            char line[192];
            wsprintfA(line,
                      "; [save] TRIGGER step=%lu #%d planet=%ld -> rc=%u (promoted=%d verify=%d)\n",
                      g_step, g_saves_done, (long)planet, rc, (int)mh::save::promotion_active(),
                      (int)mh::save::verify_active());
            // SV1-P-LOAD: archive each save under a distinct name. The per-planet path is FIXED, so a
            // sequence of saves overwrites itself -- and the period-2 invariant needs three of them
            // side by side (save1 and save3 must carry the region records in the same order, save2 in
            // the reverse, because every load pushes each node onto the head of the list).
            if (g_cfg.save_keep) {
                const char *src = mh::save::last_save_path();
                if (src && src[0]) {
                    char dst[320];
                    wsprintfA(dst, "%s.%d", src, g_saves_done);
                    CopyFileA(src, dst, FALSE);
                }
            }
            append_line(g_log_path, line);
        }
    }

    // SV1-P-CONTAINER: write a whole .sav through game::SaveGame -- the CONTAINER root, which embeds
    // the per-planet files it just wrote. Fixed name so a run is repeatable and the A/B has a path.
    if (g_cfg.savegame_at > 0 && g_step == (uint32_t)g_cfg.savegame_at) {
        static char    name[16] = "uitest";
        const unsigned rc       = mh::save::savegame_now(name);
        char           line[176];
        wsprintfA(line, "; [save] SAVEGAME step=%lu name=%s -> rc=%u (container promoted=%d)\n", g_step,
                  name, rc, (int)mh::save::container_promotion_active());
        append_line(g_log_path, line);
    }

    // SV1-P-CONTAINER: read a whole .sav back through llm_game_load -- the container READ root. It
    // REPLACES live state and REWRITES every embedded per-planet file, so it fires once and late.
    // The name matches savegame_at's, which is what makes the pair a round trip rather than two
    // unrelated triggers.
    if (g_cfg.loadgame_at > 0 && g_step == (uint32_t)g_cfg.loadgame_at) {
        const unsigned rc = mh::save::loadgame_now(g_cfg.loadgame_name);
        char           line[224];
        wsprintfA(line, "; [save] LOADGAME step=%lu name=%s -> rc=%u (container_load promoted=%d)\n",
                  g_step, g_cfg.loadgame_name, rc, (int)mh::save::container_load_promotion_active());
        append_line(g_log_path, line);
    }

    // SV1-P: LOAD THE FILE BACK, through the ORIGINAL LoadPlanetFromDisk -- the load direction is not
    // promoted, so this is a vanilla loader consuming whatever the save arm wrote. Run it in the
    // promoted arm and it answers "does a promoted build's save load in an unpromoted build"; run it
    // in the rollback arm and it answers the converse for a vanilla file. Once, near the end of the
    // run, because it REPLACES live state.
    if (g_cfg.load_at > 0 && g_loads_done < g_cfg.load_count && g_step >= (uint32_t)g_cfg.load_at &&
        ((g_step - (uint32_t)g_cfg.load_at) == 0 ||
         (g_cfg.load_every > 0 && ((g_step - (uint32_t)g_cfg.load_at) % (uint32_t)g_cfg.load_every) == 0))) {
        ++g_loads_done;
        const int32_t  planet = *reinterpret_cast<const int32_t *>(ADDR_PLANET_IDX);
        const unsigned rc     = mh::call::map_LoadPlanetFromDisk(planet, 3);
        char           line[192];
        wsprintfA(line, "; [save] LOADBACK step=%lu planet=%ld -> rc=%u (save was promoted=%d)\n", g_step,
                  (long)planet, rc, (int)mh::save::promotion_active());
        append_line(g_log_path, line);
    }

    // D2 probe: one control-group assignment, on the step named in the ini (host-only in the A/B).
    if (g_cfg.synth_ctrlgroup > 0 && g_step == (uint32_t)g_cfg.synth_ctrlgroup && g_idx_units >= 0)
        synth_ctrlgroup_issue();

    // AI1C read-bridge: one group move, then a guard 60 steps later that the six regions moved.
    if (g_cfg.synth_groupmove > 0 && g_step == (uint32_t)g_cfg.synth_groupmove && !g_synth_failed)
        synth_groupmove_issue();
    synth_groupmove_verify();

    // U32: the conquest workload. BEFORE the hash, like synth_move, so every order is staged on the
    // same step on every peer and takes effect later through the scheduled lockstep lane.
    if (g_conq_armed && !g_conq_failed && g_cfg.conq_at > 0 && g_step >= (uint32_t)g_cfg.conq_at)
        conq_issue();
    if (g_conq_armed && g_cfg.conq_probe_every > 0 && (g_step % (uint32_t)g_cfg.conq_probe_every) == 0)
        conq_probe();

    // Phase 2 replay: inject this step's recorded orders into the queue BEFORE the sim body runs
    // dispatch (the hash below reads pre-execution state either way, so ordering vs the hash is moot).
    if (g_cfg.order_mode == 2 && g_replay) order_replay_inject();

    // UNCONDITIONAL, both arms, every step -- and the "both arms" is the whole point. The clear
    // began life inside order_replay_inject, which runs only under order_mode==2, so the RECORD arm
    // kept its uncleared tails while the replay cleared them: record-vs-replay order_queue residue
    // went broad, and fixture_replay's verify correctly refused to attribute it to the known
    // instants mechanism. Symmetry is the fix. Dispatch reads only [0, count), so zeroing dead slots
    // changes no live order in either arm -- measured, not argued: the recording's orders.bin and
    // clock.bin are byte-identical across the re-records that introduced this.
    order_queue_tail_clear();

    // LIB-REF step-5000, count-ledger tag 16: the step's OPENING BALANCE for order_queue_count, read
    // at the same point in the step as the standalone host reads it (after inject + tail-clear,
    // before the hash). `g_replay_injected` is 0 in the RECORD arm, which is correct -- nothing was
    // injected there -- so the note describes both arms without a mode test.
    mh::sim::rng_trace_add_note(16u, (uint32_t)g_replay_injected, 0u, 0u, 0u,
                                (uint32_t)*reinterpret_cast<const int *>(ADDR_ORDER_QCOUNT()), 0u);

    // The save-state index probe: one "; AISTATE" line per player, once. See aistate_probe_at.
    if (g_cfg.aistate_probe_at > 0 && g_step == (uint32_t)g_cfg.aistate_probe_at) emit_aistate();

    // All-AI: prove the conversion took effect, once, from the first step after landing. Placed here
    // rather than at the end of the landing hook because spawn_ai_base runs INSIDE the function we
    // hooked the entry of -- reading player_data before it returns would read the pre-spawn state and
    // report NOT-SPAWNED for a conversion that worked.
    allai_verify();

    // Has the match resolved? Cheap (8 dword reads), but on a cadence anyway -- a soak runs tens of
    // thousands of steps and this answers a question that changes at most a handful of times.
    if (g_cfg.gameover_step > 0 && (g_step % (uint32_t)g_cfg.gameover_step) == 0) gameover_check();

    // D10 anti-vacuity probe: prove the AI subsystem is RUNNING, on a cadence.
    if (g_cfg.ai_probe_step > 0 && (g_step % (uint32_t)g_cfg.ai_probe_step) == 0) {
        const uint32_t ai_on = *mh::state::ptr<const uint32_t>(mh::state::RID_STRAT_AI_ENABLED);
        const uint32_t nplay =
            *reinterpret_cast<const uint32_t *>(mh::addr::_G_LLM_STRAT_AI_ACTIVE_PLAYER_COUNT);
        char en[PLAYER_DATA_COUNT + 1];
        int  eo = 0;
        for (int i = 0; i < PLAYER_DATA_COUNT; ++i) {
            const uint32_t f = *reinterpret_cast<const uint32_t *>(
                ADDR_PLAYER_DATA + (uintptr_t)i * PLAYER_DATA_STRIDE + PD_AI_ENABLED_OFF);
            eo += wsprintfA(en + eo, "%d", f ? 1 : 0);
        }
        const uint32_t *rng = reinterpret_cast<const uint32_t *>(mh::state::hash_base(IDX_RNG));
        char            al[200];
        wsprintfA(al, "; AIPROBE step=%lu ai_on=%lu nplayers=%lu ai_enabled=[%s] rng_ai=%08X rng_strat=%08X\n",
                  g_step, (unsigned long)ai_on, (unsigned long)nplay, en, rng[2], rng[0]);
        append_line(g_log_path, al);

        // Per-claimed-player detail: the FIRST run with an AI seated showed ai_on=1, nplayers=3 and
        // ai_enabled=[00100000] -- the AI correctly registered at slot 2 -- while rng_ai stayed
        // 00000000 for all 3000 steps. "Enabled" is evidently not "doing anything", so log what
        // separates the candidate explanations instead of guessing between them:
        //   acc/t0/t1/t2 = llm_strat_ai_players_tick's shared elapsed accumulator (+0x1003c) and the
        //     three phase timers (+0x10040/44/48). A phase fires only while its timer < acc, so
        //     acc not advancing => the AI never ticks; acc advancing past t* => it does.
        //   mother = the player's primary_mother_unit on the current planet -- 0 means it owns no
        //     mothership, i.e. nothing to think about.
        // Win32 wsprintfA has no %f, so floats are printed as milli-units via an explicit cast.
        const unsigned planet_i = *reinterpret_cast<const uint32_t *>(ADDR_PLANET_IDX);
        for (unsigned i = 0; i < nplay && i < (unsigned)PLAYER_DATA_COUNT; ++i) {
            const uintptr_t pd     = ADDR_PLAYER_DATA + (uintptr_t)i * PLAYER_DATA_STRIDE;
            const float     acc    = *reinterpret_cast<const float *>(pd + 0x1003c);
            const float     t0     = *reinterpret_cast<const float *>(pd + 0x10040);
            const float     t1     = *reinterpret_cast<const float *>(pd + 0x10044);
            const float     t2     = *reinterpret_cast<const float *>(pd + 0x10048);
            int             mother = -1;
            if (planet_i < 32) {
                const auto *prof =
                    reinterpret_cast<const mh::game::mh_llm_strat_player_profile *>(ADDR_PLAYERS_ARR) + i;
                mother = prof->primary_mother_unit[planet_i];
            }
            // D12 preconditions. EVERY slot-2 PRNG draw is gated behind AI GROUPS -- the reachable
            // paths are ai_player_tick -> group_expansion_form_or_repurpose -> llm_rand_below_ai, and
            // ai_unit_group_tick -> group_task_activate -> {scatter_random -> random_point_near,
            // loiter_wander}. So log what predicts a draw instead of re-running blind:
            //   phase = ai_phase_flags (+0x14): 0x1 construction planner, 0x2 unit training,
            //           0x4 UNIT-GROUP TASK MACHINE. Without 0x4, two of the three paths are dead
            //           regardless of how long the run is.
            //   groups = ai_group_count (+0x10564), max 0x20 -- group tasks have nothing to activate
            //           at 0, so this is THE predictor.
            //   inv = ai_invasion_force (+0x1c): 0 = full AI base (runs the economy planner and the
            //           group tasks), 1 = invasion-force-only, which skips straight past them.
            //   units = the game's OWN cached alive count, units[p][0].energy rounded -- slot 0 is a
            //           HEADER, not a unit (llm_strat_unit_count_alive_excluding_ctrl_group reads it
            //           exactly this way to bound its scan).
            // ai_established (+0x10) is the ONE-SHOT LATCH set when this AI's starting unit spawns,
            // and it gates all further per-tick AI phases in llm_strat_ai_player_tick -- so phase=07
            // means nothing if this is 0. bldg = the AI's building count, which separates "landed its
            // mothership and is developing a base" (the reading the user's skirmish experience
            // supports) from "never got anything".
            const uint32_t est   = *reinterpret_cast<const uint32_t *>(pd + 0x10);
            int            bldgs = -1;
            if (g_idx_bldgs >= 0) {
                const auto *bb =
                    reinterpret_cast<const mh::game::mh_map_object_building *>(mh::state::hash_base(g_idx_bldgs)) +
                    (uintptr_t)i * 100u;
                bldgs = 0;
                for (uint32_t b = 0; b < 100; ++b)
                    if (bb[b].building_id != 0) ++bldgs; // occupied slot
            }
            const uint8_t  phase  = *reinterpret_cast<const uint8_t *>(pd + 0x14);
            const uint32_t inv    = *reinterpret_cast<const uint32_t *>(pd + 0x1c);
            const uint32_t groups = *reinterpret_cast<const uint32_t *>(pd + 0x10564);
            // BOTH the cached count AND a real scan. units[p][0].energy is a CACHE the game trusts to
            // bound its own scans; whether it is maintained for every player is not something to
            // assume, and the first run read cached=0 for the AI while it still held a mothership
            // handle -- internally inconsistent, so the two are logged separately and compared.
            int units_cached = -1, units_scan = -1;
            if (g_idx_units >= 0) {
                const auto *ub =
                    reinterpret_cast<const mh::game::mh_map_object_unit *>(mh::state::hash_base(g_idx_units)) +
                    (uintptr_t)i * 100u;
                units_cached = (int)(ub[0].energy + 0.5); // slot 0 is a HEADER: the cached count
                units_scan   = 0;
                for (uint32_t u = 1; u < 100; ++u)
                    if (ub[u].energy > 0.0) ++units_scan;
            }
            char pl[300];
            wsprintfA(pl,
                      ";   AIP p%u en=%d est=%lu inv=%lu phase=%02X groups=%lu uc=%d us=%d bldg=%d mother=%d acc_ms=%d t0_ms=%d t1_ms=%d t2_ms=%d\n",
                      i, *reinterpret_cast<const uint32_t *>(pd + PD_AI_ENABLED_OFF) ? 1 : 0,
                      (unsigned long)est, (unsigned long)inv, (unsigned)phase, (unsigned long)groups,
                      units_cached, units_scan, bldgs, mother, (int)(acc * 1000.0f),
                      (int)(t0 * 1000.0f), (int)(t1 * 1000.0f), (int)(t2 * 1000.0f));
            append_line(g_log_path, pl);
        }
    }

    // D11 negative test: flip one byte in every region at/after region_poke_min, this peer only, once.
    // `region_poke_only` narrows that to a single index -- see its declaration for why the range is
    // wrong for a per-module red arm.
    if (g_cfg.region_poke_at > 0 && (g_cfg.region_poke_min >= 0 || g_cfg.region_poke_only >= 0) &&
        g_step == (uint32_t)g_cfg.region_poke_at) {
        const int lo = g_cfg.region_poke_only >= 0 ? g_cfg.region_poke_only : g_cfg.region_poke_min;
        const int hi = g_cfg.region_poke_only >= 0 ? g_cfg.region_poke_only + 1 : N_REGIONS;
        for (int i = lo; i < hi && i < N_REGIONS; ++i) {
            uint8_t *b = mh::state::mutate_target(i, (uint32_t)g_cfg.region_poke_off);
            *b ^= 0xA5;
            // REPORT WHERE IT ACTUALLY WROTE, not what was asked for. mutate_target CLAMPS the offset
            // to len-1, and the A/B's red arm deliberately asks for a huge one to mean "the region's
            // last byte" -- so printing the request logged `@00DD8C48+268435455`, an address outside
            // the region, for a write that landed at +186399. Derived from the returned pointer, so the
            // line and the write cannot drift.
            const uint32_t real_off =
                (uint32_t)(b - reinterpret_cast<uint8_t *>(
                                   static_cast<uintptr_t>(mh::state::hash_base(i))));
            char pl[176];
            wsprintfA(pl, "; REGION POKE step=%lu idx=%d %-14s @%08X+%lu -> %02X (D11 negative test)\n",
                      g_step, i, REGIONS[i].name, (unsigned)mh::state::hash_base(i),
                      (unsigned long)real_off, (unsigned)*b);
            append_line(g_log_path, pl);
        }
    }

    // U30(b) negative test: one unknown-outer-tag frame, this peer -> the others, once.
    if (g_cfg.garble_at > 0 && g_step == (uint32_t)g_cfg.garble_at) {
        unsigned char tag = 6; // outside the valid outer set (1..5); see lockstep/turn_engine.h
        MH_Net_Send(MH_NET_BROADCAST, &tag, 1);
        char pl[160];
        wsprintfA(pl, "; GARBLE SEND step=%lu outer_tag=6 -> peers (U30(b) negative test)\n", g_step);
        append_line(g_log_path, pl);
    }

    // D3 negative test: flip one PRNG slot on this peer only, once. Done BEFORE the hash so the very
    // step named in the ini is the step the divergence is reported at (no off-by-one to argue about).
    if (g_cfg.rng_perturb_slot >= 0 && g_cfg.rng_perturb_slot < 4 && g_cfg.rng_perturb_step > 0 &&
        g_step == (uint32_t)g_cfg.rng_perturb_step) {
        uint32_t      *rng          = reinterpret_cast<uint32_t *>(mh::state::hash_base(IDX_RNG));
        const uint32_t before       = rng[g_cfg.rng_perturb_slot];
        rng[g_cfg.rng_perturb_slot] = (before ^ 0x1234u) & 0xffffu; // stay in the PRNG's 16-bit domain
        char pl[160];
        wsprintfA(pl, "; RNG PERTURB step=%lu slot=%d %08X -> %08X (D3 negative test)\n",
                  g_step, g_cfg.rng_perturb_slot, before, rng[g_cfg.rng_perturb_slot]);
        append_line(g_log_path, pl);
    }

    // hash: combined (all regions) + state-only (excludes the state_excluded() set: p0/p1_ai_econ +
    // order_pending -- see the IDX_* block). The state-only hash isolates whether the *simulated game
    // state* replays identically. Two regions are hashed MASKED rather than flat, both for the same
    // reason -- they interleave per-SIM-STEP state with per-FRAME state, and frame timing is not a
    // desync: tile_objects (fog bits) and rng_state (the fx PRNG slot; D3, 2026-07-27).
    uint64_t combined = 1469598103934665603ULL;
    uint64_t state    = 1469598103934665603ULL;
    uint64_t per[N_REGIONS];
    for (int i = 0; i < N_REGIONS; ++i) {
        // ST6 phase 1: the slice emits ITSELF into a hash sink. The three masked walks moved into
        // state/region_view.h as emitters; this loop no longer knows which regions are special, and
        // it no longer dereferences a base.
        per[i]   = mh::state::hash_slice(i, g_cfg.mask_ctrl_group != 0, g_cfg.mask_soldier_anim != 0,
                                         g_cfg.mask_planets_gfx != 0);
        combined = fnv1a(&per[i], sizeof(per[i]), combined); // fold region hashes in order
        if (!state_excluded(i)) state = fnv1a(&per[i], sizeof(per[i]), state);
    }

    // D21: hand the runtime desync detector THIS hash rather than making it walk the same 2.79 MB a
    // second time. It is the same boundary and the same manifest order, so the number it puts on the
    // wire and the number on the next line of this log are the same number by construction. When no
    // harness is armed the detector is driven by its own trampoline instead (net_seams.cpp) -- see
    // desync/desync_watch.cpp for why one hook cannot serve both configurations.
    // Once, at the first hashed step: the inbound refusal count since open. See the open site.
    if (g_step == 1) {
        {
            char ib[160];
            wsprintfA(ib, "; [libmh_in] inbound refusals since open: %d (first: %s)\n",
                      mh::libmh_in::trap_count() - MH_Core_TrapsAtOpen(), mh::libmh_in::last_trap());
            append_line(g_log_path, ib);
        }
    }
    mh::desync::on_sim_step_hashed(per, N_REGIONS, state);

    const uint64_t clock = *reinterpret_cast<const uint64_t *>(mh::state::hash_base(IDX_CLOCK)); // game_clock bits
    // The clock track. `order_mode == 1` IMPLIES clock_record, so every recording path that predates
    // the split writes exactly the clock it always did; the standalone key exists for LIB-REF-LIVE's
    // order-free fixture, which needs a clock and must NOT produce an order stream. See Config.
    if (g_cfg.order_mode == 1 || g_cfg.clock_record) clock_record(clock);

    // LIB-WORLD: the step-0 world capture, HERE and nowhere else.
    //
    // This placement is the item's whole pairing argument, so it is worth being explicit about. The
    // blob has to describe the same instant as the hash the oracle reproduces, and the cheapest way
    // to guarantee that is not to argue it -- it is to take the capture from the SAME `combined` and
    // `state` variables one statement after they are computed, before anything else in this function
    // runs. There is then no window for state to move, and the run's own log line on the next line
    // carries the same two numbers, so the blob is cross-checkable against the log rather than only
    // against a field it wrote itself.
    //
    // AND `g_step == 1` IS THE STEP-0 STATE, which reads backwards until you see why: on_sim_step is
    // the ENTRY detour on llm_strat_sim_step, so it runs PRE-BODY -- it incremented g_step at the
    // top of this function and is hashing what exists BEFORE the first sim step executes.
    //
    // THE COST IS REAL AND IS DELIBERATELY PAID HERE. Building ~7.7 MB, hashing it, censusing its
    // dwords and writing the file stalls this step by tens of milliseconds. That is harmless
    // BECAUSE of where it sits: the hash is already taken, so nothing the oracle compares can move,
    // and in a recording run the stall lands in the clock track like any other slow frame and is
    // replayed faithfully. (A 2-peer recording should still arm this on ONE peer -- a stall this
    // size on both is a lockstep hiccup nobody needs.)
    if (g_cfg.world_capture && g_step == 1) world_snapshot_capture(combined, state, clock);

    // mp:X1b -- the live snapshot verbs, HERE for world_snapshot_capture's reason one line up: the
    // blob and the per-region hashes it must reproduce are taken from the same `per[]`/`combined`/
    // `state` this block just computed, so there is no window between them for state to move.
    if (g_cfg.snapshot_at > 0 && g_step >= (uint32_t)g_cfg.snapshot_at)
        snapshot_send_now(per, combined, state, clock);
    if (g_cfg.snapshot_import) snapshot_poll_now();

    char line[160];
    wsprintfA(line, "%lu %08X%08X %08X%08X %08X%08X\n", g_step,
              (unsigned)(clock >> 32), (unsigned)clock,
              (unsigned)(combined >> 32), (unsigned)combined,
              (unsigned)(state >> 32), (unsigned)state);
    append_line(g_log_path, line);

    // mp:X3: the SUB-DOMAIN trail, BEFORE the R row and from the same per[], so a step carrying
    // both has them describing one instant with the coarse row first -- which is also the order a
    // reader wants them in (which subsystem, then which region inside it).
    if (g_cfg.domain_hash_step > 0 && (g_step % (uint32_t)g_cfg.domain_hash_step) == 0) {
        subdomain_map_emit(); // one-shot; a compare per row after the first
        subdomain_row(per);
    }

    // Phase 1a: per-region hash line every N steps -- "R <step> <h0> <h1> ... <hN>". mp_analyze.py
    // reads this to localize the (step, region) of a cross-peer desync without re-running.
    if (g_cfg.region_hash_step > 0 && (g_step % (uint32_t)g_cfg.region_hash_step) == 0) {
        // Capacity is DERIVED from N_REGIONS, never a round number. This was `char rl[512]` and it
        // overflowed the moment D11 took the manifest from 23 regions to 41: each region appends
        // exactly 17 chars (" %08X%08X"), so 41 needs ~711 and 23 needed ~405 -- it had been fitting
        // with no margin, and nothing said so. Both peers died one step into every run, which reads
        // like a game/mapping fault and is not one. Growing REGIONS[] must never again require
        // remembering a buffer size somewhere else in the file.
        constexpr int R_HDR = 2 + 10 + 2; // "R " + up to 10 digits of step + '\n' + NUL
        char          rl[R_HDR + N_REGIONS * 17];
        int           off = wsprintfA(rl, "R %lu", g_step);
        for (int i = 0; i < N_REGIONS; ++i)
            off += wsprintfA(rl + off, " %08X%08X", (unsigned)(per[i] >> 32), (unsigned)per[i]);
        rl[off++] = '\n';
        rl[off]   = '\0';
        append_line(g_log_path, rl);
    }

    // The RAW REGION DUMP (rdump_*), at the SAME sample point as the `R` line above so a byte here and
    // a hash there describe the same instant. See the config block for what it is for and its one
    // caveat. Read-only: it copies bytes out and touches nothing.
    if (g_cfg.rdump_hi >= g_cfg.rdump_lo && g_step >= (uint32_t)g_cfg.rdump_lo &&
        g_step <= (uint32_t)g_cfg.rdump_hi) {
        // THE FP ENVIRONMENT, at the same sample point, so the two arms' control words are comparable
        // instant-for-instant with the bytes above. One line per dumped step; see mh::fp::raw_x87_cw.
        {
            char fb[96];
            wsprintfA(fb, "FPENV %lu cw=%04X mxcsr=%08X\n", (unsigned long)g_step,
                      (unsigned)mh::fp::raw_x87_cw(), (unsigned)mh::fp::raw_mxcsr());
            append_line(g_log_path, fb);
        }
        // C-prime: flush the draws recorded SINCE THE LAST DUMP, so each line's `step` is the step
        // the draw belongs to and the file reads as one ordered sequence. Emitted at the same sample
        // point as everything else in this block.
        {
            static int flushed = 0;
            const int  n       = mh::sim::rng_trace_count();
            char       rb[128];
            for (int i = flushed; i < n; ++i) {
                const mh::sim::rng_trace_entry &e = mh::sim::rng_trace_at(i);
                wsprintfA(rb, "RNGD %d %lu %d %04X %p %p\n", i, (unsigned long)e.step, e.channel,
                          (unsigned)e.after, e.ra, e.site);
                append_line(g_log_path, rb);
            }
            flushed             = n;
            static int nflushed = 0;
            const int  nn       = mh::sim::rng_trace_note_count();
            for (int i = nflushed; i < nn; ++i) {
                const mh::sim::rng_trace_note &e2 = mh::sim::rng_trace_note_at(i);
                wsprintfA(rb, "NOTE %d %lu %lu %08X %08X %08X %08X %08X %08X\n", i,
                          (unsigned long)e2.step, (unsigned long)e2.tag, e2.a, e2.b, e2.c, e2.d,
                          e2.e, e2.f);
                append_line(g_log_path, rb);
            }
            nflushed = nn;
            if (mh::sim::rng_trace_overflowed())
                append_line(g_log_path, "; RNGD OVERFLOW -- the window is too wide, indices past the "
                                        "cap are MISSING and the sequence diff is invalid\n");
        }
        if (g_cfg.rdump_rid >= 0 && g_cfg.rdump_rid < N_REGIONS) {
            const int rid = g_cfg.rdump_rid;
            rdump_emit(reinterpret_cast<const uint8_t *>(mh::state::hash_base(rid)), REGIONS[rid].len,
                       rid);
        }
        // The registry route: ANY region, hashed or not, at wherever it LIVES. Emitted with column
        // -2 so a reader can never confuse it with a hash-column dump (-1 is the address window).
        if (g_cfg.rdump_regid >= 0 && g_cfg.rdump_regid < (int)mh::state::RID_COUNT) {
            const auto     r = static_cast<mh::state::region_id>(g_cfg.rdump_regid);
            const uint32_t n = mh::state::reach_of(r);
            const auto    *b = reinterpret_cast<const uint8_t *>(mh::state::live_base(r));
            if (b != nullptr && n > 0) rdump_emit(b, n, -2);
        }
        // THE ADDRESS WINDOW, gated once on readability and on the cap. Both refusals are LOUD and
        // once-only: a silent skip here reads exactly like "nothing differed", which is the single
        // wrong answer this instrument must never give.
        if (g_cfg.rdump_addr != 0 && g_cfg.rdump_len > 0) {
            static bool announced = false, usable = false;
            if (!announced) {
                announced = true;
                char b[256];
                if ((uint32_t)g_cfg.rdump_len > RDUMP_ADDR_MAX) {
                    wsprintfA(b,
                              "; [rdump] REFUSED: rdump_len %d exceeds the %lu-byte cap -- narrow the "
                              "window rather than raising it\n",
                              g_cfg.rdump_len, (unsigned long)RDUMP_ADDR_MAX);
                } else if (!range_is_readable((uintptr_t)(uint32_t)g_cfg.rdump_addr,
                                              (uint32_t)g_cfg.rdump_len)) {
                    wsprintfA(b,
                              "; [rdump] REFUSED: [0x%08lX, +%d) is not fully committed and readable "
                              "-- check the address, nothing was dumped\n",
                              (unsigned long)(uint32_t)g_cfg.rdump_addr, g_cfg.rdump_len);
                } else {
                    usable = true;
                    wsprintfA(b, "; [rdump] ADDR 0x%08lX len %d, steps %d..%d (rid field is -1)\n",
                              (unsigned long)(uint32_t)g_cfg.rdump_addr, g_cfg.rdump_len,
                              g_cfg.rdump_lo, g_cfg.rdump_hi);
                }
                append_line(g_log_path, b);
            }
            if (usable)
                rdump_emit(reinterpret_cast<const uint8_t *>((uintptr_t)(uint32_t)g_cfg.rdump_addr),
                           (uint32_t)g_cfg.rdump_len, -1);
        }
    }

    // Order-buffer dump (order_log): the "order_pending" region hashed above is only a COUNT; this dumps
    // the actual scheduled-order contents at the SAME sample point, so a host-vs-client diff of the ";ord"
    // lines at a desync step shows whether the orders are identical (count read at a different release
    // phase = benign) or genuinely different (a real bug). Only emitted when a buffer is non-empty.
    if (g_cfg.order_log) {
        int pc = *reinterpret_cast<const int *>(ADDR_ORDER_PCOUNT());
        int qc = *reinterpret_cast<const int *>(ADDR_ORDER_QCOUNT());
        // SIM1-P clause 8: STAGING joins the sample, so the channel describes all THREE order regions
        // the strategic sim decides through instead of two. Same sample point and same line format, so
        // nothing that reads `;ord` has to learn a second shape -- an `S` tag beside `P` and `Q`.
        int sc = *reinterpret_cast<const int *>(ADDR_ORDER_SCOUNT());
        if (pc > 0 || qc > 0 || sc > 0) {
            char h[80];
            wsprintfA(h, ";ord %lu pend=%d q=%d stage=%d\n", g_step, pc, qc, sc);
            append_line(g_log_path, h);
            order_dump_array("P", ADDR_ORDER_PENDING(), pc, ORDER_PCAP);
            order_dump_array("Q", ADDR_ORDER_QUEUE(), qc, ORDER_QCAP);
            order_dump_array("S", ADDR_ORDER_STAGING(), sc, ORDER_SCAP);
        }
    }

    if (g_cfg.stop_step && g_step >= (uint32_t)g_cfg.stop_step) {
        char hdr[96];
        wsprintfA(hdr, "; per-region breakdown at stop step %lu:\n", g_step);
        append_line(g_log_path, hdr);
        for (int i = 0; i < N_REGIONS; ++i) {
            char rl[128];
            wsprintfA(rl, ";   %-16s %08X%08X\n", REGIONS[i].name,
                      (unsigned)(per[i] >> 32), (unsigned)per[i]);
            append_line(g_log_path, rl);
        }
        // raw rng_state dump, UNMASKED -- localise which PRNG slot(s) drift across replays. Four
        // slots, not sixteen (D3): strat / fx / ai / unreachable. The fx slot is masked out of the
        // hash above, so this line is the only place it stays observable -- keep it.
        const uint32_t *rng = reinterpret_cast<const uint32_t *>(mh::state::hash_base(IDX_RNG));
        char            rl[128];
        wsprintfA(rl, ";   rng strat=%08X fx=%08X ai=%08X unused=%08X\n",
                  rng[0], rng[1], rng[2], rng[3]);
        append_line(g_log_path, rl);
        // The shadow sites' FINAL verdict, here rather than in shadow.cpp, because this is the one
        // place that knows the run is over. Without it a site called fewer than 200 times ends its
        // log on the call-#1 progress line, which reads `1 call(s), 0 divergence(s)` by construction.
        // LIB-REF-SOAK: the injector's evidence line, UNCONDITIONAL in a replay run -- it does not
        // depend on rng_trace being armed, on a window, or on anything a later run might forget to
        // pass. fixture_replay's recorder guard reads it to decide whether a `0 -> n` fixture is the
        // benign shape (a sparse order stream over an idle queue) or the refused one (an injector
        // that appends without setting the count). Absent line == no evidence == still refused.
        if (g_cfg.order_mode == 2 && g_replay) {
            char ib[160];
            wsprintfA(ib, "; injector: count SET at %d of %d arrival step(s), %d append-without-set\n",
                      g_inject_set_ok, g_inject_arrivals, g_inject_no_set);
            append_line(g_log_path, ib);
        }
        g_active = false;
        // FLUSH UNCONDITIONALLY, and the NON-exiting path is the one that needs it. append_line only
        // writes on the NEXT call (a buffer overflow, or the 1 s cadence it checks on entry), and
        // g_active=false guarantees there is no next call -- so without this the run's last <=64 KB
        // dies with the process, every time, silently.
        //
        // MEASURED 2026-08-27, after "why does it stall at step 786?": at stop_step=800 the tail
        // steps AND this whole breakdown block sat unflushed in the buffer. mp_run.steps_done keys
        // `done` off the "; per-region breakdown" line, so it never saw the run finish; ui_test's
        // progress watchdog then aborted the run as a STALL at 786; and mp_analyze compared 786 of
        // 800 steps while still printing ALL PAIRS IDENTICAL. One missing flush, three wrong
        // readings, none of which looked like a logging bug.
        log_flush();
        // LIB-REF-REC: THE EXIT STEP'S ORDERS (user ruling, 2026-09-11). This block runs from
        // on_sim_step, i.e. at llm_strat_sim_step's ENTRY -- so exiting HERE kills the process before
        // the stop step's BODY runs, and the recorder never sees that step: order_record() hangs off
        // llm_strat_order_queue_dispatch's entry, which lives inside the body. Measured on the fifth
        // fixture: mh_orders.bin's highest step was 4999 for a stop_step=5000 run, while mh_clock.bin
        // (written above, pre-body) carried all 5000 -- so the recording's LAST STEP had a hash line
        // and a clock but no orders, and the replay injected k=0 there. That is what left the
        // standalone reference host one order short at step 5000 while every earlier step was masked
        // by the injector's `count = k` overwrite.
        //
        // THE FIX IS TO DEFER THE EXIT BY ONE STEP BOUNDARY, not to snapshot the queue here. Here is
        // the HASH instant; the recorder's instant is dispatch entry, and llm_strat_sim_step runs
        // ai_players_tick BETWEEN them (sim_step.cpp: the head gates). Recording the exit step at this
        // instant would give that one step a different sampling point from the other 4999 -- the very
        // mechanism fixture_replay's attribute() exists to account for, silently mis-stated for the
        // last row. Deferring instead lets the body run exactly as every other step's did, so the
        // exit step's record is taken by the same code at the same instant as all the rest.
        //
        // SAFE BECAUSE g_active IS ALREADY FALSE: the next on_sim_step hashes nothing, records no
        // clock and advances no counter -- it returns at the `!g_active` gate. The latch is tested
        // ABOVE that gate (and in on_sim_tick, which is the per-frame pump and therefore fires even
        // if the sim never takes another step), so both exits are provably AFTER the stop step's
        // body and before anything of step N+1 is observed.
        if (g_cfg.exit_on_stop) g_exit_after_body = true;
    }
}

// ---- debug-console command hook (SHIFT+ENTER line, raw, pre-uppercase) ---------------------------
// EAX at console entry = char* to the typed line. We recognise a few harness verbs; we always let
// the original dispatcher run afterwards (an unmatched line is a harmless no-op there).
void on_console(const char *line) {
    if (!line) return;
    char u[64];
    int  i = 0;
    for (; line[i] && i < 63; ++i) {
        char c = line[i];
        u[i]   = (c >= 'a' && c <= 'z') ? c - 32 : c;
    }
    u[i]     = '\0';
    auto has = [&](const char *k) { for (int j = 0; u[j]; ++j){ int m=0; while(k[m]&&u[j+m]==k[m])++m; if(!k[m])return true; } return false; };
    if (has("HDUMP")) {
        seed_dump();
        append_line(g_log_path, "; console: HDUMP\n");
    } else if (has("HLOAD")) {
        seed_inject();
        append_line(g_log_path, "; console: HLOAD\n");
    } else if (has("HSTOP")) {
        g_active = false;
        append_line(g_log_path, "; console: HSTOP\n");
    } else if (has("HGO")) {
        g_active = true;
        append_line(g_log_path, "; console: HGO\n");
    } else if (has("HRESET")) {
        g_step       = 0;
        g_seed_done  = false;
        g_saves_done = 0; // SV1-P: the save budget is per-run, and HRESET restarts the run
        g_loads_done = 0;
        append_line(g_log_path, "; console: HRESET\n");
    }
}

// ---- inline trampoline hooking (5-byte E9 detour, steal >=5 whole bytes) -------------------------
// naked detours reference the trampoline via these globals (indirect jmp through memory)
void *g_tick_tramp = nullptr;
void *g_sim_tramp  = nullptr;
void *g_con_tramp  = nullptr;
void *g_disp_tramp = nullptr;
void *g_land_tramp = nullptr;
// C10. The generated entry thunk for mh::sim::land_players_on_planet, or null. Set by
// MH_Harness_RebindLandPlayers when the sim_resid promotion finds this detour already holding the
// entry -- the three-outcome shape sim_tick_detour uses (C6). Without it the two facilities were
// mutually exclusive: the detour arms first (MH_Harness_Init runs before every promotion), so the
// promotion's own entry install was REFUSED and the domain reported "30/31 seams installed --
// PARTIAL, treat this run as invalid" on every all-AI soak.
void *g_land_promoted = nullptr;
// SIM1-P clause 6: set in the early gate-load block (which precedes every install) and consumed by the
// derived yield after the installs, because the derivation needs claims that do not exist yet up there.
bool g_yield_claimed_rebinds = false;
// F4E: set by MH_Harness_SetModuleRefused when this module could not bind its own contract tables.
// Zero in net_selftest.exe, which compiles this file with no boundary to bind across.
bool  g_module_refused = false;
void *g_tact_tramp     = nullptr; // TACT-PREP: llm_tact_frame's stolen prologue

// RI-SIM / SIM1F domain-root PROMOTED-GOLDEN (the C6 rebind pattern, one instrument over from sim_tick).
// The harness owns sim_step's entry, so the reimpl cannot MH_EXPORT_REPLACE-install it (refused "already
// hooked"). Instead reimpl_probe reads [promote] sim_step and calls MH_Harness_RebindSimStep with the
// generated entry thunk for mh::sim::sim_step; when set, sim_step_detour FALLS THROUGH to that thunk
// instead of the stolen-prologue trampoline -- AFTER on_sim_step has hashed the pre-body state, so the
// per-step golden trajectory compares OUR body's output against the original's. A `void*` (thunk
// address, or null) is both the flag and the jump target, exactly like g_tick_promoted below.
void *g_sim_step_promoted = nullptr;

// C6 REBIND TARGET -- the same shape C4 used for time_tick (net_lockstep.cpp, g_tt_promoted).
// Null = fall through to the original via the stolen-prologue trampoline, which is the shipped default
// and byte-for-byte today's behaviour. Non-null = fall through to mh::lockstep's promoted sim_tick.
//
// ONE OWNER PER ENTRY, and here that rule is doing more work than usual. The instrument and the subject
// both want ADDR_SIM_TICK, and whichever patches second refuses -- with the asymmetric failure mode that
// if the HARNESS is the one that refuses, the run is silently VOIDED (no hashes recorded) rather than
// failed. So the harness keeps the entry unconditionally and only its DESTINATION moves; the promotion
// side asks for the rebind instead of competing for the bytes.
void *g_tick_promoted = nullptr;
// D18. Same role for the order-record detour: null = fall through to the original's stolen prologue,
// non-null = the generated entry thunk for mh::sim::promoted_arm::order_queue_dispatch. Both flag and
// jump target in one word, exactly like g_tick_promoted.
void *g_disp_promoted = nullptr;

// order_queue_dispatch entry hook -- record mode snapshots the queue about to execute.
void on_dispatch() {
    if (g_cfg.order_mode == 1) order_record();
}

__declspec(naked) void dispatch_detour() {
    __asm {
        pushad
        pushfd
        call on_dispatch
        popfd
        popad
                            // D18: the C6 rebind, copied from sim_tick_detour below -- see the EFLAGS note there, which
                            // applies here for the same reason (this is a FUNCTION ENTRY; nothing reads incoming flags).
                            // Without this the recorder and `[promote] sim_dispatch` are mutually exclusive: whichever
                            // arms second loses, and MEASURED, the loser was the promotion -- refused with "entry bytes
                            // ... (wrong build, or already hooked)", a build diagnosis from a perfectly good build.
        cmp  dword ptr [g_disp_promoted], 0
        jne  promoted
        jmp  dword ptr [g_disp_tramp] // stolen 8-byte prologue + jmp back to dispatch+8
    promoted:
        jmp  dword ptr [g_disp_promoted] // the generated entry thunk -> mh::sim promoted body
    }
}

__declspec(naked) void sim_tick_detour() {
    __asm {
        pushad
        pushfd
        call on_sim_tick
        popfd
        popad
                            // C6. The CMP clobbers EFLAGS after the POPFD restored them. Safe here and only here: this is a
                            // FUNCTION ENTRY, and neither llm_strat_sim_tick's Watcom prologue nor our entry thunk reads
                            // incoming flags -- an entry takes its arguments in registers and on the stack, never in the
                            // status word. Do not copy this pattern to a mid-function splice. (Same note as
                            // net_lockstep.cpp's time_tick_detour, which is where this pattern comes from.)
        cmp  dword ptr [g_tick_promoted], 0
        jne  promoted
        jmp  dword ptr [g_tick_tramp] // stolen prologue + jmp back to sim_tick+8
    promoted:
        jmp  dword ptr [g_tick_promoted] // the generated entry thunk -> mh::lockstep::sim_tick
    }
}

// Entry hook on llm_game_land_players_on_planet. Its 8-byte prologue is `55 89 e5 68 54 00 00 00`
// (push ebp / mov ebp,esp / push 0x54), so an 8-byte steal lands exactly on an instruction boundary
// -- the same generated arm-guard bytes mh_export.gen.h carries for this function.
__declspec(naked) void land_players_detour() {
    __asm {
        // SPCAMP-SEED: stash the __watcall argument (planet_index, in EAX) BEFORE pushad, so
        // on_land_players can name the planet whose landing slots to read. A plain MOV to memory
        // touches no flags and no other register, so it is safe ahead of the save pair.
        mov  dword ptr [g_land_planet], eax
        pushad
        pushfd
        call on_land_players
        popfd
        popad
                                       // C10, the same three-outcome tail as sim_tick_detour. The CMP clobbers EFLAGS after the
                                       // POPFD restored them, which is safe HERE and only here for the reason recorded there: this
                                       // is a FUNCTION ENTRY, and neither the Watcom prologue nor our entry thunk reads incoming
                                       // flags -- an entry takes its arguments in registers and on the stack, never in the status
                                       // word. Do not copy this to a mid-function splice.
        cmp  dword ptr [g_land_promoted], 0
        jne  promoted
        jmp  dword ptr [g_land_tramp] // stolen prologue + jmp back to land_players+8
    promoted:
        jmp  dword ptr [g_land_promoted] // the generated entry thunk -> mh::sim::land_players_on_planet
    }
}

__declspec(naked) void sim_step_detour() {
    __asm {
        pushad
        pushfd
        call on_sim_step
        popfd
        popad
                            // mp:X3 THE SIM HOLD. Checked BEFORE the promotion branch, because it has to hold both
                            // arms: a promoted run reaches the body through g_sim_step_promoted and an unpromoted one
                            // through the stolen prologue, and "the sim must not step" is a statement about the body,
                            // not about which implementation of it would have run. The CMP clobbers EFLAGS after POPFD
                            // restored them -- safe here for the reason the promotion CMP below is safe and no other:
                            // this is a FUNCTION ENTRY, and neither llm_strat_sim_step's Watcom prologue nor its caller
                            // reads incoming flags. RET (not JMP) because llm_strat_sim_step is `void (void)` with no
                            // stack arguments, so returning to its caller leaves exactly the machine state the body's
                            // own return would have. Do not copy this to a mid-function splice.
        cmp  dword ptr [g_sim_hold], 0
        jne  sim_step_held
            // RI-SIM / SIM1F domain-root PROMOTED-GOLDEN (C6 rebind, mirroring sim_tick_detour above): with
            // [promote] sim_step=1 the detour falls through to OUR entry thunk instead of the original body,
            // AFTER on_sim_step hashed the pre-body state, so the per-step golden trajectory measures our
            // body against the original's. The CMP clobbers EFLAGS after POPFD restored them -- safe here
            // and only here because this is a FUNCTION ENTRY: neither llm_strat_sim_step's Watcom prologue
            // nor our entry thunk reads incoming flags. The thunk is jumped to (tail), so it returns
            // straight to llm_strat_sim_step's caller, skipping the original body. Do not copy to a
            // mid-function splice. (Same note as sim_tick_detour / net_lockstep.cpp's time_tick_detour.)
        cmp  dword ptr [g_sim_step_promoted], 0
        jne  sim_step_promoted
        jmp  dword ptr [g_sim_tramp] // NOT promoted: stolen prologue + jmp back to sim_step+8 (original)
    sim_step_promoted:
        jmp  dword ptr [g_sim_step_promoted] // the generated entry thunk -> mh::sim::sim_step
    sim_step_held:
        ret // mp:X3: the body does not run this step, nor any later one
    }
}

// TACT1E (2026-08-28) C6 REBIND TARGET -- the tactical twin of g_sim_step_promoted above, and it
// exists for the same reason: llm_tact_frame's entry has exactly one owner and it must be THIS
// detour (it carries the per-frame `T` hash the whole tactical trajectory oracle is built on), so
// the reimplementation cannot win the entry with an export install -- measured 2026-08-28:
// "; [export] llm_tact_frame NOT armed -- entry is OWNED by an unnamed DLL detour". Null = fall
// through to the original via the stolen prologue, which is the shipped default and byte-for-byte
// today's behaviour. Non-null = the generated entry thunk for mh::tact::promoted_arm::frame.
void *g_tact_frame_promoted = nullptr;

// TACT-PREP: llm_tact_frame's entry. Same shape as sim_step_detour -- observe, then fall through to
// the stolen prologue, or (TACT1E) to OUR body when the promotion rebound it. The `cmp/jne` was
// dead weight until something reimplemented the tactical frame pump; mh::tact::frame now does.
__declspec(naked) void tact_frame_detour() {
    __asm {
        pushad
        pushfd
        call on_tact_frame
        popfd
        popad
                            // The CMP clobbers EFLAGS after POPFD restored them -- safe here and ONLY here because this
                            // is a FUNCTION ENTRY: neither llm_tact_frame's Watcom prologue nor our entry thunk reads
                            // incoming flags. (Same note as sim_step_detour / sim_tick_detour; do not copy to a
                            // mid-function splice.) The thunk is jumped to, so it returns straight to llm_tact_frame's
                            // caller, skipping the original body entirely.
        cmp  dword ptr [g_tact_frame_promoted], 0
        jne  tact_frame_promoted
        jmp  dword ptr [g_tact_tramp]
    tact_frame_promoted:
        jmp  dword ptr [g_tact_frame_promoted]
    }
}

__declspec(naked) void console_detour() {
    __asm {
        pushfd
        pushad
        mov  eax, [esp+0x1C] // saved EAX (watcall arg0 = line ptr) after pushfd+pushad
        push eax
        call on_console
        add  esp, 4
        popad
        popfd
        jmp  dword ptr [g_con_tramp]
    }
}

} // namespace

// RI-SIM / SIM1F: point the harness's sim_step detour at a REPLACEMENT body (the generated entry thunk
// for mh::sim::sim_step), exactly as MH_Harness_RebindSimStep's sibling does for sim_tick (C6). The
// harness owns sim_step's entry (its per-step golden-hash + SIM-CUT boundary), so the reimpl cannot win
// the entry with an export install -- instead the detour keeps the entry and only its fall-through
// moves. Returns 1 if the rebind took, 0 if the harness is not armed in this process (no
// `[harness] enable=1`),
// in which case nobody owns the entry and an ordinary entry install is the correct route. Called from
// reimpl_probe after it reads [promote] sim_step from mh_net.ini.
extern "C" int MH_Harness_RebindSimStep(void *ours) {
    if (!g_sim_tramp || !ours) return 0;
    g_sim_step_promoted = ours;
    return 1;
}

// C10: the FOURTH use of the same rebind, for llm_game_land_players_on_planet. The all-AI soak's
// landing-conversion detour arms inside MH_Harness_Init, which runs before every promotion, so the
// detour ALWAYS wins this entry and sim_resid's own install was always refused -- 30/31, "PARTIAL,
// treat this run as invalid", on every all-AI soak since SIM-RESID-P landed. The recorded symptom in
// C10 had it the other way round (the promotion winning and the detour reporting "ALLAI NOT armed");
// that never happens, and it was measured on 2026-09-05.
//
// Returns 1 if the rebind took, 0 if the landing detour is not armed this run (no [test] all_ai=1),
// in which case nobody owns the entry and the ordinary entry install is the right route.
//
// THIS IS NOT SUFFICIENT ON ITS OWN and the reason belongs next to the code: the rebind fixes the
// ENTRY contest, so an ORIGINAL caller still runs the conversion and then lands in our body. It does
// nothing for a SIBLING that never reaches the entry -- our own promoted session_begin_multi calls
// detail::land_players_on_planet directly. That edge is covered by mh::sim::set_land_players_observer
// below, and both are needed.
extern "C" int MH_Harness_RebindLandPlayers(void *ours) {
    if (!g_land_tramp || !ours) return 0;
    g_land_promoted = ours;
    append_line(g_log_path, "; ALLAI REBOUND: the landing detour now falls through to OUR promoted "
                            "land_players body -- conversion still runs first (C10)\n");
    return 1;
}

// THE TWO TACTICAL REBINDS ARE GONE (fork F5H, 2026-09-14). MH_Harness_RebindTactFrame and
// MH_Harness_RebindTactEnqueue stood here, one per tactical detour, and F2E took their only callers
// with the 18 tactical MH_EXPORT_REPLACE installs: tact is demoted permanently, so nothing asks for
// either rebind. F4E measured them callerless and parked the delete to F5's sweep; this is it. The
// detours themselves are untouched -- g_tact_frame_promoted / g_tj_enq_promoted still exist and
// their naked fall-through branch still reads them, so a future tactical promotion re-adds a setter
// rather than re-plumbing the detour.

// D18: the same for llm_strat_order_queue_dispatch, and it exists because the order RECORDER holds
// that entry whenever `[test] order_mode=1`. Measured 2026-08-27: with the recorder on,
// `[promote] sim_dispatch=1` was REFUSED -- "entry bytes 90909070837229E9 != expected
// 0000008468E58955 (wrong build, or already hooked)" -- so the two facilities were silently exclusive
// and the message blamed the image. Returns 1 if the rebind took, 0 if no detour is installed here (no
// recording this run), in which case nobody owns the entry and the ordinary export install is right.
// D18. Everything the harness must do AFTER the promotions exist. Called at the end of MH_Seam_Init,
// which is the only point in the process where "is this entry ours?" has a stable answer:
// MH_Harness_Init runs BEFORE MH_Seam_Init, so anything the harness decides at its own init time
// necessarily predates every promotion.
//
// Today that is one thing -- the `[harness] replay_suppress_enqueue` neuter of
// llm_strat_order_enqueue. Doing it at harness-init time meant the harness always won the entry and
// `[promote] orders` (default ON; enqueue is one of its nine seams) then found it rewritten:
// MEASURED, "[export] llm_strat_order_enqueue NOT armed -- entry bytes 0000002868C3C031 != expected
// 0000002868E58955 (wrong build, or already hooked)" and then "[promote] orders: 8/9 seams installed
// -- PARTIAL, treat this run as invalid". Using one facility invalidated the other, in language that
// blamed the image.
//
// Deferring lets each configuration take the mechanism that fits it: if the entry is OURS the
// suppression is carried as behaviour by our body (mh::orders::set_suppress_enqueue), and if it is
// still the original's the byte neuter goes in exactly as before.
//
// F4G: the body is a static so the export can FLUSH after it whichever of its three exits it takes.
// Same reason MH_Harness_Init flushes at its end -- this is the last of the arm report, it runs
// before any frame, and a run killed at the menu never appends again.
static void late_arm_report(void) {
    if (g_cfg.order_mode != 2 || !g_cfg.replay_suppress_enqueue) return;

    // THE SINK GATE IS SET UNCONDITIONALLY (user ruling, 2026-09-11), promoted or not. It lives in
    // mh::orders::detail::enqueue now -- the one point every append passes through -- so it catches
    // BOTH the game's entry (via the promotion thunk) and every libmh-internal caller that
    // MH_LIBMH_BIND routes straight to our body, which is the population the old promotion-wrapper
    // placement could not see and which appended one synth_move order per step straight past it
    // (LIB-REF's step-5000 residual). Setting it even when the entry is NOT ours is deliberate and
    // not redundant: the rebind rows are armed independently of `[promote] orders`, so in that
    // configuration the byte neuter below covers the original's entry and this covers the rebound
    // internal callers. Neither alone is the documented sentence.
    mh::orders::set_suppress_enqueue(true);

    if (mh::hook::promoted_owner_of(ADDR_ORDER_ENQUEUE)) {
        append_line(g_log_path,
                    "; replay_suppress_enqueue: llm_strat_order_enqueue is PROMOTED -- suppression "
                    "CARRIED by our body AT THE SINK (mh::orders::detail::enqueue returns 0 and "
                    "appends nothing, for the game's entry AND every rebound internal caller) "
                    "instead of a byte neuter, and the orders promotion stays whole (D18)\n");
        return;
    }

    // Neuter llm_strat_order_enqueue -> `xor eax,eax; ret` (return 0, append nothing). __watcall
    // passes its 4 args in registers so no stack cleanup is needed; the entry has no frame yet.
    // Guarded by the Watcom prologue. Lets the AI keep running while ONLY injected orders reach the
    // queue -- the faithful-substitute test.
    //
    // D5: the guard and the three bytes live in the hook point's row now (`point::order_enqueue`).
    // This was the ONE place in harness-owned code that hand-rolled its own VirtualProtect instead of
    // going through a primitive at all, which is why check_fork_d5_hooks forbids VirtualProtect here
    // as well as the three install primitives -- "zero raw-primitive callers" would otherwise be
    // satisfiable by writing the bytes by hand.
    if (mh::hook::arm_neuter(mh::hook::point::order_enqueue)) {
        append_line(g_log_path, "; replay_suppress_enqueue: armed (original entry neutered)\n");
    } else {
        // Neither the prologue nor promoted: a genuine wrong-build/unknown entry. This branch NEVER
        // degrades quietly, because the damage here is not a blind measurement but a WRONG one --
        // un-neutered, the AI's own orders keep reaching the queue beside the replayed ones and the
        // isolation the whole test rests on is gone, on a run that otherwise looks normal.
        append_line(g_log_path,
                    "; replay_suppress_enqueue: enqueue prologue mismatch -- NOT armed. THE REPLAY IS "
                    "NOT ISOLATED IN THIS RUN: AI orders still reach the queue.\n");
    }
}

extern "C" void MH_Harness_LateArm(void) {
    late_arm_report();
    log_flush();
}

extern "C" int MH_Harness_RebindOrderDispatch(void *ours) {
    if (!g_disp_tramp || !ours) return 0;
    g_disp_promoted = ours;
    append_line(g_log_path,
                "; [promote] sim_dispatch REBOUND: the order-record detour keeps the entry and now "
                "falls through to OURS (recording still runs first) -- D18\n");
    return 1;
}


// ==== MH_Harness_Init -- the harness PROPER, and nothing else ====================================
//
// Everything that used to run unconditionally above the gate is MH_Core_Arm_Early() now (F3B); this
// function is the instrument from its first statement. DllMain calls the two back to back, in that
// order, so nothing about the boot sequence moved -- only the ownership of the first half.
// F4E: mh_harness.dll's own module init calls this when either of its two contract tables came back
// short -- most importantly when there is no libmh.dll in the process (ruling Q4: a hard loud refusal,
// not an instrument reading invented state). The refusal itself has already happened by then, on
// three channels; this only makes the arm gate below honour it.
//
// IT IS A SETTER RATHER THAN A READ OF THE MODULE, because this file is compiled by two projects.
// net_selftest.exe links the whole seam set into one offline image where there is no boundary and no
// module: nothing calls this, the flag stays 0, and the instrument behaves exactly as it always did.
extern "C" void MH_Harness_SetModuleRefused(int refused) { g_module_refused = refused != 0; }

extern "C" int MH_Harness_Init(void) {
    // F4E: TAKE THE PATHS FIRST, because everything below reads one -- harness_enabled() itself
    // reads g_ini_path. mh.dll's build_paths() filled them inside MH_Core_Arm_Early, which DllMain
    // calls immediately before this, so they are final by the time we get here. Copied rather than
    // pointed at: two static CRTs mean two heaps and nothing crosses owning memory (the standing
    // rule -- docs/dll-split.md). See mh/include/mh_core_arm_paths.h for why mh.dll composes them.
    copy_arm_paths();
    // SIM1-P clause 6: set BEFORE the gate, so its value is the same one MH_Core_Arm_Early used to
    // set it to (it was `harness_enabled()` there, evaluated one call earlier in the same boot). The
    // derived yield that consumes it runs after the installs, further down this same function.
    g_yield_claimed_rebinds = harness_enabled();
    // F2G (ruling Q6): the arm is an EXPLICIT key now -- `[harness] enable=1` in mh_net.ini --
    // where it used to be the mere existence of mh_harness.ini. Default off, so a plain install and
    // every un-instrumented run are unchanged. See harness_enabled().
    if (!harness_enabled()) return 0; // not armed => harness inert, ship-safe
    // F4E / ruling Q4: the instrument's own dependency is missing (no libmh.dll in this process, or a
    // mismatched pair). The refusal was already written to mh_harness_refused.log, OutputDebugString
    // and stderr by the module init; what must NOT happen here is arming half an instrument. And
    // nothing is written to mh_harness.log ON PURPOSE -- that file's EXISTENCE is what every consumer
    // treats as "this run was instrumented", so a refused run must leave none.
    if (g_module_refused) return 0;
    if (!mh::en_build_ok()) { // EN-only: arm nothing on any other image
        append_line(g_log_path, "; harness NOT armed: not the EN build\n");
        return 0;
    }
    load_config();

    // ST2M: the runtime manifest-index guard that stood here is GONE, and its absence is the point.
    // It checked at every arm that IDX_RNG/IDX_CLOCK and the nine exclusion indices still named the
    // regions they were written for, because a hand-appended table could silently re-label them (they
    // shifted +5 when player_data was split). The indices are now generated FROM the table and the
    // exclusion flag travels WITH the entry, so the drift the guard watched for cannot be expressed.
    // The equivalent check survives as a build-time one: mh_regions.gen.h static_asserts every slice
    // against its registry region, and tools/gen_state_registry.py --check fails on any drift.

    // Version/host guard: both targets must open with the exact Watcom prologue 55 89 e5 68
    // (LE dword 0x68e58955). If not, this is the wrong exe/build (or a stray ini in another
    // process) -- do NOT patch random memory; leave the host untouched and report inert.
    constexpr uint32_t PROLOGUE = 0x68e58955u;
    if (*reinterpret_cast<const uint32_t *>(ADDR_SIM_TICK) != PROLOGUE ||
        *reinterpret_cast<const uint32_t *>(ADDR_SIM_STEP) != PROLOGUE ||
        *reinterpret_cast<const uint32_t *>(ADDR_CONSOLE) != PROLOGUE) {
        append_line(g_log_path, "; harness NOT armed: unexpected prologue bytes "
                                "(wrong mh.exe build or wrong host process)\n");
        return 0;
    }

    if (g_cfg.pin_fpu) {
        unsigned cur = 0;
        _controlfp_s(&cur, _PC_53, _MCW_PC);
        _controlfp_s(&cur, _RC_NEAR, _MCW_RC);
    }

    // P0-SPDET: pin the wall clock BEFORE the sim hooks go in, so the very first frame already sees
    // the deterministic value. Logged ARMED / NOT-armed by name either way: a not-armed pin and a
    // pin that armed but was never called look identical downstream, and only this line separates
    // them (the lesson from the shadow sites -- read the ARMED line before any count line).
    if (g_cfg.pin_wallclock) {
        // U30: the caller-side prologue compare is gone (the primitive owns it), so the two NOT-armed
        // branches collapse to one. Note WHERE the reason appears: this file writes mh_harness.log and
        // MH_Harness_Init runs before the promotion logger exists, so the primitive's per-refusal line
        // is not written -- but the refusal IS filed, and MH_Seam_Init's end-of-arming [interlock]
        // summary in mh_net.log names it.
        g_pin_now = static_cast<double>(g_cfg.pin_clock_base_s);
        g_pin_dt  = static_cast<double>(g_cfg.pin_clock_dt_us) / 1000000.0;
        if (mh::hook::arm_replacement(mh::hook::point::pin_wallclock,
                                      reinterpret_cast<const void *>(pinned_get_current_time))) {
            char m[160];
            wsprintfA(m, "; pin_wallclock ARMED: GetCurrentTime -> base=%ds dt=%dus (deterministic)\n",
                      g_cfg.pin_clock_base_s, g_cfg.pin_clock_dt_us);
            append_line(g_log_path, m);
        } else {
            append_line(g_log_path, "; pin_wallclock NOT armed: install_jmp REFUSED (named in the "
                                    "[interlock] summary in mh_net.log)\n");
            g_cfg.pin_wallclock = 0;
        }
    }
    // pin_menu_clock rides on the same counter, so it needs pin_wallclock's base/dt to have been set
    // -- but it is a SEPARATE key, because replacing the binary's MASTER ms timer has a much wider
    // blast radius than replacing one FP leaf and should be opted into on its own.
    if (g_cfg.pin_menu_clock) {
        if (!g_cfg.pin_wallclock) {
            g_pin_now = static_cast<double>(g_cfg.pin_clock_base_s);
            g_pin_dt  = static_cast<double>(g_cfg.pin_clock_dt_us) / 1000000.0;
        }
        // MH_EXPORT_REPLACE, NOT install_jmp, and the refusal that forced this is worth keeping:
        // install_jmp guards on the WATCOM PROLOGUE, and this function does not have one -- it opens
        // `INC dword ptr [_G_LLM_TIME_TICK_QUERY_COUNT]`, so the guard read a "prologue MISMATCH" and
        // declined. The export generator already carries this entry's real first 8 bytes as its arm
        // guard (entry_llm_time_get_ticks_ms), which is exactly the check that suits a function whose
        // entry is ordinary code.
        if (mh_export_install_llm_time_get_ticks_ms()) {
            // G146: and once the clock IS pinned, the journal barrier's settle dwell must be measured
            // in it rather than in GetTickCount -- otherwise the one wall-clock wait left in the
            // replay path costs a frame-rate-dependent number of presents, which is the whole leak.
            // Armed here, next to the pin itself, so the two can never be on different clocks. The
            // reader is OURS rather than the game's published global for the reason the export's
            // comment gives: that global stops being written once the menu stops running.
            MH_UIDrive_SetSettleClock(pinned_ticks_ms_value);
            char m[208];
            wsprintfA(m,
                      "; pin_menu_clock ARMED: llm_time_get_ticks_ms -> pinned dt=%dus/advance"
                      " (menu input stops depending on the frame rate; screen dwell pinned too)\n",
                      g_cfg.pin_clock_dt_us);
            append_line(g_log_path, m);
        } else {
            append_line(g_log_path, "; pin_menu_clock NOT armed: install_jmp REFUSED\n");
            g_cfg.pin_menu_clock = 0;
        }
    }

    // SPCAMP-FLAKE: the two UI-path entry counters the TJPS row reports. Run-before-and-continue
    // detours, armed only when the row is being logged at all, so an ordinary run keeps the ship
    // instruction stream. Both entries open with the Watcom prologue (mh_export.gen.h carries their
    // 8 arm-guard bytes: 0x...68e58955 for each), so the primitive's own default guard is the right
    // one and a wrong build refuses instead of corrupting. entry_claim::exclusive -- neither
    // function is promoted; if one ever is, the refusal names it in the [interlock] summary rather
    // than silently producing a counter that never moves.
    if (g_cfg.tj_ps_log > 0) {
        const bool iu = mh::hook::arm_observer(mh::hook::point::ui_input_update,
                                               (void *)probe_iu_detour, &g_probe_iu_tramp);
        const bool pn = mh::hook::arm_observer(mh::hook::point::ui_storage_panel,
                                               (void *)probe_pn_detour, &g_probe_pn_tramp);
        char       m[200];
        wsprintfA(m, "; tj_ps_log probes: input_update=%s storage_bldg_panel=%s\n",
                  iu ? "ARMED" : "REFUSED", pn ? "ARMED" : "REFUSED");
        append_line(g_log_path, m);
    }

    // TACT-PREP: the deterministic rand. Installed BEFORE the tactical hook and before anything can
    // draw, for the same reason pin_wallclock goes in before the sim hooks -- the very first draw
    // must already be ours, and llm_rand is reachable from the menus (llm_ui_demo_slideshow_tick).
    // Whole-body replacement, so the guard is the exact 8 entry bytes the export header generated
    // against, not the generic Watcom prologue: llm_rand is 36 bytes and does not open with one.
    if (g_cfg.pin_rand) {
        if (!mh::hook::entry_bytes_match(mh::hook::point::pin_rand)) {
            append_line(g_log_path, "; pin_rand NOT armed: entry bytes at llm_rand do not match the "
                                    "8 the header was generated against\n");
            g_cfg.pin_rand = 0;
            // D5: the point's row carries `expect = 0` for the reason stated two lines up -- llm_rand
            // is 36 bytes and DOES NOT open with a Watcom frame, so the guard is the exact eight
            // entry bytes the export header was generated against (the row's entry8, which
            // entry_bytes_match just asked). Handing the primitive WATCOM_PROLOGUE would refuse this
            // hook outright. It still asks the ownership half, which is the part that was missing.
        } else if (mh::hook::arm_replacement(mh::hook::point::pin_rand,
                                             reinterpret_cast<const void *>(pinned_rand))) {
            g_rand_state = (uint32_t)g_cfg.rand_seed;
            // NO defer_entry() HERE, and the reason is worth stating: llm_rand carries
            // `gate: false` in tools/data/tact_effect_classes.json because its six stolen bytes are
            // PUSH EDX + CALL rel32, which install_trampoline would copy without relocating. So
            // there is no gate to hand over -- nothing ever writes over this entry but us. What
            // makes the disposition real anyway is pinned_rand_next() below asking the seam.
            char m[128];
            wsprintfA(m, "; pin_rand ARMED: llm_rand -> deterministic LCG, seed=%d\n", g_cfg.rand_seed);
            append_line(g_log_path, m);
        } else {
            append_line(g_log_path, "; pin_rand NOT armed: install_jmp REFUSED (named in the "
                                    "[interlock] summary in mh_net.log)\n");
            g_cfg.pin_rand = 0;
        }
    }

    // SPCAMP-SEED: the strategic seed. Installed here, beside pin_rand, because they are neighbours in
    // intent and NOT in effect -- see the Config comment: pin_rand pins the CRT rand() and reaches no
    // strategic state at all. Armed before anything can start a session, since the one call that
    // matters happens inside llm_strat_planet_session_begin, at the top, on the menu's own thread.
    if (g_cfg.pin_strat_seed) {
        if (!mh::hook::entry_bytes_match(mh::hook::point::pin_strat_seed)) {
            append_line(g_log_path, "; pin_strat_seed NOT armed: entry bytes at "
                                    "llm_strat_rng_seed_wallclock_seconds do not match the 8 the "
                                    "header was generated against\n");
            g_cfg.pin_strat_seed = 0;
        } else if (mh::hook::arm_replacement(mh::hook::point::pin_strat_seed,
                                             reinterpret_cast<const void *>(pinned_strat_seed))) {
            g_strat_seed_value = (uint32_t)g_cfg.strat_seed;
            // LIFT-TABLE S5: the pin has to reach libmh's PUSHED session seed too, not only the
            // original's entry. Our promoted llm_strat_planet_session_begin reads
            // mh::state::session_seed(), so a trampoline over the original function would leave the
            // promoted arm on the wall clock and the stock arm pinned -- an A/B whose two arms
            // disagree about the seed is worse than no pin at all.
            libmh_set_session_seed((int32_t)g_cfg.strat_seed);
            char m[160];
            wsprintfA(m, "; pin_strat_seed ARMED: llm_strat_rng_seed_wallclock_seconds -> %d "
                         "(the campaign landing roll stops depending on the clock second)\n",
                      g_cfg.strat_seed);
            append_line(g_log_path, m);
        } else {
            append_line(g_log_path, "; pin_strat_seed NOT armed: install_jmp REFUSED (named in the "
                                    "[interlock] summary in mh_net.log)\n");
            g_cfg.pin_strat_seed = 0;
        }
    }

    // The sampler starts here rather than in DllMain: it duplicates a handle to the CALLING
    // thread, and DllMain runs on whatever thread performed the injection, which is not necessarily
    // the one that will run frames. Arming from the arm path means the sampled thread is the thread
    // that does the work.
    hash_fingerprint_report();
    prof_start();
    // TACT-PREP: the tactical cadence. Only installed when asked for -- with tact_hash_step=0 no
    // byte of llm_tact_frame is touched, so every existing run keeps its exact behaviour and the
    // strategic goldens are unaffected by this file having grown a second oracle.
    // TACT-SYNTH rides this same hook -- llm_tact_frame's entry is the only place a tactical frame
    // boundary is visible -- so the hook goes in when EITHER is asked for, while the two remain
    // independent knobs. Coupling them would make "hash every 4th frame" silently quarter the order
    // rate, which is a coverage number quietly changed by an unrelated setting.
    if (g_cfg.tact_synth && g_cfg.tact_synth_seed == 0) {
        // HARD ERROR, the same rule synth_seed carries: an unseeded workload is not reproducible
        // evidence, and a silent default would make two runs agree for a reason that is not
        // determinism.
        append_line(g_log_path, "; tact_synth NOT armed: tact_synth_seed=0. A pinned seed is "
                                "required -- an unseeded workload is not evidence.\n");
        g_cfg.tact_synth = 0;
    }
    // TACT-REC: the order-journal seams. Armed only when something asks for them, so a run that
    // wants no journal keeps the stock entries byte-for-byte.
    if (g_cfg.tact_journal_rec || g_cfg.tact_journal_probe || g_cfg.tact_journal[0]) {
        // RECORDING A REPLAY IS REFUSED, not merged. The recorder cannot tell the replayer's
        // injected orders from a player's -- they arrive through the same API -- so a run doing both
        // would write a journal with every order duplicated, and the duplicate would look exactly
        // like a real one.
        if (g_cfg.tact_journal_rec && g_cfg.tact_journal[0] && !g_cfg.tact_journal_verify) {
            append_line(g_log_path,
                        "; TJ NOT ARMED: tact_journal_rec and tact_journal are mutually exclusive"
                        " -- recording a replay would double every order\n");
            g_cfg.tact_journal_rec = 0;
            g_cfg.tact_journal[0]  = 0;
        }
        // ...EXCEPT in verify mode, where doing both at once IS the measurement -- and it is safe
        // for a reason worth stating, because the exclusion above is otherwise correct. The order
        // detours journal a call only when tj_is_player() says the CALL SITE is one of the game's
        // own UI/frame sites. Verify mode injects no orders at all (see tj_replay_frame), and any
        // order it did inject would carry a call site inside mh.dll, which is not a player site.
        // So every record this mode writes was emitted by the game, caused by replayed input.
        if (g_cfg.tact_journal_verify && !g_cfg.tact_journal[0]) {
            append_line(g_log_path,
                        "; TJ NOT ARMED: tact_journal_verify needs a journal to replay\n");
            g_cfg.tact_journal_verify = 0;
        } else if (g_cfg.tact_journal_verify) {
            g_cfg.tact_journal_rec = 1; // the order seams must be armed to observe anything
        }
    }
    if (g_cfg.tact_journal_rec || g_cfg.tact_journal_probe) {
        // U30, two changes. (1) The joint pre-check STAYS -- it is all-or-nothing on purpose, and a
        // half-installed journal records half a game -- but it is now the PRIMITIVE'S decision asked
        // without the write (detour_refusal), so an entry another mechanism owns is named instead of
        // being reported as an unexpected prologue. (2) The two install returns were DISCARDED, so a
        // VirtualProtect failure left the ARMED line below asserting coverage the run did not have.
        const bool ok_e = mh::hook::available(mh::hook::point::tact_order_enqueue);
        const bool ok_g = mh::hook::available(mh::hook::point::tact_group_order);
        if (!ok_e || !ok_g) {
            append_line(g_log_path,
                        "; TJ RECORD NOT ARMED: an order seam's entry is unavailable (see the "
                        "[interlock] summary in mh_net.log for which and why)\n");
            g_cfg.tact_journal_rec   = 0;
            g_cfg.tact_journal_probe = 0;
        } else if (!mh::hook::arm_observer(mh::hook::point::tact_order_enqueue,
                                           reinterpret_cast<void *>(tj_enqueue_detour),
                                           &g_tj_enq_tramp) ||
                   !mh::hook::arm_observer(mh::hook::point::tact_group_order,
                                           reinterpret_cast<void *>(tj_group_detour),
                                           &g_tj_grp_tramp)) {
            append_line(g_log_path,
                        "; TJ RECORD NOT ARMED: an order seam's trampoline install FAILED after its "
                        "entry was clear -- treat any journal from this run as incomplete\n");
            g_cfg.tact_journal_rec   = 0;
            g_cfg.tact_journal_probe = 0;
        } else {
            char m[200];
            wsprintfA(m, "; TJ RECORD ARMED: both order seams observed (rec=%d probe=%d verify=%d)\n",
                      g_cfg.tact_journal_rec, g_cfg.tact_journal_probe,
                      g_cfg.tact_journal_verify);
            append_line(g_log_path, m);
        }
    }
    // UI-REC: the present-indexed arm. Two refusals, both for the tactical arm's reasons.
    if (ui_journal_armed()) {
        // ONE JOURNAL, ONE INDEX. Both arms stamp g_tj_idx, so a run with both armed would write a
        // file whose frame column alternates between the present counter and the tactical frame
        // counter -- monotonic-looking, parseable, and describing no session that ever happened.
        if (g_cfg.tact_journal_rec || g_cfg.tact_journal[0]) {
            append_line(g_log_path,
                        "; UI-REC NOT ARMED: the ui_journal and tact_journal arms are mutually"
                        " exclusive -- one journal cannot carry two cadences' frame numbers\n");
            g_cfg.ui_journal_rec = 0;
            g_cfg.ui_journal[0]  = 0;
        } else if (g_cfg.ui_journal_rec && g_cfg.ui_journal[0]) {
            append_line(g_log_path,
                        "; UI-REC NOT ARMED: ui_journal_rec and ui_journal are mutually exclusive --"
                        " the recorder cannot tell our injected events from a player's\n");
            g_cfg.ui_journal_rec = 0;
            g_cfg.ui_journal[0]  = 0;
        } else {
            if (g_cfg.ui_journal[0]) tj_load(g_cfg.ui_journal);
            char m[220];
            wsprintfA(m, "; UI-REC ARMED: present-indexed input journal (rec=%d journal=%s)\n",
                      g_cfg.ui_journal_rec, g_cfg.ui_journal[0] ? g_cfg.ui_journal : "(none)");
            append_line(g_log_path, m);
        }
    }
    if (g_cfg.tact_journal[0]) tj_load(g_cfg.tact_journal);

    // ---- SPCAMP-FLAKE: A REPLAY MUST HAVE EXACTLY ONE INPUT PRODUCER ------------------------
    //
    // The journal injects into the game's own event rings. So does the game, all the time:
    // llm_input_wndproc_tap runs on EVERY window message, from llm_wnd_proc, and is the rings'
    // sole producer entry (the DirectInput poll is a sub-path INSIDE it, taken when
    // _G_LLM_DI_MOUSE_DEVICE is non-null -- so zeroing that pointer selects the other arm rather
    // than removing a producer). The drain runs once per WM_PAINT. Two producers, one consumer,
    // and a batch boundary set by the OS message schedule: that is the whole of the flake.
    // MEASURED: a divergent run's mouse-ring write cursor ran 2 ahead of a clean run's SEVEN
    // STEPS BEFORE the state hash moved, with the cursor at (0,479) -- a position that appears
    // in none of the journal's 37,383 records.
    //
    // WHY A WHOLE-BODY NO-OP IS SAFE HERE, checked rather than assumed (Ghidra, read-only):
    // the tap is `void`; llm_wnd_proc discards it and decides eat-vs-DefWindowProc entirely on
    // its own uMsg dispatch, so suppressing it cannot change message routing. Its other outputs
    // are all things the replay supplies itself: the mouse BUTTON_STATE/wheel/last-XY latches
    // exist only to stamp events the tap itself queues (ours carry the journal's own values),
    // the key ring is injected directly, and _G_LLM_INPUT_KEYSTATE is already written by
    // tji_keystate_apply and re-asserted every present. Its one remaining write, an hWnd copy at
    // 0x00e69dbc, has exactly ONE reference in the binary -- that write. Nothing reads it.
    // Suppressing the tap also retires the reason tji_keystate_reassert exists: the DI keyboard
    // poll it fights is reached only from here.
    //
    // NOT the arm guard the prologue check would use: this entry is `push esi; push edi; push
    // ebp; mov ebp,esp; ...`, not the Watcom frame, so it is guarded on the exact eight bytes
    // the export header generated against -- strictly stronger, and the same shape pin_rand uses.
    // Default ON for a replay and OFF otherwise; `[harness] replay_isolate_input=0` turns it off,
    // which is the NEGATIVE arm this item's done_when requires.
    if (g_tj_ev && g_cfg.replay_isolate_input) {
        if (!mh::hook::entry_bytes_match(mh::hook::point::pin_input_wndproc_tap)) {
            append_line(g_log_path,
                        "; replay_isolate_input NOT armed: entry bytes at llm_input_wndproc_tap"
                        " do not match the 8 the header was generated against\n");
        } else if (mh::hook::arm_replacement(mh::hook::point::pin_input_wndproc_tap,
                                             (const void *)wndproc_tap_suppressed)) {
            append_line(g_log_path,
                        "; replay_isolate_input ARMED: llm_input_wndproc_tap suppressed -- the"
                        " journal is the ONLY writer of the input rings for this run\n");
        } else {
            append_line(g_log_path, "; replay_isolate_input NOT armed: install_jmp REFUSED\n");
        }
    }

    if (g_cfg.tact_hash_step > 0 || g_cfg.tact_synth) {
        // U30: the compare moved into the primitive, and the return -- previously DISCARDED -- now
        // decides whether the cadence reports itself armed.
        if (!mh::hook::arm_observer(mh::hook::point::tact_frame,
                                    reinterpret_cast<void *>(tact_frame_detour), &g_tact_tramp)) {
            append_line(g_log_path, "; tact cadence NOT armed: llm_tact_frame REFUSED (named in the "
                                    "[interlock] summary in mh_net.log)\n");
            g_cfg.tact_hash_step = 0;
            g_cfg.tact_synth     = 0;
        } else {
            char m[160];
            wsprintfA(m, "; tact cadence ARMED: llm_tact_frame -> T/TR every %d frame(s) over %d "
                         "regions (poke idx=%d at frame=%d)\n",
                      g_cfg.tact_hash_step, mh::state::TACT_HASH_REGION_COUNT, g_cfg.tact_poke_idx,
                      g_cfg.tact_poke_at);
            append_line(g_log_path, m);
            if (g_cfg.tact_synth) {
                wsprintfA(m, "; tact_synth ARMED: seed=%d at=%d every=%d stop=%d units=%d owner=%d "
                             "direct=%d actions=%d\n",
                          g_cfg.tact_synth_seed, g_cfg.tact_synth_at, g_cfg.tact_synth_every,
                          g_cfg.tact_synth_stop, g_cfg.tact_synth_units, g_cfg.tact_synth_owner,
                          g_cfg.tact_synth_direct, (int)TA_COUNT);
                append_line(g_log_path, m);
            }
        }
    }

    // entry_claim::rebind (C9): sim_tick/sim_step are PROMOTABLE, and this detour keeping their
    // entries is the C6 protocol rather than a collision -- reimpl_probe later rebinds what these
    // fall through to (MH_Harness_RebindSimStep). Without the claim the interlock would refuse them.
    //
    // U30, two things. (1) All three returns were DISCARDED: a VirtualProtect failure here is
    // invisible and the harness goes on emitting per-step hashes from a hook that is not installed --
    // the determinism verdict would be read off a run that never hooked the sim. They are checked
    // now. (2) They keep the DEFAULT byte expectation, because these three are prologue-guarded
    // already -- not per site, but by the whole-function gate at the top of MH_Harness_Init, which
    // checks exactly these three addresses and returns inert. Passing the default re-states that
    // guard where the write happens; nothing between the two rewrites these entries.
    {
        const bool ok_tick = mh::hook::arm_observer(mh::hook::point::sim_tick,
                                                    reinterpret_cast<void *>(sim_tick_detour),
                                                    &g_tick_tramp);
        const bool ok_step = mh::hook::arm_observer(mh::hook::point::sim_step,
                                                    reinterpret_cast<void *>(sim_step_detour),
                                                    &g_sim_tramp);
        const bool ok_con  = mh::hook::arm_observer(mh::hook::point::console,
                                                    reinterpret_cast<void *>(console_detour),
                                                    &g_con_tramp);
        if (!ok_tick || !ok_step || !ok_con) {
            char m[224];
            wsprintfA(m,
                      "; HARNESS HOOKS REFUSED: sim_tick=%d sim_step=%d console=%d -- a 0 means that "
                      "hook is NOT installed, so any per-step hash or console output this run reports "
                      "is NOT evidence. See the [interlock] summary in mh_net.log.\n",
                      (int)ok_tick, (int)ok_step, (int)ok_con);
            append_line(g_log_path, m);
        }
    }
    // (RI-SIM / SIM1F: sim_step's detour falls through to OUR body when reimpl_probe later rebinds it
    // via MH_Harness_RebindSimStep -- the "REBOUND" line is logged there, like sim_tick's, not here.)

    // The all-AI soak. Guarded on the same prologue as the three above and logged either way -- an
    // unarmed hook and an armed one that never converted anything produce the same silence otherwise.
    // `|| land_log` (SPCAMP-SEED): the landing observable rides this same detour -- it is the only
    // hook that sees the planet index and the pre-roll RNG state. The two are independent settings;
    // a refusal disarms BOTH, because a land_log that silently never fires would report an empty
    // landing as a matching one.
    if (g_cfg.all_ai || g_cfg.land_log) {
        if (!mh::hook::entry_bytes_match(mh::hook::point::land_players)) {
            append_line(g_log_path, "; ALLAI NOT armed: entry bytes at land_players_on_planet do not "
                                    "match the 8 the header was generated against\n");
            g_cfg.all_ai   = 0;
            g_cfg.land_log = 0;
        } else {
            // The point's row carries both decisions this site used to spell out. `expect = 0`: the
            // exact eight entry bytes were compared above (entry_bytes_match), which is the stronger
            // guard. `entry_claim::rebind` (C10), was ::exclusive -- llm_game_land_players_on_planet
            // IS PROMOTABLE (sim_resid), and this detour keeping the entry is the DESIGN rather than
            // a conflict: MH_Harness_RebindLandPlayers moves only its fall-through. With ::exclusive
            // the interlock refused the promotion instead, which is what made [test] all_ai=1 and
            // [promote] sim_resid=1 mutually exclusive.
            if (!mh::hook::arm_observer(mh::hook::point::land_players,
                                        reinterpret_cast<void *>(land_players_detour),
                                        &g_land_tramp)) {
                append_line(g_log_path, "; ALLAI NOT armed: install REFUSED (named in the [interlock] "
                                        "summary in mh_net.log)\n");
                g_cfg.all_ai   = 0;
                g_cfg.land_log = 0;
            } else {
                // C10: the SEAM, which is the half the rebind cannot cover. Our promoted
                // session_begin_multi reaches land_players by a direct intra-slice C++ call and never
                // touches the entry this detour sits on, so without this registration the conversion
                // simply does not happen under [promote] sim_resid=1 -- converted=0, slot 0 stays
                // HUMAN, and the run is an ordinary one-human skirmish wearing the soak's name.
                // on_land_players is one-shot (g_allai_ran), so reaching it from BOTH the detour and
                // the body in a run that still has an original caller converts exactly once.
                mh::hook::register_callback(mh::hook::point::land_players_observer,
                                            &on_land_players);
                char m[224];
                wsprintfA(m,
                          "; ALLAI ARMED: every enabled HUMAN slot -> AI at landing (observer=%d, "
                          "all_ai=%d); seam registered, so a promoted sibling's direct call converts "
                          "too (C10)\n",
                          g_cfg.all_ai_observer, g_cfg.all_ai);
                append_line(g_log_path, m);
                if (g_cfg.land_log)
                    append_line(g_log_path, "; LAND LOG ARMED: the landing detour will report the RNG "
                                            "channel state either side of the roll and every enabled "
                                            "slot's chosen spot\n");
            }
        }
    }

    // Phase 2 order record/replay. Record hooks order_queue_dispatch (same Watcom prologue, guarded);
    // replay needs no hook (it injects from on_sim_step) -- just load the file.
    if (g_cfg.order_mode == 1) {
        // U30: the compare moved into the primitive, so the recorder either takes the entry or is
        // told by name who has it -- and the D18 branch below still gets its turn.
        const bool rec_armed = mh::hook::arm_observer(mh::hook::point::order_dispatch,
                                                      reinterpret_cast<void *>(dispatch_detour),
                                                      &g_disp_tramp);
        if (rec_armed) {
            // nothing further: the trampoline owns the entry and records from there
        } else if (mh::hook::promoted_owner_of(ADDR_ORDER_DISPATCH)) {
            // D18, the promotion-already-won branch. It does NOT fire in the shipped init order --
            // MH_Harness_Init runs BEFORE MH_Seam_Init, which is where every promotion happens, so at
            // this point nothing is promoted yet and the trampoline above takes the entry. The
            // collision this facility actually has is the REVERSE one, and the rebind
            // (MH_Harness_RebindOrderDispatch) is what resolves it. This branch is the belt to that
            // braces: if the order ever changes, the recorder degrades to an observer on our body
            // instead of silently recording nothing.
            //
            // Exactly one mechanism is live per configuration -- trampoline when the original owns
            // the entry, observer when we do -- which is what stops it double-recording. order_mode
            // STAYS 1 either way.
            mh::hook::register_callback(mh::hook::point::dispatch_observer, &on_dispatch);
            append_line(g_log_path,
                        "; order_record: dispatch is PROMOTED -- recording via the promoted body's "
                        "observer instead of a trampoline (D18). order_mode stays 1.\n");
        } else {
            // A genuine wrong-build/unknown prologue, OR an entry another detour owns. Distinct from
            // the promoted case above, and it has to stay distinct: conflating them is what made D17
            // read as a build problem. The primitive has already said which of the two it was.
            append_line(g_log_path, "; order_record: dispatch entry unavailable -- disabled (the "
                                    "reason is in the [interlock] summary in mh_net.log)\n");
            g_cfg.order_mode = 0;
        }
    } else if (g_cfg.order_mode == 2) {
        order_replay_load();
        clock_load(); // optional mh_clock.bin -> reproduce a real-time recording's per-step deltas
        // The enqueue neuter is DEFERRED to MH_Harness_LateArm -- see it for why. Doing it here is
        // what made `replay_suppress_enqueue=1` invalidate the whole orders promotion.
    }

    // ---- D6: arm the synthetic moving-unit workload -------------------------------------------
    // ST2M: the by-name resolve loop that stood here (units/tile_objects/buildings for their special
    // hash functions, and the three time_tick outputs for the exclusion set) is gone -- those indices
    // are generated constants now, and the exclusion flag rides on the entry itself.
    if (g_cfg.synth_move) {
        if (g_cfg.synth_seed == 0) {
            // HARD ERROR, not a warning. A zero seed means every run picks the same destination,
            // which is precisely the fixed-path vacuity this workload exists to eliminate -- and a
            // silently-degraded workload would still let the run report a clean pass.
            append_line(g_log_path, "; SYNTH FAIL: synth_move=1 with synth_seed=0 -- the runner must"
                                    " supply a per-run random seed. Workload NOT armed.\n");
            g_synth_failed = true;
        } else if (g_idx_units < 0) {
            append_line(g_log_path, "; SYNTH FAIL: REGIONS[] has no \"units\" entry -- the workload's"
                                    " motion could not be verified (D1 not applied?). NOT armed.\n");
            g_synth_failed = true;
        } else {
            g_synth_armed = true;
        }
    }

    // ---- U32: arm the conquest workload --------------------------------------------------------
    // Armed when EITHER conq (the host workload) or conq_land_self_at (the peer's self-deploy
    // fallback) is set, because the second is a valid configuration on a peer that runs nothing else.
    if (g_cfg.conq || g_cfg.conq_land_self_at) {
        if (g_idx_units < 0 || g_idx_bldgs < 0) {
            append_line(g_log_path, "; CONQ FAIL: REGIONS[] has no \"units\"/\"buildings\" entry -- the"
                                    " workload could not be verified. NOT armed.\n");
            g_conq_failed = true;
        } else if (g_cfg.all_ai) {
            // all_ai converts human slots to AI LOCALLY and is not replicated (tracker U32). Any run
            // combining the two is desynced by construction, and its verdict would be read as a
            // regression in THIS workload.
            append_line(g_log_path, "; CONQ FAIL: conq=1 with all_ai=1 -- all_ai is not replicated and"
                                    " desyncs by construction. NOT armed.\n");
            g_conq_failed = true;
        } else if (g_cfg.synth_move) {
            // MEASURED, not defensive. synth_move orders each peer's own mothership to a random
            // destination EVERY step, so it overwrites this workload's move/deploy on the following
            // step and re-randomises the match length run to run. Two workloads driving one unit is
            // not a configuration, it is a bug with an ini key -- refuse it by name.
            append_line(g_log_path, "; CONQ FAIL: conq=1 with synth_move=1 -- synth_move re-orders "
                                    "the SAME mothership every step and overwrites the deploy. Pass "
                                    "--synth-move 0. NOT armed.\n");
            g_conq_failed = true;
        } else if (g_cfg.conq_peer_land_at > 0 && g_cfg.conq_land_self_at > 0) {
            // Both arms would deploy the same mothership twice. The second order would land on a unit
            // that no longer exists -- a no-op that looks fine and means nothing.
            append_line(g_log_path, "; CONQ FAIL: conq_peer_land_at and conq_land_self_at are mutually"
                                    " exclusive -- pick the host-issued or the self-issued deploy.\n");
            g_conq_failed = true;
        } else {
            g_conq_armed = true;
            // gameover_check is what turns an elimination into a stop + a report. Armed here rather
            // than left to the operator: a conquest run whose end condition nothing watches ends by
            // running out of steps, which reads identically to one that never ended.
            if (g_cfg.gameover_step == 0) g_cfg.gameover_step = 50;
        }
    }

    char banner[256];
    wsprintfA(banner, "; ==== mh replay harness armed: seed_step=%d seed_mode=%d stop_step=%d "
                      "fixed_step=%d pin_fpu=%d region_hash_step=%d order_mode=%d replay_ai_off=%d "
                      "suppress_enqueue=%d ====\n",
              g_cfg.seed_step, g_cfg.seed_mode, g_cfg.stop_step, g_cfg.fixed_step, g_cfg.pin_fpu,
              g_cfg.region_hash_step, g_cfg.order_mode, g_cfg.replay_ai_off, g_cfg.replay_suppress_enqueue);
    append_line(g_log_path, banner);
    // Same reason as the synth line below: the soak's shape must be readable from the log header, so
    // an all-AI run can never be mistaken for the one-AI default (or vice versa) after the fact.
    wsprintfA(banner, "; all_ai=%d observer=%d gameover_step=%d gameover_stop=%d\n", g_cfg.all_ai,
              g_cfg.all_ai_observer, g_cfg.gameover_step, g_cfg.gameover_stop);
    append_line(g_log_path, banner);
    // D6: state the workload in the header of every run, so a log can never be read as "a determinism
    // run" without showing whether the world was actually in motion.
    wsprintfA(banner, "; synth_move=%d armed=%d seed=%d at=%d every=%d (D6 moving-unit workload)\n",
              g_cfg.synth_move, (int)g_synth_armed, g_cfg.synth_seed, g_cfg.synth_at, g_cfg.synth_every);
    append_line(g_log_path, banner);
    wsprintfA(banner, "; conq=%d armed=%d seed=%d at=%d peerland=%d selfland=%d academy=%d"
                      " barracks=%d soldier=%d n=%d forcekill=%d (U32 conquest workload)\n",
              g_cfg.conq, (int)g_conq_armed, g_cfg.conq_seed, g_cfg.conq_at, g_cfg.conq_peer_land_at,
              g_cfg.conq_land_self_at, g_cfg.conq_academy, g_cfg.conq_barracks, g_cfg.conq_soldier,
              g_cfg.conq_n_soldiers, g_cfg.conq_force_kill_at);
    append_line(g_log_path, banner);
    // Name both sides of the input/output split explicitly -- a recorded run writes into the run
    // folder, a replay reads next to the exe, and "why is my replay reading nothing" is otherwise a
    // silent guess.
    if (g_cfg.order_mode == 1 || g_cfg.seed_mode == 0 || g_cfg.clock_record) {
        wsprintfA(banner, "; harness OUTPUT dir: %s\n", MH_RunDir());
        append_line(g_log_path, banner);
    }
    // LIB-REF-LIVE: say so, because a clock track with no order stream is an unusual-looking run
    // directory and "the recorder was off" is the reading it otherwise invites.
    if (g_cfg.clock_record && g_cfg.order_mode != 1)
        append_line(g_log_path, "; clock_record: mh_clock.bin written WITHOUT an order stream "
                                "(order_mode=0) -- the live-loop fixture's shape\n");
    if (g_cfg.order_mode == 2 || g_cfg.seed_mode == 1) {
        wsprintfA(banner, "; harness INPUT dir: %s (copy a banked recording here)\n", g_dir);
        append_line(g_log_path, banner);
    }
    // PLACED LAST, AFTER EVERY INSTALL, and the position is load-bearing. An earlier draft sat
    // BEFORE the order record/replay section -- so llm_strat_order_queue_dispatch's detour, which
    // installs on the far side of it, had not claimed its entry yet and that row was never
    // yielded. That is exactly the hole clause 6 was opened for: sim_step reaching the dispatcher
    // through an armed rebind and bypassing dispatch_detour, with no line and no refusal. A new
    // harness detour must install ABOVE this point -- there is nothing to remember to ADD to, only
    // somewhere not to go after.
    // ---- SIM1-P clause 6: DERIVE which rebindable rows must yield their binding -----------------
    //
    // A rebound caller calls our body DIRECTLY and reaches no entry hook. So any row whose ORIGINAL
    // entry some instrument has claimed must have its libmh binding turned OFF, or that instrument
    // silently stops seeing the calls -- and for the trajectory-hash detours that VOIDS a determinism
    // run rather than failing it (measured once as the MP host sitting at step 0 with an empty
    // mh_harness.log, bisected to llm_strat_sim_step out of all 676 rows).
    //
    // This was a hand-kept list of two names. It is now the intersection of two facts neither of which
    // a human maintains: mh::rebind::row_addr[] (each row's original VA, emitted by gen_libmh_rebind.py
    // -- computed since 2026-09-04 and DISCARDED until 2026-09-05) and the claim table that every
    // install primitive already writes through claim_entry().
    //
    // BOTH DIRECTIONS ARE REPORTED, which is what makes it a gate rather than a loop: a claimed entry
    // matching no rebindable row is COUNTED and named in the summary, because otherwise "the derivation
    // found nothing to do" and "the derivation is broken" produce the same silence -- exactly the
    // failure this clause exists to remove.
    if (g_yield_claimed_rebinds) {
        int yielded = 0, matched = 0;
        for (int i = 0; i < (int)mh::rebind::ROW_COUNT; ++i) {
            const uintptr_t a = mh::rebind::row_addr[i];
            if (!a) continue; // no exported address -> matches nothing; never yielded on a guess
            const char *who = mh::hook::entry_claimant_of(a);
            if (!who) continue;
            ++matched;
            if (mh::rebind::set_armed(mh::rebind::row_names[i], false)) {
                ++yielded;
                char line[256];
                wsprintfA(line,
                          "; [rebind] %s YIELDED to %s (that instrument owns the entry; a rebound "
                          "caller would bypass it)\n",
                          mh::rebind::row_names[i],
                          who);
                append_line(g_log_path, line);
            }
        }
        int unmapped = 0;
        for (int i = 0; i < mh::hook::entry_claim_count(); ++i) {
            uintptr_t   e     = 0;
            const char *who   = nullptr;
            bool        share = false;
            if (!mh::hook::entry_claim_at(i, &e, &who, &share)) continue;
            bool found = false;
            for (int r = 0; r < (int)mh::rebind::ROW_COUNT && !found; ++r)
                if (mh::rebind::row_addr[r] == e) found = true;
            if (!found) {
                ++unmapped;
                char line[256];
                wsprintfA(line,
                          "; [rebind] claimed entry %08X (%s) maps to NO rebindable row -- nothing to "
                          "yield, reported so an absent yield is never mistaken for a broken scan\n",
                          (unsigned)e,
                          who ? who : "(unnamed)");
                append_line(g_log_path, line);
            }
        }
        char sum[256];
        wsprintfA(sum,
                  "; [rebind] harness-owned yield DERIVED (clause 6): %d claimed entr(ies), %d matched "
                  "a rebindable row, %d yielded, %d mapped to no row\n",
                  mh::hook::entry_claim_count(),
                  matched, yielded, unmapped);
        append_line(g_log_path, sum);
    }

    // F4G: THE ARM REPORT GOES TO DISK HERE, before a single frame runs. Everything above is the
    // instrument stating what it armed, and it is the one part of mh_harness.log whose reader may
    // never get a second chance: a run killed at the menu takes no sim step, so nothing appends
    // again and the buffered report dies with the process (measured at F4F -- an armed lane left the
    // file EMPTY, and an empty instrument log is indistinguishable from an instrument that refused).
    // One WriteFile per armed boot; un-armed boots return above and never reach it.
    log_flush();
    return 1;
}

// C6: let the promotion side CHAIN onto the harness's sim_tick detour instead of competing for the
// entry. Returns 1 if a detour is armed and the rebind took, 0 if the harness never armed -- in which
// case nobody owns the entry and the caller should do an ordinary entry install. That is the same
// three-outcome shape install_time_tick_promotion() uses, and reporting WHICH route was taken is the
// point: "promotion silently did not happen" is the failure this project keeps rediscovering.
//
// Idempotent and order-independent: the harness arms from DllMain, long before any promotion request,
// so g_tick_tramp is already set by the time this can be called.
extern "C" int MH_Harness_RebindSimTick(void *ours) {
    if (!g_tick_tramp || !ours) return 0;
    g_tick_promoted = ours;
    return 1;
}

// LT1F (2026-09-02): the C6 chain hook for OUR frame spine. The translated llm_strat_frame pair
// calls our sim_tick body DIRECTLY (sim_lt_frame.h's routing decision), which skips the
// sim_tick_detour above -- so the frame bodies chain this instead: on_sim_tick, ARMED-GATED, a
// no-op in every run the harness never armed (no `[harness] enable=1`), so both paths stay
// byte-equivalent per config. One call per sim_tick invocation, exactly like the detour.
extern "C" void MH_Harness_OnSimTick(void) {
    if (g_tick_tramp) on_sim_tick();
}

// F5J: the sim-step fence -- the contract and the reasoning are in mh/include/mh_harness_export.h.
//
// GATED ON g_sim_tramp, NOT on harness_enabled(): the caller wants the STEP COUNTER, and the thing
// that makes g_step advance is the sim_step detour. An armed harness whose hook was REFUSED (the
// "HARNESS HOOKS REFUSED" line in MH_Harness_Init) would otherwise answer 0, 0, 0 forever and a
// `simstep` script would sit out its whole watchdog against a counter that is never going to move.
// -1 says "there is no step counter here" for both shapes of that, which is exactly the question
// ui_drive needs answered.
extern "C" int MH_Harness_StepFence(int target) {
    if (!g_sim_tramp) return -1;
    if (target > 0) {
        if (g_fence_at != target) { // idempotent: the caller re-asks every present
            g_fence_at  = target;
            g_fence_hit = false;
            char m[160];
            wsprintfA(m, "; [simfence] ARMED at step %d (the sim is at %lu)\n", target, g_step);
            append_line(g_log_path, m);
        }
    } else if (g_fence_at) {
        g_fence_at  = 0;
        g_fence_hit = false;
        append_line(g_log_path, "; [simfence] RELEASED -- the sim runs on\n");
    }
    return (int)g_step;
}

// LIB-TRANS-P (2026-09-02): does this run's harness config claim time_GetCurrentTime's entry for
// the deterministic wall-clock pin? The lib_trans promotion of that same entry YIELDS when this
// answers nonzero (sim_lt_promote.cpp) -- measured on the first SP-oracle run: the pin arms AFTER
// MH_Seam_Init's promotions, so without the yield OUR install won the entry, the pin reported NOT
// ARMED, and P0-SPDET refused the run. g_cfg is zero-initialized and only read from mh_net.ini
// when the harness armed, so an unharnessed process answers 0 and the row promotes normally.
extern "C" int MH_Harness_WantsWallclockPin(void) {
    return g_cfg.pin_wallclock != 0;
}

// UI-REC: the present-cadence tick, called from net_lockstep::on_present -- the one hook that runs in
// every game mode, which is what the menu/strategic half of a game-start recording needs and what
// llm_tact_frame could never give it. Cheap when idle: ui_journal_present returns on its first line
// unless a UI journal is armed, so every existing run pays one predictable branch per present.
//
// Called unconditionally by the present hook rather than gated there, so the "is the harness armed"
// question stays inside this file with the config that answers it (g_cfg is zero-initialised, so an
// unharnessed process answers no).
extern "C" void MH_Harness_OnPresent(void) {
    ui_journal_present();
    boot_snapshot_tick(); // LIB-BOOT: one-shot, fires on the first present in GAME_MODE 3
    // F4G: make LOG_FLUSH_MS a real bound for the runs that install this hook. See the log writer's
    // constraint 1 -- append_line's timer is only read by the next append, so a run that logs and
    // then goes quiet (a menu, a paused replay, a mission waiting for its wall-clock kill) keeps the
    // buffer. This costs one compare per present in an un-instrumented run, because g_log_used is 0.
    log_flush_due();
}

// The movie tick, NOT the present hook -- see MH_Harness_WantsMovieTick's header comment. The intro
// screens never reach llm_gfx_present_flip, so this is the only callback that runs while one is up.
extern "C" void MH_Harness_OnMovieTick(void) { avi_skip_tick(); }
extern "C" int  MH_Harness_WantsMovieTick(void) { return g_cfg.skip_intro_avi ? 1 : 0; }

// UI-REC: does this run NEED the present hook installed? The hook is otherwise gated on three
// unrelated [net]/[trace] keys (frametime_log, eager_advertise, temporal), all of which ship at 1 --
// so the UI journal would arm today and die silently the first time a fragment turned one of them
// off, with a recorder that reports ARMED and records nothing. An arm that depends on another
// mechanism's default is not armed, it is lucky. Read by lockstep_install_present_gameover.
extern "C" int MH_Harness_WantsPresentTick(void) {
    // LIB-BOOT arms it too: the snapshot capture fires on the first present in GAME_MODE 3,
    // and an arm that depends on another mechanism being armed is not armed, it is lucky --
    // the same reasoning the UI-journal clause above was written for.
    return (ui_journal_armed() || g_cfg.boot_snapshot) ? 1 : 0;
}

#else // non-x86 build config: harness is inert (mh.exe is 32-bit; only Win32 is ever shipped)

// F4E: MH_Core_Arm_Early's stub went with the real one, to mh/seams/core_arm.cpp -- which has no
// x86 guard because it has no asm and no fixed VAs. Leaving it here would be a DUPLICATE definition
// in any non-x86 build, i.e. exactly the failure this file's split was supposed to remove.
// MH_Harness_SetModuleRefused gets a stub instead: it is the module's one way into this file,
// and an inert harness that cannot be told anything is still inert.
extern "C" void MH_Harness_SetModuleRefused(int) {}
extern "C" int  MH_Harness_Init(void) { return 0; }
extern "C" int  MH_Harness_RebindSimTick(void *) { return 0; }
extern "C" void MH_Harness_OnSimTick(void) {}
extern "C" int  MH_Harness_WantsWallclockPin(void) { return 0; }
extern "C" int  MH_Harness_RebindSimStep(void *) { return 0; }
extern "C" int  MH_Harness_RebindLandPlayers(void *) { return 0; }
extern "C" int  MH_Harness_RebindOrderDispatch(void *) { return 0; }
extern "C" void MH_Harness_LateArm(void) {}
extern "C" void MH_Harness_OnPresent(void) {}
extern "C" void MH_Harness_OnMovieTick(void) {}
extern "C" int  MH_Harness_WantsMovieTick(void) { return 0; }
extern "C" int  MH_Harness_WantsPresentTick(void) { return 0; }

#endif
