#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "../../libmh/orders/order_index_codec.h"
#include "addr/mh_structs.gen.h"
#include "caps/caps.h"
#include "caps/orders.h"
#include "en_guard.h"
#include "hook/patch.h"
#include "hook/promoted.h"
#include "include/mh_order_witness.h"
#include "state/region_runtime.h"
#include "ui/target_line_intents.h"
#include "ui/target_line_order_events.h"
#include "seams/order_tasks_native.h"
#include "seams/order_tasks_hosted.h"
extern "C" void order_tasks_dispatch_guard_gateway();

extern "C" int MH_OrderWideIndex_Enabled();

namespace mh::ui::selected_targets::detail {
namespace {

namespace W = mh::order_witness;
namespace O = mh::caps::orders;

using order = mh::game::mh_llm_strat_order;

static_assert(sizeof(W::canonical_order) == 60);
static_assert(offsetof(order, unit_index) == 8);
static_assert(sizeof(order) == O::REC_BYTES);
static_assert(O::QUEUE == 0 && O::PENDING == 1 && O::STAGING == 2);
static_assert(offsetof(mh::game::mh_map_object_unit, state) == 6);

// Filled only after the complete installed instruction passes preflight.
// Units BASE relocation is static; subsequent caps limit toggles do not
// change this displacement.
uint32_t cancel_state_displacement = 0;

W::store &witness() {
    auto *s = selected_targets_order_witness_store();
    if (!s) std::abort();
    return *s;
}

uint32_t word32(const uint8_t *frame, unsigned offset) {
    uint32_t result;
    std::memcpy(&result, frame - offset, sizeof(result));
    return result;
}

uint16_t low16(uint32_t value) {
    return static_cast<uint16_t>(value);
}

void copy_slot(W::store &s, W::container dst, uint32_t dst_slot,
               W::container src, uint32_t src_slot) {
    const W::handle h = W::pin(s, src, src_slot);
    W::copy(s, dst, dst_slot, h);
    if (h) W::unpin(s, h);
}

// At most four nested native dispatcher frames. Each holds one source pin.
// Individual copy operations and deferred dequeue hold one transient pin.
// These are bounded setup storage, not per-frame allocations.
//
// The common store separately enforces its global eight-pin allowance,
// including pins held by the translated carrier.
struct dispatch_frame {
    const uint8_t *frame         = nullptr;
    W::handle      source        = 0;
    uint32_t       generation    = 0;
    uint32_t       slot          = 0;
    uint32_t       retained_slot = 0;
    bool           retained      = false;
};

dispatch_frame dispatch_frames[4]{};
uint32_t       dispatch_depth = 0;

dispatch_frame &capture_frame(W::store &s, const uint8_t *frame) {
    if (dispatch_depth == 0 ||
        dispatch_frames[dispatch_depth - 1].frame != frame) {
        if (dispatch_depth == 4) std::abort();
        dispatch_frames[dispatch_depth++]         = {};
        dispatch_frames[dispatch_depth - 1].frame = frame;
    }

    auto &d = dispatch_frames[dispatch_depth - 1];
    if (d.source) std::abort(); // preceding iteration must have finalized
    d.slot          = word32(frame, 0x24);
    d.generation    = W::generation(s);
    d.retained      = false;
    d.retained_slot = 0;
    d.source        = W::pin(s, W::container::queue, d.slot);
    return d;
}

dispatch_frame &current_frame(const uint8_t *frame) {
    if (dispatch_depth == 0 ||
        dispatch_frames[dispatch_depth - 1].frame != frame)
        std::abort();
    return dispatch_frames[dispatch_depth - 1];
}

void finish_source(W::store &s, dispatch_frame &d) {
    // reset_world preserves pinned nodes until unpin, but their old-epoch
    // structural operations must not clear new-world slot ownership.
    if (d.generation == W::generation(s)) {
        if (!d.retained || d.retained_slot != d.slot)
            W::clear(s, W::container::queue, d.slot);
    }

    // Always unpin, including after a world reset. The common store suppresses
    // old-world outcomes and does not recycle a node while it remains pinned.
    if (d.source) W::unpin(s, d.source);
    d.source   = 0;
    d.retained = false;
}

void ambient_terminal(W::store &s, const uint8_t *frame) {
    const uint16_t state = low16(word32(frame, 0x18));
    if (state != 1 && state != 0x13) return;

    // These setters write through CUR_UNIT, not through CUR_PLAYER/CUR_INDEX.
    // Resolve the actual destination record instead of assuming the three
    // ambient globals necessarily agree.
    const auto *cur =
        mh::state::ptr<uint32_t>(mh::state::RID_STRAT_CUR_UNIT);
    if (!cur || !*cur) return;

    const auto &g = mh::caps::g(mh::caps::UNITS);
    if (!g.base || !g.elem || !g.row || *cur < g.base) return;

    const uintptr_t delta =
        static_cast<uintptr_t>(*cur) - static_cast<uintptr_t>(g.base);
    if (delta % g.elem != 0) return;

    const uintptr_t ordinal = delta / g.elem;
    const uintptr_t player  = ordinal / g.row;
    const uintptr_t slot    = ordinal % g.row;
    if (player >= g.players || slot >= g.limit) return;

    W::primary_terminal(s, static_cast<uint32_t>(player),
                        static_cast<uint32_t>(slot));
}

void explicit_terminal(W::store &s, const uint8_t *frame) {
    const uint16_t state = low16(word32(frame, 0x10));
    if (state != 1 && state != 0x13) return;

    // Explicit setters use full-width player/index in their address arithmetic.
    const uint32_t player = word32(frame, 0x18);
    const uint32_t slot   = word32(frame, 0x14);
    const auto    &g      = mh::caps::g(mh::caps::UNITS);
    if (player >= g.players || slot >= g.limit) return;

    W::primary_terminal(s, player, slot);
}

// Called only through the saved-machine-state gateway.
// Operation numbers are private to this translation unit.
void __cdecl observe_native(uint32_t operation, const uint8_t *frame) {
    auto &s = witness();

    switch (operation) {
        case 0: { // STAGING -> PENDING, after successful native INC
            const uint32_t n = O::live_count(O::PENDING);
            if (!n) std::abort();
            mh::order_tasks::native::retained_record(frame, O::STAGING, word32(frame, 0x24), false);
            copy_slot(s, W::container::pending, n - 1,
                      W::container::staging, word32(frame, 0x24));
            break;
        }

        case 1: // retained STAGING copy, before output cursor INC
            mh::order_tasks::native::retained_record(frame, O::STAGING, word32(frame, 0x24), true);
            copy_slot(s, W::container::staging, word32(frame, 0x20),
                      W::container::staging, word32(frame, 0x24));
            break;

        case 2: // STAGING count commit
            mh::order_tasks::hosted::retained_view(O::STAGING, 0, 0);
            W::clear_from(s, W::container::staging, word32(frame, 0x20));
            break;

        case 3: // actual PENDING -> QUEUE overwrite branch
            mh::order_tasks::native::retained_record(frame, O::PENDING, word32(frame, 0x24), false);
            mh::order_tasks::native::before_queue_overwrite(frame);
            copy_slot(s, W::container::queue, word32(frame, 0x30),
                      W::container::pending, word32(frame, 0x24));
            break;

        case 4: { // PENDING -> QUEUE append, after successful INC
            const uint32_t n = O::live_count(O::QUEUE);
            if (!n) std::abort();
            mh::order_tasks::native::retained_record(frame, O::PENDING, word32(frame, 0x24), false);
            copy_slot(s, W::container::queue, n - 1,
                      W::container::pending, word32(frame, 0x24));
            break;
        }

        case 5: // retained PENDING copy
            mh::order_tasks::native::retained_record(frame, O::PENDING, word32(frame, 0x24), true);
            copy_slot(s, W::container::pending, word32(frame, 0x20),
                      W::container::pending, word32(frame, 0x24));
            break;

        case 6: // per-player PENDING count commit
            mh::order_tasks::hosted::retained_view(O::PENDING, 0, 0);
            W::clear_from(s, W::container::pending, word32(frame, 0x20));
            break;

        case 7: // AFTER native queue wipe / caps counted refusal
            if (!O::on()) {
                W::clear_from(s, W::container::queue, 0);
                mh::order_tasks::native::lost_records(O::QUEUE, O::stock_cap(O::QUEUE));
            }
            break;

        case 8: // AFTER native remote-pending wipe / caps refusal
            if (!O::on()) {
                W::clear_from(s, W::container::pending, 0);
                mh::order_tasks::native::lost_records(O::PENDING, O::stock_cap(O::PENDING));
            }
            break;

        case 9:
            capture_frame(s, frame);
            break;

        case 10: { // dispatcher retained copy, including self-copy
            auto &d         = current_frame(frame);
            d.retained_slot = word32(frame, 0x20);
            d.retained      = true;
            if (d.generation == W::generation(s))
                W::copy(s, W::container::queue, d.retained_slot, d.source);
            break;
        }

        case 11: { // full ordinary primary application completed
            auto &d = current_frame(frame);
            mh::order_tasks::native::applied_dispatch(frame);
            if (d.generation == W::generation(s) && d.source)
                W::applied(s, d.source);
            break;
        }

        case 12:
            finish_source(s, current_frame(frame));
            mh::order_tasks::native::finish_dispatch(frame);
            break;

        case 13: { // dispatcher count commit, possibly an empty invocation
            if (dispatch_depth != 0 &&
                dispatch_frames[dispatch_depth - 1].frame == frame) {
                auto &d = dispatch_frames[dispatch_depth - 1];
                if (d.source) std::abort();
                if (d.generation == W::generation(s))
                    W::clear_from(s, W::container::queue,
                                  word32(frame, 0x20));
                dispatch_frames[--dispatch_depth] = {};
            } else {
                // An invocation with no visited records has no captured frame.
                W::clear_from(s, W::container::queue, word32(frame, 0x20));
            }
            break;
        }

        case 14:
            W::begin_move_group(s);
            break;

        case 15:
            W::end_move_group(s);
            break;

        case 16:
            ambient_terminal(s, frame);
            break;

        case 17:
            explicit_terminal(s, frame);
            break;

        default:
            std::abort();
    }
}

// Gateway entry stack:
//   +00 return into site stub
//   +04 private operation number
//
// PUSHAD frame:
//   +00 EDI, +04 ESI, +08 EBP, +0c saved ESP,
//   +10 EBX, +14 EDX, +18 ECX, +1c EAX,
//   +20 EFLAGS, +24 gateway return, +28 operation.
//
// Restoring the original FX state also restores the incoming MXCSR/XMM state.
// The native flags are restored unchanged. Any displaced arithmetic instruction
// runs outside this envelope, so its original flags remain authoritative.
__declspec(naked) void observer_gateway() {
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
        push dword ptr [esi + 08h]
        push dword ptr [esi + 28h]
        call observe_native
        add esp, 8
        fxrstor [esp]
        mov esp, esi
        popad
        popfd
        ret
    }
}

// Every site below is entered by JMP, not CALL.
// Consequently, epilogue splices may reproduce LEA ESP / POP instructions
// without accidentally discarding a patched-CALL return address.
#define OBSERVE_NATIVE(ID) \
    __asm push ID __asm call observer_gateway __asm lea esp, [esp + 4]

#define RESUME_NATIVE(VA) \
    __asm push VA __asm ret

__declspec(naked) void schedule_transfer() {
    __asm inc dword ptr ds : [005d01a8h] OBSERVE_NATIVE(0)
        RESUME_NATIVE(0046649fh)
}

__declspec(naked) void schedule_retain() {
    OBSERVE_NATIVE(1)
    __asm mov esi, dword ptr[ebp - 20h] __asm inc dword ptr[ebp - 20h] RESUME_NATIVE(004664f6h)
}

__declspec(naked) void schedule_tail() {
    OBSERVE_NATIVE(2)
    __asm mov dword ptr                       ds : [005d01a4h], eax
                                              RESUME_NATIVE(00466503h)
}

__declspec(naked) void release_overwrite() {
    OBSERVE_NATIVE(3)
    __asm mov ecx, 11h RESUME_NATIVE(004666efh)
}

__declspec(naked) void release_append() {
    __asm inc dword ptr ds : [005d0194h] OBSERVE_NATIVE(4)
        RESUME_NATIVE(00466733h)
}

__declspec(naked) void release_retain() {
    OBSERVE_NATIVE(5)
    __asm mov esi, dword ptr[ebp - 20h] __asm inc dword ptr[ebp - 20h] RESUME_NATIVE(00466756h)
}

__declspec(naked) void release_tail() {
    OBSERVE_NATIVE(6)
    __asm mov dword ptr                       ds : [005d01a8h], eax
                                              RESUME_NATIVE(00466763h)
}

__declspec(naked) void queue_overflow_completed() {
    OBSERVE_NATIVE(7)
    __asm mov dword ptr[ebp - 20h], 0 RESUME_NATIVE(00466164h)
}

__declspec(naked) void pending_overflow_completed() {
    OBSERVE_NATIVE(8)
    __asm mov dword ptr[ebp - 18h], 0 RESUME_NATIVE(00466803h)
}

__declspec(naked) void dispatch_source() {
    __asm mov dword ptr[ebp - 30h], eax OBSERVE_NATIVE(9) __asm call order_tasks_dispatch_guard_gateway __asm cmp dword ptr[ebp - 38h], 40h RESUME_NATIVE(0046690eh)
}

__declspec(naked) void dispatch_retain() {
    OBSERVE_NATIVE(10)
    __asm mov eax, dword ptr[ebp - 20h] __asm inc dword ptr[ebp - 20h] RESUME_NATIVE(00466c3eh)
}

__declspec(naked) void dispatch_applied() {
    OBSERVE_NATIVE(11)
    // Displaced instruction was JMP 004672a7, not fall-through.
    RESUME_NATIVE(004672a7h)
}

__declspec(naked) void dispatch_iteration_end() {
    OBSERVE_NATIVE(12)
    __asm mov eax, dword ptr[ebp - 24h] __asm inc dword ptr[ebp - 24h] RESUME_NATIVE(004668ceh)
}

__declspec(naked) void dispatch_tail() {
    OBSERVE_NATIVE(13)
    __asm mov eax, dword ptr[ebp - 20h] __asm mov dword ptr ds : [005d0194h], eax
                                                                                  RESUME_NATIVE(0046998ch)
}

// Deferred root: inputs already saved, no admission has happened.
__declspec(naked) void group_deferred_begin() {
    __asm mov dword ptr[ebp - 10h], 0 OBSERVE_NATIVE(14)
                                        RESUME_NATIVE(00444e85h)
}

__declspec(naked) void group_deferred_end() {
    OBSERVE_NATIVE(15)
    __asm lea          esp, [ebp - 0ch] __asm pop edi __asm pop esi __asm pop ecx
                       RESUME_NATIVE(00444fc9h)
}

__declspec(naked) void group_confirmed_begin() {
    __asm mov dword ptr[ebp - 18h], 0 OBSERVE_NATIVE(14)
                                        RESUME_NATIVE(00444fefh)
}

__declspec(naked) void group_confirmed_end() {
    OBSERVE_NATIVE(15)
    __asm lea          esp, [ebp - 10h] __asm pop edi __asm pop esi __asm pop ecx
                       RESUME_NATIVE(004451cfh)
}

__declspec(naked) void group_simple_begin() {
    __asm mov dword ptr[ebp - 14h], 0 OBSERVE_NATIVE(14)
                                        RESUME_NATIVE(00445322h)
}

__declspec(naked) void group_simple_end() {
    OBSERVE_NATIVE(15)
    __asm lea          esp, [ebp - 10h] __asm pop edi __asm pop esi __asm pop ecx
                       RESUME_NATIVE(004454a3h)
}

// All terminal callbacks occur AFTER the setter's actual final store.
// Callback input is the still-live native frame, not volatile return registers.
__declspec(naked) void terminal_state_order() {
    OBSERVE_NATIVE(16)
    __asm lea          esp, [ebp - 10h] __asm pop edi __asm pop esi
                       RESUME_NATIVE(00486693h)
}

__declspec(naked) void terminal_state() {
    OBSERVE_NATIVE(16)
    __asm lea          esp, [ebp - 14h] __asm pop edi __asm pop esi
                       RESUME_NATIVE(004866f6h)
}

__declspec(naked) void terminal_state_order_of() {
    OBSERVE_NATIVE(17)
    __asm lea          esp, [ebp - 08h] __asm pop edi __asm pop esi
                       RESUME_NATIVE(0048696dh)
}

__declspec(naked) void terminal_state_of() {
    OBSERVE_NATIVE(17)
    __asm lea          esp, [ebp - 0ch] __asm pop edi __asm pop esi
                       RESUME_NATIVE(004869eeh)
}

__declspec(naked) void dispatch_exit_cancel_applied() {
    // EAX is the original native player-row + unit-element byte offset.
    // Reproduce the displaced state store before publishing APPLIED.
    //
    // PUSH/MOV/POP do not alter flags. EDX is restored, so the original
    // ADD's flags and every register reach the observation gateway intact.
    __asm push edx __asm mov edx, dword ptr[cancel_state_displacement] __asm mov word ptr[eax + edx], 23h __asm pop edx

                                                                                                          OBSERVE_NATIVE(11) RESUME_NATIVE(00466bf9h)
}
#undef RESUME_NATIVE
#undef OBSERVE_NATIVE

struct site_patch {
    uintptr_t   site  = 0;
    uintptr_t   owner = 0;
    const void *thunk = nullptr;
    uint8_t     expected[9]{};
    uint8_t     replacement[9]{};
    uint32_t    length  = 0;
    bool        enabled = false;
    bool        applied = false;
};

template <size_t N>
site_patch make_site(uintptr_t site, uintptr_t owner, const void *thunk,
                     const char (&expected)[N]) {
    static_assert(N - 1 >= 5 && N - 1 <= 9);
    site_patch p{};
    p.site   = site;
    p.owner  = owner;
    p.thunk  = thunk;
    p.length = static_cast<uint32_t>(N - 1);
    std::memcpy(p.expected, expected, N - 1);
    return p;
}

#define NATIVE_SITE(SITE, OWNER, THUNK, BYTES) \
    make_site(SITE, OWNER, reinterpret_cast<const void *>(&THUNK), BYTES)

site_patch sites[] = {
    NATIVE_SITE(0x00466499u, 0x00466348u, schedule_transfer,
                "\xff\x05\xa8\x01\x5d\x00"),
    NATIVE_SITE(0x004664f0u, 0x00466348u, schedule_retain,
                "\x8b\x75\xe0\xff\x45\xe0"),
    NATIVE_SITE(0x004664feu, 0x00466348u, schedule_tail,
                "\xa3\xa4\x01\x5d\x00"),

    NATIVE_SITE(0x004666eau, 0x0046652eu, release_overwrite,
                "\xb9\x11\x00\x00\x00"),
    NATIVE_SITE(0x0046672du, 0x0046652eu, release_append,
                "\xff\x05\x94\x01\x5d\x00"),
    NATIVE_SITE(0x00466750u, 0x0046652eu, release_retain,
                "\x8b\x75\xe0\xff\x45\xe0"),
    NATIVE_SITE(0x0046675eu, 0x0046652eu, release_tail,
                "\xa3\xa8\x01\x5d\x00"),

    // Deliberately AFTER the caps-toggle-owned wipe/refusal instructions.
    NATIVE_SITE(0x0046615du, 0x00466094u, queue_overflow_completed,
                "\xc7\x45\xe0\x00\x00\x00\x00"),
    NATIVE_SITE(0x004667fcu, 0x00466790u, pending_overflow_completed,
                "\xc7\x45\xe8\x00\x00\x00\x00"),

    NATIVE_SITE(0x00466907u, 0x00466892u, dispatch_source,
                "\x89\x45\xd0\x83\x7d\xc8\x40"),
    NATIVE_SITE(0x00466c38u, 0x00466892u, dispatch_retain,
                "\x8b\x45\xe0\xff\x45\xe0"),
    NATIVE_SITE(0x00467113u, 0x00466892u, dispatch_applied,
                "\xe9\x8f\x01\x00\x00"),
    NATIVE_SITE(0x00466bf0u, 0x00466892u,
                dispatch_exit_cancel_applied,
                "\x66\xc7\x80\x4e\x8c\xdd\x00\x23\x00"),
    NATIVE_SITE(0x004668c8u, 0x00466892u, dispatch_iteration_end,
                "\x8b\x45\xdc\xff\x45\xdc"),
    NATIVE_SITE(0x00469984u, 0x00466892u, dispatch_tail,
                "\x8b\x45\xe0\xa3\x94\x01\x5d\x00"),

    NATIVE_SITE(0x00444e7eu, 0x00444e5fu, group_deferred_begin,
                "\xc7\x45\xf0\x00\x00\x00\x00"),
    NATIVE_SITE(0x00444fc3u, 0x00444e5fu, group_deferred_end,
                "\x8d\x65\xf4\x5f\x5e\x59"),
    NATIVE_SITE(0x00444fe8u, 0x00444fcbu, group_confirmed_begin,
                "\xc7\x45\xe8\x00\x00\x00\x00"),
    NATIVE_SITE(0x004451c9u, 0x00444fcbu, group_confirmed_end,
                "\x8d\x65\xf0\x5f\x5e\x59"),
    NATIVE_SITE(0x0044531bu, 0x004452feu, group_simple_begin,
                "\xc7\x45\xec\x00\x00\x00\x00"),
    NATIVE_SITE(0x0044549du, 0x004452feu, group_simple_end,
                "\x8d\x65\xf0\x5f\x5e\x59"),

    NATIVE_SITE(0x0048668eu, 0x00486657u, terminal_state_order,
                "\x8d\x65\xf0\x5f\x5e"),
    NATIVE_SITE(0x004866f1u, 0x004866c9u, terminal_state,
                "\x8d\x65\xec\x5f\x5e"),
    NATIVE_SITE(0x00486968u, 0x00486913u, terminal_state_order_of,
                "\x8d\x65\xf8\x5f\x5e"),
    NATIVE_SITE(0x004869e9u, 0x004869b0u, terminal_state_of,
                "\x8d\x65\xf4\x5f\x5e"),
};

#undef NATIVE_SITE

bool installed = false;

void rollback() {
    for (size_t i = sizeof(sites) / sizeof(sites[0]); i != 0;) {
        auto &p = sites[--i];
        if (!p.applied) continue;
        if (!mh::hook::patch_bytes_guarded(
                p.site, p.replacement, p.expected,
                static_cast<int>(p.length)))
            std::abort();
        p.applied = false;
    }
}

} // namespace

void native_order_admitted(uint32_t container) {
    if (container >= O::N) std::abort();
    const auto     which = static_cast<O::which>(container);
    const uint32_t n     = O::live_count(which);
    const uint8_t *base  = O::base(which);
    if (!n || !base) std::abort();

    W::canonical_order command{};
    std::memcpy(&command,
                base + static_cast<size_t>(n - 1) * O::REC_BYTES +
                    offsetof(order, unit_index),
                sizeof(command));

    if (which == O::STAGING) {
        command.unit_index =
            MH_OrderWideIndex_Enabled()
                ? mh::orders::decode_index(command.unit_index)
                : static_cast<uint16_t>(command.unit_index & 0xff);
        command.owner_and_kind &= 0xff;
        command.param0 = static_cast<int16_t>(
            static_cast<uint16_t>(command.param0) & 0xff);
        command.order_code &= 0xff;
    }

    W::admit(witness(), static_cast<W::container>(container), n - 1,
             command, which == O::PENDING);
}

void native_order_dequeued(uint32_t index, bool applied) {
    auto          &s = witness();
    const uint32_t n = O::live_count(O::QUEUE);
    if (index >= n) std::abort();

    const W::handle source = W::pin(s, W::container::queue, index);
    if (source && applied) W::applied(s, source);

    // Parallel presentation-only shift, before the native DEC/MOVSD loop.
    // No native records/counts are changed here.
    for (uint32_t j = index; j + 1 < n; ++j)
        copy_slot(s, W::container::queue, j,
                  W::container::queue, j + 1);
    W::clear(s, W::container::queue, n - 1);

    if (source) W::unpin(s, source);
}

void native_order_geometry_changed() {
    auto *s = selected_targets_order_witness_store();
    if (!s) return; // caps initial setup may precede observer allocation
    for (uint32_t i = 0; i < O::N; ++i) {
        const auto which = static_cast<O::which>(i);
        W::clear_from(*s, static_cast<W::container>(i), O::live_count(which));
    }
}

bool install_order_mutation_observers() {
    if (installed) return true;
    if (!mh::en_build_ok() || !selected_targets_order_witness_store())
        return false;

    // Preflight every site before publishing any partial group scope or
    // container mutation observer.
    for (auto &p : sites) {
        p.applied = false;
        p.enabled = !mh::hook::promoted_owner_of(p.owner);
        if (!p.enabled) continue;

        if (p.site == 0x00466bf0u) {
            // Validate the installed static relocation, not only the stock operand.
            const uint32_t expected_displacement =
                mh::caps::g(mh::caps::UNITS).base +
                static_cast<uint32_t>(
                    offsetof(mh::game::mh_map_object_unit, state));
            std::memcpy(p.expected + 3, &expected_displacement,
                        sizeof(expected_displacement));
        }

        if (mh::hook::promoted_owner_of(p.site) ||
            std::memcmp(reinterpret_cast<const void *>(p.site),
                        p.expected, p.length) != 0)
            return false;

        if (p.site == 0x00466bf0u) {
            // Snapshot only AFTER the full nine-byte opcode/operand/immediate guard.
            // The thunk owns this scalar; it never borrows an instruction pointer.
            std::memcpy(
                &cancel_state_displacement,
                reinterpret_cast<const uint8_t *>(p.site) + 3,
                sizeof(cancel_state_displacement));
        }

        std::memset(p.replacement, 0x90, sizeof(p.replacement));
        p.replacement[0]       = 0xe9;
        const int32_t relative = static_cast<int32_t>(
            reinterpret_cast<uintptr_t>(p.thunk) - p.site - 5);
        std::memcpy(p.replacement + 1, &relative, sizeof(relative));
    }

    for (auto &p : sites) {
        if (!p.enabled) continue;
        if (!mh::hook::patch_bytes_guarded_caps(
                p.site, p.expected, p.replacement,
                static_cast<int>(p.length))) {
            rollback();
            return false;
        }
        p.applied = true;
    }

    installed = true;
    return true;
}

order_metadata_edit begin_order_metadata_edit(uint32_t lane, uint32_t index) {
    order_metadata_edit edit{};
    auto               *store = selected_targets_order_witness_store();
    if (lane > 2 || !store || !W::valid(store) ||
        index >= O::live_count(static_cast<O::which>(lane))) return edit;
    edit.lane    = lane;
    edit.slot    = index;
    edit.binding = W::metadata_at(*store, static_cast<W::container>(lane), index);
    if (!edit.binding.value) return edit;
    const auto *records = reinterpret_cast<const order *>(O::base(static_cast<O::which>(lane)));
    edit.before         = W::canonical(records[index]);
    if (lane == O::STAGING) {
        edit.before.unit_index = MH_OrderWideIndex_Enabled()
                                     ? mh::orders::decode_index(edit.before.unit_index)
                                     : static_cast<uint16_t>(edit.before.unit_index & 0xff);
        edit.before.owner_and_kind &= 0xff;
        edit.before.order_code &= 0xff;
        edit.before.param0 = static_cast<int16_t>(static_cast<uint16_t>(edit.before.param0) & 0xff);
    }
    return edit;
}
void finish_order_metadata_edit(const order_metadata_edit &edit) {
    if (!edit.binding.value || edit.lane > 2) return;
    auto      *store = selected_targets_order_witness_store();
    const auto lane  = static_cast<O::which>(edit.lane);
    if (!store || !W::valid(store) || edit.slot >= O::live_count(lane)) return;
    const auto *records = reinterpret_cast<const order *>(O::base(lane));
    auto        after   = W::canonical(records[edit.slot]);
    if (lane == O::STAGING) {
        after.unit_index = MH_OrderWideIndex_Enabled()
                               ? mh::orders::decode_index(after.unit_index)
                               : static_cast<uint16_t>(after.unit_index & 0xff);
        after.owner_and_kind &= 0xff;
        after.order_code &= 0xff;
        after.param0 = static_cast<int16_t>(static_cast<uint16_t>(after.param0) & 0xff);
    }
    (void)W::amend_metadata(*store, static_cast<W::container>(edit.lane), edit.slot,
                            edit.binding, edit.before, after);
}
} // namespace mh::ui::selected_targets::detail
