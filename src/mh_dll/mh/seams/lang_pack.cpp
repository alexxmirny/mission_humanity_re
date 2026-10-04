//
// seams/lang_pack.cpp -- mods:LANG1: `[lang] pack=<id>` resolves mh_ex (+ Msgs.dat) to
// lang\<id>\ next to the exe; and the LANG2 7-button-art probe mp_menu.cpp asks before it restyles.
//
// Mechanism, the two patched operands and why the path is relative: include/mh_langpack_export.h.
//
#include <windows.h>
#include <cstdarg>
#include <cstdint>
#include <cstring>

#include "include/mh_langpack_export.h"
#include "include/mh_run_context.h" // mh_run_path
#include "config/ini_read.h"        // read_ini_string -- strips a trailing `;comment`
#include "hook/patch.h"             // patch_bytes_guarded
#include "en_guard.h"               // EN-only build gate

#pragma comment(lib, "user32.lib") // wsprintfA / wvsprintfA

namespace {

// ---- the two operands (EN VAs; ReVA-read 2026-09-29, raw VAs on the gfx_font_guard precedent) --
//
// llm_boot_progress_draw 0x004c3c53:  0x004c3dba  B8 E2 31 50 00   MOV EAX, s_mh_ex_005031e2
//                                     0x004c3dbf  E8 9F F7 FF FF   CALL rsr_TryReadRsrFile
// ReadMsgsDat            0x004cb69f:  0x004cb6bc  B8 CF 34 50 00   MOV EAX, s_Msgs_dat_005034cf
//                                     0x004cb6c1  E8 FE 3F 00 00   CALL utils_open_file
// Each is the ONLY reference to its string (find-cross-references), so repointing the operand is
// exactly "open a different file" and nothing else.
constexpr uintptr_t ADDR_MH_EX_MOV  = 0x004c3dbau;
constexpr uintptr_t ADDR_MSGS_MOV   = 0x004cb6bcu;
constexpr uint8_t   MH_EX_EXPECT[5] = {0xB8, 0xE2, 0x31, 0x50, 0x00};
constexpr uint8_t   MSGS_EXPECT[5]  = {0xB8, 0xCF, 0x34, 0x50, 0x00};

// rsr_ReadRsrFile formats "%s.nam" / "%s.rsr" into a 128-byte stack buffer (local_b0), and
// rsr_TryReadRsrFile first tries "<cd>:\<SrcPath>\%s" in a 260-byte one. A pack path must leave
// room for the extension: 128 - ".nam" - NUL.
constexpr int RSR_NAME_MAX = 128 - 5;

char g_id[17]          = {0}; // the armed pack id, "" = stock
char g_mh_ex[MAX_PATH] = {0}; // what rsr_TryReadRsrFile gets instead of "mh_ex"
char g_msgs[MAX_PATH]  = {0}; // what utils_open_file gets instead of "Msgs.dat"
bool g_done            = false;
int  g_art7            = -2; // MH_LangPack_MenuHas7ButtonArt cache; -2 = not probed yet

char          g_log[MAX_PATH];
unsigned long g_log_gen = 0;

void lp_log(const char *fmt, ...) {
    mh_run_path(g_log, MAX_PATH, "%smh_video.log", &g_log_gen);
    char    line[400];
    va_list ap;
    va_start(ap, fmt);
    wvsprintfA(line, fmt, ap);
    va_end(ap);
    int n = lstrlenA(line);
    if (n < (int)sizeof(line) - 2) {
        line[n++] = '\n';
        line[n]   = 0;
    }
    HANDLE h = CreateFileA(g_log, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    SetFilePointer(h, 0, nullptr, FILE_END);
    DWORD wrote = 0;
    WriteFile(h, line, lstrlenA(line), &wrote, nullptr);
    CloseHandle(h);
}

// "<exe dir>\" (with the trailing separator).
void exe_dir(char *out, int cap) {
    GetModuleFileNameA(nullptr, out, cap);
    char *s = out;
    for (char *p = out; *p; ++p)
        if (*p == '\\' || *p == '/') s = p;
    s[1] = 0;
}

bool file_exists(const char *p) {
    const DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool dir_exists(const char *p) {
    const DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// An id is a folder NAME, never a path: [A-Za-z0-9_-], 1..16 chars.
bool id_ok(const char *id) {
    int n = 0;
    for (const char *p = id; *p; ++p, ++n) {
        const char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
              c == '-'))
            return false;
    }
    return n >= 1 && n <= 16;
}

// Is the working directory the exe's directory? The stock bare name "mh_ex" already depends on it,
// so a relative pack path is exactly as safe as retail -- and immune to a long install path.
bool cwd_is_exe_dir(const char *dir) {
    char        cwd[MAX_PATH];
    const DWORD n = GetCurrentDirectoryA(MAX_PATH, cwd);
    if (n == 0 || n >= MAX_PATH) return false;
    char a[MAX_PATH], b[MAX_PATH];
    lstrcpynA(a, cwd, MAX_PATH);
    lstrcpynA(b, dir, MAX_PATH);
    int la = lstrlenA(a), lb = lstrlenA(b);
    while (la > 0 && (a[la - 1] == '\\' || a[la - 1] == '/')) a[--la] = 0;
    while (lb > 0 && (b[lb - 1] == '\\' || b[lb - 1] == '/')) b[--lb] = 0;
    return lstrcmpiA(a, b) == 0;
}

// The replacement `mov eax, imm32` for a DLL-owned string. The two patch_bytes_guarded call sites
// below name their address constants directly (tools/lint_dll_patches.py resolves each call site's
// address expression against tools/data/dll_patch_manifest.json -- rows lang_pack_mh_ex/_msgs).
void mov_eax_imm(uint8_t out[5], const char *to) {
    const uintptr_t v = (uintptr_t)to;
    out[0]            = 0xB8;
    out[1]            = (uint8_t)(v & 0xff);
    out[2]            = (uint8_t)((v >> 8) & 0xff);
    out[3]            = (uint8_t)((v >> 16) & 0xff);
    out[4]            = (uint8_t)((v >> 24) & 0xff);
}

// ---- LANG2: the .nam/.rsr read the art probe needs -----------------------------------------------
//
// A .nam is an array of 64-byte entries {char name[47]; char type[5]; u32 offset, size, final_size}
// (src/formats/unpack.py). Names are compared case-insensitively ("menu\MenuBck2.gfx" in RU,
// "menu\MENUBCK2.GFX" in EN), which is what rsr_ReadRsrFile's ToLower makes the game do too.
struct nam_entry {
    char     name[47];
    char     type[5];
    uint32_t offset, size, final_size;
};
static_assert(sizeof(nam_entry) == 64, "rsr file_entry is 64 bytes");

bool find_entry(const char *nam_path, const char *name, nam_entry *out) {
    HANDLE h = CreateFileA(nam_path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool      hit = false;
    nam_entry e;
    DWORD     got = 0;
    while (ReadFile(h, &e, sizeof(e), &got, nullptr) && got == sizeof(e)) {
        char n[48];
        lstrcpynA(n, e.name, sizeof(n));
        if (lstrcmpiA(n, name) == 0) {
            *out = e;
            hit  = true;
            break;
        }
    }
    CloseHandle(h);
    return hit;
}

// Byte-compare two members' STORED payloads in one .rsr. Equal stored bytes = equal art.
int same_payload(const char *rsr_path, const nam_entry &a, const nam_entry &b) {
    if (a.size != b.size || a.final_size != b.final_size) return 0;
    if (a.offset == b.offset) return 1;
    HANDLE h = CreateFileA(rsr_path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return -1;
    int     verdict = 1;
    uint8_t ba[4096], bb[4096];
    for (uint32_t done = 0; done < a.size && verdict == 1;) {
        const DWORD want = (a.size - done) < sizeof(ba) ? (a.size - done) : (DWORD)sizeof(ba);
        DWORD       ga = 0, gb = 0;
        SetFilePointer(h, (LONG)(a.offset + done), nullptr, FILE_BEGIN);
        if (!ReadFile(h, ba, want, &ga, nullptr) || ga != want) verdict = -1;
        SetFilePointer(h, (LONG)(b.offset + done), nullptr, FILE_BEGIN);
        if (verdict == 1 && (!ReadFile(h, bb, want, &gb, nullptr) || gb != want)) verdict = -1;
        if (verdict == 1 && memcmp(ba, bb, want) != 0) verdict = 0;
        done += want;
    }
    CloseHandle(h);
    return verdict == 0 ? 2 : verdict; // 2 = sizes equal, bytes differ
}

} // namespace

extern "C" const char *MH_LangPack_Id(void) { return g_id; }

extern "C" int MH_LangPack_Install(void) {
    if (g_done) return g_id[0] ? 1 : 0;
    g_done = true;
    if (!mh::en_build_ok()) return 0; // EN-only: both operands are EN VAs

    char dir[MAX_PATH], ini[MAX_PATH];
    exe_dir(dir, MAX_PATH);
    wsprintfA(ini, "%smh_net.ini", dir);

    char id[64];
    mh::config::read_ini_string("lang", "pack", "", id, sizeof(id), ini);
    // STOCK: say nothing at all. An unset key is every existing install, and the advisory channel
    // gains no line -- the stock run is the stock run.
    if (!id[0] || lstrcmpiA(id, "en") == 0) return 0;

    if (!id_ok(id)) {
        lp_log("; [lang] pack=%s REFUSED: a pack id is a folder name ([A-Za-z0-9_-], 1..16 chars) -- "
               "using the stock mh_ex",
               id);
        return 0;
    }
    char pack_dir[MAX_PATH], nam[MAX_PATH], rsr[MAX_PATH], msgs[MAX_PATH];
    wsprintfA(pack_dir, "%slang\\%s", dir, id);
    wsprintfA(nam, "%s\\mh_ex.nam", pack_dir);
    wsprintfA(rsr, "%s\\mh_ex.rsr", pack_dir);
    wsprintfA(msgs, "%s\\Msgs.dat", pack_dir);
    if (!dir_exists(pack_dir) || !file_exists(nam) || !file_exists(rsr)) {
        // The pair or nothing: a .rsr without its .nam index is the Insert-CD modal (make_lane.py's
        // matched-pair note), so a half pack is treated exactly like no pack.
        lp_log("; [lang] pack=%s MISSING: %s (%s) -- falling back to the stock mh_ex", id, pack_dir,
               !dir_exists(pack_dir) ? "no such folder"
                                     : (!file_exists(nam) ? "mh_ex.nam absent" : "mh_ex.rsr absent"));
        return 0;
    }

    // Relative when the working directory is the exe's (the stock name's own assumption), else
    // absolute if it fits rsr_ReadRsrFile's 128-byte buffer, else refuse rather than overflow it.
    const bool rel = cwd_is_exe_dir(dir);
    if (rel)
        wsprintfA(g_mh_ex, "lang\\%s\\mh_ex", id);
    else
        wsprintfA(g_mh_ex, "%s\\mh_ex", pack_dir);
    if (lstrlenA(g_mh_ex) > RSR_NAME_MAX) {
        lp_log("; [lang] pack=%s REFUSED: the working directory is not the exe's and %s is %d chars "
               "(the game's pack-name buffer holds %d) -- using the stock mh_ex",
               id, g_mh_ex, lstrlenA(g_mh_ex), RSR_NAME_MAX);
        g_mh_ex[0] = 0;
        return 0;
    }
    uint8_t repl[5];
    mov_eax_imm(repl, g_mh_ex);
    if (!mh::hook::patch_bytes_guarded(ADDR_MH_EX_MOV, MH_EX_EXPECT, repl, 5)) {
        lp_log("; [lang] pack=%s NOT armed: the mh_ex operand at 0x%08x is not the expected bytes "
               "(already patched, or not the stock EN exe) -- the stock mh_ex loads",
               id, (unsigned)ADDR_MH_EX_MOV);
        g_mh_ex[0] = 0;
        return 0;
    }
    lstrcpynA(g_id, id, sizeof(g_id));

    const char *msgs_state = "absent -- the stock Msgs.dat path is kept";
    if (file_exists(msgs)) {
        if (rel)
            wsprintfA(g_msgs, "lang\\%s\\Msgs.dat", id);
        else
            lstrcpynA(g_msgs, msgs, MAX_PATH);
        mov_eax_imm(repl, g_msgs);
        msgs_state = mh::hook::patch_bytes_guarded(ADDR_MSGS_MOV, MSGS_EXPECT, repl, 5)
                         ? "redirected"
                         : "NOT redirected (operand bytes differ) -- the stock path is kept";
    }
    lp_log("; [lang] pack=%s armed: mh_ex -> %s (%s path); Msgs.dat %s", g_id, g_mh_ex,
           rel ? "relative" : "absolute", msgs_state);
    return 1;
}

extern "C" int MH_LangPack_MenuHas7ButtonArt(void) {
    if (g_art7 != -2) return g_art7;
    char dir[MAX_PATH], nam[MAX_PATH], rsr[MAX_PATH];
    exe_dir(dir, MAX_PATH);
    if (g_id[0]) {
        wsprintfA(nam, "%slang\\%s\\mh_ex.nam", dir, g_id);
        wsprintfA(rsr, "%slang\\%s\\mh_ex.rsr", dir, g_id);
    } else {
        wsprintfA(nam, "%smh_ex.nam", dir);
        wsprintfA(rsr, "%smh_ex.rsr", dir);
    }
    nam_entry b1, b2;
    if (!file_exists(nam) || !file_exists(rsr)) {
        g_art7 = -1; // cannot tell (e.g. a CD-only layout): the caller keeps its stock behaviour
    } else if (!find_entry(nam, "menu\\menubck1.gfx", &b1) || !find_entry(nam, "menu\\menubck2.gfx", &b2)) {
        g_art7 = find_entry(nam, "menu\\menubck2.gfx", &b2) ? 1 : 0; // no MENUBCK2 at all = no 7-button art
    } else {
        const int s = same_payload(rsr, b1, b2);
        g_art7      = s == -1 ? -1 : (s == 1 ? 0 : 1);
    }
    // Only a NON-stock answer is logged: the stock EN pack's art is the 7-button art, and a stock
    // run keeps its advisory channel exactly as it was.
    if (g_id[0] || g_art7 != 1)
        lp_log("; [lang] menu art (%s): %s", g_id[0] ? g_id : "stock mh_ex",
               g_art7 == 1 ? "MENUBCK2 is a real 7-button background"
                           : (g_art7 == 0 ? "MENUBCK2 == MENUBCK1 (6-button art only) -- the network "
                                            "button falls back to a self-rendered label"
                                          : "unreadable -- stock behaviour"));
    return g_art7;
}
