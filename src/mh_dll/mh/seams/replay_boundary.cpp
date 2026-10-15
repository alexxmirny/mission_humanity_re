//
// seams/replay_boundary.cpp -- hotfix rc9.4: NOTE every wholesale state replacement for the replay
// record (desync/replay_record.h "SEGMENTS"), so a session that loads a save, crosses a campaign planet
// transition, returns from a tactical mission or starts a new campaign can be replayed past that point.
//
// HOW. Five CALL rel32 retargets (E8 -> E8), each to a thunk that saves every register, notes the
// boundary (replay_rec::note_boundary: the state hash, the clock and the source file's content hash at
// that instant -- BEFORE the call replaces the state) and tail-jumps to the original callee. OBSERVE
// ONLY: no game memory is written; the callee runs with the caller's registers and return address.
//
//   0x004bb74c  llm_menu_loadgame_confirm        CALL llm_game_load(EAX = save name)     -> save_load
//   0x0044cece  SwitchToPlanet                   CALL map_SavePlanetToDisk(old, 1)        -> planet_transition
//               (after the production drain, before G_PLANET_INDEX moves; target = [EBP-0x20], the
//               function's planet_index local, 0x0044ce2b MOV [EBP-0x20],EAX)
//   0x0044d418  llm_tact_mission_end_return_to_strategic  CALL map_LoadPlanetFromDisk(EAX, 1) -> tact_return
//   0x004b8528  llm_menu_campaign_start_and_enter        CALL llm_strat_planet_session_begin(EAX side, EDX 0) -> new_game
//   0x004c64ed  llm_menu_campaign_start_confirm_cb       CALL llm_strat_planet_session_begin(EAX side, EDX 1) -> new_game
//
// The harness's own load (save_triggers.cpp loadgame_now) calls llm_game_load directly and notes it via
// replay_boundary_note_save_load. Anything else that replaces the state (a promoted SwitchToPlanet in
// configuration (2), a world resync) is cut by replay_record's per-step poll instead (kind=unhooked).
//
// THE FILES. A save is `save\<name>.sav` (G_SAVE_DIR + name); a planet file is
// `save\temp\save%02d.dat` (G_SAVE_DIR + G_SAVE_TEMP_DIR, the kind-1 name LoadPlanetFromDisk reads:
// every visited planet is parked there on departure and restored on return). A first-visit planet (G_PLANET_STATUS[target] == 0) is GENERATED (map_ReadMap_pre),
// so it names no file.
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "seams/replay_boundary.h"
#include "addr/mh_addrs.gen.h"
#include "desync/replay_record.h"
#include "hook/patch.h"
#include "hook/promoted.h"
#include "net_internal.h" // seam_log
#include "en_guard.h"

namespace {

namespace rr = mh::desync::replay_rec;

constexpr uintptr_t SITE_LOADGAME       = 0x004bb74cu; // llm_menu_loadgame_confirm: CALL llm_game_load
constexpr uintptr_t SITE_PLANET_SWITCH  = 0x0044ceceu; // SwitchToPlanet: CALL map_SavePlanetToDisk
constexpr uintptr_t SITE_TACT_RETURN    = 0x0044d418u; // llm_tact_mission_end_return_to_strategic: CALL map_LoadPlanetFromDisk
constexpr uintptr_t SITE_NEWGAME        = 0x004b8528u; // llm_menu_campaign_start_and_enter: CALL llm_strat_planet_session_begin
constexpr uintptr_t SITE_NEWGAME_SYSTEM = 0x004c64edu; // llm_menu_campaign_start_confirm_cb: CALL llm_strat_planet_session_begin

const char *g_armed_sites = "";

void read_cstr(uintptr_t a, char *dst, int cap) {
    lstrcpynA(dst, reinterpret_cast<const char *>(a), cap);
}

void planet_file(int planet, char *dst, int cap) {
    char sd[64], td[64];
    read_cstr(mh::addr::G_SAVE_DIR, sd, sizeof(sd));
    read_cstr(mh::addr::G_SAVE_TEMP_DIR, td, sizeof(td));
    _snprintf_s(dst, cap, _TRUNCATE, "%s%ssave%02d.dat", sd, td, planet);
}

bool planet_known(int planet) {
    return planet >= 0 && planet < 32 && reinterpret_cast<const int32_t *>(mh::addr::G_PLANET_STATUS)[planet] != 0;
}

} // namespace

void replay_boundary_note_save_load(const char *save_name) {
    char sd[64], path[MAX_PATH];
    read_cstr(mh::addr::G_SAVE_DIR, sd, sizeof(sd));
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s%s.sav", sd, save_name ? save_name : "");
    rr::note_boundary(rr::boundary::save_load, save_name ? save_name : "", path, -1);
}

namespace {

void __cdecl on_loadgame(const char *name) { replay_boundary_note_save_load(name); }

void __cdecl on_planet_switch(uintptr_t ebp) {
    const int target = *reinterpret_cast<const int32_t *>(ebp - 0x20);
    char      what[48], file[MAX_PATH];
    file[0] = '\0';
    if (planet_known(target)) {
        planet_file(target, file, sizeof(file));
        _snprintf_s(what, sizeof(what), _TRUNCATE, "planet %d (restored from its file)", target);
    } else {
        _snprintf_s(what, sizeof(what), _TRUNCATE, "planet %d (first visit: generated)", target);
    }
    rr::note_boundary(rr::boundary::planet_transition, what, file, target);
}

void __cdecl on_tact_return(int planet) {
    char what[48], file[MAX_PATH];
    planet_file(planet, file, sizeof(file));
    _snprintf_s(what, sizeof(what), _TRUNCATE, "planet %d (tactical mission end)", planet);
    rr::note_boundary(rr::boundary::tact_return, what, file, planet);
}

void __cdecl on_new_game(int side, int reset) {
    char what[64];
    _snprintf_s(what, sizeof(what), _TRUNCATE, "campaign start side=%d reset_flag=%d", side, reset);
    rr::note_boundary(rr::boundary::new_game, what, "", -1);
}

// clang-format off
__declspec(naked) void th_loadgame() {
    __asm {
        pushad
        push eax
        call on_loadgame
        add  esp, 4
        popad
        push 0x004475de   // llm_game_load
        ret
    }
}
__declspec(naked) void th_planet_switch() {
    __asm {
        pushad
        push ebp
        call on_planet_switch
        add  esp, 4
        popad
        push 0x00447bb3   // map_SavePlanetToDisk
        ret
    }
}
__declspec(naked) void th_tact_return() {
    __asm {
        pushad
        push eax
        call on_tact_return
        add  esp, 4
        popad
        push 0x00448107   // map_LoadPlanetFromDisk
        ret
    }
}
__declspec(naked) void th_new_game() {
    __asm {
        pushad
        push edx
        push eax
        call on_new_game
        add  esp, 8
        popad
        push 0x004541c3   // llm_strat_planet_session_begin
        ret
    }
}
// clang-format on

struct site_t {
    uintptr_t   site;
    uint8_t     expect[5];
    void       *thunk;
    const char *name;
};

const site_t SITES[] = {
    {SITE_LOADGAME, {0xE8, 0x8D, 0xBE, 0xF8, 0xFF}, (void *)&th_loadgame, "save_load"},
    {SITE_PLANET_SWITCH, {0xE8, 0xE0, 0xAC, 0xFF, 0xFF}, (void *)&th_planet_switch, "planet_transition"},
    {SITE_TACT_RETURN, {0xE8, 0xEA, 0xAC, 0xFF, 0xFF}, (void *)&th_tact_return, "tact_return"},
    {SITE_NEWGAME, {0xE8, 0x96, 0xBC, 0xF9, 0xFF}, (void *)&th_new_game, "new_game"},
    {SITE_NEWGAME_SYSTEM, {0xE8, 0xD1, 0xDC, 0xF8, 0xFF}, (void *)&th_new_game, "new_game(reset=1)"},
};

bool arm(const site_t &s) {
    uint8_t repl[5];
    repl[0]           = 0xE8;
    const int32_t rel = (int32_t)((uintptr_t)s.thunk - (s.site + 5));
    memcpy(repl + 1, &rel, sizeof(rel));
    return mh::hook::patch_bytes_guarded(s.site, s.expect, repl, 5);
}

} // namespace

void replay_boundary_install() {
    if (!mh::en_build_ok() || !rr::enabled()) return;
    static bool done = false;
    if (done) return;
    done = true;
    char line[512];
    int  at = _snprintf_s(line, sizeof(line), _TRUNCATE, "; [desync] REPLAY BOUNDARY splices:");
    int  ok = 0;
    for (const site_t &s : SITES) {
        const char *owner = mh::hook::promoted_owner_of(s.site);
        const char *res;
        if (owner) res = "DISPLACED (promoted; the per-step poll stands in)";
        else if (arm(s)) {
            res = "armed";
            ++ok;
        } else
            res = "NOT armed (bytes differ)";
        if (at > 0 && at < (int)sizeof(line))
            at += _snprintf_s(line + at, sizeof(line) - at, _TRUNCATE, " %s@%08X %s;", s.name, (unsigned)s.site, res);
    }
    if (at > 0 && at < (int)sizeof(line) - 1) lstrcatA(line, "\n");
    seam_log(line);
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "; [desync] REPLAY BOUNDARY: %d/%d armed -- each notes the state it is about to replace "
                "(hash, clock, source file) for the next step's segment seed\n",
                ok, (int)(sizeof(SITES) / sizeof(SITES[0])));
    seam_log(line);
    g_armed_sites = ok ? "armed" : "none";
}
