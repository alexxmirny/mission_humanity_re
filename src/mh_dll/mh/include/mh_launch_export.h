#pragma once
//
// D17 launch-to-state harness (dev + UX) -- drive mh.exe directly into a chosen state from the
// command line, skipping the main menu. See launch.cpp; D17.
// mh.exe-specific: patches fixed VAs in the RU retail build.
//
// Mechanism (step 1): parse the exe command line; if a launch verb is present, install a one-shot
// inline hook on the per-frame menu driver llm_ui_menu_state_tick (0x004b774b). The first frame the
// MAIN MENU is idle (_G_LLM_GAME_MODE==3 && _G_LLM_UI_MENU_STATE==3), fire the parsed action.
//
// Verbs:
//   --load <name>            [step 2, IMPLEMENTED] reuse the live SP load path: point the selected-
//                            save global (_G_LLM_MENU_SAVE_NAME_PTR @0x00654462) at <name>, clear
//                            dlg flag bit8, call llm_menu_loadgame_confirm (0x004bb22b) -> loads
//                            save\<name>.sav and enters strategic gameplay (GAME_MODE=2).
//   --newgame [race]         [step 3, IMPLEMENTED] start a fresh CAMPAIGN (race h|a, default h): set
//                            _G_LLM_MENU_NEWGAME_RACE (0x00e642a4) then call llm_menu_campaign_start_and_enter
//                            (0x004b7ffb) -- session_begin(race,0) does the full new-game reset + start
//                            planet + player profiles internally, then enters gameplay (GAME_MODE=2).
//                            Race-only (the campaign always starts on System[1]'s planet; no planet select).
//   --mp-host / --mp-join    [step 4, NOT YET] the MP bootstrap (mp-restoration "A").
//   --tactical <name>        [TACT-PREP, IMPLEMENTED] enter a TACTICAL mission with no human
//                            input. Loads <name> exactly as --load does (a mission needs a live
//                            strategic session behind it), then SYNTHESISES the squad blackboard
//                            at 0xe15e60 -- the 64-slot record array llm_strat_bldg_gather_nearby_
//                            squad_status would have filled -- saves the planet (the exit restores
//                            the three shared map planes by re-reading that file), sets
//                            _G_LLM_GAME_MODE=6 and calls llm_tact_mission_start (0x004290ea).
//                            Writing the blackboard rather than calling the entry gate is what
//                            avoids having to satisfy gather()'s gameplay predicates (ctrl group 0
//                            stocked with soldier-carriers within 15 tiles of an enemy main base).
//                            Squad size / hp% / commando / target owner+building come from
//                            [tactical] in mh_net.ini. See the tactical-probe work.
//
// Flags (combinable with any verb, or standalone):
//   --skip-intro             skip the ~10s startup movie Res\Intro\LOGO.AVI (played by llm_intro_frame,
//                            mode 7). Hooks llm_intro_frame (0x004c317c): lets the first frame run its
//                            one-time init + open the movie (so NO missing-file modal), then on the next
//                            frame tears the movie down via the game's own teardown (FUN_004c2d12) so it
//                            advances to boot instead of waiting. (INTRO.AVI is never shown on startup.)
//                            mp:U21: "later" = once the movie's audio thread is past CreateSoundBuffer
//                            (MH_Launch_IntroSkipReady), never on frame 2 -- see launch.cpp.
//                            Standalone --skip-intro (no verb) just skips the movie and lands at the menu.
//
#ifdef __cplusplus
extern "C" {
#endif

// Parse GetCommandLineA(). If a launch verb and/or --skip-intro is present (and the relevant stub
// carries the expected Watcom prologue), install the corresponding hooks and return 1 (DllMain then
// early-returns, keeping the sim otherwise vanilla). Returns 0 (inert, nothing patched) if nothing was
// requested / installed -- ship-safe. Call once from DllMain (byte patches only; loader-lock safe).
int MH_Launch_Init(void);

// Test seam: parse an explicit command line with no game state touched. Returns the verb code
// (0=none, 1=--load, 2=--newgame, 3=--mp-host, 4=--mp-join, ..., 8=--tactical -- APPEND ONLY,
// mh_nettest launchtest asserts these as literals), writes its argument into arg_out[arg_cap],
// and (if skip_out != nullptr) writes 1/0 for whether --skip-intro was present. Used by mh_nettest
// launchtest to prove the CLI grammar off-target.
int MH_Launch_ParseCmdline(const char *cmdline, char *arg_out, int arg_cap, int *skip_out);

// mp:U21 test seam: may --skip-intro tear LOGO.AVI down on this frame? Pure (no game state), so
// launchtest proves the decision table off-target. WAIT = let the movie play one more frame; GO = the
// movie's audio thread cannot be inside CreateSoundBuffer reading the format block the teardown frees;
// TIMEOUT = waited cap_ms and it never settled, tear down anyway (logged). See launch.cpp on_intro_tick.
enum { MH_INTRO_SKIP_WAIT    = 0,
       MH_INTRO_SKIP_GO      = 1,
       MH_INTRO_SKIP_TIMEOUT = 2 };
int MH_Launch_IntroSkipReady(int has_audio_stream, int has_dsound, int has_dsbuf, int has_pcm,
                             int thread_alive, unsigned waited_ms, unsigned cap_ms);

#ifdef __cplusplus
}
#endif
