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
#include "include/mh_run_context.h"  // mh_run_path
#include "include/mh_config_dir.h"   // RL3: <config dir>mh_net.ini
#include "include/mh_log_sink.h"     // LOG1: async log sink
#include "config/ini_read.h"         // read_ini_string -- strips a trailing `;comment`
#include "hook/patch.h"              // patch_bytes_guarded
#include "en_guard.h"                // EN-only build gate
#include "mh_net_proto/net_crypto.h" // exact selected INITLANG fingerprints

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
UINT g_pack_cp         = 0;  // RL17: lang\<id>\pack.ini [pack] codepage, 0 = none
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
    mh_logq_write(g_log, line, lstrlenA(line));
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
    exe_dir(dir, MAX_PATH);    // the lang\ packs belong to the INSTALLATION: they stay beside the exe
    mh::cfgdir::ini_path(ini); // RL3: the ini is the player's -- <config dir>mh_net.ini

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

    // RL17: the pack's own codepage (pack.ini), applied by ui_chat_input when [input] is unpinned.
    char pini[MAX_PATH];
    wsprintfA(pini, "%s\\%s", pack_dir, "pack.ini");
    if (file_exists(pini)) {
        const UINT cp = GetPrivateProfileIntA("pack", "codepage", 0, pini);
        if (cp > 0 && cp < 65000u && IsValidCodePage(cp)) g_pack_cp = cp;
    }

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
    lp_log("; [lang] pack=%s armed: mh_ex -> %s (%s path); Msgs.dat %s; pack codepage %u", g_id, g_mh_ex,
           rel ? "relative" : "absolute", msgs_state, g_pack_cp);
    return 1;
}

extern "C" const char *MH_LangPack_Locale(const unsigned char *initlang, unsigned size) {
    static char locale[39] = {};
    static bool read       = false;
    if (read) return locale;
    read = true;
    char ini[MAX_PATH];
    mh::cfgdir::ini_path(ini);
    DWORD n = mh::config::read_ini_string("lang", "locale", "", locale, sizeof(locale), ini);
    if (!n && g_id[0]) {
        char dir[MAX_PATH], pack_ini[MAX_PATH];
        exe_dir(dir, MAX_PATH);
        if (lstrlenA(dir) + lstrlenA(g_id) + sizeof("lang\\\\pack.ini") >= MAX_PATH) return locale;
        wsprintfA(pack_ini, "%slang\\%s\\pack.ini", dir, g_id);
        // External pack.ini metadata, not mh_net.ini: match the neighboring pack
        // codepage reader. Player-owned [lang] locale above still uses the ship gate.
        n = GetPrivateProfileStringA("pack", "locale", "", locale, sizeof(locale), pack_ini);
        if (n >= sizeof(locale) - 1) {
            locale[0] = 0;
            return locale;
        }
        if (mh::config::strip_ini_comment(locale)) n = static_cast<DWORD>(lstrlenA(locale));
    }
    if (n > 37) {
        locale[0] = 0;
        return locale;
    }
    for (char *p = locale; *p; ++p) {
        if (*p >= 'A' && *p <= 'Z') *p = char(*p + ('a' - 'A'));
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_')) {
            locale[0] = 0;
            return locale;
        }
    }
    if (!locale[0] && initlang && size) {
        unsigned char digest[32];
        mh_net_proto::sha256(initlang, size, digest);
        // Exact bytes of original English INITLANG, including its UTF-16 BOM (220070 bytes).
        static constexpr unsigned char stock_en[32] = {0x63, 0xd7, 0x8c, 0x96, 0xba, 0xf8, 0x5b, 0xa8, 0x66, 0x5b, 0x2f, 0x59, 0x25, 0x1b, 0x1e, 0x80, 0x34, 0x1a, 0x5e, 0x50, 0xfb, 0xad, 0x84, 0x15, 0x5d, 0x13, 0xc0, 0xb7, 0xd6, 0x6a, 0x98, 0x09};
        // Existing built RU (including ru_noart) and Polish packs, exact decoded bytes.
        static constexpr unsigned char merged_ru[32] = {0xf4, 0xf9, 0x9f, 0x55, 0xde, 0xee, 0x9d, 0x4a, 0xbf, 0x21, 0xd6, 0xac, 0x10, 0x32, 0x12, 0xe3, 0x1d, 0xe5, 0xc0, 0xfa, 0x30, 0x5f, 0x92, 0x77, 0x76, 0x8c, 0x35, 0x7d, 0x0e, 0x0d, 0x5f, 0x33};
        static constexpr unsigned char merged_pl[32] = {0xd1, 0x89, 0x17, 0xda, 0x76, 0xf9, 0x94, 0x03, 0xb5, 0x0b, 0x5a, 0xa3, 0xe0, 0xf8, 0xe8, 0x4e, 0x1f, 0x34, 0x54, 0x75, 0x8c, 0xfd, 0x12, 0xac, 0xce, 0x48, 0x9b, 0xf3, 0x49, 0x03, 0xfc, 0xc7};
        if (size == 220070 && std::memcmp(digest, stock_en, 32) == 0)
            lstrcpynA(locale, "en", sizeof(locale));
        else if (size == 221810 && std::memcmp(digest, merged_ru, 32) == 0)
            lstrcpynA(locale, "ru", sizeof(locale));
        else if (size == 92436 && std::memcmp(digest, merged_pl, 32) == 0)
            lstrcpynA(locale, "pl", sizeof(locale));
    }
    return locale;
}

extern "C" unsigned MH_LangPack_Codepage(void) { return g_pack_cp; }

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
