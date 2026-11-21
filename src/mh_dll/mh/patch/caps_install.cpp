//
// patch/caps_install.cpp -- patching:PT-CAP8 (v0.2.2 cap raise), slice C: the boot arm.
//
//   1. apply the RELOCATION manifest (cap_v022_reloc_EN): every raised array lives in `.mhcapd` from now
//      on, whatever `[caps] raise` says. All-or-nothing; a refusal (a promoted body holds a site, a
//      foreign byte, no memory) leaves the vanilla layout and a one-line reason.
//   2. apply the LIMIT manifest (cap_v022_limits_EN, `toggle`): its caves are placed and its 51 sites
//      verified and STAGED; nothing is written until mh::caps::set_limits(true) at a quiescent point.
//   2b. (PT-CAP9-U1, v0.2.3) the same two steps for units and soldiers: cap_v023_units_reloc_EN (`.mhcapu`, always) and
//      cap_v023_units_limits_EN (`toggle`, staged). Independent of 1/2 -- either pair may be refused without the other -- but
//      set_limits flips both limit layers as one.
//   3. fill the mh::caps geometry table (D3) so every converted hook and naked stub follows the move.
//   4. install the two canonical-mirror hooks (D1): the stock .bss extents stay a vanilla-shaped mirror
//      of the wide arrays so the hash/recorder/ring/scene dump/snapshot code and the save/load block
//      sites (which keep their ORIGINAL bases and sizes) are untouched.
//        * GATHER at map_SavePlanetToDisk's entry (the save block reads the mirror),
//        * SCATTER right after map_LoadPlanetFromDisk's last array block (the load wrote the mirror).
//      The per-step gather lives in the sim-step feeders (net_seams.cpp on_desync_sim_step, the
//      harness's on_sim_step); the scene-import scatter in desync/scene_dump.cpp.
//
//   5. (slice G, PT-CAP8 S3) the RAISED-CAPS SAVE: the planet-file overflow trailer (save tail before the file closes,
//      load tail inside the D1 scatter detour), the container version STAMP (header write + header read) and the
//      load-failure message. Four byte splices, installed only when the relocation applied; see caps/ovf_save.h.
//
// G68's order: this runs after set_owner_table + reimpl_probe_install (so the C1 registry knows every
// promoted body and a reloc site inside one refuses the manifest) and before every detour and byte patch
// (so whichever of the two arrives second is the one refused -- the three composing hooks use
// patch_bytes_guarded_caps for exactly that reason).
//
// ONE LOG LINE, ALWAYS, in the shape the spec fixes:
//   ; [caps] relocation APPLIED (3945 sites, 2 sections, base .mhcapd=XXXXXXXX) in N ms; limits OFF
//   ; [caps] relocation REFUSED: <why> -- vanilla layout, raise unavailable
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "mh_ini_gate.h"   // RL2: the ship gate every ini read goes through
#include "mh_config_dir.h" // PT-CAP9-U8b: config_dir() for the post-match dump's default root

#include <cstdio>
#include <cstring>

#include "caps/ai_scratch_layout.gen.h"
#include "caps/caps.h"
#include "caps/ovf.h"
#include "caps/orders.h"
#include "caps/ovf_save.h"
#include "mods/save_meta.h"
#include "seams/map_transfer.h"
#include "seams/replay_boundary.h"
#include "seams/storage_policy_save.h"
#include "caps/save_host.h"
#include "hook/promoted.h"
#include "caps/path_pool.h"
#include "config/ini_read.h"
#include "addr/mh_calls.gen.h"
#include "hook/detour.h"
#include "hook/patch.h"
#include "include/mh_caps_export.h"
#include "include/mh_inmem_patch_export.h"
#include "include/mh_log_sink.h"
#include "patch/inmem_patch.h"
#include "patch/manifests.gen.h"
#include "ui/player_strings.h"

// net_seams.cpp; declared rather than pulling in net_internal.h for one log sink.
void seam_log(const char *s);

namespace {

mh::patch::apply_result g_reloc_r   = {};
mh::patch::apply_result g_limits_r  = {};
mh::patch::apply_result g_ureloc_r  = {}; // PT-CAP9-U1: units + soldiers
mh::patch::apply_result g_ulimits_r = {};
mh::patch::apply_result g_ai_r      = {};
mh::patch::apply_result g_orders_r  = {}; // PT-CAP9-U5
bool                    g_attempted = false;
bool                    g_post_dump = false; // PT-CAP9-U8b: `[caps] post_match_dump=1` -- see MH_Caps_DumpPostMatch

// PT-CAP9-U5 U5c: the PLAYER's half of a refused order burst. orders.cpp calls this (at most once per sim step, i.e. per click) when the sender
// quota / a full staging list refused a record. Only the LOCAL player's own click is announced (an AI player's refused orders are counted, not shown);
// the text is the red floating message the cheat refusal and the desync alert already use (G_TEXT_TMP -> llm_ui_print_floating_msg_red), a row of
// ui/player_strings.def so a language pack (ru) translates it.
void orders_refused_notice(unsigned owner, bool quota) {
    if (owner != (unsigned)*reinterpret_cast<const volatile uint16_t *>(mh::addr::PlayerSide)) return;
    wsprintfW(reinterpret_cast<wchar_t *>(mh::addr::G_TEXT_TMP), L"%s", mh::ui::tr(quota ? mh::ui::Str::ORDERS_QUOTA : mh::ui::Str::ORDERS_REFUSED));
    mh::call::llm_ui_print_floating_msg_red(reinterpret_cast<void *>(mh::addr::G_TEXT_TMP));
    char b[160];
    _snprintf_s(b, sizeof(b), _TRUNCATE, "; [caps] orders: player notice shown (%s) for player %u\n", quota ? "per-player quota" : "orders queue full", owner);
    seam_log(b);
}

constexpr uintptr_t ADDR_SAVE_PLANET = 0x00447bb3; // map_SavePlanetToDisk (Watcom prologue: stolen 8)
// PT-CAP9-U7b: the scatter / trailer / after-load splice sits behind the LAST block of the pool, the path free-count block (utils block 0xb63bc0): until then
// the file's path buffers (0x4484ec), busy flags (0x448501) and free count (0x448516) have not been read into the stock mirror, and a scatter any earlier
// publishes the PRE-load mirror (the loaded pool was lost on every planet load, raise ON or OFF). The six bytes are the same shape as the old site
// (`add [ebp-0x20],eax ; mov ebx,[ebp-0x28]`), so the detour and the frame offsets are unchanged. Nothing between the old and the new site reads a wide array.
constexpr uintptr_t ADDR_LOAD_ARRAYS_END  = 0x0044851b;                           // map_LoadPlanetFromDisk: ADD [EBP-0x20],EAX after the path free-count block
const uint8_t       LOAD_SPLICE_EXPECT[6] = {0x01, 0x45, 0xE0, 0x8B, 0x5D, 0xD8}; // add [ebp-0x20],eax ; mov ebx,[ebp-0x28]

void *g_save_tramp = nullptr;
void *g_load_tramp = nullptr;

// ---- slice G: the raised-caps save, game side ----------------------------------------------------------------
// CRT entry points the game's own save code uses (Watcom register convention; marshalled by mh::call::detail).
constexpr uintptr_t FN_FSEEK  = 0x004cfc13; // fseek(EAX file, EDX offset, EBX whence) -> 0 / -1
constexpr uintptr_t FN_FTELL  = 0x004cfd07; // ftell(EAX file) -> pos / -1
constexpr uintptr_t FN_FREAD  = 0x004cfd58; // utils_read_from_file
constexpr uintptr_t FN_FCLOSE = 0x004cf7d1; // utils_close_file
constexpr uintptr_t FN_MSGBOX = 0x004bc6aa; // llm_ui_dlg_msgbox_ok

// The four call splices (each a 5-byte `E8 rel32`; the stub's tail JMPs to the original target).
constexpr uintptr_t ADDR_SAVE_CLOSE        = 0x004480e0;                     // map_SavePlanetToDisk: CALL utils_close_file (EAX = the FILE)
constexpr uintptr_t ADDR_HDR_READ          = 0x004476b4;                     // llm_game_load: CALL utils_read_from_file of the 40-byte version header
constexpr uintptr_t ADDR_LOADFAIL_MSG      = 0x004bb76d;                     // llm_menu_loadgame_confirm: CALL llm_ui_dlg_msgbox_ok of the failure text
const uint8_t       SAVE_CLOSE_EXPECT[5]   = {0xE8, 0xEC, 0x76, 0x08, 0x00}; // -> 0x004cf7d1
const uint8_t       HDR_READ_EXPECT[5]     = {0xE8, 0x9F, 0x86, 0x08, 0x00}; // -> 0x004cfd58
const uint8_t       LOADFAIL_MSG_EXPECT[5] = {0xE8, 0x38, 0x0F, 0x00, 0x00}; // -> 0x004bc6aa

// DATAEXT1 (mh/mods/save_meta): the OUTER-container mod-metadata carrier. Its three game sites: the
// SUCCESSFUL close of game_SaveGame (0x004475c5, after the trailing media block -- the failure close
// at 0x004473ef is NOT spliced, so a failed save never grows a carrier), the load-succeeded branch of
// llm_menu_loadgame_confirm (0x004bb761, the CALL llm_menu_finish_enter_gameplay) and -- only when
// install_save_splices did NOT land the composed header-read stub -- the header-read site itself
// (see install_mod_save_hooks; ownership is tracked by g_caps_splices_mask, never guessed from bytes).
constexpr uintptr_t ADDR_CONTAINER_CLOSE      = 0x004475c5;                     // game_SaveGame: CALL utils_close_file (EAX = the FILE)
constexpr uintptr_t ADDR_LOAD_OK_PUMP         = 0x004bb761;                     // llm_menu_loadgame_confirm: CALL llm_menu_finish_enter_gameplay
constexpr uintptr_t FN_PUMP                   = 0x004b8245;                     // llm_menu_finish_enter_gameplay
const uint8_t       CONTAINER_CLOSE_EXPECT[5] = {0xE8, 0x07, 0x82, 0x08, 0x00}; // -> 0x004cf7d1 (verified against /eng/mh.exe)
const uint8_t       LOAD_OK_PUMP_EXPECT[5]    = {0xE8, 0xDF, 0xCA, 0xFF, 0xFF}; // -> 0x004b8245 (verified against /eng/mh.exe)
// ORDERSEQ1: successful CONTAINER read close, not member-output close00447b17.
// EN llm_game_load004475de reaches this only after extraction has succeeded.
constexpr uintptr_t ADDR_CONTAINER_LOAD_CLOSE      = 0x00447b24u;
const uint8_t       CONTAINER_LOAD_CLOSE_EXPECT[5] = {0xe8, 0xa8, 0x7c, 0x08, 0x00};
constexpr uintptr_t ADDR_CONTAINER_LOAD_BEGIN      = 0x00447785u;
constexpr uintptr_t ADDR_PLANET_LOAD_BEGIN         = 0x00448222u;
const uint8_t       CONTAINER_LOAD_BEGIN_EXPECT[5] = {0xe8, 0x5c, 0x0f, 0x00, 0x00};
const uint8_t       PLANET_LOAD_BEGIN_EXPECT[5]    = {0xe8, 0xbf, 0x04, 0x00, 0x00};
uintptr_t           g_native_first_block_read      = 0x004486e6u;

// The container FILE at the 40-byte header read: BOTH header-read stubs store ECX here (the game
// loads it from [EBP-0x28] at 0x004476a2) BEFORE the read call clobbers it; the mod probe reads it
// afterwards. extern "C" so the inline asm can name it.
extern "C" uintptr_t g_hdr_read_file; // defined in mods/save_meta.cpp

// Game data the splices read (EN image, fixed addresses).
constexpr uintptr_t ADDR_VERSION_TEMP = 0x00a4f128; // G_LZW_TEMP_DATA: where llm_game_load reads the 40 bytes

uintptr_t g_fn_read        = FN_FREAD; // storage for `call dword ptr [..]` in the naked stubs
uintptr_t g_fn_close       = FN_FCLOSE;
uintptr_t g_fn_msgbox      = FN_MSGBOX;
uintptr_t g_fn_pump        = FN_PUMP; // DATAEXT1: llm_menu_finish_enter_gameplay, through load_ok_pump_stub
uintptr_t g_fn_save_planet = ADDR_SAVE_PLANET;

int32_t crt_tell(void *f) { return static_cast<int32_t>(mh::call::detail::s_u32_EAX(FN_FTELL, reinterpret_cast<uint32_t>(f))); }
int     crt_seek(void *f, int32_t off, int whence) {
    return static_cast<int>(mh::call::detail::s_u32_EAX_EDX_EBX(FN_FSEEK, reinterpret_cast<uint32_t>(f), static_cast<uint32_t>(off), static_cast<uint32_t>(whence)));
}
// fread / fwrite of ONE element of n bytes: the Watcom routines return the element count (1 = all of it).
uint32_t crt_read(void *f, void *dst, uint32_t n) {
    return n && mh::call::utils_read_from_file(dst, n, 1, f) == 1 ? n : 0;
}
uint32_t crt_write(void *f, const void *src, uint32_t n) {
    return n && mh::call::utils_write_to_file(reinterpret_cast<uint32_t>(f), 1, const_cast<void *>(src), n) == 1 ? n : 0;
}
const mh::caps::ovf::file_io CRT_IO = {&crt_tell, &crt_seek, &crt_read, &crt_write};

wchar_t g_refusal_text[mh::ui::STR_MAX];

void on_save_entry() {
    mh::caps::canon_gather();
    mh::caps::path::save_begin(); // restored on EVERY result path at the common native save finalizer
}
// canon_scatter() zeroes the wide tails, THEN the trailer (if any) writes them back; the file position is restored. Runs after the LAST pool block.
void on_load_arrays_done(void *file, int32_t *fail) {
    mh::caps::canon_scatter();
    mh::caps::ovf::load_tail(CRT_IO, file, fail);
    mh::caps::orders::finish_scatter();                                                                     // PT-CAP9-U7c: the queue count the file carried (past 300) comes back with its tail records, or stays clamped (logged)
    mh::caps::path::after_load((mh::caps::ovf::last_load_features() & mh::caps::ovf::FEAT_PATH_ID16) != 0); // PT-CAP9-U4/U7b: the 16-bit path ids + free counts
}
int on_save_tail(void *file) {
    const bool ok = mh::storage_policy::save::write_planet(CRT_IO, file);
    return ok ? 1 : 0;
}
void on_save_restore() { mh::caps::savehost::restore(); }
int  g_caps_splices_mask = 0; // install_save_splices' landed bits; bit 4 = the composed stub owns the header-read site
void on_header_read() {
    if (!mh::storage_policy::save::host_normalize_version(reinterpret_cast<void *>(ADDR_VERSION_TEMP)))
        memset(reinterpret_cast<void *>(ADDR_VERSION_TEMP), 0, 40);
}
int on_container_tail(void *file) {
    return mh::storage_policy::save::host_finish_container(static_cast<int32_t>(reinterpret_cast<uintptr_t>(file)));
}
int on_container_loaded(void *file) noexcept {
    try {
        return mh::storage_policy::save::host_load_container(static_cast<int32_t>(reinterpret_cast<uintptr_t>(file)));
    } catch (...) { return 0; }
}
int on_load_begin() noexcept {
    try {
        return mh::storage_policy::save::host_begin_load();
    } catch (...) { return 0; }
}
void on_load_result(uintptr_t frame) {
    auto &result     = *reinterpret_cast<uint32_t *>(frame - 0x14);
    auto &file_value = *reinterpret_cast<int32_t *>(frame - 0x28);
    if (file_value == 0) return;
    if (result == 1 && !mh::storage_policy::save::host_load_planet(file_value)) result = 0;
    mh::call::utils_close_file(reinterpret_cast<void *>(static_cast<uintptr_t>(file_value)));
    file_value = 0;
}
// -> the text to show instead of the generic load-failure line, or null.
const void *on_loadfail_text() {
    const bool raised          = mh::caps::ovf::take_refused_raised();
    const bool missing_tileset = mh::seams::maps::take_missing_save_tileset_refusal();
    if (!raised && !missing_tileset) return nullptr;
    wcsncpy_s(g_refusal_text, mh::ui::tr(raised ? mh::ui::Str::CAPS_LOAD_REFUSED : mh::ui::Str::MODS_SAVE_TILESET_MISSING), _TRUNCATE);
    // The box draws centred lines but never wraps (the 87-char English text ran off both edges as one line): break the text at the
    // space nearest the middle -- an embedded LF is honoured (measured on the rig, tmp/capraise/s3g/png/refused_en.png).
    {
        const size_t n    = wcslen(g_refusal_text);
        size_t       best = 0;
        for (size_t i = 1; i + 1 < n; ++i)
            if (g_refusal_text[i] == L' ' && (best == 0 || (i > n / 2 ? i - n / 2 : n / 2 - i) < (best > n / 2 ? best - n / 2 : n / 2 - best))) best = i;
        if (best) g_refusal_text[best] = L'\n';
    }
    seam_log(raised ? "; [caps] load REFUSED: showing the raised-limits message\n"
                    : "; [mods] load REFUSED: showing the missing-tileset message\n");
    return g_refusal_text;
}

// Run-before detours: the displaced instructions run from the trampoline afterwards. pushad/popad keep
// the Watcom register arguments (save: EAX/EDX; load: EAX is live -- the block size just read).
// clang-format off
__declspec(naked) void save_entry_detour() {
    __asm {
        pushad
        pushfd
        call on_save_entry
        popfd
        popad
        jmp  dword ptr [g_save_tramp]
    }
}
// EBP is map_LoadPlanetFromDisk's frame here: [EBP-0x28] = the FILE, [EBP-0x20] = the failure accumulator.
__declspec(naked) void load_arrays_detour() {
    __asm {
        pushad
        pushfd
        lea  eax, [ebp - 0x20]
        push eax
        mov  eax, dword ptr [ebp - 0x28]
        push eax
        call on_load_arrays_done
        add  esp, 8
        popfd
        popad
        jmp  dword ptr [g_load_tramp]
    }
}
// map_SavePlanetToDisk's last operation: close the FILE (EAX). Append the trailer first.
__declspec(naked) void save_close_stub() {
    __asm {
        pushad
        pushfd
        push eax
        call on_save_tail
        add  esp, 4
        test eax, eax
        jnz tail_ok
        inc dword ptr [ebp - 0x24]
    tail_ok:
        popfd
        popad
        jmp  dword ptr [g_fn_close]
    }
}
// llm_game_load's 40-byte header read: do the read, then look at what arrived (EAX = the read's result
// survives). ECX (the FILE) is captured BEFORE the read clobbers it: the mod-metadata probe needs the
// handle. One stub serves both consumers -- the caps stamp check (armed only when the relocation
// applied) and the mod probe -- so this site has exactly one writer per run.
__declspec(naked) void header_read_stub() {
    __asm {
        mov  dword ptr [g_hdr_read_file], ecx
        call dword ptr [g_fn_read]
        pushad
        pushfd
        call on_header_read
        call mh_mods_save_on_header_probe
        popfd
        popad
        ret
    }
}
// llm_menu_loadgame_confirm's failure message box: EAX = the text.
__declspec(naked) void loadfail_msg_stub() {
    __asm {
        pushad
        pushfd
        call on_loadfail_text
        test eax, eax
        jz   keep
        mov  dword ptr [esp + 32], eax
    keep:
        popfd
        popad
        jmp  dword ptr [g_fn_msgbox]
    }
}
// game_SaveGame's SUCCESSFUL close (0x004475c5, after the trailing media block; the failure close at
// 0x004473ef is not spliced): EAX = the FILE. Compose required metadata, then close once.
// A metadata failure clears the native save result and resumes its common failure return.
__declspec(naked) void container_close_stub() {
    __asm {
        pushad
        pushfd
        push eax
        call on_container_tail
        add  esp, 4
        test eax, eax
        jnz container_ok
        mov dword ptr [ebp - 0x18], 0
        mov dword ptr [esp + 36], 004475d1h
    container_ok:
        popfd
        popad
        jmp  dword ptr [g_fn_close]
    }
}
// The original CALL's entry ESP points at00447b29, EAX is the opaque Watcom FILE.
// pushfd+pushad adds36 bytes: [esp+28]=saved EAX, [esp+32]=flags,
// [esp+36]=ORIGINAL CALL return. EBX captures that frame before aligned FXSAVE scratch.
// On refusal the original close still runs once, but returns to00447b9f, which writes
// [ebp-18]=0 before the common00447ba6 return. No apply callback/current-member load.
__declspec(naked) void container_load_close_stub() {
    __asm {
        pushfd
        pushad
        mov ebx, esp
        sub esp, 527
        and esp, -16
        fxsave [esp]
        fninit
        fldcw word ptr [esp]
        cld
        push eax
        call on_container_loaded
        add esp, 4
        cmp eax, 1
        je container_loaded
        mov dword ptr [ebx + 36], 00447b9fh
    container_loaded:
        fxrstor [esp]
        mov esp, ebx
        popad
        popfd
        jmp dword ptr [g_fn_close]
    }
}
// Accepted native version/open only. Quarantine outgoing ownership before the first
// incoming bulk write, including same-world reloads. Preflight/header/resource probes stay read-only.
// EAX=destination, EDX=bytes, EBX=opaque FILE; all are restored before original reader.
__declspec(naked) void container_load_begin_stub() {
    __asm {
        pushfd
        pushad
        mov ebx, esp
        sub esp, 527
        and esp, -16
        fxsave [esp]
        fninit
        fldcw word ptr [esp]
        cld
        call on_load_begin
        cmp eax, 1
        je container_begin_ok
        mov dword ptr [ebx + 36], 00447994h
        fxrstor [esp]
        mov esp, ebx
        popad
        popfd
        ret
    container_begin_ok:
        fxrstor [esp]
        mov esp, ebx
        popad
        popfd
        jmp dword ptr [g_native_first_block_read]
    }
}
__declspec(naked) void planet_load_begin_stub() {
    __asm {
        pushfd
        pushad
        mov ebx, esp
        sub esp, 527
        and esp, -16
        fxsave [esp]
        fninit
        fldcw word ptr [esp]
        cld
        call on_load_begin
        cmp eax, 1
        je planet_begin_ok
        mov dword ptr [ebp - 20h], 1
        mov dword ptr [ebx + 36], 004485e5h
        fxrstor [esp]
        mov esp, ebx
        popad
        popfd
        ret
    planet_begin_ok:
        fxrstor [esp]
        mov esp, ebx
        popad
        popfd
        jmp dword ptr [g_native_first_block_read]
    }
}
// llm_menu_loadgame_confirm's load-succeeded branch: commit the probe's staged provenance and print
// the queued notice once, BEFORE the menu hands over to the game (the save's own message queue is
// already restored by then, so our entry is not overwritten by the file; still no modal, no net pump).
__declspec(naked) void load_ok_pump_stub() {
    __asm {
        pushad
        pushfd
        call mh_mods_save_commit_and_notify
        popfd
        popad
        jmp  dword ptr [g_fn_pump]
    }
}
// Every native save exit, including NULL open, restores temporary path encoding exactly once.
__declspec(naked) void planet_save_result_stub() {
    __asm {
        pushfd
        pushad
        mov ebx, esp
        sub esp, 527
        and esp, -16
        fxsave [esp]
        fninit
        fldcw word ptr [esp]
        call on_save_restore
        fxrstor [esp]
        mov esp, ebx
        popad
        popfd
        mov eax, dword ptr [ebp - 0x14]
        lea esp, [ebp - 0x10]
        push 00448101h
        ret
    }
}
// Deferred FILE close: all block/tileset/apply failures converge here before epilogue.
__declspec(naked) void planet_load_result_stub() {
    __asm {
        pushfd
        pushad
        mov ebx, esp
        sub esp, 527
        and esp, -16
        fxsave [esp]
        fninit
        fldcw word ptr [esp]
        push ebp
        call on_load_result
        add esp, 4
        fxrstor [esp]
        mov esp, ebx
        popad
        popfd
        mov eax, dword ptr [ebp - 0x14]
        lea esp, [ebp - 0x10]
        push 0044865bh
        ret
    }
}
__declspec(naked) void container_planet_save_stub() {
    __asm {
        call dword ptr [g_fn_save_planet]
        pushfd
        test eax, eax
        jnz saved
        mov dword ptr [ebp - 0x18], 0
        mov dword ptr [esp + 4], 004475d1h
    saved:
        popfd
        ret
    }
}
__declspec(naked) void tactical_planet_save_stub() {
    __asm {
        call dword ptr [g_fn_save_planet]
        pushfd
        test eax, eax
        jnz saved
        mov dword ptr [esp + 4], 0044d3a0h
    saved:
        popfd
        ret
    }
}
__declspec(naked) void container_member_guard_stub() {
    __asm {
        pushfd
        cmp dword ptr [ebp - 0x20], 0
        jz refused
        mov dword ptr [esp + 4], 00447507h
        popfd
        ret
    refused:
        mov dword ptr [esp + 4], 004473ech
        popfd
        ret
    }
}
// clang-format on

// `E8 rel32` to `stub` from `site`.
void call_repl(uint8_t (&repl)[5], uintptr_t site, void (*stub)()) {
    repl[0]           = 0xE8;
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(stub) - (site + 5));
    memcpy(repl + 1, &rel, sizeof(rel));
}

struct load_lifecycle_patch {
    uintptr_t      site;
    const uint8_t *original;
    void (*stub)();
    uint8_t replacement[5]{};
    bool    owned = false;
};
load_lifecycle_patch g_load_lifecycle[] = {
    {ADDR_CONTAINER_LOAD_BEGIN, CONTAINER_LOAD_BEGIN_EXPECT, &container_load_begin_stub},
    {ADDR_PLANET_LOAD_BEGIN, PLANET_LOAD_BEGIN_EXPECT, &planet_load_begin_stub},
    {ADDR_CONTAINER_LOAD_CLOSE, CONTAINER_LOAD_CLOSE_EXPECT, &container_load_close_stub},
};
bool install_load_lifecycle() {
    for (auto &p : g_load_lifecycle) {
        if (mh::hook::promoted_owner_of(p.site)) {
            if (p.owned) return false;
            continue;
        }
        if (p.owned) {
            if (memcmp(reinterpret_cast<const void *>(p.site), p.replacement, sizeof(p.replacement))) return false;
            continue;
        }
        call_repl(p.replacement, p.site, p.stub);
        if (!mh::hook::patch_bytes_guarded(p.site, p.original, p.replacement, 5)) return false;
        p.owned = true;
    }
    return true;
}
bool rollback_load_lifecycle() {
    bool ok = true;
    for (uint32_t i = uint32_t(sizeof(g_load_lifecycle) / sizeof(g_load_lifecycle[0])); i; --i) {
        auto &p = g_load_lifecycle[i - 1];
        if (!p.owned) continue;
        // Never restore through a foreign replacement. Continue restoring other owned sites.
        if (!mh::hook::patch_bytes_guarded(p.site, p.replacement, p.original, 5)) {
            ok = false;
            continue;
        }
        p.owned = false;
        memset(p.replacement, 0, sizeof(p.replacement));
    }
    return ok;
}
// Required extensions are cap-independent; promoted bodies own matching host callbacks.
bool required_call(uintptr_t site, const uint8_t *expected, void (*stub)()) {
    if (mh::hook::promoted_owner_of(site)) return true;
    uint8_t replacement[5];
    call_repl(replacement, site, stub);
    return mh::hook::patch_bytes_guarded(site, expected, replacement, 5);
}
int install_save_splices() {
    mh::caps::ovf::set_save_logger(&seam_log);
    int        mask       = 0;
    const bool save_tail  = required_call(ADDR_SAVE_CLOSE, SAVE_CLOSE_EXPECT, &save_close_stub);
    bool       save_final = mh::hook::promoted_owner_of(0x004480fbu) != nullptr;
    if (!save_final) {
        const uint8_t expected[6] = {0x8b, 0x45, 0xec, 0x8d, 0x65, 0xf0};
        uint8_t       jump[6]     = {0xe9, 0, 0, 0, 0, 0x90};
        const int32_t rel         = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&planet_save_result_stub) - (0x004480fbu + 5));
        memcpy(jump + 1, &rel, 4);
        save_final = mh::hook::patch_bytes_guarded(0x004480fbu, expected, jump, 6);
    }
    if (save_tail && save_final) mask |= 1;
    bool load_final = mh::hook::promoted_owner_of(0x00448655u) != nullptr;
    if (!load_final) {
        const uint8_t close_expected[5]  = {0xe8, 0xe4, 0x71, 0x08, 0x00};
        const uint8_t no_close[5]        = {0x90, 0x90, 0x90, 0x90, 0x90};
        const uint8_t result_expected[6] = {0x8b, 0x45, 0xec, 0x8d, 0x65, 0xf0};
        uint8_t       jump[6]            = {0xe9, 0, 0, 0, 0, 0x90};
        const int32_t rel                = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&planet_load_result_stub) - (0x00448655u + 5));
        memcpy(jump + 1, &rel, 4);
        load_final = mh::hook::patch_bytes_guarded(0x004485e8u, close_expected, no_close, 5) &&
                     mh::hook::patch_bytes_guarded(0x00448655u, result_expected, jump, 6);
    }
    if (load_final) mask |= 2;
    if (required_call(ADDR_HDR_READ, HDR_READ_EXPECT, &header_read_stub)) mask |= 4;
    if (required_call(ADDR_LOADFAIL_MSG, LOADFAIL_MSG_EXPECT, &loadfail_msg_stub)) mask |= 8;
    const uint8_t first_save[5] = {0xe8, 0x14, 0x0a, 0x00, 0x00};
    if (required_call(0x0044719au, first_save, &container_planet_save_stub)) mask |= 16;
    bool member_guard = mh::hook::promoted_owner_of(0x004474f5u) != nullptr;
    if (!member_guard) {
        const uint8_t expected[6]    = {0x83, 0x7d, 0xcc, 0x00, 0x75, 0x0c};
        uint8_t       replacement[6] = {0, 0, 0, 0, 0, 0x90};
        uint8_t       call[5];
        call_repl(call, 0x004474f5u, &container_member_guard_stub);
        memcpy(replacement, call, 5);
        member_guard = mh::hook::patch_bytes_guarded(0x004474f5u, expected, replacement, 6);
    }
    if (member_guard) mask |= 32;
    const uint8_t tactical[5] = {0xe8, 0x1f, 0xa8, 0xff, 0xff};
    if (required_call(0x0044d38fu, tactical, &tactical_planet_save_stub)) mask |= 64;
    if (required_call(ADDR_CONTAINER_CLOSE, CONTAINER_CLOSE_EXPECT, &container_close_stub)) mask |= 128;
    g_caps_splices_mask = mask;
    return mask;
}

// Provenance hooks share the required storage header/close owners in every caps configuration.
// The promoted twins call the same hosted preflight/finish callbacks; no competing header stub.
bool install_mod_save_hooks() {
    mh::mods::save::set_logger(&seam_log);
    // (the active-set source is wired ONCE by the resource boot arm after mount_snapshot -- not here)
    // The same failure-dialog splice also carries missing-save-tileset refusals in stock/no-mod boots.
    if (!(g_caps_splices_mask & 8)) {
        uint8_t notice_repl[5];
        call_repl(notice_repl, ADDR_LOADFAIL_MSG, &loadfail_msg_stub);
        if (mh::hook::patch_bytes_guarded(ADDR_LOADFAIL_MSG, LOADFAIL_MSG_EXPECT, notice_repl, 5))
            g_caps_splices_mask |= 8;
        else
            seam_log("; [mods] save missing-tileset failure notice hook NOT armed\n");
    }
    const bool probe_by_caps = (g_caps_splices_mask & 4) != 0; // the composed stub owns the site
    uint8_t    repl[5];
    int        m = 0;
    if ((g_caps_splices_mask & 128) || required_call(ADDR_CONTAINER_CLOSE, CONTAINER_CLOSE_EXPECT, &container_close_stub)) m |= 2;
    call_repl(repl, ADDR_LOAD_OK_PUMP, &load_ok_pump_stub);
    if (mh::hook::patch_bytes_guarded(ADDR_LOAD_OK_PUMP, LOAD_OK_PUMP_EXPECT, repl, 5)) m |= 4;
    const bool continuation = replay_boundary_install_mod_continue();
    mh::mods::save::set_continue_ready(continuation);
    const bool probe_ok = probe_by_caps || (m & 1);
    const bool ok       = probe_ok && (m & 2) && (m & 4);
    char       b[400];
    _snprintf_s(b, sizeof(b), _TRUNCATE, "; [mods] save hooks: probe %s, container close %s, load notice %s, campaign continuation %s -- mods %s\n",
                probe_by_caps ? "armed (caps stub)" : ((m & 1) ? "armed" : "NOT armed"), (m & 2) ? "armed" : "NOT armed",
                (m & 4) ? "armed" : "NOT armed", continuation ? "armed" : "NOT armed", ok ? "may mount" : "HELD: whole-stock fallback");
    seam_log(b);
    mh::mods::save::set_hooks_ready(ok);
    return ok;
}

void fmt_ms(char *out, size_t cap, uint32_t micros) {
    _snprintf_s(out, cap, _TRUNCATE, "%u.%02u", (unsigned)(micros / 1000u), (unsigned)((micros % 1000u) / 10u));
}

} // namespace

int caps_install_impl(const char *ini_path) {
    if (!ini_path) return 0;
    g_attempted = true;
    g_post_dump = mh_ini_get_int("caps", "post_match_dump", 0, ini_path) != 0;
    MH_InMemPatch_BindLog();
    mh::caps::set_logger(&seam_log);
    char b[400];

    if (!mh_ini_get_int("caps", "relocate", 1, ini_path)) {
        seam_log("; [caps] relocation OFF ([caps] relocate=0) -- vanilla layout, raise unavailable\n");
        return 0;
    }
    // [harness] relocate_state=1 moves live().base into a DLL arena: the geometry table would go stale and
    // the hash would read the arena. The two are mutually exclusive; the harness diagnostic wins.
    if (mh_ini_get_int("harness", "enable", 0, ini_path) && mh_ini_get_int("harness", "relocate_state", 0, ini_path)) {
        seam_log("; [caps] relocation REFUSED: [harness] relocate_state=1 moves the state registry bases (mutually exclusive) -- vanilla layout, raise unavailable\n");
        return 0;
    }

    g_reloc_r = mh::patch::apply(*mh::patch::CAPS_RELOC, nullptr, mh::patch::APPLY_QUIET_OK);
    if (!g_reloc_r.applied) {
        // The headline is name-free on purpose (the arm-order gate pins its text; a function name here would red it whenever the
        // promoted set changes); the detail line carries site/function and is folded out of the gate (check_arm_order BULK_RES).
        _snprintf_s(b, sizeof(b), _TRUNCATE, "; [caps] relocation REFUSED: %s -- vanilla layout, raise unavailable\n",
                    g_reloc_r.refusal ? g_reloc_r.refusal : "?");
        seam_log(b);
        if (g_reloc_r.detail[0]) {
            const char *d = g_reloc_r.detail;
            if (strncmp(d, "; [inmem] ", 10) == 0) d += 10;
            _snprintf_s(b, sizeof(b), _TRUNCATE, "; [caps] relocation REFUSED detail: %s", d);
            size_t n = strlen(b);
            if (n && b[n - 1] != '\n' && n + 1 < sizeof(b)) {
                b[n]     = '\n';
                b[n + 1] = 0;
            }
            seam_log(b);
        }
        return 0;
    }
    const mh::patch::inmem_manifest &rm = *mh::patch::CAPS_RELOC;
    if (rm.section_count < 1 || strcmp(rm.sections[0].name, ".mhcapd") != 0 || !mh::caps::adopt_relocation(g_reloc_r.actual_base[0])) {
        // Written but unusable: the sites already point into .mhcapd, so this is NOT a refusal we can undo.
        seam_log("; [caps] relocation APPLIED but the layout table refused -- the arrays are in .mhcapd and the geometry table is STOCK: this run is invalid\n");
        return 0;
    }
    int rc = MH_CAPS_RELOCATED;

    g_limits_r      = mh::patch::apply(*mh::patch::CAPS_LIMITS, &mh::caps::limits_state(), mh::patch::APPLY_QUIET_OK);
    uint32_t micros = g_reloc_r.micros;
    if (g_limits_r.applied) {
        rc |= MH_CAPS_LIMITS;
        micros += g_limits_r.micros;
    }
    // PT-CAP11: the AI scratch layer (a toggle manifest like the limit layer): its `.mhcapa` section holds the enlarged AI scratch arrays
    // and their guards; its 165 sites are written only when the raise goes ON (mh::caps::set_limits). `[caps] ai_scratch=0` leaves it
    // out -- the negative arm, where the raised AI runs on the vanilla scratch arrays.
    if (mh_ini_get_int("caps", "ai_scratch", 1, ini_path)) {
        g_ai_r = mh::patch::apply(*mh::patch::CAPS_AI, &mh::caps::ai_state(), mh::patch::APPLY_QUIET_OK);
        if (g_ai_r.applied && mh::patch::CAPS_AI->section_count >= 1 && strcmp(mh::patch::CAPS_AI->sections[0].name, ".mhcapa") == 0 &&
            mh::caps::ai_adopt(g_ai_r.actual_base[0])) {
            micros += g_ai_r.micros;
            rc |= MH_CAPS_AI;
            _snprintf_s(b, sizeof(b), _TRUNCATE, "; [caps] ai-scratch layer STAGED (%d sites, %u arrays, .mhcapa=0x%08X); written with the raise\n",
                        mh::patch::CAPS_AI->site_count, (unsigned)mh::caps::ai_scratch_layout::COUNT, (unsigned)g_ai_r.actual_base[0]);
        } else {
            _snprintf_s(b, sizeof(b), _TRUNCATE, "; [caps] ai-scratch layer REFUSED: %s -- the AI scratch arrays stay vanilla-sized\n",
                        g_ai_r.refusal ? g_ai_r.refusal : "section/guard setup failed");
            if (g_ai_r.applied) mh::caps::ai_state().staged = false; // staged but unusable: never flip it
        }
        seam_log(b);
    } else {
        seam_log("; [caps] ai-scratch layer OFF ([caps] ai_scratch=0) -- the raised AI runs on the vanilla scratch arrays (negative arm)\n");
    }
    // PT-CAP9-U1: units + soldiers. A refusal leaves them at the stock layout (the v0.2.2 arrays are already relocated and stay so).
    bool units_ok = false;
    g_ureloc_r    = mh::patch::apply(*mh::patch::CAPS_UNITS_RELOC, nullptr, mh::patch::APPLY_QUIET_OK);
    if (g_ureloc_r.applied) {
        const mh::patch::inmem_manifest &um = *mh::patch::CAPS_UNITS_RELOC;
        if (um.section_count < 1 || strcmp(um.sections[0].name, ".mhcapu") != 0 || !mh::caps::adopt_units_relocation(g_ureloc_r.actual_base[0])) {
            seam_log("; [caps] units relocation APPLIED but the layout table refused -- units/soldiers are in .mhcapu and the geometry table is STOCK: this run is invalid\n");
        } else {
            units_ok = true;
            rc |= MH_CAPS_UNITS_RELOCATED;
            // PT-CAP9-U4: the same manifest also places the path-slot pool (`.mhcapp`) and its code cave section (`.mhcapq`).
            if (um.section_count >= 3 && strcmp(um.sections[1].name, ".mhcapp") == 0 && strcmp(um.sections[2].name, ".mhcapq") == 0 &&
                mh::caps::path::adopt(g_ureloc_r.actual_base[1])) {
                rc |= MH_CAPS_PATH;
                _snprintf_s(b, sizeof(b), _TRUNCATE, "; [caps] path pool relocated (8 players x 501 slots, .mhcapp=0x%08X, caves .mhcapq=0x%08X)\n",
                            (unsigned)g_ureloc_r.actual_base[1], (unsigned)g_ureloc_r.actual_base[2]);
                seam_log(b);
            } else {
                seam_log("; [caps] units relocation APPLIED but the path-pool layout table refused -- the pool is in .mhcapp and the geometry is STOCK: this run is invalid\n");
            }
            micros += g_ureloc_r.micros;
            g_ulimits_r = mh::patch::apply(*mh::patch::CAPS_UNITS_LIMITS, &mh::caps::units_limits_state(), mh::patch::APPLY_QUIET_OK);
            if (g_ulimits_r.applied) {
                // PT-CAP9-U4: the limit layer's path rewrites JUMP into the reloc layer's `.mhcapq` caves, and a rel32 into ANOTHER
                // manifest's section is not one of this manifest's refs. When mhpatch's baked placement did not hold (the section
                // sits at its actual base), shift every such staged JMP by the move so it lands on the live cave.
                const auto &um = *mh::patch::CAPS_UNITS_RELOC;
                if (um.section_count >= 3) {
                    auto          &ts  = mh::caps::units_limits_state();
                    const uint32_t qlo = um.sections[2].preferred_va, qhi = qlo + um.sections[2].vsize;
                    const int32_t  move  = (int32_t)(g_ureloc_r.actual_base[2] - qlo);
                    int            fixed = 0;
                    for (int i = 0; move && i < ts.m->site_count; ++i) {
                        const auto &st = ts.m->sites[i];
                        if (st.len < 5 || ts.on_bytes[i][0] != 0xE9) continue;
                        int32_t rel;
                        memcpy(&rel, &ts.on_bytes[i][1], 4);
                        const uint32_t tgt = st.va + 5u + (uint32_t)rel;
                        if (tgt < qlo || tgt >= qhi) continue;
                        rel += move;
                        memcpy(&ts.on_bytes[i][1], &rel, 4);
                        ++fixed;
                    }
                    if (fixed) {
                        _snprintf_s(b, sizeof(b), _TRUNCATE, "; [caps] units limit layer: %d staged jump(s) re-aimed at the live .mhcapq (moved %+d)\n", fixed, (int)move);
                        seam_log(b);
                    }
                }
                rc |= MH_CAPS_UNITS_LIMITS;
                micros += g_ulimits_r.micros;
            }
        }
    }
    // PT-CAP9-U5: the ORDER layer (toggle manifest, flipped with the limit layer): `.mhcapo` holds the enlarged queue / pending / staging
    // arrays, `.mhcapr` the sender-quota thunk. `[caps] orders_layer=0` leaves it out (the negative arm: the raised roster on the vanilla
    // order arrays, where a 500-order frame is dropped). A refusal leaves the vanilla order arrays.
    if (mh_ini_get_int("caps", "orders_layer", 1, ini_path)) {
        g_orders_r = mh::patch::apply(*mh::patch::CAPS_ORDERS, &mh::caps::orders::state(), mh::patch::APPLY_QUIET_OK);
        if (g_orders_r.applied && mh::patch::CAPS_ORDERS->section_count >= 2 && strcmp(mh::patch::CAPS_ORDERS->sections[0].name, ".mhcapo") == 0 &&
            mh::caps::orders::adopt(g_orders_r.actual_base[0])) {
            micros += g_orders_r.micros;
            rc |= MH_CAPS_ORDERS;
            mh::caps::orders::set_notice_hook(&orders_refused_notice);
            mh::caps::orders::set_release_cap_for_test((uint32_t)mh_ini_get_int("caps", "orders_release_cap", 0, ini_path));
            _snprintf_s(b, sizeof(b), _TRUNCATE, "; [caps] orders layer STAGED (%d sites, queue/pending/staging, .mhcapo=0x%08X); written with the raise\n",
                        mh::patch::CAPS_ORDERS->site_count, (unsigned)g_orders_r.actual_base[0]);
        } else {
            _snprintf_s(b, sizeof(b), _TRUNCATE, "; [caps] orders layer REFUSED: %s -- the order arrays stay vanilla-sized (queue 300 / staging 300 / pending 1000)\n",
                        g_orders_r.refusal ? g_orders_r.refusal : "section/guard setup failed");
            if (g_orders_r.applied) mh::caps::orders::state().staged = false; // staged but unusable: never flip it
        }
        seam_log(b);
    } else {
        seam_log("; [caps] orders layer OFF ([caps] orders_layer=0) -- the raised roster runs on the vanilla order arrays (negative arm)\n");
    }
    const bool mirror_on = mh_ini_get_int("caps", "mirror_check", 0, ini_path) != 0;
    mh::caps::set_mirror_check(mirror_on);
    if (mh_ini_get_int("caps", "path_check", 0, ini_path)) {
        mh::caps::path::set_check(true);
        seam_log("; [caps] path_check ARMED: the path pool's invariants are checked every sim step (first violation of each kind is logged)\n");
    }
    if (mirror_on) seam_log("; [caps] mirror_check ARMED: a writer that touches a stock (canonical-mirror) address is reported once per array\n");

    char ms[24];
    fmt_ms(ms, sizeof(ms), micros);
    _snprintf_s(b, sizeof(b), _TRUNCATE,
                "; [caps] relocation APPLIED (%d sites, %d sections, base .mhcapd=0x%08X) in %s ms; limits OFF%s\n",
                rm.site_count, rm.section_count, (unsigned)g_reloc_r.actual_base[0], ms,
                g_limits_r.applied ? "" : " (the limit layer was NOT staged -- raise unavailable)");
    seam_log(b);
    if (!g_limits_r.applied) {
        _snprintf_s(b, sizeof(b), _TRUNCATE, "; [caps] limit layer REFUSED: %s\n", g_limits_r.refusal ? g_limits_r.refusal : "?");
        seam_log(b);
    }
    if (units_ok) {
        _snprintf_s(b, sizeof(b), _TRUNCATE, "; [caps] units relocation APPLIED (%d sites, base .mhcapu=0x%08X); units limits %s\n",
                    mh::patch::CAPS_UNITS_RELOC->site_count, (unsigned)g_ureloc_r.actual_base[0],
                    g_ulimits_r.applied ? "staged" : "NOT staged (units stay at 100 when the raise is on)");
        seam_log(b);
        if (!g_ulimits_r.applied) {
            _snprintf_s(b, sizeof(b), _TRUNCATE, "; [caps] units limit layer REFUSED: %s (site %d, va %08X)\n", g_ulimits_r.refusal ? g_ulimits_r.refusal : "?", g_ulimits_r.refused_site,
                        g_ulimits_r.refused_site >= 0 ? (unsigned)mh::patch::CAPS_UNITS_LIMITS->sites[g_ulimits_r.refused_site].va : 0u);
            seam_log(b);
        }
    } else {
        _snprintf_s(b, sizeof(b), _TRUNCATE, "; [caps] units relocation REFUSED: %s -- units and soldiers stay at 100\n",
                    g_ureloc_r.refusal ? g_ureloc_r.refusal : "(layout table)");
        seam_log(b);
    }

    // D1 hooks. The load splice is a mid-body trampoline: check its six bytes by hand (the primitive's own
    // prologue guard is off for a non-entry) and refuse rather than splice an unknown instruction.
    bool save_ok = mh::hook::promoted_owner_of(ADDR_SAVE_PLANET) != nullptr;
    if (!save_ok) save_ok = mh::hook::install_trampoline(ADDR_SAVE_PLANET, (void *)save_entry_detour, &g_save_tramp, 8,
                                                         mh::hook::entry_claim::exclusive, "the caps canonical-mirror gather (planet save)");
    bool load_ok = mh::hook::promoted_owner_of(ADDR_LOAD_ARRAYS_END) != nullptr;
    if (!load_ok && memcmp(reinterpret_cast<const void *>(ADDR_LOAD_ARRAYS_END), LOAD_SPLICE_EXPECT, sizeof(LOAD_SPLICE_EXPECT)) == 0)
        load_ok = mh::hook::install_trampoline(ADDR_LOAD_ARRAYS_END, (void *)load_arrays_detour, &g_load_tramp, 6,
                                               mh::hook::entry_claim::exclusive, "the caps canonical-mirror scatter (planet load)", 0);
    _snprintf_s(b, sizeof(b), _TRUNCATE, "; [caps] canonical-mirror hooks: save gather %s, load scatter %s\n",
                save_ok ? "armed" : "NOT armed", load_ok ? "armed" : "NOT armed");
    seam_log(b);
    if (!save_ok || !load_ok)
        seam_log("; [caps] WARNING: a mirror hook is missing -- saves/loads through the stock-shaped blocks are WRONG this run\n");
    return rc;
}

namespace mh::caps::savehost {
int32_t prepare() {
    canon_gather();
    path::save_begin();
    return 1;
}
void    restore() { path::save_end(); }
int32_t load_extensions(int32_t handle) {
    int32_t failed = 0;
    canon_scatter();
    const int result = ovf::load_tail(CRT_IO, reinterpret_cast<void *>(static_cast<uintptr_t>(handle)), &failed);
    orders::finish_scatter();
    path::after_load((ovf::last_load_features() & ovf::FEAT_PATH_ID16) != 0);
    return result >= 0 && failed == 0 ? 1 : 0;
}
} // namespace mh::caps::savehost


// DATAEXT1: the mod hooks install AFTER the caps decision, in every configuration -- the relocation
// gate must not gate the mod carrier. The wrapper FOLLOWS the definition, so caps_install_impl is
// declared where the wrapper compiles.
extern "C" int MH_Caps_Install(const char *ini_path) {
    const int rc = caps_install_impl(ini_path);
    if (!install_load_lifecycle() || install_save_splices() != 255 || !replay_boundary_install_storage_save()) {
        if (!rollback_load_lifecycle())
            seam_log("; [storage] load lifecycle rollback REFUSED: site no longer matches owned bytes\n");
        seam_log("; [storage] required save carriers could not arm; refusing unsafe game execution\n");
        TerminateProcess(GetCurrentProcess(), ERROR_INVALID_DATA);
        return 0;
    }
    install_mod_save_hooks();
    return rc;
}

extern "C" void MH_Caps_CanonGather(void) {
    mh::caps::note_sim_step();
    mh::caps::canon_gather();
}
extern "C" void  MH_Caps_CanonScatter(void) { mh::caps::canon_scatter(); }
extern "C" void *MH_Caps_Rec(unsigned arr, unsigned player, unsigned idx) {
    if (arr == MH_CAPS_IDX_PATH_SLOT) { // PT-CAP9-U7d: the path pool's slot `idx` of `player` (600 waypoint bytes); null unless relocated and in range
        if (!mh::caps::path::relocated() || player >= 8u || idx >= 501u) return nullptr;
        return mh::caps::path::waypoint(player, idx, 0);
    }
    if (arr >= MH_CAPS_COUNT) return nullptr;
    return mh::caps::rec<uint8_t>(static_cast<mh::caps::arr>(arr), player, idx);
}

extern "C" unsigned MH_Caps_OrderGeom(unsigned which, unsigned *base) {
    if (which >= mh::caps::orders::N) return 0;
    const auto w = static_cast<mh::caps::orders::which>(which);
    if (base) *base = static_cast<unsigned>(reinterpret_cast<uintptr_t>(mh::caps::orders::base(w)));
    return mh::caps::orders::cap(w);
}

extern "C" void MH_Caps_OvfSums(unsigned long long *out16) {
    if (!out16) return;
    if (!mh::caps::relocated() && !mh::caps::units_relocated() && !mh::caps::path::relocated() && !mh::caps::orders::on()) {
        for (int e = 0; e < mh::caps::ovf::SLICES; ++e) out16[e] = 0;
        return;
    }
    uint64_t s[mh::caps::ovf::SLICES];
    mh::caps::ovf::recompute_all(s);
    for (int e = 0; e < mh::caps::ovf::SLICES; ++e) out16[e] = s[e];
}
// PT-CAP9-U4: the path-slot pool, for the harness (which cannot link mh::caps).
extern "C" void MH_Caps_PathCensus(unsigned player, unsigned *holding, unsigned *max_id, unsigned *flags_set, int *free_count) {
    const mh::caps::path::census c = mh::caps::path::take_census(player);
    if (holding) *holding = c.holding;
    if (max_id) *max_id = c.max_id;
    if (flags_set) *flags_set = c.flags_set;
    if (free_count) *free_count = c.free_count;
}
extern "C" unsigned MH_Caps_PathLimit(void) { return mh::caps::path::slot_limit(); }
extern "C" unsigned MH_Caps_PathCheckNow(void) { return mh::caps::path::check_now(); }
extern "C" unsigned MH_Caps_PathViolations(void) { return mh::caps::path::violations_total(); }
extern "C" void     MH_Caps_PathDigests(unsigned long long *out4) {
    if (!out4) return;
    out4[0] = mh::caps::path::pool_digest(mh::caps::path::DIGEST_WINDOW);
    out4[1] = mh::caps::path::pool_digest(mh::caps::path::DIGEST_TAILS);
    out4[2] = mh::caps::path::pool_digest(mh::caps::path::DIGEST_POOL);
    out4[3] = (mh::caps::relocated() || mh::caps::units_relocated()) ? mh::caps::ovf::digest() : 0ull;
}
extern "C" unsigned long long MH_Caps_OvfDigest(void) { return (mh::caps::relocated() || mh::caps::units_relocated()) ? mh::caps::ovf::digest_persisted() : 0ull; }

// PT-CAP9-U8b. The runtime proof that a raised match left the toggle layers' sites at the ORIGINAL bytes: after the session-close revert
// (net_discovery close_restore_local) write the four toggle layers' live sites, in the parity dump's format, to
// `<MH_LOG_ROOT>\caps_post_match_<pid>.<layer>`; tools/check_caps_off_clean.py compares them with the clean exe. Off unless
// `[caps] post_match_dump=1` (a test aid; MH_LOG_ROOT when the launcher owns the logs root, else <config dir>logs, like the crash marker).
extern "C" void MH_Caps_DumpPostMatch(void) {
    if (!g_post_dump || !g_attempted) return;
    if (mh::caps::raised()) { // the local setting keeps the raise (the host's case): there is no revert to prove
        seam_log("; [caps] post-match dump skipped: the limits are still ON (the local setting keeps the raise)\n");
        return;
    }
    char  root[MAX_PATH] = {};
    DWORD n              = GetEnvironmentVariableA("MH_LOG_ROOT", root, MAX_PATH);
    if (!n || n >= MAX_PATH) { // the same root the crash marker uses: <config dir>logs (the rig lane: beside the exe)
        _snprintf_s(root, sizeof(root), _TRUNCATE, "%slogs", mh::cfgdir::config_dir());
        n = (DWORD)lstrlenA(root);
    }
    while (n > 1 && (root[n - 1] == '\\' || root[n - 1] == '/')) root[--n] = '\0';
    char base[MAX_PATH + 48];
    _snprintf_s(base, sizeof(base), _TRUNCATE, "%s\\caps_post_match_%lu", root, (unsigned long)GetCurrentProcessId());
    struct row {
        const char                      *ext;
        const mh::patch::inmem_manifest *m;
        const mh::patch::apply_result   *r;
    };
    // The two relocation layers ride along: they are written at boot and stay, and they own the wildcard bytes of the limit sites (a masked-off
    // rel32 / abs32 operand inside the instruction a limit site rewrites), so the checker's expected image is the clean exe plus these.
    const row rows[] = {{"reloc", mh::patch::CAPS_RELOC, &g_reloc_r},
                        {"units", mh::patch::CAPS_UNITS_RELOC, &g_ureloc_r},
                        {"limits", mh::patch::CAPS_LIMITS, &g_limits_r},
                        {"units.limits", mh::patch::CAPS_UNITS_LIMITS, &g_ulimits_r},
                        {"ai", mh::patch::CAPS_AI, &g_ai_r},
                        {"orders", mh::patch::CAPS_ORDERS, &g_orders_r}};
    int       wrote  = 0;
    for (const row &w : rows) {
        if (!w.r->applied) continue;
        char path[MAX_PATH + 64];
        _snprintf_s(path, sizeof(path), _TRUNCATE, "%s.%s", base, w.ext);
        wrote += mh::patch::write_parity_dump(*w.m, *w.r, path) ? 1 : 0;
    }
    char b[MAX_PATH + 160];
    _snprintf_s(b, sizeof(b), _TRUNCATE, "; [caps] post-match dump: %d toggle layer(s) -> %s.* (limits %s)\n", wrote, base,
                mh::caps::raised() ? "ON" : "OFF");
    seam_log(b);
}

extern "C" void MH_Caps_DumpIfAsked(const char *ini_path) {
    if (!ini_path || !g_attempted) return;
    // With `[patch] inmem=1` the in-memory patch spike owns [patch] dump / dump_exit.
    if (mh_ini_get_int("patch", "inmem", 0, ini_path)) return;
    char dump[MAX_PATH] = {};
    mh::config::read_ini_string("patch", "dump", "", dump, sizeof(dump), ini_path);
    if (!dump[0]) return;
    char b[MAX_PATH * 2 + 120];
    bool ok = mh::patch::write_parity_dump(*mh::patch::CAPS_RELOC, g_reloc_r, dump);
    char dump2[MAX_PATH + 8];
    _snprintf_s(dump2, sizeof(dump2), _TRUNCATE, "%s.limits", dump);
    ok = mh::patch::write_parity_dump(*mh::patch::CAPS_LIMITS, g_limits_r, dump2) && ok;
    if (g_ureloc_r.applied) {
        char dump3[MAX_PATH + 16];
        _snprintf_s(dump3, sizeof(dump3), _TRUNCATE, "%s.units", dump);
        ok = mh::patch::write_parity_dump(*mh::patch::CAPS_UNITS_RELOC, g_ureloc_r, dump3) && ok;
        _snprintf_s(dump3, sizeof(dump3), _TRUNCATE, "%s.units.limits", dump);
        ok = mh::patch::write_parity_dump(*mh::patch::CAPS_UNITS_LIMITS, g_ulimits_r, dump3) && ok;
    }
    if (g_ai_r.applied) { // PT-CAP11: the AI scratch layer, as `<path>.ai` (absent when `[caps] ai_scratch=0`)
        char dump3[MAX_PATH + 8];
        _snprintf_s(dump3, sizeof(dump3), _TRUNCATE, "%s.ai", dump);
        ok = mh::patch::write_parity_dump(*mh::patch::CAPS_AI, g_ai_r, dump3) && ok;
    }
    if (g_orders_r.applied) { // PT-CAP9-U5: the order layer, as `<path>.orders` (absent when `[caps] orders_layer=0`)
        char dump3[MAX_PATH + 12];
        _snprintf_s(dump3, sizeof(dump3), _TRUNCATE, "%s.orders", dump);
        ok = mh::patch::write_parity_dump(*mh::patch::CAPS_ORDERS, g_orders_r, dump3) && ok;
    }
    _snprintf_s(b, sizeof(b), _TRUNCATE, "; [caps] parity dump %s -> %s (+ .limits; limits %s)\n", ok ? "written" : "FAILED", dump,
                mh::caps::raised() ? "ON" : "OFF");
    seam_log(b);
    if (mh_ini_get_int("patch", "dump_exit", 0, ini_path)) {
        seam_log("; [caps] [patch] dump_exit=1 -- terminating after the dump\n");
        mh_logq_flush(1000);
        TerminateProcess(GetCurrentProcess(), 0);
    }
}
