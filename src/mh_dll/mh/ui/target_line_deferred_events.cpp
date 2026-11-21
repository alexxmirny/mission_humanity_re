#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include "ui/target_line_deferred_events.h"
#include "ui/target_line_intents.h"
#include "ui/target_line_order_events.h"
#include "addr/mh_structs.gen.h"
#include "caps/orders.h"
#include "en_guard.h"
#include "hook/patch.h"
#include "hook/promoted.h"
#include "state/region_runtime.h"
#include "seams/order_tasks_native.h"

namespace mh::ui::selected_targets::detail {
namespace {
// EN469d9d is after every successful args/order/home store and target rebind, BEFORE
// decrement/compaction. The original native frame owns the still-current queue index here.
int32_t *__cdecl committed_from_frame(const uint8_t *frame) {
    auto *count = mh::state::ptr<int32_t>(mh::state::RID_STRAT_ORDER_QUEUE_COUNT);
    if (!count) std::abort(); // cannot reproduce the original DEC against an unbound world
    const unsigned player  = *reinterpret_cast<const uint16_t *>(frame - 0x10);
    const unsigned id      = *reinterpret_cast<const uint32_t *>(frame - 0x18);
    const int      index   = *reinterpret_cast<const int32_t *>(frame - 0x1c);
    const unsigned n       = mh::caps::orders::live_count(mh::caps::orders::QUEUE);
    const auto    *queue   = reinterpret_cast<const mh::game::mh_llm_strat_order *>(mh::caps::orders::base(mh::caps::orders::QUEUE));
    const bool     applied = mh::order_tasks::native::finish_deferred(frame);
    if (queue && index >= 0 && static_cast<unsigned>(index) < n) {
        const auto    &q    = queue[index];
        const unsigned kind = q.owner_and_kind & 0xf0;
        if (applied && (kind == 0x20 || kind == 0x80) && (q.owner_and_kind & 15) == player && q.unit_index == id)
            observe_deferred_commit(player, id, static_cast<unsigned>(index));
    }
    if (index < 0 || static_cast<unsigned>(index) >= n) std::abort();
    native_order_dequeued(static_cast<uint32_t>(index), applied);
    return count;
}
__declspec(naked) void deferred_success_thunk() {
    __asm {
        // A private slot for the bound count pointer. PUSH does not change incoming flags.
        push 0
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
        call committed_from_frame
        add esp, 4
        // PUSHAD32 + saved FLAGS4 place the private slot36 bytes above ESI.
        mov dword ptr [esi + 24h], eax
        fxrstor [esp]
        mov esp, esi
        popad
        popfd
                    // Reproduce the exact DEC AFTER restoring the incoming flags, including its unchanged
                    // CF. Neither PUSH/MOV/POP nor LEA/RET alters flags. No baked stock count address.
        push eax
        mov eax, dword ptr [esp + 4]
        dec dword ptr [eax]
        pop eax
        lea esp, [esp + 4]
        ret
    }
}
bool installed = false;
} // namespace

bool install_deferred_observer() {
    if (installed) return true;
    if (!mh::en_build_ok()) return false;
    if (mh::hook::promoted_owner_of(0x00469a37u)) return true;
    constexpr uintptr_t site = 0x00469d9du;
    // Actual original instruction: DEC dword[005d0194]. caps/orders' sorted manifest has
    // its previous operand at469c7d(len4), next at469dd0(len4); this six-byte site is disjoint.
    const uint8_t expected[6] = {0xff, 0x0d, 0x94, 0x01, 0x5d, 0x00};
    if (mh::hook::promoted_owner_of(site) ||
        std::memcmp(reinterpret_cast<const void *>(site), expected, sizeof expected) != 0) return false;
    uint8_t       replacement[6] = {0xe8, 0, 0, 0, 0, 0x90};
    const int32_t rel            = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&deferred_success_thunk) - site - 5);
    std::memcpy(replacement + 1, &rel, sizeof rel);
    // Single guarded write: failure publishes no observer. Original CALL return lands on our
    // padding NOP at469da2, then the original next instruction at469da3.
    if (!mh::hook::patch_bytes_guarded(site, expected, replacement, 6)) return false;
    installed = true;
    return true;
}
} // namespace mh::ui::selected_targets::detail
