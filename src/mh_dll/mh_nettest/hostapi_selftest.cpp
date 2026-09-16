//
// hostapi_selftest.cpp -- the host-callback ABI's binding contract, off the rig (LIB-ABI
// stage B; the endgame plan D-E3/D-E6).
//
// WHAT IS EXERCISED HERE AND WHY. The table itself is generated, so its content is the drift
// gate's business (gen_libmh_hostapi --check); what can be WRONG at runtime is the binding
// contract around it: the version handshake (a host built against a different table must be
// refused, and a refusal must not clobber a working binding), the unbound-walk (an entry the
// host forgot must be reported BY NAME, and a full table must report zero), and the trap
// discipline (a REQUIRED entry reached through the selftest host must name itself -- this
// assertion is what carries the done_when's "no-op'ing a non-notify entry is caught by a
// test that names it" when no suite path reaches a platform entry organically; accepted by
// the user 2026-09-02).
//
#include "hostapi_selftest_support.h"

#include "../libmh/state/host_api.h"
#include "../libmh/state/host_events.h"       // LIFT-EVQ: the event-channel surface's contract checks
#include "../mh/include/mh_libmh_hook_bind.h" // F4D-PRE: mh.dll's HOOK-service table...
#include "../libmh/state/hook_api.h"          // ... and libmh's side of the same contract

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

int  g_checks = 0, g_fails = 0;
void ck(bool ok, const char *what) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL: %s\n", what);
    }
}

int  g_trap_count = 0;
char g_last_trap[128];

int             g_walk_reports = 0;
char            g_walk_last[128];
extern "C" void on_unbound(const char *name) {
    ++g_walk_reports;
    snprintf(g_walk_last, sizeof(g_walk_last), "%s", name);
}

} // namespace

static int g_trap_capture = 0;

void mh_hostapi_trap(const char *name) {
    ++g_trap_count;
    snprintf(g_last_trap, sizeof(g_last_trap), "%s", name);
    printf("[hostapi] REQUIRED entry %s called with no host impl\n", name);
    if (!g_trap_capture) {
        // Fatal outside hostapitest: a suite that needs a required entry must mock it (its
        // own calls struct) or the selftest host must grow a real impl -- never limp on a
        // silent default. The exit IS the "test that names it".
        printf("[hostapi] FATAL: required host entry reached in a selftest run -- failing\n");
        fflush(stdout);
        exit(1);
    }
}

void mh_hostapi_trap_set_capture(int on) {
    g_trap_capture = on;
}

int mh_hostapi_trap_count() {
    return g_trap_count;
}

const char *mh_hostapi_last_trap() {
    return g_last_trap;
}

void mh_hostapi_trap_reset() {
    g_trap_count   = 0;
    g_last_trap[0] = '\0';
}

int run_hostapitest() {
    printf("hostapitest: the host-callback ABI binding contract "
           "(sim %u entries 0x%08X, tact %u entries 0x%08X)\n",
           (unsigned)LIBMH_HOST_API_ENTRY_COUNT, (unsigned)LIBMH_HOST_API_VERSION,
           (unsigned)LIBMH_TACT_HOST_API_ENTRY_COUNT, (unsigned)LIBMH_TACT_HOST_API_VERSION);
    mh_hostapi_trap_set_capture(1); // assert the trap's naming without the fatal exit

    // ---- version handshake (main() already bound the selftest host) ----------------------
    ck(libmh_host_api_unbound(nullptr) == 0, "main() bound the selftest host (walk == 0)");
    ck(libmh_set_host_api(&mh_hostapi_selftest_table(), LIBMH_HOST_API_VERSION + 1) == -1,
       "wrong version is refused with -1");
    ck(libmh_host_api_unbound(nullptr) == 0, "refused set kept the working binding");
    ck(libmh_set_host_api(nullptr, LIBMH_HOST_API_VERSION) == -2, "null table is refused with -2");
    ck(libmh_set_host_api(&mh_hostapi_selftest_table(), LIBMH_HOST_API_VERSION) == 0,
       "matching version is accepted");

    // ---- unbound-walk: full table is zero; a gap is reported by name ---------------------
    ck(libmh_host_api_unbound(nullptr) == 0, "selftest host binds every entry (walk == 0)");

    libmh_host_api holey = mh_hostapi_selftest_table();
    holey.vfs_open       = nullptr;
    ck(libmh_set_host_api(&holey, LIBMH_HOST_API_VERSION) == 0, "holey table still binds");
    g_walk_reports = 0;
    g_walk_last[0] = '\0';
    ck(libmh_host_api_unbound(on_unbound) == 1, "one nulled entry -> walk == 1");
    ck(g_walk_reports == 1 && strcmp(g_walk_last, "vfs_open") == 0,
       "the nulled entry is reported by name (vfs_open)");

    // A refused re-bind keeps the current (holey) table -- the failure must not clobber.
    ck(libmh_set_host_api(&mh_hostapi_selftest_table(), LIBMH_HOST_API_VERSION - 1) == -1,
       "version mismatch refused while a table is bound");
    ck(libmh_host_api_unbound(nullptr) == 1, "refused re-bind kept the previous table");
    ck(libmh_set_host_api(&mh_hostapi_selftest_table(), LIBMH_HOST_API_VERSION) == 0,
       "re-bind the full selftest host");

    // ---- the TACT table's own handshake + walk (LIB-IFACE-SPLIT) -------------------------
    //
    // Same contract, second surface: the tact table has its OWN version constant and its OWN
    // unbound-walk, and a gap in it is named by THAT walk -- the nulled entry here is
    // tact-only (llm_input_key_dequeue), so only the tact walk can name it, which is the
    // per-table half of the done_when. The sim table's walk staying zero through all of this
    // is the independence claim.
    ck(libmh_tact_host_api_unbound(nullptr) == 0, "main() bound the tact selftest host too");
    ck(libmh_set_tact_host_api(&mh_hostapi_selftest_tact_table(),
                               LIBMH_TACT_HOST_API_VERSION + 1) == -1,
       "tact: wrong version is refused with -1");
    ck(libmh_tact_host_api_unbound(nullptr) == 0, "tact: refused set kept the working binding");
    ck(libmh_set_tact_host_api(nullptr, LIBMH_TACT_HOST_API_VERSION) == -2,
       "tact: null table is refused with -2");

    libmh_tact_host_api holey_t   = mh_hostapi_selftest_tact_table();
    holey_t.llm_input_key_dequeue = nullptr;
    ck(libmh_set_tact_host_api(&holey_t, LIBMH_TACT_HOST_API_VERSION) == 0,
       "tact: holey table still binds");
    g_walk_reports = 0;
    g_walk_last[0] = '\0';
    ck(libmh_tact_host_api_unbound(on_unbound) == 1, "tact: one nulled entry -> walk == 1");
    ck(g_walk_reports == 1 && strcmp(g_walk_last, "llm_input_key_dequeue") == 0,
       "tact: the nulled entry is reported by name (llm_input_key_dequeue)");
    ck(libmh_host_api_unbound(nullptr) == 0,
       "the SIM walk stays zero while the tact table has the hole (independent walks)");
    ck(libmh_set_tact_host_api(&mh_hostapi_selftest_tact_table(), LIBMH_TACT_HOST_API_VERSION) ==
           0,
       "re-bind the full tact selftest host");

    // ---- trap discipline: required names itself, notify is silent ------------------------
    const libmh_host_api &host = mh_hostapi_selftest_table();
    mh_hostapi_trap_reset();
    ck(host.ticks_ms() == 0, "required entry returns the default");
    ck(mh_hostapi_trap_count() == 1 && strcmp(mh_hostapi_last_trap(), "ticks_ms") == 0,
       "required entry traps BY NAME (ticks_ms)");
    // A SURVIVING notify entry -- pick one, do not name it in prose. This line has now been
    // rewritten THREE times by conversions (cam_jump_queue_clear, then llm_gfx_present_flip at
    // LIFT-RESID, then llm_strat_render_present at SIMABI-NOTIFY), because whichever entry it names
    // is one lift away from not existing. What it actually tests is the CLASS -- a notify entry
    // no-ops silently where a required one traps by name.
    // IT NOW COMES FROM THE TACT TABLE, and that is the honest rendering rather than a workaround:
    // SIMABI-NOTIFY converted the last three render-notify rows, so the SIM table has NO notify
    // entry left to pick (`gen_libmh_hostapi` prints render-notify 0 for sim, 3 for tact). The tact
    // table is frozen and read-only here; the class assertion is table-independent.
    mh_hostapi_selftest_tact_table().llm_tlo_shade_table_build_tact();
    ck(mh_hostapi_trap_count() == 1, "notify entry is a silent no-op (no trap)");
    // Stage E's three consumed no-op returns (ledger noop_return) are all gone with their entries at
    // LIFT-SCREEN: overlay_dismiss and outcome_dialog had no consumer at all, and sync_overlay_show's
    // answer moved off the return entirely onto the pushed-answer slot (R9). The value each used to
    // no-op to stopped mattering when the entry stopped being called; the SCREEN-channel block below
    // covers all three, and the answer slot's own default -1 is checked there.
    mh_hostapi_trap_reset();
    mh_hostapi_trap_set_capture(0);

    // ---- LIFT-EVQ: the event-channel surface (queue mode -- no sink bound in this suite) --
    {
        libmh_event drained[8];
        (void)libmh_poll_events(drained, 8); // flush anything an earlier suite emitted

        ck(libmh_set_event_sink(nullptr, LIBMH_HOST_EVENTS_VERSION + 1) == -1,
           "event sink: wrong version is refused with -1");
        ck(libmh_set_event_sink(nullptr, LIBMH_HOST_EVENTS_VERSION) == 0,
           "event sink: NULL sink at the right version binds (queue mode)");

        // Emit order is the contract: two records, drained FIFO with kinds intact.
        mh::state::emit_invalidate(LIBMH_EVK_INV_TACT_VIEW_TILES, 7);
        mh::state::emit_event(LIBMH_EVC_EVENT, 0xBEEF, 1, 2, 3, 4);
        const uint32_t n = libmh_poll_events(drained, 8);
        ck(n == 2, "queue mode: two emits drain as two records");
        ck(n == 2 && drained[0].channel == LIBMH_EVC_INVALIDATE &&
               drained[0].kind == LIBMH_EVK_INV_TACT_VIEW_TILES && drained[0].a == 7,
           "record 0 is LIBMH_EVK_INV_TACT_VIEW_TILES (emit order held, payload intact)");
        ck(n == 2 && drained[1].channel == LIBMH_EVC_EVENT && drained[1].kind == 0xBEEF &&
               drained[1].d == 4,
           "record 1 is the second emit (kind 0xBEEF -- a drop or reorder fails BY KIND)");
        ck(libmh_poll_events(drained, 8) == 0, "drained ring polls empty");

        // Synchronous sink dispatch: bind, emit, the record must arrive at emit time.
        static libmh_event g_sunk;
        static int         g_sunk_n;
        g_sunk_n = 0;
        ck(libmh_set_event_sink(
               +[](const libmh_event *e) {
                   g_sunk = *e;
                   ++g_sunk_n;
               },
               LIBMH_HOST_EVENTS_VERSION) == 0,
           "a real sink binds");
        mh::state::emit_invalidate(LIBMH_EVK_INV_TACT_VIEW_TILES);
        ck(g_sunk_n == 1 && g_sunk.kind == LIBMH_EVK_INV_TACT_VIEW_TILES,
           "bound sink consumes SYNCHRONOUSLY at emit");
        ck(libmh_poll_events(drained, 8) == 0, "sink-consumed record never enters the ring");

        // Overflow: fill the ring past capacity in queue mode; the newest is dropped + counted,
        // the backlog keeps its order.
        ck(libmh_set_event_sink(nullptr, LIBMH_HOST_EVENTS_VERSION) == 0, "back to queue mode");
        const uint32_t before = libmh_event_overflow_count();
        for (int i = 0; i < 300; ++i)
            mh::state::emit_event(LIBMH_EVC_EVENT, (uint16_t)(i + 1));
        ck(libmh_event_overflow_count() == before + (300 - 256),
           "overflow drops the newest and counts every drop (300 emits, 256 slots)");
        uint32_t total      = 0, got;
        uint16_t first_kind = 0, last_kind = 0;
        while ((got = libmh_poll_events(drained, 8)) != 0) {
            if (total == 0) first_kind = drained[0].kind;
            last_kind = drained[got - 1].kind;
            total += got;
        }
        ck(total == 256 && first_kind == 1 && last_kind == 256,
           "the surviving backlog is the OLDEST 256, in order (kinds 1..256)");
    }

    // ---- LIFT-SCREEN: the SCREEN channel's adapters ------------------------------------
    //
    // Checked through the ADAPTERS, not emit_screen, because the adapter is what a converted call
    // site actually binds: it must land on the right (channel, kind), carry the payload, and return
    // the constant the member signature promises. The three _i32 rows are the ones whose entries
    // used to answer; their 0 here is the "no consumer anywhere" finding made mechanical.
    {
        libmh_event d[8];
        (void)libmh_poll_events(d, 8); // queue mode is still bound from the block above

        ck(mh::state::evt::outcome_dialog_i32(4) == 0, "outcome_dialog adapter returns 0");
        ck(mh::state::evt::overlay_dismiss_i32() == 0, "overlay_dismiss adapter returns 0");
        ck(mh::state::evt::wait_player_overlay_show_i32(3) == 0,
           "wait_player_overlay_show adapter returns 0");
        mh::state::evt::mp_leave_reset_game_mode();
        mh::state::evt::bldg_panel_open();
        ck(mh::state::evt::planet_select_screen_open_i32(0) == 0,
           "planet_select_screen_open adapter returns 0");
        mh::state::evt::dlg_build_from_table(LIBMH_SCR_DLGT_MAIN_MENU_QUIT);

        const uint32_t n = libmh_poll_events(d, 8);
        ck(n == 7, "seven screen adapters emit seven records");
        bool all_screen = n == 7;
        for (uint32_t i = 0; i < n; ++i)
            all_screen = all_screen && d[i].channel == LIBMH_EVC_SCREEN;
        ck(all_screen, "every screen adapter emits on LIBMH_EVC_SCREEN");
        ck(n == 7 && d[0].kind == LIBMH_EVK_SCR_OUTCOME_DIALOG && d[0].a == 4,
           "record 0: OUTCOME_DIALOG carries the outcome code");
        ck(n == 7 && d[1].kind == LIBMH_EVK_SCR_OVERLAY_DISMISS, "record 1: OVERLAY_DISMISS");
        ck(n == 7 && d[2].kind == LIBMH_EVK_SCR_WAIT_PLAYER_SHOW && d[2].a == 3,
           "record 2: WAIT_PLAYER_SHOW carries the player index");
        ck(n == 7 && d[3].kind == LIBMH_EVK_SCR_MP_LEAVE_RESET, "record 3: MP_LEAVE_RESET");
        ck(n == 7 && d[4].kind == LIBMH_EVK_SCR_BLDG_PANEL_OPEN, "record 4: BLDG_PANEL_OPEN");
        ck(n == 7 && d[5].kind == LIBMH_EVK_SCR_PLANET_SELECT_OPEN && d[5].a == 0,
           "record 5: PLANET_SELECT_OPEN carries open_arg");
        ck(n == 7 && d[6].kind == LIBMH_EVK_SCR_DLG_FROM_TABLE &&
               d[6].a == (int32_t)LIBMH_SCR_DLGT_MAIN_MENU_QUIT,
           "record 6: DLG_FROM_TABLE carries a table ID, not an address");
        ck(libmh_poll_events(d, 8) == 0, "screen ring drained empty");
    }

    // ---- LIFT-TABLE S3: the planet graphics record -------------------------------------------
    //
    // cfg_final_planet_Construct's graphics tail has NO offline suite -- its only oracle is the
    // hosted rig -- so the one thing that can be pinned off the rig is pinned here: the record
    // carries the planet SLOT and the resolved TLO INDEX, in that order, as identities. The order
    // matters because both are small integers, so a swap would be invisible everywhere else and
    // would silently write planet 5's banks for a tlo of 31.
    {
        libmh_event d[4];
        (void)libmh_poll_events(d, 4);
        mh::state::evt::inv_planet_gfx_setup(0x1f, 5);
        const uint32_t n = libmh_poll_events(d, 4);
        ck(n == 1 && d[0].channel == LIBMH_EVC_INVALIDATE &&
               d[0].kind == LIBMH_EVK_INV_PLANET_GFX_SETUP,
           "planet gfx setup emits ONE invalidate record");
        ck(n == 1 && d[0].a == 0x1f && d[0].b == 5,
           "a = planet slot, b = tlo_index -- an identity pair, and not the other way round");
    }

    // ---- SIMABI-NOTIFY: the sim table's four CONVERTs ------------------------------------------
    //
    // Every one of the four is a NO-PAYLOAD record, so what an arm can pin off the rig is exactly
    // this: the adapter lands on the right CHANNEL and the right KIND, and it carries nothing.
    // `a == 0` is asserted rather than skipped -- a payload appearing on one of these would mean
    // somebody parameterised a record whose whole design is that it names an instant (R4), and the
    // no-payload claim is the thing the kind comments make.
    {
        libmh_event d[8];
        (void)libmh_poll_events(d, 8); // queue mode is still bound from the block above

        mh::state::evt::strat_frame_present();
        mh::state::evt::strat_frame_redraw();
        mh::state::evt::inv_player_color_lut();
        mh::state::evt::inv_planet_extra_sprite_banks();

        const uint32_t n = libmh_poll_events(d, 8);
        ck(n == 4, "the four SIMABI-NOTIFY adapters emit four records");
        ck(n == 4 && d[0].channel == LIBMH_EVC_SCREEN &&
               d[0].kind == LIBMH_EVK_SCR_STRAT_FRAME_PRESENT && d[0].a == 0,
           "record 0: SCR_STRAT_FRAME_PRESENT on the screen channel, no payload");
        ck(n == 4 && d[1].channel == LIBMH_EVC_SCREEN &&
               d[1].kind == LIBMH_EVK_SCR_STRAT_FRAME_REDRAW && d[1].a == 0,
           "record 1: SCR_STRAT_FRAME_REDRAW is its OWN kind, not folded into the present");
        ck(n == 4 && d[2].channel == LIBMH_EVC_INVALIDATE &&
               d[2].kind == LIBMH_EVK_INV_PLAYER_COLOR_LUT && d[2].a == 0,
           "record 2: INV_PLAYER_COLOR_LUT on the invalidate channel, no payload");
        ck(n == 4 && d[3].channel == LIBMH_EVC_INVALIDATE &&
               d[3].kind == LIBMH_EVK_INV_PLANET_EXTRA_SPRITE_BANKS && d[3].a == 0,
           "record 3: INV_PLANET_EXTRA_SPRITE_BANKS on the invalidate channel, no payload");
        ck(libmh_poll_events(d, 8) == 0, "SIMABI-NOTIFY ring drained empty");

        // THE NEGATIVE ARM, and it is the one property the four converts actually changed: a call
        // that used to be a host-table function POINTER is now a record, so a host that binds
        // nothing must QUEUE it rather than lose it. Bind a sink, emit -> the sink consumes at emit
        // and the ring stays empty; NULL the sink, emit the same kind -> the record is in the ring
        // and drains. A regression that dropped an unbound emit on the floor prints `drained 0`.
        static int g_present_seen;
        g_present_seen = 0;
        ck(libmh_set_event_sink(
               +[](const libmh_event *e) {
                   if (e->channel == LIBMH_EVC_SCREEN && e->kind == LIBMH_EVK_SCR_STRAT_FRAME_PRESENT)
                       ++g_present_seen;
               },
               LIBMH_HOST_EVENTS_VERSION) == 0,
           "negative arm: a sink binds for SCR_STRAT_FRAME_PRESENT");
        mh::state::evt::strat_frame_present();
        ck(g_present_seen == 1 && libmh_poll_events(d, 8) == 0,
           "bound: the sink consumes at emit and the ring stays empty");

        ck(libmh_set_event_sink(nullptr, LIBMH_HOST_EVENTS_VERSION) == 0,
           "negative arm: the sink is NULLED");
        mh::state::evt::strat_frame_present();
        const uint32_t drained = libmh_poll_events(d, 8);
        printf("  [simabi-notify] sink NULLED, SCR_STRAT_FRAME_PRESENT emitted: drained %u "
               "record(s), kind %u, sink saw %d more\n",
               (unsigned)drained, (unsigned)(drained ? d[0].kind : 0u), g_present_seen - 1);
        ck(drained == 1 && d[0].kind == LIBMH_EVK_SCR_STRAT_FRAME_PRESENT && g_present_seen == 1,
           "unbound: the emit is QUEUED (drained 1), not lost -- and the sink saw nothing more");
    }

    // ---- SIMABI-DISPLAY: the sim table's one CONVERT, and it CARRIES a payload ------------------
    //
    // Different arm from the four above, because the record is different in the one way that can
    // silently break: it has an argument. The block above asserts `a == 0` on records whose design
    // is to have nothing; here the whole point is that the mode SURVIVES the adapter, so the arm
    // emits all three real modes and reads each one back. Emitting only mode 0 -- which is what the
    // single live site sends -- would pass with an adapter that hardcoded its payload.
    {
        libmh_event d[8];
        (void)libmh_poll_events(d, 8);

        mh::state::evt::set_display_mode(0);
        mh::state::evt::set_display_mode(1);
        mh::state::evt::set_display_mode(2);

        const uint32_t n = libmh_poll_events(d, 8);
        ck(n == 3, "the display-mode adapter emits one record per request");
        bool chan_kind = n == 3, payload = n == 3;
        for (uint32_t i = 0; i < n && i < 3; ++i) {
            chan_kind &= d[i].channel == LIBMH_EVC_SCREEN &&
                         d[i].kind == LIBMH_EVK_SCR_SET_DISPLAY_MODE;
            payload &= d[i].a == (int32_t)i;
        }
        ck(chan_kind, "SCR_SET_DISPLAY_MODE on the screen channel, every time");
        ck(payload, "a = the requested size_mode, 0/1/2 -- not hardcoded, not the record index");
        ck(libmh_poll_events(d, 8) == 0, "SIMABI-DISPLAY ring drained empty");

        // THE ENTRY IS GONE FROM THE SIM TABLE, and this is the arm that says so by NAME rather
        // than by the count. `llm_view_set_size_mode` was a REQUIRED `display` row, so before the
        // convert a sim-side call trapped by name; after it, the sim table has no display group at
        // all and the only thing that can reach the original is the record above. The tact table
        // still binds it -- frozen, its own site discards the return -- so the same name traps
        // THERE, which is the duplication the split exists for, observed rather than asserted in
        // prose.
        // Capture is re-armed for exactly this call: the trap is FATAL by default (a required
        // entry reached for real must kill the run), and here it is the assertion's subject.
        mh_hostapi_trap_reset();
        mh_hostapi_trap_set_capture(1);
        mh_hostapi_selftest_tact_table().llm_view_set_size_mode(0);
        mh_hostapi_trap_set_capture(0);
        ck(mh_hostapi_trap_count() == 1 &&
               strcmp(mh_hostapi_last_trap(), "llm_view_set_size_mode") == 0,
           "the converted entry survives in the FROZEN tact table, still required, still traps");
        mh_hostapi_trap_reset();
    }

    // ---- the SCREEN channel's answer path (R9): the kick modal's pushed answer -----------------
    //
    // The one screen whose answer the sim consumes. Standalone there is no game process, so the
    // read the original performed on _G_LLM_NET_LOCKSTEP_OVERLAY_RESULT reaches libmh's own slot --
    // filled by libmh_submit_screen_answer, the call a real host makes when the viewer presses
    // "Disconnect player". The read is non-destructive and the clear explicit, so a site consumes
    // an answer identically in both configs.
    {
        libmh_event d[8];
        mh::state::screen_answer_clear(LIBMH_SCR_ANS_LOCKSTEP_KICK);
        ck(mh::state::screen_answer(LIBMH_SCR_ANS_LOCKSTEP_KICK) == -1,
           "no answer pending reads -1 (the no-button value NOTE (8) branches on)");

        libmh_submit_screen_answer(LIBMH_SCR_ANS_LOCKSTEP_KICK, 2);
        ck(mh::state::screen_answer(LIBMH_SCR_ANS_LOCKSTEP_KICK) == 2,
           "a pushed answer is what the sim reads (player index 2)");
        ck(mh::state::screen_answer(LIBMH_SCR_ANS_LOCKSTEP_KICK) == 2,
           "the read is NON-destructive -- it survives until the site clears it");
        mh::state::screen_answer_clear(LIBMH_SCR_ANS_LOCKSTEP_KICK);
        ck(mh::state::screen_answer(LIBMH_SCR_ANS_LOCKSTEP_KICK) == -1,
           "the site's explicit clear consumes the answer");

        // Submitting the "none" value withdraws an answer nobody read yet.
        libmh_submit_screen_answer(LIBMH_SCR_ANS_LOCKSTEP_KICK, 5);
        libmh_submit_screen_answer(LIBMH_SCR_ANS_LOCKSTEP_KICK, -1);
        ck(mh::state::screen_answer(LIBMH_SCR_ANS_LOCKSTEP_KICK) == -1,
           "submitting -1 withdraws an unconsumed answer");

        // An unknown id is refused rather than scribbling past the slots, and reads back as "none".
        libmh_submit_screen_answer(9999u, 7);
        ck(mh::state::screen_answer(9999u) == -1, "an unknown answer id neither stores nor reads");

        // The screen REQUEST stays a request: emitting it carries no answer either way.
        (void)libmh_poll_events(d, 8);
        mh::state::evt::sync_overlay_show();
        const uint32_t n = libmh_poll_events(d, 8);
        ck(n == 1 && d[0].channel == LIBMH_EVC_SCREEN &&
               d[0].kind == LIBMH_EVK_SCR_SYNC_OVERLAY_SHOW,
           "sync_overlay_show emits a SCREEN request and nothing else");
    }

    // ---- the TEXT surface (LIFT-NOTIFY string payloads) --------------------------------
    {
        static uint16_t k_out[LIBMH_TEXT_RING_CAP + 2];
        static uint16_t t_out[LIBMH_TEXT_RING_CAP + 2][LIBMH_EVT_TEXT_MAX];
        (void)libmh_poll_texts(k_out, t_out, LIBMH_TEXT_RING_CAP + 2); // flush

        ck(libmh_set_text_sink(nullptr, LIBMH_HOST_EVENTS_VERSION + 1) == -1,
           "text sink: wrong version is refused with -1");
        ck(libmh_set_text_sink(nullptr, LIBMH_HOST_EVENTS_VERSION) == 0,
           "text sink: NULL sink binds (queue mode)");

        // Queue mode COPIES at emit -- mutate the source after emitting; the drained copy must hold
        // the emit-time content (the staleness hazard the copy exists to preclude).
        wchar_t src[8] = L"HELLO";
        mh::state::emit_text(LIBMH_EVTK_TEXT_MAIN, src);
        src[0]     = L'X';
        uint32_t n = libmh_poll_texts(k_out, t_out, LIBMH_TEXT_RING_CAP + 2);
        ck(n == 1 && k_out[0] == LIBMH_EVTK_TEXT_MAIN && t_out[0][0] == u'H' && t_out[0][4] == u'O' &&
               t_out[0][5] == 0,
           "queue mode copies at emit -- the drained text is the emit-time content, kind intact");

        // Synchronous sink: the CALLER'S OWN pointer arrives, whole, at emit time.
        static const uint16_t *g_sunk_text;
        static uint16_t        g_sunk_kind;
        static int             g_text_sunk_n;
        g_text_sunk_n = 0;
        ck(libmh_set_text_sink(
               +[](uint16_t kind, const uint16_t *text) {
                   g_sunk_kind = kind;
                   g_sunk_text = text;
                   ++g_text_sunk_n;
               },
               LIBMH_HOST_EVENTS_VERSION) == 0,
           "a real text sink binds");
        mh::state::emit_text(LIBMH_EVTK_TEXT_FLOAT_CYAN, src);
        ck(g_text_sunk_n == 1 && g_sunk_kind == LIBMH_EVTK_TEXT_FLOAT_CYAN &&
               g_sunk_text == reinterpret_cast<const uint16_t *>(src),
           "bound text sink receives the CALLER'S pointer synchronously at emit");
        ck(libmh_poll_texts(k_out, t_out, 1) == 0, "sink-consumed text never enters the ring");
        ck(libmh_set_text_sink(nullptr, LIBMH_HOST_EVENTS_VERSION) == 0, "back to queue mode");

        // Overflow: LIBMH_TEXT_RING_CAP+2 emits keep the OLDEST CAP, drop+count the newest 2.
        const uint32_t before = libmh_text_overflow_count();
        wchar_t        one[2] = L"a";
        for (uint32_t i = 0; i < LIBMH_TEXT_RING_CAP + 2; ++i) {
            one[0] = static_cast<wchar_t>(L'a' + i);
            mh::state::emit_text(LIBMH_EVTK_TEXT_FLOAT_RED, one);
        }
        ck(libmh_text_overflow_count() == before + 2,
           "text overflow drops the newest and counts every drop");
        n = libmh_poll_texts(k_out, t_out, LIBMH_TEXT_RING_CAP + 2);
        ck(n == LIBMH_TEXT_RING_CAP && t_out[0][0] == u'a' &&
               t_out[LIBMH_TEXT_RING_CAP - 1][0] == u'a' + LIBMH_TEXT_RING_CAP - 1,
           "the surviving text backlog is the OLDEST CAP, in order");
    }

    // ---- F4D-PRE: the HOOK-SERVICE table (libmh/include/libmh_hook.h) -------------------
    //
    // The THIRD binding contract in this file, and the one whose semantics differ: absence is a
    // shipped CONFIGURATION here, not a boot-order bug. mh::host() aborts unbound because a module
    // that cannot reach its host callbacks cannot run at all; a module that cannot reach the
    // INJECTION harness simply is not injected, which is what a standalone libmh always is. So the
    // arms below assert the thing libmh_hook.h documents per row -- the unbound ANSWER -- rather
    // than a refusal, and they are why that answer is a tested property instead of five comments.
    // They are the same values the eight per-TU `#ifdef MH_LIBMH_BUILD` stubs this item deleted
    // used to give, which is what makes the replacement behaviour-neutral.
    //
    // THE TABLE IS SAVED AND RESTORED. main() bound mh.dll's real one before any suite ran, and
    // later suites in this process reach promotion code through it; leaving this suite's unbound
    // state behind would silently change what they do.
    {
        ck(mh::hosthook::bound(), "F4D-PRE: main() bound mh.dll's hook table");
        ck(libmh_hook_api_unbound(nullptr) == 0, "...with every one of its five rows (walk == 0)");

        ck(libmh_set_hook_api(&mh::hostapi::mhdll_hook_table(), LIBMH_HOOK_API_VERSION + 1) == -1,
           "hook table: a wrong version is refused with -1");
        ck(libmh_hook_api_unbound(nullptr) == 0, "hook table: a refused set kept the working table");
        ck(libmh_set_hook_api(nullptr, LIBMH_HOOK_API_VERSION) == -2,
           "hook table: a null table is refused with -2 (and so cannot UNBIND one)");

        // The unbound-walk names the gap, exactly as the two tables above do.
        libmh_hook_api holey    = mh::hostapi::mhdll_hook_table();
        holey.install_export_ok = nullptr;
        ck(libmh_set_hook_api(&holey, LIBMH_HOOK_API_VERSION) == 0, "hook table: a holey table binds");
        g_walk_reports = 0;
        g_walk_last[0] = '\0';
        ck(libmh_hook_api_unbound(on_unbound) == 1, "hook table: one nulled row -> walk == 1");
        ck(g_walk_reports == 1 && strcmp(g_walk_last, "install_export_ok") == 0,
           "hook table: the nulled row is reported BY NAME (install_export_ok)");
        // A NULL ROW IS NOT A CRASH, and that is the difference from the two tables above: the
        // accessor tests the SLOT, not just the table, so a host that binds four of five rows gets
        // four services and one documented refusal rather than a call through null.
        ck(mh::hosthook::install_export_ok(0, nullptr, "planted", 0) == false,
           "hook table: a NULLED row answers its absent value; it does not dereference");
        ck(mh::hosthook::entry_owner_of(0x0043f512u) == nullptr,
           "hook table: ...and the rows beside it still answer");

        // THE FIVE UNBOUND ANSWERS -- the contract the eight deleted per-TU stubs used to carry.
        // The two installers are asked with a target of 0: unbound they must return BEFORE touching
        // it, so a regression that forwarded into the harness anyway faults here instead of passing.
        {
            libmh_hook_api empty = {};
            ck(libmh_set_hook_api(&empty, LIBMH_HOOK_API_VERSION) == 0, "an all-null table binds");
            ck(libmh_hook_api_unbound(nullptr) == (int)LIBMH_HOOK_API_ENTRY_COUNT,
               "...and the walk reports every row -- this is what 'no host' looks like");
            ck(mh::hosthook::entry_owner_of(0x0043f512u) == nullptr,
               "unbound entry_owner_of answers 'unowned' -- the flat refusal message");
            ck(mh::hosthook::install_export_ok(0, nullptr, "planted", 0) == false,
               "unbound install_export_ok REFUSES (a host with no original cannot patch an entry)");
            void *tramp = reinterpret_cast<void *>(0x1234);
            ck(mh::hosthook::install_trampoline(0, nullptr, &tramp, 8, LIBMH_ENTRY_CLAIM_REBIND,
                                                "planted", 0) == false,
               "unbound install_trampoline REFUSES");
            ck(tramp == reinterpret_cast<void *>(0x1234),
               "...and publishes no trampoline pointer on the way out");
            ck(mh::hosthook::harness_rebind_land_players(nullptr) == 0,
               "unbound harness_rebind_land_players answers 0 -- 'no harness rebound it'");
            ck(mh::hosthook::harness_wants_wallclock_pin() == 0,
               "unbound harness_wants_wallclock_pin answers 0 -- there is no pin to yield to");
        }

        // RESTORE, and prove the restore took rather than assuming it.
        ck(MH_LibMH_BindHookApi() == 0, "hook table: mh.dll's real table is bound again");
        ck(libmh_hook_api_unbound(nullptr) == 0,
           "hook table: ...with all five rows, exactly as main() left it");
    }

    printf("hostapitest: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
