//
// seams/reimpl_probe.cpp -- P0-EXPORT's proof: a game function actually replaced by a C++ body,
// using the generated interop layer alone and no hand-written asm at this seam.
//
// The target is `utils::w_strlen` @0x004d0582 -- 11 instructions, a pure wide-string length with no
// side effects, called from 59 sites. It was chosen for three reasons:
//   * It is exactly replicable, so "identical behaviour" is a claim we can actually make.
//   * It is __mhfastocall, not __watcall, so this also exercises a custom calling convention rather
//     than the common case.
//   * It has NO Watcom frame prologue (`PUSH EDX; MOV EDX,EAX`), which is precisely why the arm
//     guard had to become per-function expected BYTES instead of hook/detour.h's prologue constant.
//
// WHAT MAKES THIS A PROOF RATHER THAN A HOPE
//   1. Before patching anything, the original is CALLED through the generated call-side wrapper
//      (mh_calls.gen.h) and compared against the C++ body on a set of inputs. Both directions of the
//      generated layer are therefore checked against each other on the real binary, and a mismatch
//      means we arm NOTHING. This is a miniature of the SHADOW-MODE oracle in the reimplementation plan.
//   2. It counts its own invocations and logs the count. "The determinism run stayed identical" is
//      worthless if the replacement was never called -- a gate has to have had the DATA to go red
//      (a green gate that could not have failed proves nothing).
//
#include "seams/net_internal.h" // seam_log

#include "config/config.h"              // F2A: the D11 selector every promotion default below derives from
#include "ai/ai_state.h"                // RI-AI batch A owns its own shadow wiring, same arrangement
#include "addr/mh_calls.gen.h"          // generated: CALL the original (tools/gen_dll_calls.py)
#include "fp/st0_call.h"                // ... and the thunk that reaches the ORIGINAL
#include "crt/crt_st0_sweep.h"          // ... and the sweep both arms walk
#include "crt/crt_math.h"               // LIB-REF-SPLIT: the vendored ST0 sin/cos under proof
#include "addr/mh_export.gen.h"         // generated: BE the original  (tools/gen_dll_exports.py)
#include "lockstep/turn_engine.h"       // the L1 turn-engine module, same arrangement
#include "ai/ai_promote.h"              // AI1-P: whole-domain promotion of the 167 verified AI rows
#include "orders/order_queue.h"         // the O2 container module owns its own shadow wiring
#include "orders/issue/issue_state.h"   // RI-ORDERS / O4A: the order-issue domain's own shadow wiring
#include "orders/issue/issue_promote.h" // O4-P: whole-domain promotion of the 62 verified wrappers
#include "save/save_live.h"             // SV1-P: the save direction's live bindings + its own promote key
#include "tact/tact_state.h"            // mh::tact::install_shadow, the tactical domain aggregator
#include "include/mh_harness_export.h"  // MH_Harness_RebindSimStep -- harness-mediated sim_step promotion (C6)

// The sim domain. Its ~300 per-TU shadow installers now live in mh::sim::install_shadow
// (sim/sim_state.cpp), beside the co-arm and arm_ready constraints that bind them; what stays here is
// the ONE delegation plus the PROMOTIONS, which read the SHIP_PROMOTE_* ship defaults out of
// seams/net_internal.h and so cannot move into a reimplementation TU (the layering lint).
#include "sim/sim_state.h"                        // mh::sim::install_shadow, the sim domain aggregator
#include "sim/resid/resid_promote.h"              // SIM-RESID-P: whole-domain promotion of the 32 resid bodies
#include "sim/libtrans/sim_lt_promote.h"          // LIB-TRANS-P: whole-domain promotion of the 43 translated callees
#include "sim/sim_hostreach_promote.h"            // SIM-HOSTREACH: the 18 front-end-reached sim leaves
#include "sim/hostreach/sim_batch_h_promote.h"    // SIM1-H: batch H, the 15 translated sim bodies
#include "sim/sim_xtl_promote.h"                  // X-TL-P: X-TL-DRAIN's three translated bodies
#include "sim/sim_bldg_tick_animation_state.h"    // promotion: the per-building tick2 dispatcher
#include "sim/sim_building_tick.h"                // promotion: the per-building spine tick dispatcher
#include "sim/sim_order_dispatch.h"               // promotion: the order-queue EXECUTOR
#include "sim/sim_register_bldg_type_callbacks.h" // promotion: our per-building-TYPE callback registrar
#include "sim/sim_register_state_handlers.h"      // promotion: our state-machine dispatch-table registrar
#include "sim/sim_step.h"                         // promotion: the domain root's entry thunk
#include "sim/sim_unit_tick.h"                    // promotion: the per-unit spine tick dispatcher

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <float.h> // _controlfp_s -- the ST0 goldens sweep runs at both PC settings

namespace {

volatile long g_w_strlen_calls = 0;

// The replacement. Compare against the original's 11 instructions: walk 16-bit units to the NUL,
// return the difference in units (the original computes `(p - str) >> 1`).
int32_t reimpl_w_strlen(void *str) {
    const uint16_t *p = static_cast<const uint16_t *>(str);
    int32_t         n = 0;
    while (p[n] != 0) ++n;

    // Non-vacuity evidence: prove from the log that game code really routes through here. Log the
    // FIRST call and then at widening milestones -- an "at 1000 only" marker was silent through a
    // full 3000-step 2-machine run, which left "ALL PAIRS IDENTICAL" unable to distinguish "the
    // replacement is correct" from "the replacement was never reached".
    const long c = ++g_w_strlen_calls;
    if (c == 1 || c == 100 || c == 1000 || c == 10000 || c == 100000) {
        char b[96];
        _snprintf_s(b, sizeof(b), _TRUNCATE, "; [reimpl] w_strlen replacement served call #%ld from game code\n", c);
        seam_log(b);
    }
    return n;
}

} // namespace

// Defines mh_export_thunk_utils_w_strlen (naked, generated) + mh_export_install_utils_w_strlen(). The binding is
// type-checked against mh::exp::sig_utils_w_strlen, so a wrong parameter list would not compile.
MH_EXPORT_REPLACE(utils_w_strlen, reimpl_w_strlen)

namespace {

// Inputs chosen to cover the boundaries the loop can get wrong: empty, single, ASCII-in-UTF16, a
// non-BMP-ish high unit, and an embedded-NUL-terminated run.
bool shadow_matches() {
    static const wchar_t *cases[] = {L"", L"a", L"mh", L"Moon Project", L"\xFEFF\x0441\x0442\x043E", L"0123456789abcdef"};
    for (int i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); ++i) {
        void *s = (void *)cases[i];
        // still the ORIGINAL body at this point -- nothing has been patched yet
        const int32_t want = mh::call::utils_w_strlen(s);
        const int32_t got  = reimpl_w_strlen(s);
        if (want != got) {
            char b[200];
            _snprintf_s(b, sizeof(b), _TRUNCATE,
                        "; [reimpl] w_strlen NOT armed -- shadow mismatch on case %d: original=%d reimpl=%d\n",
                        i, (int)want, (int)got);
            seam_log(b);
            return false;
        }
    }
    return true;
}

} // namespace

// ---- LIB-REF-SPLIT: the ST0 sin/cos differential, the ONE measurement that needs the image ------
//
// crt/crt_math.h vendors llm_math_fsin_reduce_loop @0x004dabc6 and llm_math_cos_impl @0x004dabbc for
// the standalone build. Every other vendored body is proved offline against a reference transcribed
// from the original's machine code -- but here the vendored body IS that transcription, so an
// offline reference would compare the same instructions with themselves. The non-circular reference
// is the original AT ITS ADDRESS, which exists only in a hosted process, which is why this lives in
// seams/ and runs once rather than in crt_vendor_selftest.cpp.
//
// IT RUNS AT INSTALL TIME, BEFORE ANYTHING IS PATCHED, exactly like shadow_matches() above and for
// the same reason: `mh::fp::call_f64_st0_arg(VA, x)` must reach the ORIGINAL body, not a detour.
//
// The output is the goldens. Each line carries the input and both results as RAW BIT PATTERNS --
// not %f, which would round away the very last-place differences the comparison is about -- so the
// committed table in mh_nettest/st0_sincos_goldens.h can be lifted straight out of a rig log and
// the permanent offline arm can then re-run this sweep with no game at all.
//
// Gated on `[st0] goldens=1`: it is a measurement, not a check, and re-running it on every launch
// would put 96 lines of noise into every log for a fixture that changes only when the vendored body
// does.
//
// THE KEY MOVED AT FORK F4E, out of `[harness]` and into a section this seam owns (ruling Q3: the
// thirteen oracle seams stay in mh.dll, each self-gated on its own ini section). It was the ONE
// `[harness]` read left outside the instrument -- measured, tree-wide -- and after the split it would
// have been mh.dll reading another module's configuration section: a key whose name says which DLL
// owns it, sitting in the file two DLLs read, meaning nothing to the one it names. `[st0]` is the tag
// this probe already writes its output under, so the section and the log lines now agree.
//
// A SURVIVING `[harness] st0_goldens=1` IS REFUSED, not ignored (F2E's rule, applied at key scope
// rather than section scope). That fragment is an author who believes a measurement will run; it will
// not, and the only symptom would be gen_st0_goldens.py finding no sweep in a log nobody knew was
// wrong. The refusal names the successor key, like every retired-section row.
namespace {

void st0_sweep_one(int pc, int *disagree) {
    char b[200];
    _snprintf_s(b, sizeof(b), _TRUNCATE,
                "; [st0] PC=%d BEGIN -- original @0x004dabc6/0x004dabbc vs mh::crt vendored\n", pc);
    seam_log(b);
    for (int i = 0; i < mh::crt::ST0_SWEEP_N; ++i) {
        const double x = mh::crt::st0_sweep_input(i);

        // The ORIGINAL, through the ST0-argument thunk. Nothing is patched yet.
        const double o_sin = mh::fp::call_f64_st0_arg(0x004dabc6u, x);
        const double o_cos = mh::fp::call_f64_st0_arg(0x004dabbcu, x);
        // OURS, the transcription that ships standalone.
        const double v_sin = mh::crt::llm_math_fsin_reduce_loop(x);
        const double v_cos = mh::crt::llm_math_cos_impl(x);

        uint64_t xb, osb, ocb, vsb, vcb;
        std::memcpy(&xb, &x, 8);
        std::memcpy(&osb, &o_sin, 8);
        std::memcpy(&ocb, &o_cos, 8);
        std::memcpy(&vsb, &v_sin, 8);
        std::memcpy(&vcb, &v_cos, 8);

        // BIT EQUALITY, not ==: the arms must agree on the sign of a zero, and `0.0 == -0.0` is true
        // while the bodies would be disagreeing.
        const bool ok = (osb == vsb) && (ocb == vcb);
        if (!ok) ++*disagree;
        _snprintf_s(b, sizeof(b), _TRUNCATE, "; [st0] %d %3d %016llX %016llX %016llX%s\n", pc, i,
                    (unsigned long long)xb, (unsigned long long)osb, (unsigned long long)ocb,
                    ok ? "" : "  DISAGREE");
        seam_log(b);
    }
}

void st0_goldens_probe() {
    if (GetPrivateProfileIntA("harness", "st0_goldens", 0, g_ini)) {
        char dir[MAX_PATH];
        mh::config::detail::exe_dir(dir);
        char what[512];
        wsprintfA(what, "%s sets `[harness] st0_goldens`, and fork F4E moved that key.", g_ini);
        mh::config::detail::refuse(
            dir, what,
            "The ST0 sin/cos golden sweep is mh.dll's own measurement probe (seams/reimpl_probe.cpp) "
            "and the determinism harness is a separate DLL now, so the key lives under the section "
            "this probe writes its output to: use `[st0] goldens=1`. It is refused rather than "
            "ignored because the only other symptom would be tools/gen_st0_goldens.py finding no "
            "`[st0] SWEEP END` line in a log whose author believed the sweep had run.");
    }
    if (!GetPrivateProfileIntA("st0", "goldens", 0, g_ini)) return;
    char b[200];
    int  disagree = 0;
    // BOTH PRECISION SETTINGS, because FSIN/FCOS round their RESULT to the live control word. The
    // harness pins PC=53 (harness.cpp pin_fpu); the bare x87 default is PC=64; both are reachable,
    // so a golden set captured at one says nothing about the other. crt_vendor_selftest already
    // sweeps its other math bodies at both for exactly this reason.
    // SAVE FIRST, RESTORE WHAT WAS THERE. This used to end with an unconditional
    // `_controlfp_s(&cur, _PC_53, _MCW_PC)` under the comment "leave the game's own pinned setting as
    // we found it" -- which is not what it did: it INSTALLED PC=53 whatever the setting had been, so
    // a measurement probe silently changed the process's floating-point precision for every sim step
    // after it. Inert in practice (the key defaults to 0, and CRT-X87-CPP then measured the live
    // setting to be PC=53 anyway, so the write happened to be a no-op), but "happens to be a no-op"
    // is not a property to leave in a probe whose whole job is to observe without perturbing --
    // especially one that ships in the same binary as a sim whose determinism the word decides.
    unsigned cur = 0, saved = 0;
    _controlfp_s(&saved, 0, 0); // read-only query: mask 0 writes nothing
    _controlfp_s(&cur, _PC_53, _MCW_PC);
    st0_sweep_one(53, &disagree);
    _controlfp_s(&cur, _PC_64, _MCW_PC);
    st0_sweep_one(64, &disagree);
    _controlfp_s(&cur, saved & _MCW_PC, _MCW_PC); // genuinely as we found it
    _snprintf_s(b, sizeof(b), _TRUNCATE,
                "; [st0] SWEEP END -- %d input(s) x 2 precisions, %d DISAGREEMENT(S)%s\n",
                mh::crt::ST0_SWEEP_N, disagree,
                disagree ? "" : " -- vendored bodies are bit-identical to the original");
    seam_log(b);
}

} // namespace

// The O2 order-container reimplementation used to live here. It now has its own module,
// libmh/orders/order_queue.{h,cpp} -- this file is only P0-EXPORT's proof seam.
void reimpl_probe_install() {
    // F2A / D11. Every promotion default below reads `ours_default(SHIP_PROMOTE_*)` rather than the
    // constant directly: under `[config] mode=original` the whole set is 0 whatever the ship
    // constants say, under `brokered` each keeps its own. The per-key `[promote]` entries still
    // override either way -- that surface is unadvertised now and F2E deletes it.
    //
    // ANNOUNCED ONLY WHEN IT IS NOT THE DEFAULT, and that is the point rather than economy: the ship
    // arm-log line sequence is a standing refactor gate (D2), so a selector that printed a line in
    // the brokered configuration would change every captured log to say nothing new. A run that is
    // NOT the default has to be able to state so.
    //
    // seam_log, NOT mh::ai::ai_say: the AI logger is bound further down this same function
    // (mh::ai::set_logger), so an ai_say from here writes nowhere. Measured -- the first build of
    // this block printed nothing at all in a mode=original run, which is the silent-selector failure
    // the block exists to prevent.
    using mh::config::ours_run;
    {
        // F2E RETIRED THE UNKNOWN-MODE ANNOUNCE, and it is the announce that went, not the check: a
        // mode this build does not implement now TERMINATES the process in mh::config (with the
        // reason in three channels), long before this line could run. While the per-key surface
        // existed, announce-and-continue was right -- a typo could not brick a lane, because the keys
        // still said what would execute. With the selector as the only control, continuing means
        // running the SHIP configuration under a log that names a different one.
        if (!ours_run()) {
            char b[256];
            _snprintf_s(b, sizeof(b), _TRUNCATE,
                        "; [config] mode=%s -- the ORIGINAL engine runs: nothing of ours is promoted "
                        "and no rebind row is armed.\n",
                        mh::config::mode_name());
            seam_log(b);
        }
    }

    st0_goldens_probe();
    g_w_strlen_calls = 0; // the shadow check calls the reimpl too; don't count those
    if (shadow_matches() && mh_export_install_utils_w_strlen()) {
        g_w_strlen_calls = 0;
        seam_log("; [reimpl] w_strlen REPLACED by a C++ body via the generated entry thunk (P0-EXPORT)\n");
    }

    mh::orders::set_logger(seam_log);
    mh::orders::install_promotion(ours_run());

    mh::orders::issue::install_promotion(ours_run());

    // The L1 turn engine, same deal. NOTE its sites cannot all be armed in one run: commit_horizon is
    // CALLED BY extend_if_near_horizon and reset_player_horizon, and shadow sites hook the callee
    // entry, so arming the caller and the callee together makes the caller's original arm re-enter
    // the inner dispatcher. Two ini fragments, never one -- see turn_engine.cpp.
    mh::lockstep::set_logger(seam_log);
    // C8-d: ONE call, not two. `[promote] wire` is retired and its sixteen emitters joined this
    // closure, so install_promotion now installs all 31 seams (+ time_tick and sim_tick by rebind).
    // It also REFUSES a run whose ini still names `wire` or `wire_seams`, rather than ignoring them.
    // C8-f: PROMOTION IS THE DEFAULT. SHIP_PROMOTE_* (mh/seams/net_internal.h) is the default the
    // ini overrides, not a value the ini must supply -- so `[promote] lockstep=0` is what a
    // rollback run writes, and an ini with no [promote] section at all now ships our engine.
    mh::lockstep::install_promotion(g_ini, ours_run()); // refuses partially, loudly
    // F2B / D1 R1: hand the OUTCOME to the net seams from HERE, the call site that owns the install.
    // net_lockstep.cpp's two rebind sites and the frame-pair interlock need what actually installed,
    // and this is the point at which "requested" and "installed" have the same answer -- the install
    // has returned and nothing else writes those flags before lockstep_install_core runs.
    g_lockstep_promoted = {mh::lockstep::promotion_active(), mh::lockstep::time_tick_requested(),
                           mh::lockstep::sim_tick_requested()};

    mh::save::set_logger(seam_log);
    mh::save::install_promotion(g_ini, mh::config::save_walkers_ours());
    // The load root is its OWN key and its own install, so `[promote] save` and `[promote] load` roll
    // back independently -- they carry different evidence and only one has been through a gate.
    mh::save::install_load_promotion(g_ini, mh::config::save_walkers_ours());
    // The two CONTAINER roots, likewise one key each: game::SaveGame writes the .sav, llm_game_load
    // reads it back and streams its members out again. Four save-subsystem seams, four rollbacks.
    mh::save::install_container_promotion(g_ini, mh::config::save_walkers_ours());
    mh::save::install_container_load_promotion(g_ini, mh::config::save_walkers_ours());

    mh::ai::set_logger(seam_log);
    mh::ai::install_promotion(ours_run());
    // AI1-P second half: the island move, ONLY when the closure is live -- with `[promote] ai=0`
    // the ORIGINAL AI runs and needs the island at its .bss addresses, so no move and no poison.
    if (mh::ai::promotion_active()) mh::ai::island_move();

    mh::sim::install_promotion_resid(ours_run());

    // LIB-TRANS-P (2026-09-02): the 43 translated outward callees, whole-domain under
    // `[promote] lib_trans` (sim/libtrans/sim_lt_promote.cpp). The FRAME PAIR is deliberately NOT
    // here: its installer is interlocked on the promoted spine and is called from
    // net_lockstep.cpp's lockstep_install_core, AFTER the time_tick/sim_tick promotions have had
    // their chance ([promote] lib_trans_frame). time_GetCurrentTime's install is EXPECTED to be
    // REFUSED by name in any pin_wallclock run -- the harness owns that entry from DllMain and
    // the pin IS the deterministic replacement.
    mh::sim::install_promotion_lib_trans(ours_run());

    // SIM-HOSTREACH Phase A (2026-09-10): the 18 sim leaves original FRONT-END code still entered
    // as originals (sim/sim_hostreach_promote.h). Selected by the promotion reconciliation's
    // section 5 rather than by batch, so they are leaves of unrelated sub-systems and a refusal
    // here really is just one row staying original -- unlike the ai closure, nothing else in this
    // set calls through them. Per-row rollback `[promote_skip] <name>`.
    mh::sim::install_promotion_hostreach(ours_run());

    // SIM1-H (2026-09-10): batch H's 15 bodies -- the 8 host-side sim CALLERS section 5's fixpoint
    // surfaced (the TRANSLATE-LATER third of that same classification, which the user's standing
    // ruling says gets TRANSLATED rather than installed around) plus the 7 rows their translation
    // pulled in: the nav-region pipeline's other half, which turned out to be in no ledger at all
    // while its siblings were verified rows, and one pure sprite-geometry helper.
    // Ordered AFTER hostreach deliberately: nothing in either set calls into the other, but both
    // install over entries and a stable order makes a REFUSED line attributable on sight. Per-row
    // rollback `[promote_skip] <name>` -- but read sim_batch_h_promote.h first, because for a
    // PIPELINE member it rolls back the entry and not the intra-batch call graph.
    mh::sim::install_promotion_batch_h(ours_run());

    // X-TL-P (2026-09-12): X-TL-DRAIN's three bodies -- llm_strat_bldg_init_all,
    // llm_strat_bldg_instant_construct_find_slot, llm_unit_order_disembark_soldiers. Installing
    // them is what returns report_promotion_reconciliation's section 5 to zero rows: step 4 flipped
    // all three to `state: verified` while their ORIGINALS stayed reachable (from llm_boot_stage_tick
    // and llm_strat_input_update), which is exactly the `ours != running` split X-SPINE closed on.
    // Ordered AFTER batch H deliberately, for the same reason batch H is ordered after hostreach:
    // nothing in any of the three sets calls into another, but all of them install over entry bytes
    // and a stable order makes a REFUSED line attributable on sight.
    mh::sim::install_promotion_xtl(ours_run());

    // RI-SIM. The domain's PROMOTIONS live here rather than in mh::sim::install_shadow because
    // SHIP_PROMOTE_* lives in mh/seams/net_internal.h with the rest of the ship defaults and a
    // reimplementation TU must not include a seams header (the layering lint). They all run BEFORE
    // the shadow delegation at the end of this block: promotion and shadow both install over the
    // game's entry bytes, so whichever runs second lands on top of the first, and each shadow
    // installer consults the promotion flag and REFUSES rather than producing a run whose "original"
    // arm is our own body.

    // llm_strat_order_queue_dispatch. Off by default (SHIP_PROMOTE_SIM_DISPATCH = 0): the promoted
    // path is the integration oracle, run deliberately via `[promote] sim_dispatch=1` and compared
    // against an unpromoted golden, not something a normal run should be doing.
    // D18: the ORDER RECORDER detours this same entry whenever [test] order_mode=1, and MEASURED it
    // won -- `[promote] sim_dispatch=1` was then refused with "entry bytes ... (wrong build, or
    // already hooked)", so the two facilities were silently exclusive and the message blamed the
    // image. Try the rebind FIRST, exactly as sim_step does above: the detour keeps the entry (so
    // recording still runs before the body) and only its fall-through moves to ours. If no recorder
    // is armed this run the rebind returns 0, nobody owns the entry, and the ordinary entry install
    // below is the right route.
    if (ours_run() &&
        MH_Harness_RebindOrderDispatch(mh::sim::order_queue_dispatch_entry_thunk())) {
        mh::sim::mark_promoted_dispatch_installed(true);
        mh::ai::ai_say("; [promote] sim_dispatch REBOUND onto the order-record detour -- ours IS the "
                       "function, and the recorder still sees every dispatch (D18)\n");
    } else {
        mh::sim::install_promotion_dispatch(ours_run());
    }

    // unit_tick is a DISPATCHER: its shadow closure is the whole sim, so its
    // real verification is the promoted-vs-unpromoted per-step golden A/B, not a per-call shadow
    // site. Off by default (SHIP_PROMOTE_UNIT_TICK=0); armed deliberately via
    // `[promote] unit_tick=1` for a --soak-golden run.
    mh::sim::install_promotion_unit_tick(ours_run());

    // Two more dispatchers of the same class: llm_strat_building_tick
    // indirects through _G_LLM_STRAT_BLDG_STATE_FUNCS/_BLDG_DONE_FUNCS, and its own callee
    // llm_strat_bldg_tick_animation_state through _G_LLM_STRAT_BLDG_TICK2_FUNCS -- an unbounded
    // shadow closure each, so their real oracle is the promoted golden A/B
    // (test_ui.py --soak --soak-golden). Off by default.
    mh::sim::install_promotion_building_tick(ours_run());
    mh::sim::install_promotion_bldg_tick_animation_state(ours_run());

    // The layer UNDER those three dispatchers: our own llm_strat_register_state_handlers, which fills
    // _G_LLM_STRAT_UNIT_STATE_FUNCS and _G_LLM_STRAT_BLDG_STATE_FUNCS with OUR 68 handler entry
    // thunks instead of the game's. Order does not matter against the tick promotions -- this one
    // takes a DIFFERENT entry (llm_strat_register_state_handlers), and its effect lands later still,
    // when llm_strat_mode_init calls it at strategic-mode entry. There is no shadow arm to refuse: a
    // registrar has no per-call comparison to make, its oracle is the golden A/B of what the handlers
    // then do. Off by default (SHIP_PROMOTE_STATE_HANDLERS=0).
    mh::sim::install_promotion_state_handlers(ours_run());

    // The SECOND registry at that same layer, and NOT the same switch: our own
    // llm_strat_register_bldg_type_callbacks, which fills _G_LLM_STRAT_BLDG_DONE_FUNCS and
    // _G_LLM_STRAT_BLDG_TICK2_FUNCS with OUR 30 callback entry thunks. Independent of
    // state_handlers above -- different entry, different tables, different callbacks -- so neither
    // one's first-dispatch line is evidence about the other. Its effect lands at boot stage 5 / the
    // next game load, and only once the cfg Building table has been parsed (the fill SCANS cfg types;
    // see the registrar's header). No shadow arm to refuse, same reason as its sibling. Off by
    // default (SHIP_PROMOTE_BLDG_TYPE_CALLBACKS=0).
    mh::sim::install_promotion_bldg_type_callbacks(ours_run());

    // The RI-SIM domain root, llm_strat_sim_step. TWO INSTALL ROUTES SINCE ROOTS-LIVE (2026-09-04),
    // and the order between them is the C4 one-owner rule rather than a preference.
    //
    // REBIND FIRST. Whenever the determinism harness armed, it owns this entry (harness.cpp
    // install_trampoline(ADDR_SIM_STEP) -> sim_step_detour, carrying the per-step golden hash + the
    // SIM-CUT exactly-once probe) and it must KEEP it: if the promotion won that race the INSTRUMENT
    // would be the thing that refused, which voids a determinism run silently instead of failing it.
    // So the detour keeps the entry and only its fall-through moves.
    //
    // DIRECT ENTRY INSTALL when nothing owns the entry -- exactly what sim_tick has done since C6.
    // This is the ROOTS-LIVE change. Until today the else-arm here printed "NOT promoted" and the
    // ORIGINAL root ran in every no-harness run, i.e. in ordinary gameplay and 17 of the 18 UI
    // scenarios, leaving the 526 rows reachable only through our root inert -- G106's CAUSE, not a
    // second instance of it. The withheld argument was "a no-harness run has nothing to compare, so
    // promoting would just run our body untested"; that treats the A/B as a runtime monitor when it is
    // a gate-time instrument, and every other promoted domain ships live with no runtime oracle
    // (C8-f). What replaces it is the reimplementation plan section 3, "The ship-config oracle": banked
    // equivalence + the UI suite as the only unpinned real-clock exercise + the determinism runs +
    // force-armed tombstones + an ENUMERATED clock/RNG residual. Ship default ON
    // (SHIP_PROMOTE_SIM_STEP = 1); rollback is `[promote] sim_step=0`.
    //
    // BOTH ARMS REPORT AFFIRMATIVELY, NAMING THE ROUTE, and that is a requirement rather than polish:
    // the bug this item fixes was invisible for weeks because "promoted" was inferred from the ABSENCE
    // of a NOT-promoted line. A run must be able to state which route made ours live, or it cannot
    // state that ours is live at all.
    if (ours_run()) {
        void *ours = mh::sim::sim_step_entry_thunk();
        if (MH_Harness_RebindSimStep(ours))
            mh::sim::register_promotion_sim_step_rebound();
        else if (!mh::sim::install_promotion_sim_step_direct())
            // Neither a detour to rebind nor an installable entry. TREAT THIS RUN AS INVALID rather
            // than as unpromoted: the request was made, so a reader will assume our body ran, and the
            // ORIGINAL is what will execute. Same wording and same reasoning as sim_tick's third arm.
            mh::ai::ai_say("; [promote] sim_step NOT promoted -- neither a harness detour to rebind nor "
                           "an installable entry. TREAT THIS RUN AS INVALID: the request was made, so a "
                           "reader will assume our body ran, and the ORIGINAL is what will execute.\n");
    } else
        // NAMES THE CAUSE. F2A gave this branch a second way to be taken and F2E left it ONLY that
        // one: `[config] mode=original` is now the only thing that can reach here, so the line states
        // the mode rather than choosing between two possible keys, one of which no longer exists.
        mh::ai::ai_say("; [promote] sim_step ROLLED BACK ([config] mode=%s) -- the ORIGINAL strategic "
                       "root runs, and with it the 526 rows reachable only through it\n",
                       mh::config::mode_name());

    // RI-SIM shadow, the whole domain in one call -- same shape as mh::ai and mh::tact above. Every
    // per-TU installer, and every co-arm / arm_ready / DO-NOT-ARM constraint that governs it, lives
    // in mh::sim::install_shadow (sim/sim_state.cpp) next to the call it binds.

    // ---- TACTICAL MODE IS DEMOTED PERMANENTLY (fork F2E) -------------------------------------
    //
    // This block used to install the ten mission/character parsers and the C4 rows over the game's
    // entries, rebind the tactical cadence detour's fall-through onto our pump, and hand the TJ
    // order-enqueue recorder a thunk to fall through to. All of it is gone: the fork's selector has
    // two hosted answers and the reimplemented spine either serves a domain in `brokered` or the
    // game's own body does, and TACTICAL IS THE GAME'S IN BOTH. There is no configuration left in
    // which a tactical install could be armed, so the installers went with the vocabulary that used
    // to arm them (libmh/tact/tact_promote.{h,cpp}, deleted).
    //
    // WHAT STILL REACHES THE TACTICAL BODIES: net_selftest's tacttest drives them directly, and
    // their rebind rows survive (ruling Q2 -- the BIND survives; the 9+1 seams those rows derived
    // from are frozen in tools/data/libmh_rebind_frozen_seams.json, the F2D precedent one drop on).
    // A standalone host therefore binds them unconditionally, which is the configuration the
    // tactical translation is archive material FOR.
    // RI-TACT / TACT-RIG. The tactical SHADOW site, LAST -- after both promotion blocks, so that
    // install_shadow_frame's "refuse when promoted" guard is asked a question that already has its
    // real answer. One site exists: llm_tact_quantize_facing_dir, which touches no state and so needs
    // neither TACT-CUT's effect seam nor a tactical state interface. It is here to prove the tactical
    // VEHICLE, i.e. that migration_sweep --domain tact --mode tact reaches GAME_MODE 6 and records a
    // non-zero call count; a strategic soak or sp run arms this same site and reads ZERO CALLS, which
    // is the negative arm TACT-RIG's acceptance test requires.
}
