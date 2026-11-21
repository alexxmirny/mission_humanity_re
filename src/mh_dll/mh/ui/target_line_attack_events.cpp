#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include "ui/target_line_attack_events.h"
#include "ui/target_line_attack_provenance.h"
#include "ui/target_line_intents.h"
#include "ui/target_line_order_events.h"
#include "caps/orders.h"
#include "en_guard.h"
#include "hook/detour.h"
#include "hook/patch.h"
#include "hook/promoted.h"
#include "state/region_runtime.h"

namespace mh::ui::selected_targets::detail {
namespace {
constexpr uintptr_t group_entry       = 0x00444894u;
constexpr uint8_t   group_expected[8] = {
    0x55, 0x89, 0xe5, 0x68, 0x38, 0x00, 0x00, 0x00};

void           *group_trampoline   = nullptr;
const uintptr_t native_target_bind = 0x004dac44u;

// Resolve on each admission, inside the saved-machine-state envelope, just like
// target_line_deferred_events.cpp. No cached pointer can outlive a world binding.
// These are scalar regions; caps changes the record arrays, not these counters.
int32_t *__cdecl admission_count(unsigned container) {
    int32_t *count = nullptr;
    switch (container) {
        case mh::caps::orders::QUEUE:
            count = mh::state::ptr<int32_t>(mh::state::RID_STRAT_ORDER_QUEUE_COUNT);
            break;
        case mh::caps::orders::PENDING:
            count = mh::state::ptr<int32_t>(mh::state::RID_STRAT_ORDER_PENDING_COUNT);
            break;
        case mh::caps::orders::STAGING:
            count = mh::state::ptr<int32_t>(mh::state::RID_STRAT_ORDER_STAGING_COUNT);
            break;
        default:
            std::abort();
    }
    if (!count) std::abort(); // cannot reproduce the displaced native INC
    return count;
}

// Called only AFTER the original successful-admission INC has been reproduced.
// Neither canonical mirrors nor input records identify the slot: use the live
// caps container and its post-INC count. PENDING is an unknown-origin ingress.
void __cdecl admitted_last(unsigned container) {
    native_order_admitted(container);
}

// Actual dispatcher locals, not registers returned by the binding helper.
// All three locals are DWORDs in the native frame.
void __cdecl accepted_from_frame(const uint8_t *frame, unsigned secondary) {
    const unsigned player = *reinterpret_cast<const uint32_t *>(frame - 0x34);
    const unsigned unit   = *reinterpret_cast<const uint32_t *>(frame - 0x30);
    const unsigned index  = *reinterpret_cast<const uint32_t *>(frame - 0x24);
    observe_attack_assignment(player, unit, index, secondary != 0);
}

// Native group_issue_attack_order:
//   EAX = target slot, EDX = target player, BX = selector.
//   No stack arguments, plain RET; EBX is deliberately volatile.
//
// Preserve ALL incoming state around begin, then preserve the ORIGINAL's actual
// outgoing state around end. In particular, never restore incoming EBX over the
// native routine's outgoing EBX. No shared return-address scratch is needed.
__declspec(naked) void attack_group_thunk() {
    __asm {
        pushfd
        pushad
        mov esi, esp
        sub esp, 527
        and esp, -16
        fxsave [esp]
        fninit
        fldcw word ptr [esp]
        cld
                // PUSHAD frame: EBX+10h, EDX+14h, EAX+1ch.
        movzx eax, word ptr [esi + 10h]
        push eax
        push dword ptr [esi + 14h]
        push dword ptr [esi + 1ch]
        call begin_attack_group
        add esp, 12
        fxrstor [esp]
        mov esp, esi
        popad
        popfd

                    // The stolen eight bytes contain only complete, position-independent
                    // instructions. The trampoline executes them and resumes at0044489c.
        call dword ptr [group_trampoline]

        pushfd
        pushad
        mov esi, esp
        sub esp, 527
        and esp, -16
        fxsave [esp]
        fninit
        fldcw word ptr [esp]
        cld
        call end_attack_group
        fxrstor [esp]
        mov esp, esi
        popad
        popfd
        ret
    }
}

// The small entry stubs push a private container word without altering flags.
// The common thunk saves the incoming state BEFORE any flag-changing operation.
//
// Resolve the count pointer with an empty x87 stack and the caller's CW. Restore
// incoming EFLAGS immediately before INC, so its unchanged CF is the real CF.
// Replace the saved flags with INC's result before running the observer. This
// preserves both incoming CF and every outgoing arithmetic flag, together with
// all GPRs, x87 state, MXCSR/XMM state and DF.
__declspec(naked) void admission_thunk() {
    __asm {
        // Stack on entry: container, original patched-CALL return address.
        pushfd
        pushad
        mov esi, esp
        sub esp, 527
        and esp, -16
        fxsave [esp]
        fninit
        fldcw word ptr [esp]
        cld

                // PUSHAD32 + FLAGS4 puts the private container word atESI+24h.
        push dword ptr [esi + 24h]
        call admission_count
        add esp, 4

        push dword ptr [esi + 20h]
        popfd
        inc dword ptr [eax]
        pushfd
        pop dword ptr [esi + 20h]

        // Original DF may be set. C++ must still run with DF clear.
        cld
        push dword ptr [esi + 24h]
        call admitted_last
        add esp, 4

        fxrstor [esp]
        mov esp, esi
        popad
        popfd
        lea esp, [esp + 4]
        ret
    }
}

static_assert(mh::caps::orders::QUEUE == 0 &&
              mh::caps::orders::PENDING == 1 &&
              mh::caps::orders::STAGING == 2);

__declspec(naked) void queue_admission_thunk() {
    __asm {
        push 0
        jmp admission_thunk
    }
}

__declspec(naked) void pending_admission_thunk() {
    __asm {
        push 1
        jmp admission_thunk
    }
}

__declspec(naked) void staging_admission_thunk() {
    __asm {
        push 2
        jmp admission_thunk
    }
}

// The original004dac44 call uses EAX/EDX/EBX, no stack arguments, and RET0.
// Call it with the untouched incoming argument registers/flags/FPU state.
// Only AFTER it returns do we save its actual outgoing machine state.
//
// The private channel word is pushed AFTER the original call. Consequently it
// cannot disturb that call's arguments or EBX-volatile result.
__declspec(naked) void accepted_thunk() {
    __asm {
        // Stack on entry: secondary, original patched-CALL return address.
        pushfd
        pushad
        mov esi, esp
        sub esp, 527
        and esp, -16
        fxsave [esp]
        fninit
        fldcw word ptr [esp]
        cld

        push dword ptr [esi + 24h]
        push ebp
        call accepted_from_frame
        add esp, 8

        fxrstor [esp]
        mov esp, esi
        popad
        popfd
        lea esp, [esp + 4]
        ret
    }
}

__declspec(naked) void primary_accepted_thunk() {
    __asm {
        call dword ptr [native_target_bind]
        push 0
        jmp accepted_thunk
    }
}

__declspec(naked) void secondary_accepted_thunk() {
    __asm {
        call dword ptr [native_target_bind]
        push 1
        jmp accepted_thunk
    }
}

struct patch {
    uintptr_t   site;
    uintptr_t   owner;
    uint8_t     expected[6];
    unsigned    length;
    const void *thunk;
    uint8_t     replacement[6]{};
    bool        enabled = false;
    bool        applied = false;
};

patch sites[] = {
    {0x00466143u, 0x00466094u, {0xff, 0x05, 0x94, 0x01, 0x5d, 0x00}, 6, reinterpret_cast<const void *>(&queue_admission_thunk)},
    {0x00466325u, 0x00466211u, {0xff, 0x05, 0xa4, 0x01, 0x5d, 0x00}, 6, reinterpret_cast<const void *>(&staging_admission_thunk)},
    {0x004667e3u, 0x00466790u, {0xff, 0x05, 0xa8, 0x01, 0x5d, 0x00}, 6, reinterpret_cast<const void *>(&pending_admission_thunk)},
    {0x00467088u, 0x00466892u, {0xe8, 0xb7, 0x3b, 0x07, 0x00}, 5, reinterpret_cast<const void *>(&primary_accepted_thunk)},
    {0x004672a2u, 0x00466892u, {0xe8, 0x9d, 0x39, 0x07, 0x00}, 5, reinterpret_cast<const void *>(&secondary_accepted_thunk)},
};

void rollback_sites() {
    for (unsigned i = static_cast<unsigned>(sizeof sites / sizeof sites[0]); i != 0;) {
        auto &p = sites[--i];
        if (!p.applied) continue;
        if (!mh::hook::patch_bytes_guarded(
                p.site, p.replacement, p.expected, static_cast<int>(p.length)))
            std::abort(); // an indeterminate partial observer installation is unsafe
        p.applied = false;
    }
}

bool installed = false;
} // namespace

bool install_attack_observers() {
    if (installed) return true;
    if (!mh::en_build_ok()) return false;
    if (!reserve_attack_provenance()) return false;

    const bool native_group = !mh::hook::promoted_owner_of(group_entry);

    // Ask the existing interlock before the stronger full-eight-byte compare.
    // Never mask a promoted/owned-entry refusal as a prologue mismatch.
    if (native_group) {
        if (mh::hook::detour_refusal(
                group_entry, mh::hook::entry_claim::exclusive,
                mh::hook::WATCOM_PROLOGUE) != mh::hook::refuse_reason::none)
            return false;
        if (std::memcmp(reinterpret_cast<const void *>(group_entry),
                        group_expected, sizeof group_expected) != 0)
            return false;
    }

    // Full preflight before any writes. Skip each promoted OWNER independently:
    // mixed native/translated configurations still observe every live origin.
    for (auto &p : sites) {
        p.applied = false;
        p.enabled = !mh::hook::promoted_owner_of(p.owner);
        if (!p.enabled) continue;
        if (mh::hook::promoted_owner_of(p.site) ||
            std::memcmp(reinterpret_cast<const void *>(p.site),
                        p.expected, p.length) != 0)
            return false;

        std::memset(p.replacement, 0x90, sizeof p.replacement);
        p.replacement[0]  = 0xe8;
        const int32_t rel = static_cast<int32_t>(
            reinterpret_cast<uintptr_t>(p.thunk) - p.site - 5);
        std::memcpy(p.replacement + 1, &rel, sizeof rel);
    }

    for (auto &p : sites) {
        if (!p.enabled) continue;
        if (!mh::hook::patch_bytes_guarded(
                p.site, p.expected, p.replacement, static_cast<int>(p.length))) {
            rollback_sites();
            return false;
        }
        p.applied = true;
    }

    // Install LAST. install_trampoline fails atomically and publishes its thunk
    // only on success. Thus every remaining failure needs to roll back only
    // guarded byte patches; no invented trampoline-uninstall engine is needed.
    if (native_group &&
        !mh::hook::install_trampoline(
            group_entry, reinterpret_cast<void *>(&attack_group_thunk),
            &group_trampoline, 8, mh::hook::entry_claim::exclusive,
            "the selected-target attack group provenance observer")) {
        rollback_sites();
        return false;
    }

    installed = true;
    return true;
}
} // namespace mh::ui::selected_targets::detail
