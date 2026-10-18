// seams/ui_minimap_plus.cpp -- mods:MM1-MM3: resources at every minimap zoom, no round mask and a view box at the
// widest zoom. Mechanism, patch sites and settings are in include/mh_minimapplus_export.h.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "mh_ini_gate.h" // RL2: the ship gate every ini read goes through
#include <cstdint>
#include <cstring>

#include "include/mh_minimapplus_export.h"
#include "hook/patch.h"    // patch_bytes_guarded
#include "hook/promoted.h" // promoted_owner_of
#include "net_internal.h"  // seam_log; g_ini
#include "en_guard.h"      // EN-only build gate

namespace {

// Read by the thunks. 1 = the stock mask is SKIPPED / the box IS drawn at the widest zoom.
volatile int g_skip_mask = 0;
volatile int g_top_box   = 0;

// The four resource-gate jes: `74 rel8` -> NOP NOP. Zoom 5, 1, 2, 4 in address order.
struct res_site {
    uintptr_t va;
    uint8_t   rel;
};
const res_site RES_SITES[4] = {{0x004a191cu, 0x41}, {0x004a1b15u, 0x41}, {0x004a1ce6u, 0x47}, {0x004a20cau, 0x76}};

constexpr uintptr_t GATE_SITE      = 0x004a22f0u; // 0F 84 25 F7 FF FF  je 0x004a1a1b
constexpr uintptr_t END_SITE       = 0x004a1aadu; // E9 49 08 00 00     jmp 0x004a22fb
const uint8_t       GATE_EXPECT[6] = {0x0F, 0x84, 0x25, 0xF7, 0xFF, 0xFF};
const uint8_t       END_EXPECT[5]  = {0xE9, 0x49, 0x08, 0x00, 0x00};

void end_thunk(); // below

// clang-format off
// Replaces `je 0x004a1a1b`; the flags are those of the preceding `cmp dword [0x005586f2],1`.
__declspec(naked) void gate_thunk() {
    __asm {
        jne  stock_box              // flag != 1: the stock fall-through (the box call)
        cmp  dword ptr [g_skip_mask], 0
        jne  no_mask
        push 0x004a1a1b             // the stock round-mask pass (ends in end_thunk)
        ret
    no_mask:
        push offset end_thunk
        ret
    stock_box:
        push 0x004a22f6
        ret
    }
}

// Replaces the mask pass's `jmp 0x004a22fb`, and is entered from gate_thunk when the mask is skipped.
__declspec(naked) void end_thunk() {
    __asm {
        cmp  dword ptr ds:[0x005586f2], 1
        jne  done
        cmp  dword ptr [g_top_box], 0
        je   done
        pushad
        mov  eax, 0x004a2344        // llm_strat_minimap_draw_viewport_box (RES9's thunk sits on its entry)
        call eax
        popad
    done:
        push 0x004a22fb
        ret
    }
}
// clang-format on

int follow(int v, int plus) { return v < 0 ? plus : (v ? 1 : 0); } // -1 = follow minimap_plus

bool refused(uintptr_t va, const char *what, char *why, size_t n) {
    if (mh::hook::promoted_owner_of(va)) {
        wsprintfA(why, "DISPLACED (%s at %08X is inside a promoted function)", what, (unsigned)va);
        return true;
    }
    return false;
}

} // namespace

extern "C" int MH_MinimapPlus_Install(void) {
    if (!mh::en_build_ok()) return 0; // EN-only, like every seam that names an EN VA
    const int plus = mh_ini_get_int("ui", "minimap_plus", 1, g_ini) ? 1 : 0;
    const int res  = follow(mh_ini_get_int("ui", "minimap_res_all_zooms", -1, g_ini), plus);
    const int nomk = follow(mh_ini_get_int("ui", "minimap_top_no_mask", -1, g_ini), plus);
    const int box  = follow(mh_ini_get_int("ui", "minimap_top_box", -1, g_ini), plus);

    int  armed    = 0;
    char why[200] = "";
    bool res_on = false, mask_off = false, box_on = false;

    if (res) {
        int ok = 0;
        for (const res_site &s : RES_SITES) {
            const uint8_t expect[2] = {0x74, s.rel}, repl[2] = {0x90, 0x90};
            if (refused(s.va, "resource gate", why, sizeof(why))) continue;
            if (mh::hook::patch_bytes_guarded(s.va, expect, repl, 2)) ++ok;
        }
        res_on = ok == 4;
        armed += ok;
        if (!res_on) seam_log("; minimap: MM1 resources at every zoom NOT fully armed (a gate's bytes differ or a function is promoted)\n");
    }

    if (nomk || box) {
        g_skip_mask = nomk;
        g_top_box   = box;
        bool ok     = false;
        if (refused(GATE_SITE, "round-mask gate", why, sizeof(why)) || refused(END_SITE, "mask exit", why, sizeof(why))) {
            seam_log("; minimap: MM2/MM3 top-level mask/box NOT armed: ");
            seam_log(why);
            seam_log("\n");
        } else {
            uint8_t g[6], e[5];
            g[0]               = 0xE9;
            const int32_t grel = (int32_t)((uintptr_t)&gate_thunk - (GATE_SITE + 5));
            memcpy(g + 1, &grel, 4);
            g[5]               = 0x90;
            e[0]               = 0xE9;
            const int32_t erel = (int32_t)((uintptr_t)&end_thunk - (END_SITE + 5));
            memcpy(e + 1, &erel, 4);
            // Exit first: the mask pass may only reach end_thunk once the gate can route to it.
            ok = mh::hook::patch_bytes_guarded(END_SITE, END_EXPECT, e, 5) &&
                 mh::hook::patch_bytes_guarded(GATE_SITE, GATE_EXPECT, g, 6);
            if (!ok) seam_log("; minimap: MM2/MM3 top-level mask/box NOT armed (entry bytes differ)\n");
        }
        if (ok) {
            mask_off = nomk != 0;
            box_on   = box != 0;
            armed += 2;
        }
    }

    char m[200];
    wsprintfA(m, "; minimap: resources at every zoom %s, top-level mask %s, top-level box %s\n", res_on ? "ON" : "off",
              mask_off ? "off" : "ON", box_on ? "ON" : "off");
    seam_log(m);
    return armed;
}
