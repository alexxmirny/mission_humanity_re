#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cfloat>
#include <cstring>

#include "seams/lobby_speed.h"
#include "addr/mh_export.gen.h"
#include "hook/detour.h"
#include "hook/promoted.h"
#include "include/mh_net_export.h"
#include "mh_net_proto/lobby_speed.h"
#include "state/region_runtime.h"
#include "sim/sim_game_speed_recompute.h"

namespace mh::seams::lobby_speed {
namespace {

SRWLOCK                  g_lock     = SRWLOCK_INIT;
bool                     g_open     = false;
bool                     g_host     = false;
std::uint32_t            g_tag      = 0;
std::uint32_t            g_revision = 0;
int                      g_display  = 100;
mh_net_proto::LobbySpeed g_pending;
bool                     g_have_pending = false;
bool                     g_publish      = false;
DWORD                    g_published_at = 0;
// The heartbeat may read the scalar on a worker thread; do not publish an unaligned double.
volatile LONG g_match                = 0;
void         *g_recompute_trampoline = nullptr;

bool scalar_ok(double scalar) noexcept {
    return scalar > 0.0 && scalar <= DBL_MAX;
}

int start_percent(const mh_net_proto::GameplaySet &set) noexcept {
    if (set.n < 0 || set.n > mh_net_proto::GAMEPLAY_MAX_ENTRIES) return 0;
    const std::uint32_t key     = mh_net_proto::gameplay_key_id("net", "lobby_speed_pct");
    int                 percent = 0;
    bool                found   = false;
    for (int i = 0; i < set.n; ++i) {
        const mh_net_proto::GameplayEntry &e = set.e[i];
        if (e.key_id != key) continue;
        if (found || e.type != mh_net_proto::GAMEPLAY_T_INT || !valid(e.i)) return 0;
        found   = true;
        percent = e.i;
    }
    return percent;
}

void seal(int percent) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_open         = false;
    g_have_pending = false;
    g_publish      = false;
    InterlockedExchange(&g_match, percent);
    ReleaseSRWLockExclusive(&g_lock);
    enforce();
}

void __cdecl after_native_recompute() noexcept {
    enforce();
}

// Original void __watcall(void), generated entry bytes 55 89 e5 68 20 00 00 00.
// Call the original once, then restore its complete integer/flags result around our pin.
// clang-format off
__declspec(naked) void recompute_detour() {
    __asm {
        call dword ptr [g_recompute_trampoline]
        pushad
        pushfd
        cld
        call after_native_recompute
        popfd
        popad
        ret
    }
}
// clang-format on

} // namespace

bool valid(int percent) noexcept {
    return mh_net_proto::lobby_speed_valid(percent);
}

void open(bool host, std::uint32_t lobby_tag) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_open         = true;
    g_host         = host;
    g_tag          = lobby_tag;
    g_revision     = host ? 1u : 0u;
    g_display      = 100;
    g_have_pending = false;
    g_publish      = host;
    g_published_at = 0;
    InterlockedExchange(&g_match, 0);
    ReleaseSRWLockExclusive(&g_lock);
}

void bind(std::uint32_t lobby_tag) noexcept {
    if (!lobby_tag) return;
    AcquireSRWLockExclusive(&g_lock);
    if (g_open && g_tag != lobby_tag) {
        // Host tag minting follows screen open. Never lose a click made before the first advert.
        // A client changes identity by open(false, tag) at JOIN, not by binding arbitrary adverts.
        if (g_host && g_tag == 0) {
            g_tag     = lobby_tag;
            g_publish = true;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

bool host_cycle() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    const bool can = g_open && g_host && g_match == 0 && g_revision != UINT32_MAX;
    if (can) {
        g_display = g_display == 200 ? 100 : g_display + 25;
        ++g_revision;
        g_publish = true;
    }
    ReleaseSRWLockExclusive(&g_lock);
    return can;
}

int display_percent() noexcept {
    AcquireSRWLockShared(&g_lock);
    const int percent = g_display;
    ReleaseSRWLockShared(&g_lock);
    return percent;
}

bool editable() noexcept {
    AcquireSRWLockShared(&g_lock);
    const bool can = g_open && g_host && g_match == 0 && g_revision != UINT32_MAX;
    ReleaseSRWLockShared(&g_lock);
    return can;
}

void receive(const unsigned char *bytes, int len) noexcept {
    mh_net_proto::LobbySpeed value;
    if (len < 0 || !mh_net_proto::lobby_speed_decode(bytes, static_cast<std::size_t>(len), value)) return;
    AcquireSRWLockExclusive(&g_lock);
    if (g_open && !g_host && g_match == 0 && value.lobby_tag == g_tag &&
        value.revision > g_revision && (!g_have_pending || value.revision > g_pending.revision)) {
        g_pending      = value;
        g_have_pending = true;
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void tick() noexcept {
    const DWORD              now = GetTickCount();
    mh_net_proto::LobbySpeed value;
    bool                     publish = false;
    AcquireSRWLockExclusive(&g_lock);
    if (g_open && g_match == 0) {
        if (!g_host && g_have_pending) {
            g_display      = g_pending.percent;
            g_revision     = g_pending.revision;
            g_have_pending = false;
        }
        // A periodic refresh serves a joining/reconnecting peer without another revision or queue.
        if (g_host && g_tag && MH_Net_IsStarted() &&
            (g_publish || static_cast<DWORD>(now - g_published_at) >= 1000u)) {
            value.lobby_tag = g_tag;
            value.revision  = g_revision;
            value.percent   = static_cast<std::uint16_t>(g_display);
            publish         = true;
            g_publish       = false;
            g_published_at  = now;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (publish) {
        std::uint8_t      bytes[mh_net_proto::LOBBY_SPEED_BYTES];
        const std::size_t n = mh_net_proto::lobby_speed_encode(value, bytes);
        MH_Net_SendAnnounce(bytes, static_cast<int>(n));
    }
}

bool validate_start(const mh_net_proto::GameplaySet &set) noexcept {
    return start_percent(set) != 0;
}

bool overlay_start(mh_net_proto::GameplaySet &set) noexcept {
    if (!validate_start(set)) return false;
    AcquireSRWLockShared(&g_lock);
    const bool can     = g_open && g_host && g_match == 0;
    const int  percent = g_display;
    ReleaseSRWLockShared(&g_lock);
    if (!can) return false;
    const std::uint32_t key = mh_net_proto::gameplay_key_id("net", "lobby_speed_pct");
    for (int i = 0; i < set.n; ++i) {
        if (set.e[i].key_id != key) continue;
        set.e[i].i      = percent;
        set.e[i].f_bits = 0;
        return true;
    }
    return false;
}

bool freeze_start(const mh_net_proto::GameplaySet &set) noexcept {
    const int percent = start_percent(set);
    if (!percent) return false;
    const int frozen = match_percent();
    if (frozen != 0) return frozen == percent;
    seal(percent);
    return true;
}

bool replay_enter(int recorded_percent) noexcept {
    if (recorded_percent == 0) {
        clear();
        return true;
    }
    if (!valid(recorded_percent)) return false;
    seal(recorded_percent);
    return true;
}

int match_percent() noexcept {
    return static_cast<int>(InterlockedCompareExchange(&g_match, 0, 0));
}

void enforce() noexcept {
    const int percent = match_percent();
    if (!percent) return;
    const double scalar = static_cast<double>(percent) / 100.0;
    std::memcpy(mh::state::ptr<double>(mh::state::RID_GAME_SPEED), &scalar, sizeof(scalar));
}

double effective_scalar(double native_scalar, double developer_pin) noexcept {
    const int percent = match_percent();
    if (percent) return static_cast<double>(percent) / 100.0;
    if (scalar_ok(developer_pin)) return developer_pin;
    return scalar_ok(native_scalar) ? native_scalar : 1.0;
}

double game_to_wall(double game_value, double scalar) noexcept {
    return scalar == 1.0 ? game_value : game_value / (scalar_ok(scalar) ? scalar : 1.0);
}

double wall_to_game(double wall_value, double scalar) noexcept {
    return scalar == 1.0 ? wall_value : wall_value * (scalar_ok(scalar) ? scalar : 1.0);
}

void clear() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_open         = false;
    g_host         = false;
    g_tag          = 0;
    g_revision     = 0;
    g_display      = 100;
    g_have_pending = false;
    g_publish      = false;
    g_published_at = 0;
    InterlockedExchange(&g_match, 0);
    ReleaseSRWLockExclusive(&g_lock);
}

bool install_native_recompute() {
    mh::sim::set_game_speed_recompute_observer(enforce);
    if (g_recompute_trampoline) return true;
    // The promoted live wrapper calls enforce directly; no shadowed original-body patch.
    if (mh::hook::promoted_owner_of(mh::exp::addr_llm_game_speed_recompute)) return true;
    return mh::hook::install_trampoline(mh::exp::addr_llm_game_speed_recompute, reinterpret_cast<void *>(recompute_detour),
                                        &g_recompute_trampoline, 8, mh::hook::entry_claim::exclusive,
                                        "LOBBYSPEED1 native speed recompute post-pin");
}

} // namespace mh::seams::lobby_speed

extern "C" __declspec(dllexport) int MH_LobbySpeed_ReplayEnter(int recorded_percent) {
    return mh::seams::lobby_speed::replay_enter(recorded_percent) ? 1 : 0;
}
