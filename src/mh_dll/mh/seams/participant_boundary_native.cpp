#include "seams/participant_boundary_hosted.h"
#include "addr/mh_addrs.gen.h"
#include "addr/mh_calls.gen.h"
#include "addr/mh_regions.gen.h"
#include "addr/mh_structs.gen.h"
#include "hook/detour.h"
#include "hook/patch.h"
#include "hook/promoted.h"
#include "hook/watcall.h"
#include "lockstep/turn_engine.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>

namespace {
namespace pb = mh::participant_boundary;
namespace H  = pb::hosted;
using namespace mh::state;
using profile = mh::game::mh_llm_strat_player_profile;
struct machine {
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax, flags, return_to, target;
};
static_assert(sizeof(machine) == 44);
uint64_t            g_body_tokens[8]{};
uint32_t            g_body_depth = 0;
void               *g_emit_original[2]{};
constexpr uintptr_t g_emit_normal  = 0x0049dca3;
constexpr uintptr_t g_emit_timeout = 0x0049ddc1;

[[noreturn]] void fail(const char *why) {
    std::fprintf(stderr, "native participant boundary: %s\n", why);
    std::abort();
}
template <class T>
T load(const machine &m, uint32_t offset) {
    T value;
    std::memcpy(&value, reinterpret_cast<const void *>(m.ebp - offset), sizeof(value));
    return value;
}
void cursor_end(const machine &m, uint32_t len) {
    std::memcpy(reinterpret_cast<void *>(m.ebp - 0x40), &len, sizeof(len));
}
bool human(int index) {
    return index >= 0 && index < 8 && (ptr<const profile>(RID_STRAT_PLAYERS)[index].status_flags & 6u) == 6u;
}
int survivors(int target) {
    int n = 0;
    for (int i = 0; i < 8; ++i)
        if (i != target && human(i)) ++n;
    return n;
}
bool continuing(int index) {
    return index != *ptr<const uint16_t>(RID_PLAYERSIDE) && human(index) && survivors(index) > 1;
}
void mark_gone(int index) {
    auto &f = ptr<profile>(RID_STRAT_PLAYERS)[index].status_flags;
    f &= ~4u;
    f |= 0x10u;
    f |= 8u;
}
void notice(int index, bool consensus) {
    const auto *name  = ptr<const profile>(RID_STRAT_PLAYERS)[index].name;
    const auto  wide  = mh::hook::call_watcall1(0x004cf3e0, const_cast<char *>(name));
    using format_fn   = int(__cdecl *)(void *, const void *, const void *, const void *);
    const auto format = reinterpret_cast<format_fn>(0x004d0320);
    format(ptr<void>(RID_G_TEXT_TMP), reinterpret_cast<const void *>(consensus ? 0x005017bc : 0x005009e8),
           ptr<void *const>(RID_G_TEXT_PTRS)[166], reinterpret_cast<const void *>(wide));
    mh::call::llm_ui_print_floating_msg_red(ptr<void>(RID_G_TEXT_TMP));
}
void counters() {
    --*ptr<int32_t>(RID_NET_LOCKSTEP_PLAYER_COUNT);
    --*ptr<int32_t>(RID_NET_LOBBY_SCAN_HOST_COUNT);
}
enum class action : uint32_t { emit,
                               emit_timeout,
                               timeout,
                               consensus,
                               receive,
                               receive_unsynced };
struct removal {
    action   kind;
    int32_t  side, index;
    uint64_t horizon;
};
removal copy(const void *bytes, uint32_t size) {
    static_assert(std::is_trivially_copyable_v<removal>);
    if (!bytes || size != sizeof(removal)) fail("bad owned continuation");
    removal p;
    std::memcpy(&p, bytes, sizeof(p));
    return p;
}
void emit(const removal &p, bool timeout) {
    const int       which = timeout ? 1 : 0;
    const uintptr_t entry = g_emit_original[which] ? reinterpret_cast<uintptr_t>(g_emit_original[which]) : (timeout ? g_emit_timeout : g_emit_normal);
    mh::hook::call_watcall1(entry, reinterpret_cast<void *>(intptr_t(p.side)));
}
bool valid(const void *bytes, uint32_t size) {
    const auto p = copy(bytes, size);
    if (mh::call::llm_strat_player_by_side_id(p.side) != p.index || !human(p.index) ||
        pb::bits(ptr<const double>(RID_NET_PEER_HORIZON)[p.index]) != p.horizon) return false;
    if (p.kind == action::timeout || p.kind == action::consensus) {
        const auto *states = ptr<const uint8_t>(RID_NET_LOCKSTEP_PEER_STATE);
        const int   local  = *ptr<const uint16_t>(RID_PLAYERSIDE);
        if ((*ptr<const uint8_t>(RID_NET_LOCKSTEP_STATUS_FLAGS) & 0x80u) == 0 || states[p.index * 8 + local] != 0)
            return false;
        if (p.kind == action::consensus)
            for (int i = 0; i < 8; ++i)
                if (i != p.index && human(i) && states[p.index * 8 + i] == mh::lockstep::PEER_STATE_PRESENT)
                    return false;
    }
    return true;
}
bool terminal(const void *bytes, uint32_t size) {
    const auto p = copy(bytes, size);
    return survivors(p.index) <= 1;
}
bool execute(const void *bytes, uint32_t size) {
    const auto p = copy(bytes, size); // survives reentrant callbacks and match reset
    if (p.kind == action::emit || p.kind == action::emit_timeout) {
        emit(p, p.kind == action::emit_timeout);
        return true;
    }
    if (p.kind == action::timeout || p.kind == action::consensus) {
        emit(p, p.kind == action::timeout);
        notice(p.index, p.kind == action::consensus);
        *ptr<uint8_t>(RID_NET_LOCKSTEP_STATUS_FLAGS) &= 0x7fu;
        counters();
        if (p.kind == action::consensus) mh::call::llm_net_lockstep_sync_busywait();
        mh::call::llm_strat_time_resync_and_tick();
        return true;
    }
    mark_gone(p.index);
    if (mh::call::llm_net_lockstep_count_active_players() > 1) {
        mh::call::llm_net_lockstep_commit_horizon();
        counters();
        if (p.kind == action::receive) mh::call::llm_net_lockstep_sync_busywait();
        mh::call::llm_strat_time_resync_and_tick();
        return true;
    }
    mh::call::llm_strat_player_presence_lost(uint32_t(p.index), 1);
    mh::call::llm_ui_chat_recalc_target_mode();
    return false;
}
pb::admission submit(const removal &p, const machine *frame = nullptr) {
    pb::request r{};
    r.struct_size  = sizeof(r);
    r.slot_index   = p.index;
    r.horizon_bits = p.horizon;
    r.execute      = execute;
    r.payload      = &p;
    r.payload_size = sizeof(p);
    r.terminal     = terminal;
    if (frame) {
        const auto len = load<uint32_t>(*frame, 0x38), cursor = load<uint32_t>(*frame, 0x40);
        if (len > pb::TAIL_CAPACITY || cursor > len) fail("invalid receive continuation extent");
        r.received         = 1;
        r.tail_sender      = load<int32_t>(*frame, 0x34);
        r.tail_sender_slot = load<uint32_t>(*frame, 0x28);
        r.tail             = ptr<const uint8_t>(RID_NET_SEND_BUF) + cursor;
        r.tail_size        = len - cursor;
    } else r.validate = valid;
    const auto result = static_cast<pb::admission>(H::participant_removal_admit(&r, sizeof(r)));
    if (result == pb::admission::invalid) fail("owned removal admission refused");
    return result;
}

void __cdecl observe(uint32_t op, machine *m);
// All splice callbacks preserve GP, EFLAGS, x87 and XMM/MXCSR. The event slot becomes the
// continuation address; RET consumes it without changing the native stack or flags.
// clang-format off
__declspec(naked) void gateway() {
    __asm {
        pushfd
        pushad
        mov esi,esp
        sub esp,527
        and esp,-16
        fxsave [esp]
        fninit
        fldcw word ptr [esp]
        cld
        push esi
        push dword ptr [esi+28h]
        call observe
        add esp,8
        fxrstor [esp]
        mov esp,esi
        popad
        popfd
        ret
    }
}
#define EVENT(ID) __asm push ID __asm call gateway __asm ret
#define RESUME(VA) __asm push VA __asm ret
__declspec(naked) void return_only() { __asm ret }
__declspec(naked) void body_begin() { __asm sub esp,14h EVENT(1) }
__declspec(naked) void body_complete() { EVENT(2) }
__declspec(naked) void body_epilogue() {
    __asm lea esp,[ebp-14h] __asm pop edi __asm pop esi RESUME(0043fe19h)
}
__declspec(naked) void normal_entry() { EVENT(4) }
__declspec(naked) void timeout_entry() { EVENT(5) }
__declspec(naked) void timeout_whole() { EVENT(6) }
__declspec(naked) void consensus_whole() { EVENT(7) }
__declspec(naked) void receive_synced() { EVENT(8) }
__declspec(naked) void receive_unsynced() { EVENT(9) }
__declspec(naked) void timeout_replay() {
    __asm mov eax,[ebp-2ch] __asm call dword ptr[g_emit_timeout] RESUME(0043efbdh)
}
__declspec(naked) void consensus_replay() {
    __asm mov eax,[ebp-3ch] __asm call dword ptr[g_emit_normal] RESUME(0049c902h)
}
__declspec(naked) void synced_replay() { __asm imul eax,[ebp-30h],740h RESUME(0049cb44h) }
__declspec(naked) void unsynced_replay() { __asm imul eax,[ebp-30h],740h RESUME(0049cd8eh) }
// clang-format on

void __cdecl observe(uint32_t op, machine *m) {
    if (op == 1) {
        if (g_body_depth == 8) fail("native body scope overflow");
        const auto token = H::participant_body_boundary(uint32_t(pb::operation::begin), 0);
        if (!token) fail("actual native body entered with an unbound authority");
        g_body_tokens[g_body_depth++] = token;
        m->target                     = 0x0043f52a;
        return;
    }
    if (op == 2) {
        if (!g_body_depth) fail("unmatched native body completion");
        H::participant_body_boundary(uint32_t(pb::operation::complete), g_body_tokens[--g_body_depth]);
        m->target = uint32_t(reinterpret_cast<uintptr_t>(&body_epilogue));
        return;
    }
    if (op == 3) {
        H::poll();
        m->target = uint32_t(reinterpret_cast<uintptr_t>(&return_only));
        return;
    }
    removal        p{};
    const machine *rx     = nullptr;
    uintptr_t      replay = 0, skip = 0;
    if (op == 4 || op == 5) {
        p.kind  = op == 4 ? action::emit : action::emit_timeout;
        p.side  = int32_t(m->eax);
        p.index = mh::call::llm_strat_player_by_side_id(p.side);
        replay  = reinterpret_cast<uintptr_t>(g_emit_original[op - 4]);
        skip    = reinterpret_cast<uintptr_t>(&return_only);
    } else if (op == 6 || op == 7) {
        p.kind  = op == 6 ? action::timeout : action::consensus;
        p.side  = load<int32_t>(*m, op == 6 ? 0x2c : 0x3c);
        p.index = load<int32_t>(*m, op == 6 ? 0x28 : 0x30);
        replay  = reinterpret_cast<uintptr_t>(op == 6 ? &timeout_replay : &consensus_replay);
        skip    = op == 6 ? 0x0043f012 : 0x0049c95c;
    } else if (op == 8 || op == 9) {
        p.kind    = op == 8 ? action::receive : action::receive_unsynced;
        p.side    = load<int32_t>(*m, 0x3c);
        p.index   = load<int32_t>(*m, 0x30);
        p.horizon = load<uint64_t>(*m, 0x48);
        rx        = m;
        replay    = reinterpret_cast<uintptr_t>(op == 8 ? &synced_replay : &unsynced_replay);
        skip      = 0x0049d123;
    } else fail("unknown native carrier");
    m->target = uint32_t(replay);
    if (!continuing(p.index)) return; // original self/last-peer/terminal branch
    if (rx && !mh::lockstep::detail::x87_equal_or_unordered(pb::value(p.horizon),
                                                            ptr<const double>(RID_NET_PEER_HORIZON)[p.index])) return;
    if (!rx) p.horizon = pb::bits(ptr<const double>(RID_NET_PEER_HORIZON)[p.index]);
    const auto result = submit(p, rx);
    if (result == pb::admission::execute_now) return;
    if (rx) {
        cursor_end(*m, load<uint32_t>(*m, 0x38));
        if (result == pb::admission::applied_stop) skip = 0x0049d31e;
    }
    m->target = uint32_t(skip);
}
#undef EVENT
#undef RESUME

bool splice(uintptr_t address, const uint8_t *expected, int size, const void *target) {
    if (mh::hook::promoted_owner_of(address)) return true; // translated counterpart carries it
    uint8_t bytes[8];
    if (size < 5 || size > int(sizeof(bytes))) fail("invalid carrier extent");
    std::memset(bytes, 0x90, size);
    bytes[0]            = 0xe9;
    const int32_t delta = int32_t(reinterpret_cast<uintptr_t>(target) - (address + 5));
    std::memcpy(bytes + 1, &delta, sizeof(delta));
    return mh::hook::patch_bytes_guarded(address, expected, bytes, size);
}
} // namespace

extern "C" __declspec(naked) void MH_Participant_PollGateway() {
    __asm push 3 __asm call gateway __asm ret
}

namespace mh::participant_boundary::hosted {
bool install_native() {
    const uint8_t begin[]     = {0x81, 0xec, 0x14, 0, 0, 0};
    const uint8_t end[]       = {0x8d, 0x65, 0xec, 0x5f, 0x5e};
    const uint8_t timeout[]   = {0x8b, 0x45, 0xd4, 0xe8, 0x04, 0xee, 0x05, 0};
    const uint8_t consensus[] = {0x8b, 0x45, 0xc4, 0xe8, 0xa1, 0x13, 0, 0};
    const uint8_t receive[]   = {0x69, 0x45, 0xd0, 0x40, 0x07, 0, 0};
    bool          ok          = splice(0x0043f524, begin, sizeof(begin), &body_begin);
    ok                        = splice(0x0043fe14, end, sizeof(end), &body_complete) && ok;
    ok                        = splice(0x0043efb5, timeout, sizeof(timeout), &timeout_whole) && ok;
    ok                        = splice(0x0049c8fa, consensus, sizeof(consensus), &consensus_whole) && ok;
    ok                        = splice(0x0049cb3d, receive, sizeof(receive), &receive_synced) && ok;
    ok                        = splice(0x0049cd87, receive, sizeof(receive), &receive_unsynced) && ok;
    if (!mh::hook::promoted_owner_of(g_emit_normal))
        ok = mh::hook::install_trampoline(g_emit_normal, reinterpret_cast<void *>(&normal_entry),
                                          &g_emit_original[0], 8, mh::hook::entry_claim::exclusive, "completed-body normal removal") &&
             ok;
    if (!mh::hook::promoted_owner_of(g_emit_timeout))
        ok = mh::hook::install_trampoline(g_emit_timeout, reinterpret_cast<void *>(&timeout_entry),
                                          &g_emit_original[1], 8, mh::hook::entry_claim::exclusive, "completed-body timeout removal") &&
             ok;
    return ok;
}
} // namespace mh::participant_boundary::hosted
