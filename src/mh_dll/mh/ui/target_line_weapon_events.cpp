#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include "ui/target_line_weapon_events.h"
#include "ui/target_line_intents.h"
#include "en_guard.h"
#include "hook/patch.h"
#include "hook/promoted.h"

namespace mh::ui::selected_targets::detail {
namespace {
// EN fire_weapon frame, verified against the actual three effect CALL sites. The same frame
// carries the exact target even when its owner is firing primary and secondary in one tick.
void __cdecl fired_from_frame(const uint8_t *frame) {
    const auto player     = *reinterpret_cast<const uint16_t *>(frame - 0x1c);
    const auto shooter    = *reinterpret_cast<const uint32_t *>(frame - 0x68);
    const auto target_ref = *reinterpret_cast<const uint16_t *>(frame - 0x20);
    const auto target_id  = *reinterpret_cast<const uint16_t *>(frame + 8);
    observe_weapon_fired(player, shooter, target_ref, target_id);
}
const uintptr_t native_projectile_spawn = 0x00464932u;
const uintptr_t native_area_damage      = 0x0044c3fbu;

// The wrapper has its own return address. Copy the original eight stack words BEFORE calling
// the original, which RET20 consumes; RET20 below consumes the caller's original eight words.
// Repeated PUSH[ESP+32] walks them last-to-first without changing ANY argument register or flags.
// No global/thread-local return scratch, and no reliance on a particular recursive call depth.
// After the real call, PUSHFD/PUSHAD + aligned FXSAVE preserve its complete outgoing machine
// state around the observer. FNINIT gives C++ an empty x87 stack while FLDCW retains caller
// precision/rounding. FXRSTOR restores x87 tags/registers/status and MXCSR/XMM, not just its CW.
__declspec(naked) void projectile_success_thunk() {
    __asm {
        push dword ptr [esp + 20h]
        push dword ptr [esp + 20h]
        push dword ptr [esp + 20h]
        push dword ptr [esp + 20h]
        push dword ptr [esp + 20h]
        push dword ptr [esp + 20h]
        push dword ptr [esp + 20h]
        push dword ptr [esp + 20h]
        call dword ptr [native_projectile_spawn]
        pushfd
        pushad
        mov esi, esp
        sub esp, 527
        and esp, -16
        fxsave [esp]
        fninit
        fldcw word ptr [esp]
        cld
                      // Native projectile_spawn returns EAX=1 only AFTER committing a live projectile;
                      // EAX=0 on full/no free slot. Saved EAX is +28 in PUSHAD's output frame.
        cmp dword ptr [esi + 1ch], 0
        je no_projectile
        push ebp
        call fired_from_frame
        add esp, 4
    no_projectile:
        fxrstor [esp]
        mov esp, esi
        popad
        popfd
        ret 20h
    }
}
// Direct/beam types3/4 commit their effect through apply_area_damage rather than a projectile.
// Six stack words (damage double plus four scalar arguments), consumed by native RET18 and
// then by our RET18. Completing the actual damage action is the success boundary: finding no
// victims does not mean that no beam was fired. Type7's no-op never reaches any of these sites.
__declspec(naked) void direct_success_thunk() {
    __asm {
        push dword ptr [esp + 18h]
        push dword ptr [esp + 18h]
        push dword ptr [esp + 18h]
        push dword ptr [esp + 18h]
        push dword ptr [esp + 18h]
        push dword ptr [esp + 18h]
        call dword ptr [native_area_damage]
        pushfd
        pushad
        mov esi, esp
        sub esp, 527
        and esp, -16
        fxsave [esp]
        fninit
        fldcw word ptr [esp]
        cld
        push ebp
        call fired_from_frame
        add esp, 4
        fxrstor [esp]
        mov esp, esi
        popad
        popfd
        ret 18h
    }
}
struct patch {
    uintptr_t   site;
    uint8_t     expected[5];
    const void *thunk;
    uint8_t     replacement[5]{};
};
patch sites[] = {
    {0x0048c1efu, {0xe8, 0x07, 0x02, 0xfc, 0xff}, reinterpret_cast<const void *>(&direct_success_thunk)},
    {0x0048c414u, {0xe8, 0x19, 0x85, 0xfd, 0xff}, reinterpret_cast<const void *>(&projectile_success_thunk)},
    {0x0048c536u, {0xe8, 0xf7, 0x83, 0xfd, 0xff}, reinterpret_cast<const void *>(&projectile_success_thunk)},
};
bool installed = false;
} // namespace

bool install_weapon_observers() {
    if (installed) return true;
    if (!mh::en_build_ok()) return false;
    // One owner for the whole fire body. The translated body's real success consumers emit to
    // the existing presentation event channel; never patch unreachable instructions underneath it.
    if (mh::hook::promoted_owner_of(0x0048bb6cu)) return true;
    for (auto &p : sites) {
        if (mh::hook::promoted_owner_of(p.site) ||
            std::memcmp(reinterpret_cast<const void *>(p.site), p.expected, sizeof p.expected) != 0) return false;
        p.replacement[0]  = 0xe8;
        const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(p.thunk) - p.site - 5);
        std::memcpy(p.replacement + 1, &rel, sizeof rel);
    }
    unsigned applied = 0;
    for (; applied < sizeof sites / sizeof sites[0]; ++applied) {
        const auto &p = sites[applied];
        if (!mh::hook::patch_bytes_guarded(p.site, p.expected, p.replacement, sizeof p.expected)) break;
    }
    if (applied != sizeof sites / sizeof sites[0]) {
        while (applied) {
            const auto &p = sites[--applied];
            if (!mh::hook::patch_bytes_guarded(p.site, p.replacement, p.expected, sizeof p.expected))
                std::abort(); // cannot continue with an indeterminate partially restored firing body
        }
        return false;
    }
    installed = true;
    return true;
}
} // namespace mh::ui::selected_targets::detail
