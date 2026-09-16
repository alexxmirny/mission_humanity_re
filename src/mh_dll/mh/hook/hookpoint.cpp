//
// hook/hookpoint.cpp -- the named hook-point TABLE and the arming/registration shapes (D5).
//
// ---- THE ONE LAYERING EXCEPTION IN hook/, AND WHY IT IS DELIBERATE -------------------------------
//
// hook/promoted.h opens by saying that `hook/` carries no feature knowledge and cannot reach for a
// generated address header -- which is why the promotable-extent table is INSTALLED into it
// (set_owner_table) rather than compiled in. This TU is the exception, on purpose and in one
// direction only: it is the mh.dll ROUTER's hook surface, so it is the file whose whole job is to
// know both sides -- the generated VAs on one, the domain slots on the other. Installing the table
// instead would buy nothing and cost an ordering hazard: MH_Harness_Init arms BEFORE MH_Seam_Init
// (G104), so "the table is installed at init" would mean "installed after the first caller needs it".
//
// Nothing else in hook/ includes this file, and the rest of hook/ stays as pure as it was.
//
#include "hook/hookpoint.h"

#include "addr/mh_addrs.gen.h"  // the generated EN VAs
#include "addr/mh_export.gen.h" // addr_* + entry_* for the targets whose guard is the exact 8 bytes

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// ---- the domain slots this router forwards registrations into -------------------------------------
//
// FORWARD-DECLARED, not included, and for the reason net_lockstep.cpp already gives at its own
// mh::sim handoff: this TU needs exactly these six symbols and no sim state types. Pulling six sim
// headers into hook/ would drag the migrated bodies' type surface into the one file that must stay
// readable as a boundary. A signature drift here fails at LINK with the mangled name, which is loud.
namespace mh::sim {
int  set_sim_step_pre_hook(void (*fn)());
void set_dispatch_observer(void (*fn)());
void set_land_players_observer(void (*fn)());
void set_session_begin_multi_observer(void (*fn)());
void set_time_resync_instrument_hooks(void (*pace_time_tick_hook)());
int  install_promotion_lt_frame(int default_on, int spine_promoted, void (*pace_time_tick_hook)(),
                                void (*harness_sim_tick_hook)());
} // namespace mh::sim

namespace mh::hook {

namespace {

// The arming shape a row supports. A caller that asks for the wrong one gets `false` and no write --
// a programming error, not a runtime condition, so it fails closed rather than guessing.
enum class shape { observe,
                   replace,
                   neuter,
                   register_only };

struct row {
    const char *id;
    const char *who;
    uintptr_t   target;
    shape       kind;
    int         stolen; // observe only
    entry_claim claim;  // observe / replace
    uint32_t    expect; // the byte guard handed to the primitive (0 = none)
    uint64_t    entry8; // the exact 8 entry bytes, when that is the stronger guard (0 = none)
};

// ---- THE TABLE ------------------------------------------------------------------------------------
//
// One row per `point`, IN ENUM ORDER (asserted below). Every field was a literal at the call site
// this row replaces; nothing here is new, and the `who` strings are byte-identical to the ones the
// interlock summary and the SIM1-P clause 6 yield line printed before the move -- `check_arm_order`
// gates that text.
//
// The tactical journal's two seam addresses are LITERALS because they are not in mh_addrs.gen.h: they
// reach C++ through mh_calls.gen.h (which is how the replayer re-issues them) and only the detours
// need the raw entry. Carried verbatim from harness.cpp, comment and all.
constexpr uintptr_t ADDR_TACT_ENQUEUE = 0x0042b39du;
constexpr uintptr_t ADDR_TACT_GROUP   = 0x0042b09fu;

constexpr row TABLE[] = {
    // ---- OBSERVE ----------------------------------------------------------------------------
    // sim_tick / sim_step take entry_claim::rebind (C6): they are PROMOTABLE and the harness's
    // detour keeping their entries is the protocol, not a collision -- reimpl_probe rebinds what
    // they fall through to. The default byte expectation is right for all three: MH_Harness_Init's
    // whole-function gate already compares exactly these entries and returns inert.
    {"sim_tick", "the determinism harness sim_tick detour", mh::addr::llm_strat_sim_tick,
     shape::observe, 8, entry_claim::rebind, WATCOM_PROLOGUE, 0},
    {"sim_step", "the determinism harness sim_step detour", mh::addr::llm_strat_sim_step,
     shape::observe, 8, entry_claim::rebind, WATCOM_PROLOGUE, 0},
    {"console", "the harness console detour", mh::addr::llm_debug_console_dispatch, shape::observe, 8,
     entry_claim::exclusive, WATCOM_PROLOGUE, 0},
    {"order_dispatch", "the order-record detour ([test] order_mode=1)",
     mh::addr::llm_strat_order_queue_dispatch, shape::observe, 8, entry_claim::exclusive,
     WATCOM_PROLOGUE, 0},
    // land_players: ::rebind (C10) and NO prologue expectation -- its exact eight entry bytes are the
    // guard (entry8 below), which is strictly stronger. With ::exclusive the interlock refused the
    // sim_resid promotion instead, which is what made all_ai and that promotion mutually exclusive.
    {"land_players", "the ALLAI landing conversion detour",
     mh::exp::addr_llm_game_land_players_on_planet, shape::observe, 8, entry_claim::rebind, 0,
     mh::exp::entry_llm_game_land_players_on_planet},
    {"tact_frame", "the tactical cadence hook (tact_hash_step / tact_synth)", mh::addr::llm_tact_frame,
     shape::observe, 8, entry_claim::exclusive, WATCOM_PROLOGUE, 0},
    {"tact_order_enqueue", "the TJ order-enqueue recorder", ADDR_TACT_ENQUEUE, shape::observe, 8,
     entry_claim::exclusive, WATCOM_PROLOGUE, 0},
    {"tact_group_order", "the TJ group-order recorder", ADDR_TACT_GROUP, shape::observe, 8,
     entry_claim::exclusive, WATCOM_PROLOGUE, 0},
    {"ui_input_update", "the SPCAMP-FLAKE input_update entry counter",
     mh::exp::addr_llm_strat_input_update, shape::observe, 8, entry_claim::exclusive, WATCOM_PROLOGUE,
     0},
    {"ui_storage_panel", "the SPCAMP-FLAKE storage-panel entry counter",
     mh::exp::addr_llm_strat_ui_storage_bldg_panel, shape::observe, 8, entry_claim::exclusive,
     WATCOM_PROLOGUE, 0},

    // ---- REPLACE (the whole-body pins) --------------------------------------------------------
    // pin_wallclock is the only one of the four that DOES open with a Watcom frame; the other three
    // are guarded on their generated entry8 and pass 0 to the primitive, exactly as their call sites
    // did. Handing those WATCOM_PROLOGUE would refuse three working hooks outright (U30).
    {"pin_wallclock", "the harness pin_wallclock replacement", mh::addr::GetCurrentTime,
     shape::replace, 0, entry_claim::exclusive, WATCOM_PROLOGUE, 0},
    {"pin_rand", "the harness pin_rand LCG replacement", mh::exp::addr_llm_rand, shape::replace, 0,
     entry_claim::exclusive, 0, mh::exp::entry_llm_rand},
    {"pin_strat_seed", "the harness pin_strat_seed constant",
     mh::exp::addr_llm_strat_rng_seed_wallclock_seconds, shape::replace, 0, entry_claim::exclusive, 0,
     mh::exp::entry_llm_strat_rng_seed_wallclock_seconds},
    {"pin_input_wndproc_tap", "the replay input-producer suppressor",
     mh::exp::addr_llm_input_wndproc_tap, shape::replace, 0, entry_claim::exclusive, 0,
     mh::exp::entry_llm_input_wndproc_tap},

    // ---- NEUTER --------------------------------------------------------------------------------
    // Three bytes, not eight, and a three-byte guard to match: `55 89 e5` is the head of the Watcom
    // frame and the entry has no frame set up yet, so `xor eax,eax; ret` is instruction-boundary
    // safe. __watcall passes its four arguments in registers, so there is no stack to clean up.
    {"order_enqueue", "the replay_suppress_enqueue byte neuter", mh::addr::llm_strat_order_enqueue,
     shape::neuter, 0, entry_claim::exclusive, 0, 0},

    // ---- REGISTER-ONLY (no entry; target 0) ----------------------------------------------------
    {"sim_step_pre", "the D21 desync sampler's promoted-root pre-hook", 0, shape::register_only, 0,
     entry_claim::exclusive, 0, 0},
    {"dispatch_observer", "the order recorder's promoted-body observer (D18)", 0,
     shape::register_only, 0, entry_claim::exclusive, 0, 0},
    {"land_players_observer", "the ALLAI landing seam (C10)", 0, shape::register_only, 0,
     entry_claim::exclusive, 0, 0},
    {"session_begin_multi", "the session_begin_multi body observer (C10)", 0, shape::register_only, 0,
     entry_claim::exclusive, 0, 0},
    {"time_resync_prelude", "the SIM-SAVE-DIV time_resync prelude hook", 0, shape::register_only, 0,
     entry_claim::exclusive, 0, 0},
    {"lt_frame_pace_time_tick", "the LT1F frame pair's pacing chain hook", 0, shape::register_only, 0,
     entry_claim::exclusive, 0, 0},
    {"lt_frame_harness_sim_tick", "the LT1F frame pair's harness chain hook", 0, shape::register_only,
     0, entry_claim::exclusive, 0, 0},
};

// The table and the enum are ONE list. A point added to the enum without a row would otherwise read
// whatever followed the array -- a target of garbage, handed to install_jmp.
static_assert(sizeof(TABLE) / sizeof(TABLE[0]) == static_cast<int>(point::count_),
              "hook/hookpoint: every `point` needs exactly one TABLE row, in enum order");

inline bool       valid(point p) { return static_cast<int>(p) >= 0 && p < point::count_; }
inline const row &at(point p) { return TABLE[static_cast<int>(p)]; }

// The registered callbacks. mh.dll-private, one slot per point; a register-only point's slot is the
// value, every other point's stays null.
void (*g_cb[static_cast<int>(point::count_)])() = {nullptr};

} // namespace

const char *point_id(point p) { return valid(p) ? at(p).id : "(invalid)"; }
const char *point_who(point p) { return valid(p) ? at(p).who : "(invalid hook point)"; }
uintptr_t   point_target(point p) { return valid(p) ? at(p).target : 0; }

bool entry_bytes_match(point p) {
    if (!valid(p)) return false;
    const row &r = at(p);
    if (!r.entry8 || !r.target) return true; // nothing to disagree with
    return *reinterpret_cast<const uint64_t *>(r.target) == r.entry8;
}

bool available(point p) {
    if (!valid(p)) return false;
    const row &r = at(p);
    if (!r.target) return false; // a register-only point arms no entry
    return detour_refusal(r.target, r.claim, r.expect) == refuse_reason::none;
}

bool arm_observer(point p, void *detour, void **tramp_out) {
    if (!valid(p) || !detour || !tramp_out) return false;
    const row &r = at(p);
    if (r.kind != shape::observe) return false;
    return install_trampoline(r.target, detour, tramp_out, r.stolen, r.claim, r.who, r.expect);
}

bool arm_replacement(point p, const void *body) {
    if (!valid(p) || !body) return false;
    const row &r = at(p);
    if (r.kind != shape::replace) return false;
    return install_jmp(r.target, body, r.claim, r.who, r.expect);
}

bool arm_neuter(point p) {
    if (!valid(p)) return false;
    const row &r = at(p);
    if (r.kind != shape::neuter) return false;
    uint8_t *e = reinterpret_cast<uint8_t *>(r.target);
    if (!(e[0] == 0x55 && e[1] == 0x89 && e[2] == 0xe5)) return false;
    DWORD old = 0;
    VirtualProtect(e, 3, PAGE_EXECUTE_READWRITE, &old);
    e[0] = 0x31;
    e[1] = 0xc0;
    e[2] = 0xc3; // xor eax,eax ; ret
    VirtualProtect(e, 3, old, &old);
    FlushInstructionCache(GetCurrentProcess(), e, 3);
    return true;
}

bool register_callback(point p, void (*fn)()) {
    if (!valid(p)) return false;
    const row &r = at(p);
    if (r.kind != shape::register_only) return false;

    // Forward into the domain that owns the slot FIRST, and only record the registration if the
    // domain accepted it -- so callback_of() can never claim a subscription the owner refused.
    switch (p) {
        case point::sim_step_pre:
            if (!mh::sim::set_sim_step_pre_hook(fn)) return false;
            break;
        case point::dispatch_observer: mh::sim::set_dispatch_observer(fn); break;
        case point::land_players_observer: mh::sim::set_land_players_observer(fn); break;
        case point::session_begin_multi: mh::sim::set_session_begin_multi_observer(fn); break;
        case point::time_resync_prelude: mh::sim::set_time_resync_instrument_hooks(fn); break;
        // The two LT1F chain hooks have no setter of their own: they are ARGUMENTS to the frame
        // pair's installer, so the registry holds them until arm_frame_promotion asks.
        case point::lt_frame_pace_time_tick:
        case point::lt_frame_harness_sim_tick: break;
        default: return false;
    }
    g_cb[static_cast<int>(p)] = fn;
    return true;
}

void (*callback_of(point p))() { return valid(p) ? g_cb[static_cast<int>(p)] : nullptr; }

int arm_frame_promotion(int default_on, int spine_promoted) {
    return mh::sim::install_promotion_lt_frame(default_on, spine_promoted,
                                               callback_of(point::lt_frame_pace_time_tick),
                                               callback_of(point::lt_frame_harness_sim_tick));
}

} // namespace mh::hook
