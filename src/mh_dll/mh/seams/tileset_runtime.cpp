#include "seams/tileset_runtime.h"
#include "seams/tileset_summary.h"
#include "ui/player_strings.h"
#include <windows.h>
#include <algorithm>
#include <cstdlib>
#include "net_internal.h"
#include "include/mh_log_sink.h"
#include <cstring>
#include <string_view>
#include "mh_mod_catalog.h"
#include "mh_tileset.h"
#include "addr/mh_calls.gen.h"
#include "addr/mh_structs.gen.h"
#include "state/region_runtime.h"
#include "hook/patch.h"
#include "hook/promoted.h"
#include "en_guard.h"

namespace mh::tileset_runtime {
namespace {
Prepared g_published;
struct Profile {
    uint8_t        index, bank, soldier;
    const wchar_t *label;
};
// The registry's nine-byte rows are packed: read the live label pointer, which
// menu initialization binds according to the active language, with memcpy.
const wchar_t *stock_label(unsigned index) {
    const wchar_t *p = nullptr;
    std::memcpy(&p, reinterpret_cast<const void *>(0x0065673eu + (index - 1) * 9 + 4), sizeof(p));
    return p;
}
bool canonical(const char *token, char (&out)[13]) {
    if (!token) return false;
    unsigned n = 0;
    for (; n < 12 && token[n]; ++n) {
        unsigned char c = static_cast<unsigned char>(token[n]);
        if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c + ('a' - 'A'));
        out[n] = static_cast<char>(c);
    }
    out[n] = 0;
    return n != 0 && (n < 12 || token[12] == 0);
}
const Entry *find(const char (&key)[13]) {
    const auto &v  = g_published.entries;
    const auto  it = std::lower_bound(v.begin(), v.end(), std::string_view(key),
                                      [](const Entry &e, std::string_view k) { return std::string_view(e.token) < k; });
    return it != v.end() && it->token == key ? &*it : nullptr;
}
bool resolve(const char *token, Profile &p) {
    char key[13] = {};
    if (!canonical(token, key)) return false;
    const Entry *e = find(key);
    if (e) {
        p = {e->stock_index, e->tree_bank, e->soldier_offset, e->label.c_str()};
        return true;
    }
    const auto index = mh::mods::stock_tileset_index(std::string_view(key));
    if (!index) return false;
    static constexpr uint8_t bank[]    = {0, 51, 0, 52, 53, 55, 54, 56, 50};
    static constexpr uint8_t soldier[] = {0, 64, 0, 0, 64, 64, 0, 0, 128};
    p                                  = {static_cast<uint8_t>(index), bank[index], soldier[index], stock_label(index)};
    return true;
}
uint8_t tree_bank(const std::string &s) {
    if (s == "jungle") return 51;
    if (s == "ksie256") return 52;
    if (s == "lod256") return 53;
    if (s == "metal256") return 55;
    if (s == "ruins256") return 54;
    if (s == "skaly256") return 56;
    if (s == "diuna256") return 50;
    return 0;
}
} // namespace
bool prepare(const mh::mods::Snapshot &snapshot, Prepared &out, std::string &error) {
    Prepared candidate;
    candidate.entries.reserve(snapshot.tilesets.size());
    for (const auto &d : snapshot.tilesets) {
        if (d.trees != "none" && tree_bank(d.trees) == 0) {
            error = "invalid tileset tree profile";
            return false;
        }
        if (d.soldier_variant > 2) {
            error = "invalid tileset soldier variant";
            return false;
        }
        if (d.wide_label.empty() || d.wide_label.size() > 64) {
            error = "invalid tileset label";
            return false;
        }
        Entry e;
        e.token          = d.name;
        e.label          = d.wide_label;
        e.stock_index    = mh::mods::stock_tileset_index(d.name);
        e.tree_bank      = tree_bank(d.trees);
        e.soldier_offset = static_cast<uint8_t>(d.soldier_variant * 64);
        candidate.entries.push_back(std::move(e));
    }
    std::sort(candidate.entries.begin(), candidate.entries.end(),
              [](const Entry &a, const Entry &b) { return a.token < b.token; });
    out = std::move(candidate);
    return true;
}
void           publish(Prepared &&prepared) noexcept { g_published = std::move(prepared); }
const wchar_t *label(const char *token) {
    Profile p{};
    return resolve(token, p) ? p.label : nullptr;
}
bool apply_planet_graphics(int32_t planet_index, const char *token) {
    Profile p{};
    if (planet_index < 0 || planet_index >= 32 || !resolve(token, p)) return false;
    auto *planets                     = mh::state::ptr<mh::game::mh_cfg_final_struct_Planet>(mh::state::RID_PLANETS);
    auto &planet                      = planets[planet_index];
    planet.tlo_index                  = p.index;
    planet.soldier_sprite_bank_offset = p.soldier;
    if (planet_index == 31) {
        for (unsigned bank = 50; bank < 57; ++bank)
            mh::call::cfg_final_planet_FillBankData(0, bank, static_cast<uint32_t>(planet_index));
        mh::call::cfg_final_planet_FillBankData(1, p.bank, static_cast<uint32_t>(planet_index));
    }
    return true;
}
void require_planet_graphics(int32_t planet, const char *token) {
    if (apply_planet_graphics(planet, token)) return;
    seam_log("; [tilesets] FATAL: graphics token violated admission invariant\n");
    mh_logq_flush(1000);
    std::abort();
}

} // namespace mh::tileset_runtime

namespace mh::tileset_runtime {
namespace {
// An unresolved runtime token is a violated admission invariant. Never let the
// original switch turn it into Jungle or continue with stale graphics.
void __cdecl retail_graphics(int32_t planet, const char *token) {
    require_planet_graphics(planet, token);
}
const wchar_t *__cdecl retail_label(const char *token) {
    const wchar_t *p = label(token);
    return p ? p : mh::ui::tr(mh::ui::Str::TILESET_UNKNOWN);
}
// Graphics site: EBP-0x138 is the thirteen-byte token; EBP+0x40 is
// planet_data.index. Continuation is the original RET 0x3c epilogue.
__declspec(naked) void graphics_thunk() {
    __asm {
        pushfd
        pushad
        lea eax, [ebp-138h]
        push eax
        push dword ptr [ebp+40h]
        call retail_graphics
        add esp, 8
        popad
        popfd
        push 045ba1ch
        ret
    }
}
// Lobby site: EBP-0x13c is the token, EBP-0x1c is the label local.
// 0x4c0810 begins with CMP and recomputes flags.
__declspec(naked) void label_thunk() {
    __asm {
        pushfd
        pushad
        lea eax, [ebp-13ch]
        push eax
        call retail_label
        add esp, 4
        mov [ebp-1ch], eax
        popad
        popfd
        push 04c0810h
        ret
    }
}
// Retail call-site ABI: nine cdecl arguments, caller cleans 0x24 bytes.
int __cdecl summary_format(wchar_t *dst, const wchar_t *fmt, int marker, const wchar_t *map,
                           const wchar_t *tileset, int width, int height,
                           const wchar_t *players_text, int players) {
    return format_summary(dst, fmt, marker, map, tileset, width, height, players_text, players,
                          mh::ui::tr(mh::ui::Str::TILESET_SUMMARY_OVERFLOW));
}
void make_jump(uintptr_t at, const void *fn, uint8_t *out, unsigned n) {
    std::memset(out, 0x90, n);
    out[0]            = 0xe9;
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(fn) - (at + 5));
    std::memcpy(out + 1, &rel, 4);
}
} // namespace
bool install() {
    static bool installed = false;
    if (installed) return true;
    if (!mh::en_build_ok()) return false;
    constexpr uintptr_t      graphics = 0x0045b8feu, lobby = 0x004c07cbu;
    constexpr uintptr_t      format_a = 0x004c0856u, format_b = 0x004c0899u;
    static constexpr uint8_t ge[]       = {0x8d, 0x85, 0xc8, 0xfe, 0xff, 0xff};
    static constexpr uint8_t le[]       = {0xc7, 0x45, 0xdc, 0, 0, 0, 0};
    static constexpr uint8_t fa[]       = {0xe8, 0xc5, 0xfa, 0, 0};
    static constexpr uint8_t fb[]       = {0xe8, 0x82, 0xfa, 0, 0};
    const bool               translated = mh::hook::promoted_owner_of(graphics) != nullptr;
    if (mh::hook::promoted_owner_of(lobby) || mh::hook::promoted_owner_of(format_a) ||
        mh::hook::promoted_owner_of(format_b)) return false;
    uint8_t gr[sizeof(ge)], lr[sizeof(le)], ar[sizeof(fa)], br[sizeof(fb)];
    make_jump(graphics, reinterpret_cast<const void *>(&graphics_thunk), gr, sizeof(gr));
    make_jump(lobby, reinterpret_cast<const void *>(&label_thunk), lr, sizeof(lr));
    make_jump(format_a, reinterpret_cast<const void *>(&summary_format), ar, sizeof(ar));
    make_jump(format_b, reinterpret_cast<const void *>(&summary_format), br, sizeof(br));
    ar[0] = 0xe8;
    br[0] = 0xe8;
    struct Site {
        uintptr_t      address;
        const uint8_t *expected;
        const uint8_t *replacement;
        int            size;
        bool           skip;
    };
    const Site sites[] = {{graphics, ge, gr, sizeof(ge), translated},
                          {lobby, le, lr, sizeof(le), false},
                          {format_a, fa, ar, sizeof(fa), false},
                          {format_b, fb, br, sizeof(fb), false}};
    for (const auto &site : sites)
        if (!site.skip && std::memcmp(reinterpret_cast<const void *>(site.address),
                                      site.expected, site.size) != 0) return false;
    unsigned done = 0;
    for (; done < sizeof(sites) / sizeof(sites[0]); ++done) {
        const auto &site = sites[done];
        if (!site.skip && !mh::hook::patch_bytes_guarded(site.address, site.expected,
                                                         site.replacement, site.size)) break;
    }
    if (done != sizeof(sites) / sizeof(sites[0])) {
        while (done) {
            const auto &site = sites[--done];
            if (!site.skip && !mh::hook::patch_bytes_guarded(site.address, site.replacement,
                                                             site.expected, site.size)) {
                seam_log("; [tilesets] FATAL: graphics hook rollback failed\n");
                mh_logq_flush(1000);
                std::abort();
            }
        }
        return false;
    }
    installed = true;
    return true;
}
} // namespace mh::tileset_runtime
