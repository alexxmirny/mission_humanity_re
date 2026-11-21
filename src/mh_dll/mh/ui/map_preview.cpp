//
// mh/ui -- the right panel's PAGE SPINNER and the map preview (mods:MAPGEN2).
//
// WHAT THE PLAYER SEES. The right panel's description/status box (widget 0x006503b3) gets pages, switched by a
// "View: <page>" spinner under Cancel (the strip where the lobby's Mode selector used to sit):
//
//   map picker  (Create game):  Description | Preview                       default Preview
//   lobby       (host + client): Description/status | Preview | Settings    default Description
//
// Page 1 is today's box, untouched. Page 2 is a 128x128 thumbnail of the selected map (picker: the highlighted
// row; lobby: the host's map, on a client the file the download stored). Page 3 (lobby only) holds the U51
// Mode selector (FFA / Team): the host edits it, a client reads it under the caption "Game settings (set by the
// host)". The page is LOCAL: one int per screen for the life of the process, never saved, never sent, and no slot
// record or dirty bit is touched (Mode itself still rides host slot 0 +0x07 exactly as before).
//
// HOW. Everything is the DLL's own widgets plus one draw hook, in the three patterns lobby_widgets.cpp and
// lobby_notice.cpp already proved:
//   * the box grows 10 px (a guarded poke of widget 0x006503b3's height, 120 -> 130) so a 128 px thumbnail fits
//     exactly. Every widget below it FLOWS (resolve_position's 0x800000 chain) and moves with it, which is also
//     why the two MP browsers -- they share the box -- move together with the picker and the lobby;
//   * the pager is a clone of the lobby's 12th static child (0x0065054b, the hidden spare Mode was cloned from),
//     appended after it in a DLL-owned children array (picker: a copy of the retail array at 0x0065342f, the
//     container's +0 repointed; lobby: lobby_widgets.cpp's g_children), so it lands exactly where Mode used to be
//     and follows the slide-in through the same flow chain;
//   * the box's draw_cb (lobby_notice.cpp's status_draw) asks this file what to draw. Page 1 -> the original
//     text path; page 2 -> the empty box + the thumbnail blitted straight into the RGB565 frame (clipped);
//     page 3 -> the box with the Settings caption. Browsers, and everything when [ui] map_preview=0, are NEVER
//     asked to change: screen() is neither the picker nor the lobby.
//   * the Mode widget (lobby_widgets.cpp) is chained after the pager with keep-prev flags; the pager's draw_cb
//     writes Mode's x/y/width every frame from the box rect it just saw, so Mode sits inside the box on page 3
//     wherever the flow put the pager, and (hidden + disabled) is neither drawn nor clickable on pages 1-2.
//
// WORKS WITH libmh.dll ABSENT. Nothing here touches the sim, the wire or libmh: the file reads are plain Win32,
// the colour table is the game's own TLO through GetResourseFilePtr, the renderer is mh_mapgen's pure code.
//
// Determinism-neutral: menu pixels and widget fields only. ONE log line per rebuild (mh_net.log):
//   ; [mappreview] <file> <w>x<h> tlo=<tileset> spots=N ore=M trees=T build_us=U (tlo cached|loaded <us> us)
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <vector>
#include <string>

#include "ui/ui_internal.h"
#include "ui/player_strings.h"
#include "addr/mh_addrs.gen.h"
#include "addr/mh_calls.gen.h"
#include "mh_ini_gate.h"
#include "en_guard.h"
#include "mh_mapgen/mpm.h"
#include "mh_mapgen/preview.h"
#include "mh_mod_catalog.h"
#include <utility>
#include "seams/map_transfer.h"

#pragma comment(lib, "user32.lib")

using mh::ui::detail::ui_log;

namespace {

// ---- addresses (UI-only raw VAs: a manifest data entry would renumber the hash RIDs) ---------------------
constexpr uintptr_t ADDR_STATUS_WIDGET   = 0x006503b3u; // the description/status box
constexpr uintptr_t ADDR_SPARE_WIDGET    = 0x0065054bu; // the lobby's hidden 12th static child (Mode's template)
constexpr uintptr_t ADDR_PICKER_CHILDREN = 0x0065342fu; // retail children array of the map picker
constexpr uintptr_t ADDR_PICKER_LIST_PB  = 0x00644fdbu; // the picker list widget's param block
constexpr uintptr_t ADDR_PICKER_DIR      = 0x006567a6u; // the directory the picker lists ("Maps\")
constexpr uintptr_t ADDR_FONT_H          = 0x0065426bu;

constexpr int      STATUS_H_STOCK = 120, STATUS_H_NEW = 130; // see the header: 128 px + a 1 px margin each side
constexpr unsigned W_STRIDE = 0x44, SP_STRIDE = 0x14;
constexpr unsigned WO_ACTION = 0x04, WO_FLAGS = 0x08, WO_DRAW = 0x0c, WO_X = 0x1c, WO_Y = 0x20, WO_W = 0x24, WO_H = 0x28,
                   WO_SPIN = 0x30, WO_LABEL = 0x38;
constexpr int32_t DRAW_CONTENT = 0x004c1b15;

enum Screen { SCR_OTHER  = 0,
              SCR_PICKER = 1,
              SCR_LOBBY  = 2 };
enum Page { PAGE_DESC     = 0,
            PAGE_PREVIEW  = 1,
            PAGE_SETTINGS = 2 };

bool     g_on         = false;
int      g_page_pick  = PAGE_PREVIEW; // per screen, process lifetime only
int      g_page_lobby = PAGE_DESC;
uint32_t g_notice_seq = 0, g_seen_seq = 0;

// the box's resolved rect, recorded by its own draw (which runs before the pager's in both children arrays)
struct Rect {
    int  x = 0, y = 0, w = 0, h = 0;
    bool valid = false;
} g_box;

int (*g_resolver)(const char *base, char *out, int cap) = nullptr;

inline int32_t &wf(uintptr_t w, unsigned off) {
    return *(int32_t *)(w + off);
}

int screen() {
    const void *l = *(void **)mh::addr::_G_LLM_UI_MENU_WIDGET_LIST;
    if (l == (const void *)mh::addr::map_picker_widget_origin) return SCR_PICKER;
    if (l == (const void *)mh::addr::lobby_widget_origin) return SCR_LOBBY;
    return SCR_OTHER;
}

int &page_of(int scr) {
    return scr == SCR_PICKER ? g_page_pick : g_page_lobby;
}

int page_count(int scr) {
    return scr == SCR_PICKER ? 2 : 3;
}

// ---- the pager widgets -----------------------------------------------------------------------------------
alignas(4) uint8_t g_pager_pick_w[W_STRIDE];
alignas(4) uint8_t g_pager_pick_sp[SP_STRIDE];
alignas(4) uint8_t g_pager_lobby_w[W_STRIDE];
alignas(4) uint8_t g_pager_lobby_sp[SP_STRIDE];
const wchar_t  *g_page_txt[3];
const wchar_t **g_page_cells[3]; // cell[v] -> &g_page_txt[v]
uintptr_t       g_pick_children[16];

void init_page_tables() {
    using mh::ui::Str;
    g_page_txt[0] = mh::ui::tr(Str::PAGE_DESCRIPTION);
    g_page_txt[1] = mh::ui::tr(Str::PAGE_PREVIEW);
    g_page_txt[2] = mh::ui::tr(Str::PAGE_SETTINGS);
    for (int i = 0; i < 3; ++i) g_page_cells[i] = &g_page_txt[i];
}

void init_pager(uint8_t *w, uint8_t *sp, int pages) {
    const int font_h = *(const int *)ADDR_FONT_H;
    memset(w, 0, W_STRIDE);
    memset(sp, 0, SP_STRIDE);
    wf((uintptr_t)w, WO_X)     = wf(ADDR_SPARE_WIDGET, WO_X);
    wf((uintptr_t)w, WO_Y)     = wf(ADDR_SPARE_WIDGET, WO_Y);
    wf((uintptr_t)w, WO_W)     = wf(ADDR_SPARE_WIDGET, WO_W);
    wf((uintptr_t)w, WO_H)     = font_h;
    wf((uintptr_t)w, WO_FLAGS) = (wf(ADDR_SPARE_WIDGET, WO_FLAGS) & ~0xc0) | 0x2; // enabled, visible, takes input
    wf((uintptr_t)w, WO_SPIN)  = (int32_t)(uintptr_t)sp;
    wf((uintptr_t)sp, 0)       = (int32_t)(uintptr_t)g_page_cells;
    wf((uintptr_t)sp, 4)       = pages;
    wf((uintptr_t)sp, 8)       = 0;
    wf((uintptr_t)sp, 0xc)     = 0;
}

void page_cb_impl() {
    const int scr = screen();
    if (scr == SCR_OTHER) return;
    int &p = page_of(scr);
    p      = (p + 1) % page_count(scr);
    if (scr == SCR_LOBBY) mh::ui::lobby_mode_sync_flags(); // Mode (un)hides this frame, not at the next lobby tick
}

// Watcom __watcall action callbacks, no args, returning 1 in EAX.
// clang-format off
__declspec(naked) int page_cb_detour() {
    __asm {
        pushad
        pushfd
        cld
        call page_cb_impl
        popfd
        popad
        mov  eax, 1
        ret
    }
}
// clang-format on

// ---- the preview ------------------------------------------------------------------------------------------
struct Tileset {
    std::string                name;
    const mh::mods::Snapshot  *snapshot = nullptr;
    std::vector<std::uint16_t> tiles;
    unsigned long long         used = 0;
};
// Bounded LRU: missing resources are never cached. Mounted snapshots are immutable.
Tileset            g_tilesets[32];
unsigned long long g_tileset_clock = 0;

enum { PV_NONE = 0,
       PV_READY,
       PV_FAILED };
struct Cache {
    char                      path[MAX_PATH + 8];
    int                       status;
    DWORD                     retry_at;
    const mh::mods::Snapshot *snapshot;
    bool                      retry_resource;
    unsigned long long        sig; // size ^ mtime of the file last tried: a retry only re-reads a file that changed
    std::uint16_t             px[mh_mapgen::PREVIEW_SIZE * mh_mapgen::PREVIEW_SIZE];
    mh_mapgen::PreviewStats   st;
} g_pv;

double qpc_us() {
    static LARGE_INTEGER f = {};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER n;
    QueryPerformanceCounter(&n);
    return double(n.QuadPart) * 1e6 / double(f.QuadPart);
}

// The cache identity includes the immutable mounted content, not just its filename.
const std::vector<std::uint16_t> *tileset_table(const std::string &name, double *loaded_us) {
    *loaded_us           = -1;
    const auto *snapshot = mh::mods::mounted_snapshot();
    Tileset    *slot     = &g_tilesets[0];
    for (Tileset &t : g_tilesets) {
        if (t.snapshot == snapshot && !t.name.empty() && lstrcmpiA(t.name.c_str(), name.c_str()) == 0) {
            t.used = ++g_tileset_clock;
            return &t.tiles;
        }
        if (t.used < slot->used) slot = &t;
    }
    const double               t0 = qpc_us();
    std::vector<std::uint16_t> tiles;
    std::string                key, error;
    const mh::mods::Payload   *winner = nullptr;
    if (snapshot && mh::mods::canonical_resource_name(name.c_str(), key, error)) winner = snapshot->winner(key);
    if (winner) {
        mh_mapgen::tlo_tile_means(winner->bytes.data(), winner->bytes.size(), tiles);
    } else {
        char nm[64];
        lstrcpynA(nm, name.c_str(), sizeof(nm));
        void          *rsr   = nullptr;
        const uint8_t *entry = (const uint8_t *)mh::call::rsr_GetFileEntry(nm, &rsr);
        const uint32_t size  = entry ? *(const uint32_t *)(entry + 60) : 0;
        if (size >= 0x200 + 1024 && size <= (64u << 20)) {
            uint8_t *buf = mh::call::GetResourseFilePtr(nm);
            if (buf) {
                mh_mapgen::tlo_tile_means(buf, size, tiles);
                mh::call::utils_free(buf);
            }
        }
    }
    *loaded_us = qpc_us() - t0;
    if (tiles.empty()) return nullptr;
    slot->name     = name;
    slot->snapshot = snapshot;
    slot->tiles    = std::move(tiles);
    slot->used     = ++g_tileset_clock;
    return &slot->tiles;
}

bool read_whole(const char *path, std::vector<std::uint8_t> &out) {
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz = {};
    bool          ok = GetFileSizeEx(h, &sz) && sz.QuadPart > 0 && sz.QuadPart <= (8 << 20);
    if (ok) {
        out.resize((size_t)sz.QuadPart);
        DWORD got = 0;
        ok        = ReadFile(h, out.data(), (DWORD)out.size(), &got, nullptr) && got == out.size();
    }
    CloseHandle(h);
    return ok;
}

const char *base_name(const char *p) {
    const char *b = p;
    for (; *p; ++p)
        if (*p == '\\' || *p == '/') b = p + 1;
    return b;
}

unsigned long long file_sig(const char *path) {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fa)) return 0;
    return ((unsigned long long)fa.nFileSizeLow << 32 | fa.ftLastWriteTime.dwLowDateTime) ^ fa.ftLastWriteTime.dwHighDateTime;
}

void rebuild(const char *path, bool retry) {
    const double t0 = qpc_us();
    lstrcpynA(g_pv.path, path, sizeof(g_pv.path));
    g_pv.status         = PV_FAILED;
    g_pv.snapshot       = mh::mods::mounted_snapshot();
    g_pv.retry_resource = false;
    g_pv.sig            = file_sig(path);
    std::vector<std::uint8_t> bytes;
    mh_mapgen::MpmModel       model;
    std::string               err;
    char                      line[400];
    if (!read_whole(path, bytes) || !mh_mapgen::read_mpm(bytes.data(), bytes.size(), model, err)) {
        wsprintfA(line, "; [mappreview] %s: no preview (%s)\n", base_name(path), bytes.empty() ? "unreadable" : "not a map");
        if (!retry) ui_log(line);
        return;
    }
    if (!mh::seams::maps::validate_runtime_map(bytes.data(), bytes.size(), false, err, &g_pv.retry_resource)) {
        _snprintf_s(line, sizeof(line), _TRUNCATE, "; [mappreview] %.100s: refused (%.200s)\n",
                    base_name(path), err.c_str());
        if (!retry) ui_log(line);
        return;
    }
    double                            tlo_us = -1;
    const std::string                 tn     = model.tileset();
    const std::vector<std::uint16_t> *table  = tileset_table(tn, &tlo_us);
    if (!table) {
        g_pv.retry_resource = true;
        wsprintfA(line, "; [mappreview] %s: no preview (tileset %s is not in the resource packs)\n", base_name(path), tn.c_str());
        if (!retry) ui_log(line);
        return;
    }
    if (!mh_mapgen::render_preview(model, table->data(), table->size(), mh_mapgen::PREVIEW_SIZE, g_pv.px, &g_pv.st)) {
        wsprintfA(line, "; [mappreview] %s: no preview (render refused)\n", base_name(path));
        if (!retry) ui_log(line);
        return;
    }
    g_pv.status = PV_READY;
    char tl[40];
    if (tlo_us < 0) lstrcpyA(tl, "cached");
    else wsprintfA(tl, "loaded %d us", (int)tlo_us);
    wsprintfA(line, "; [mappreview] %s %dx%d tlo=%s spots=%d ore=%d trees=%d build_us=%d (tlo %s)\n", base_name(path), g_pv.st.w, g_pv.st.h,
              tn.c_str(), g_pv.st.spots, g_pv.st.ore_blocks, g_pv.st.trees, (int)(qpc_us() - t0), tl);
    ui_log(line);
}

enum Src { SRC_NONE,
           SRC_PATH,
           SRC_WAITING };

// What file does the CURRENT screen preview?
Src source(int scr, char *out, int cap) {
    out[0] = 0;
    if (scr == SCR_PICKER) {
        const uintptr_t pb  = ADDR_PICKER_LIST_PB;
        const uintptr_t arr = *(const uintptr_t *)pb;
        const int       n = *(const int *)(pb + 0x18), sel = *(const int *)(pb + 0x20);
        if (!arr || n <= 0 || n > 4096 || sel < 0 || sel >= n) return SRC_NONE;
        const uintptr_t entry = ((const uintptr_t *)arr)[sel];
        if (!entry) return SRC_NONE;
        if (*(const int *)(entry + 0xc) != 0) return SRC_NONE; // a folder row
        const wchar_t *text = *(const wchar_t **)entry;
        if (!text || !text[0]) return SRC_NONE;
        char dir[128];
        lstrcpynA(dir, (const char *)ADDR_PICKER_DIR, sizeof(dir));
        char name[160];
        if (!WideCharToMultiByte(CP_ACP, 0, text, -1, name, sizeof(name) - 8, nullptr, nullptr)) return SRC_NONE;
        wsprintfA(out, "%s%s.mpm", dir, name);
        // mods:DATAEXT1: the same resolver the lobby branch uses, so a mod map's picker preview
        // reads the winner's bytes -- the file the open and the load will get -- not Maps\<name>.
        if (g_resolver) {
            char use[MAX_PATH + 8];
            if (g_resolver(out, use, (int)sizeof(use)) != 0 || !use[0]) return SRC_NONE;
            lstrcpynA(out, use, cap);
        }
        return SRC_PATH;
    }
    // lobby: the map the lobby is about -- current_map_data's path + name (a client adopts the host's out of band)
    char base[MAX_PATH];
    char dir[0xe0], nm[0x40];
    lstrcpynA(dir, (const char *)(mh::addr::current_map_data + 0x18), sizeof(dir));
    lstrcpynA(nm, (const char *)(mh::addr::current_map_data + 0xfc), sizeof(nm));
    if (!nm[0]) return SRC_NONE;
    wsprintfA(base, "%s%s", dir, nm);
    if (g_resolver) {
        if (g_resolver(base, out, cap) != 0) return SRC_WAITING;
        return out[0] ? SRC_PATH : SRC_NONE;
    }
    lstrcpynA(out, base, cap);
    return SRC_PATH;
}

// Refresh the cache for this frame. Returns what the Preview page shows.
enum Show { SHOW_IMAGE,
            SHOW_NOT_RECEIVED,
            SHOW_NONE };
bool g_show_image = false; // the Preview page of THIS frame has an image to blit
Show refresh(int scr) {
    char       path[MAX_PATH + 8];
    const Src  s       = source(scr, path, sizeof(path));
    const bool is_host = *(const int *)mh::addr::_G_LLM_NET_IS_HOST != 0;
    if (s == SRC_NONE) return SHOW_NONE;
    if (s == SRC_WAITING) return SHOW_NOT_RECEIVED;
    const DWORD now = GetTickCount();
    if (lstrcmpA(path, g_pv.path) != 0 || g_pv.status == PV_NONE ||
        g_pv.snapshot != mh::mods::mounted_snapshot()) {
        rebuild(path, false);
        g_pv.retry_at = now + 750;
    } else if (g_pv.status == PV_FAILED && (int)(now - g_pv.retry_at) >= 0) {
        // a file that was not there a moment ago (a download being stored), or changed since: look again, quietly
        g_pv.retry_at              = now + 750;
        const unsigned long long s = file_sig(path);
        if (s && (s != g_pv.sig || g_pv.retry_resource)) rebuild(path, true);
    }
    if (g_pv.status == PV_READY) return SHOW_IMAGE;
    return (scr == SCR_LOBBY && !is_host) ? SHOW_NOT_RECEIVED : SHOW_NONE;
}

void blit(int x0, int y0) {
    uint8_t  *fb    = *(uint8_t **)mh::addr::_G_LLM_FRAMEBUFFER;
    const int pitch = *(const int *)mh::addr::_G_LLM_FB_PITCH;
    const int W = *(const int *)mh::addr::WindowWidth, H = *(const int *)mh::addr::WindowHeight;
    if (!fb || W <= 0 || H <= 0 || W > 4096 || H > 4096 || pitch < W * 2) return;
    constexpr int N = mh_mapgen::PREVIEW_SIZE;
    for (int j = 0; j < N; ++j) {
        const int yy = y0 + j;
        if (yy < 0 || yy >= H) continue;
        uint16_t *row = (uint16_t *)(fb + (size_t)yy * pitch);
        for (int i = 0; i < N; ++i) {
            const int xx = x0 + i;
            if (xx < 0 || xx >= W) continue;
            row[xx] = g_pv.px[j * N + i];
        }
    }
}

// ---- the pager's draw: keep the Mode widget inside the box and the value current ---------------------------
extern "C" void __cdecl pager_draw(uint8_t *w) {
    const bool lobby                                                 = (w == g_pager_lobby_w);
    const int  scr                                                   = lobby ? SCR_LOBBY : SCR_PICKER;
    wf((uintptr_t)(lobby ? g_pager_lobby_sp : g_pager_pick_sp), 0xc) = page_of(scr);
    if (lobby && g_box.valid) {
        // Mode keeps the PAGER's resolved X/Y and adds its own x/y, width = pager width + its width: aim it at the box
        uint8_t *mode = (uint8_t *)mh::ui::lobby_mode_widget();
        if (mode) {
            const int px = *(const int *)mh::addr::_G_LLM_UI_WIDGET_DRAW_X, py = *(const int *)mh::addr::_G_LLM_UI_WIDGET_DRAW_Y,
                      pw              = *(const int *)mh::addr::_G_LLM_UI_WIDGET_DRAW_W;
            wf((uintptr_t)mode, WO_X) = (g_box.x + 3) - px;
            wf((uintptr_t)mode, WO_Y) = (g_box.y + 26) - py;
            wf((uintptr_t)mode, WO_W) = (g_box.w - 6) - pw;
        }
    }
    mh::call::llm_ui_widget_draw_content(w);
}

// clang-format off
__declspec(naked) void pager_draw_thunk() {
    __asm {
        pushad
        push eax
        call pager_draw
        add  esp, 4
        popad
        ret
    }
}
// clang-format on

} // namespace

namespace mh {
namespace ui {

bool map_preview_on() {
    return g_on;
}

int map_preview_page(int scr) {
    return (g_on && (scr == SCR_PICKER || scr == SCR_LOBBY)) ? page_of(scr) : -1;
}

void set_map_file_resolver(int (*fn)(const char *base, char *out, int cap)) {
    g_resolver = fn;
}

// Arm time. Returns whether the feature armed (it never half-arms: every refusal leaves the picker + lobby stock).
bool map_preview_install(const char *ini_path) {
    if (!mh::en_build_ok()) return false;
    const int want = mh_ini_get_int("ui", "map_preview", 1, ini_path);
    if (!want) {
        ui_log("; [mappreview] off ([ui] map_preview=0): picker and lobby stay stock\n");
        return false;
    }
    // the box, its two geometry owners and the spare must still be what the design measured
    if (wf(ADDR_STATUS_WIDGET, WO_H) != STATUS_H_STOCK || wf(ADDR_SPARE_WIDGET, WO_FLAGS) != 0x008600c1 ||
        *(const uintptr_t *)mh::addr::map_picker_widget_origin != ADDR_PICKER_CHILDREN) {
        ui_log("; [mappreview] NOT armed: the right panel's widgets are not the stock ones\n");
        return false;
    }
    // the picker's retail children (NULL-terminated) must be the eleven the design counted
    const uintptr_t *ch = (const uintptr_t *)ADDR_PICKER_CHILDREN;
    int              n  = 0;
    while (n < 14 && ch[n]) ++n;
    if (n != 11 || ch[8] != ADDR_STATUS_WIDGET) {
        ui_log("; [mappreview] NOT armed: the picker's children are not the stock eleven\n");
        return false;
    }
    init_page_tables();
    init_pager(g_pager_pick_w, g_pager_pick_sp, 2);
    init_pager(g_pager_lobby_w, g_pager_lobby_sp, 3);
    uint8_t *const pagers[2] = {g_pager_pick_w, g_pager_lobby_w};
    for (uint8_t *w : pagers) {
        wf((uintptr_t)w, WO_ACTION) = (int32_t)(uintptr_t)page_cb_detour;
        wf((uintptr_t)w, WO_DRAW)   = (int32_t)(uintptr_t)pager_draw_thunk;
    }
    for (int i = 0; i < 11; ++i) g_pick_children[i] = ch[i];
    g_pick_children[11]                              = ADDR_SPARE_WIDGET;
    g_pick_children[12]                              = (uintptr_t)g_pager_pick_w;
    g_pick_children[13]                              = 0;
    wf(ADDR_STATUS_WIDGET, WO_H)                     = STATUS_H_NEW;
    *(uintptr_t *)mh::addr::map_picker_widget_origin = (uintptr_t)g_pick_children;
    g_on                                             = true;
    ui_log("; [mappreview] armed ([ui] map_preview=1): description box 120 -> 130 px, page spinner on the map picker + lobby\n");
    return true;
}

void *map_preview_lobby_pager() {
    return g_on ? (void *)g_pager_lobby_w : nullptr;
}

bool map_preview_lobby_settings_page() {
    return !g_on || g_page_lobby == PAGE_SETTINGS;
}

void map_preview_notice_posted() {
    ++g_notice_seq;
}

// Per present. A DLL notice (map transfer, no-module) that appears while the lobby is up switches the lobby back to
// page 1 ONCE; the player may page away again and it does not fight them.
void map_preview_tick() {
    if (!g_on) return;
    const int scr = screen();
    g_box.valid   = g_box.valid && scr != SCR_OTHER;
    if (g_seen_seq == g_notice_seq) return;
    g_seen_seq = g_notice_seq;
    if (scr == SCR_LOBBY && g_page_lobby != PAGE_DESC) {
        g_page_lobby = PAGE_DESC;
        ui_log("; [mappreview] a status notice appeared: lobby box back to the Description page\n");
    }
}

// The box's draw (lobby_notice.cpp's status_draw). Returns what to draw:
//   BOX_NORMAL   -- not ours (browsers, feature off) or the Description page: the original text path
//   BOX_PREVIEW  -- an empty box, then map_preview_box_after() blits; *ovr is the placeholder text (or null)
//   BOX_SETTINGS -- the box with *ovr as its label (the Settings caption)
BoxKind map_preview_box(uint8_t *w, const wchar_t **ovr) {
    *ovr = nullptr;
    if (!g_on) return BOX_NORMAL;
    const int scr = screen();
    if (scr == SCR_OTHER) return BOX_NORMAL;
    g_box.x        = *(const int *)mh::addr::_G_LLM_UI_WIDGET_DRAW_X;
    g_box.y        = *(const int *)mh::addr::_G_LLM_UI_WIDGET_DRAW_Y;
    g_box.w        = *(const int *)mh::addr::_G_LLM_UI_WIDGET_DRAW_W;
    g_box.h        = *(const int *)mh::addr::_G_LLM_UI_WIDGET_DRAW_H;
    g_box.valid    = true;
    g_show_image   = false;
    const int page = page_of(scr);
    if (page == PAGE_DESC) return BOX_NORMAL;
    (void)w;
    if (page == PAGE_SETTINGS) {
        const bool host = *(const int *)mh::addr::_G_LLM_NET_IS_HOST != 0;
        *ovr            = mh::ui::tr(host ? Str::SETTINGS_TITLE : Str::SETTINGS_HOST_ONLY);
        return BOX_SETTINGS;
    }
    switch (refresh(scr)) {
        case SHOW_IMAGE: g_show_image = true; return BOX_PREVIEW;
        case SHOW_NOT_RECEIVED: *ovr = mh::ui::tr(Str::PREVIEW_NOT_RECEIVED); return BOX_PREVIEW;
        default: *ovr = mh::ui::tr(Str::PREVIEW_NONE); return BOX_PREVIEW;
    }
}

void map_preview_box_after() {
    if (!g_on || !g_box.valid || !g_show_image || g_pv.status != PV_READY) return;
    constexpr int N = mh_mapgen::PREVIEW_SIZE;
    blit(g_box.x + (g_box.w - N) / 2, g_box.y + (g_box.h - N) / 2);
}

} // namespace ui
} // namespace mh
