#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "seams/avi_lifecycle.h"
#include "hook/detour.h"
#include "hook/patch.h"
#include "seams/net_internal.h"

// EN v425: 004c3216 cleared ctx+4 before testing its matching Leave; 004c2111
// could delete the same lock while owned. The worker also touches the format/tail
// outside the lock. Joining AFTER close cannot protect those accesses (G388).
namespace mh::avi_lifecycle {
namespace {
constexpr uintptr_t LOCK             = 0x0064461bu;
constexpr uintptr_t STOP             = 0x00644857u;
constexpr uintptr_t TASK_HANDLE      = 0x0064486bu;
constexpr uintptr_t NEXT_CALLBACK    = 0x006445f7u;
constexpr uintptr_t CURRENT_CALLBACK = 0x006542a9u;
constexpr uintptr_t SHUTDOWN_DONE    = 0x006572dau;
constexpr uintptr_t WORKER           = 0x004c2111u;
constexpr uintptr_t CLOSE            = 0x004c3216u;

HANDLE g_worker              = nullptr; // our duplicate, never the native pump's borrowed slot
HANDLE g_native_worker       = nullptr;
DWORD  g_worker_tid          = 0;
DWORD  g_generation          = 0;
bool   g_active              = false;
bool   g_initialized         = false;
bool   g_retiring            = false;
bool   g_shutdown_pending    = false;
HWND   g_close_pending       = nullptr;
void  *g_close_trampoline    = nullptr;
void  *g_boot_trampoline     = nullptr;
void  *g_shutdown_trampoline = nullptr;
void  *g_wndproc_trampoline  = nullptr;
bool   g_installed           = false;

[[noreturn]] void fail(const char *why) {
    char line[256];
    wsprintfA(line, "; fatal: AVI lifetime gen=%lu %s error=%lu\n", g_generation, why, GetLastError());
    seam_log(line);
    std::abort(); // unknown rundown must never authorize a free or another generation
}
void phase(const char *name) {
    char line[192];
    wsprintfA(line, "; avi-lifetime gen=%lu phase=%s main=%lu worker=%lu retained=%d\n",
              g_generation, name, GetCurrentThreadId(), g_worker_tid, g_worker != nullptr);
    seam_log(line);
}
void begin_retirement() {
    if (g_retiring) fail("recursive retirement");
    g_retiring = true;
    suppress_frames(true);
}
void end_retirement() {
    g_retiring = false;
    suppress_frames(false);
    HWND pending    = g_close_pending;
    g_close_pending = nullptr;
    // WM_CLOSE has no pointer payload. Posted input and WM_QUIT were never removed.
    if (pending && IsWindow(pending) && !PostMessageA(pending, WM_CLOSE, 0, 0)) fail("cannot defer WM_CLOSE");
}
void join_worker() {
    if (g_active || g_worker) *reinterpret_cast<volatile LONG *>(STOP) = 1;
    if (!g_worker) return;
    phase("stop-before-free");
    for (;;) {
        const DWORD state = WaitForSingleObject(g_worker, 0);
        if (state == WAIT_OBJECT_0) break;
        if (state != WAIT_TIMEOUT) fail("cannot query worker");
        const DWORD wake = MsgWaitForMultipleObjectsEx(1, &g_worker, INFINITE, QS_SENDMESSAGE, MWMO_INPUTAVAILABLE);
        if (wake == WAIT_OBJECT_0) break;
        if (wake != WAIT_OBJECT_0 + 1) fail("cannot wait for worker");
        // PeekMessage services incoming sent messages itself. One non-removing
        // service step, NOT a loop over a posted message that remains in the queue.
        // Paint is fenced; WndProc close and all native shutdown entries defer.
        MSG message;
        PeekMessageA(&message, nullptr, 0, 0, PM_NOREMOVE);
    }
    phase("worker-signaled-before-free");
    auto &native = *reinterpret_cast<HANDLE *>(TASK_HANDLE);
    // The only native owner is 004c3fd2: it closes-and-zeros finished handles,
    // otherwise leaves this original in the descriptor. No frame/pump runs here.
    if (native) {
        if (native != g_native_worker) fail("native worker slot changed owner");
        if (!CloseHandle(native)) fail("cannot close native worker handle");
        native = nullptr;
    }
    g_native_worker = nullptr;
}
void finish_owner() {
    if (g_worker && WaitForSingleObject(g_worker, 0) != WAIT_OBJECT_0) fail("worker not signaled at finalization");
    if (g_initialized) {
        auto *section = reinterpret_cast<CRITICAL_SECTION *>(LOCK);
        if (section->OwningThread) fail("AVI lock still owned after worker exit");
        DeleteCriticalSection(section);
        g_initialized = false;
    }
    if (g_worker && !CloseHandle(g_worker)) fail("cannot release retained worker");
    g_worker        = nullptr;
    g_native_worker = nullptr;
    g_active        = false;
}
void __cdecl finish_close() {
    finish_owner();
    phase("native-close-before-continuation");
    if (g_shutdown_pending) {
        *reinterpret_cast<uintptr_t *>(NEXT_CALLBACK)    = 0;
        *reinterpret_cast<uintptr_t *>(CURRENT_CALLBACK) = 0;
        // Bypass our deferring entry, not the native once guard. No campaign/menu
        // continuation is invoked by a shutdown requested during the join.
        reinterpret_cast<void(__cdecl *)()>(g_shutdown_trampoline)();
    }
    g_shutdown_pending = false;
    end_retirement();
}
int __cdecl before_close() {
    if (g_retiring) return 0; // movie tick retains its callback until the outer close completes
    if (*reinterpret_cast<const DWORD *>(SHUTDOWN_DONE)) {
        *reinterpret_cast<uintptr_t *>(NEXT_CALLBACK)    = 0;
        *reinterpret_cast<uintptr_t *>(CURRENT_CALLBACK) = 0;
        return 0; // shutdown already destroyed the surfaces; never reenter their close body
    }
    begin_retirement();
    join_worker();
    phase("native-close-enter");
    return 1;
}
int __cdecl before_boot() {
    if (g_retiring) fail("movie initialization reentered rundown");
    if (g_active) {
        // An explicit replacement already installed its NEW continuation before
        // entering boot_init. Dispose the previous movie without consuming it.
        auto           &pending = *reinterpret_cast<uintptr_t *>(NEXT_CALLBACK);
        const uintptr_t next    = pending;
        pending                 = 0;
        reinterpret_cast<int(__cdecl *)()>(CLOSE)();
        if (*reinterpret_cast<const DWORD *>(SHUTDOWN_DONE)) return 0;
        pending = next;
    }
    if (*reinterpret_cast<const DWORD *>(SHUTDOWN_DONE)) return 0;
    if (g_worker || g_initialized) fail("previous movie still owns resources");
    ++g_generation;
    g_active = true;
    phase("boot-enter");
    return 1;
}
int __cdecl before_shutdown() {
    if (g_retiring) {
        g_shutdown_pending = true;
        return 0; // also covers sent device-change / MCI callbacks, not just WM_CLOSE
    }
    begin_retirement();
    join_worker();
    finish_owner();
    phase("native-shutdown-enter");
    return 1;
}
void __cdecl after_shutdown() {
    g_shutdown_pending = false;
    end_retirement();
}

HANDLE WINAPI create_worker(LPSECURITY_ATTRIBUTES security, SIZE_T stack, LPTHREAD_START_ROUTINE start,
                            LPVOID parameter, DWORD flags, LPDWORD tid) {
    HANDLE created = CreateThread(security, stack, start, parameter, flags, tid);
    if (!created || reinterpret_cast<uintptr_t>(start) != WORKER) return created;
    if (g_worker || g_retiring || !g_active) fail("unexpected overlapping AVI worker");
    if (!DuplicateHandle(GetCurrentProcess(), created, GetCurrentProcess(), &g_worker, 0, FALSE, DUPLICATE_SAME_ACCESS))
        fail("cannot retain AVI worker");
    g_native_worker = created;
    g_worker_tid    = tid ? *tid : GetThreadId(g_worker);
    phase("worker-created");
    return created;
}
void WINAPI initialize_lock(LPCRITICAL_SECTION section) {
    if (reinterpret_cast<uintptr_t>(section) != LOCK || g_initialized || g_worker) fail("invalid AVI lock initialization");
    InitializeCriticalSection(section);
    g_initialized = true;
}
void WINAPI release_worker_lock(LPCRITICAL_SECTION section) {
    if (reinterpret_cast<uintptr_t>(section) != LOCK || !g_initialized) fail("invalid worker lock cleanup");
    // Native outer-stop cleanup owns no lock; its inner-stop edge owns one.
    // Never delete here: main owns deletion after the retained object signals.
    if (section->OwningThread == reinterpret_cast<HANDLE>(static_cast<uintptr_t>(GetCurrentThreadId())))
        LeaveCriticalSection(section);
}
LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (g_retiring && message == WM_CLOSE) {
        g_close_pending    = window;
        g_shutdown_pending = true;
        return 0; // before BOTH native shutdown and DestroyWindow
    }
    return reinterpret_cast<WNDPROC>(g_wndproc_trampoline)(window, message, wparam, lparam);
}

// The run-before thunks preserve Watcom's ECX/EDX and x87 state as well as the
// ordinary ABI. On a deferred close, EAX=0 means keep the async callback alive.
// clang-format off
__declspec(naked) void close_detour() {
    __asm {
        pushfd
        pushad
        sub esp, 108
        fnsave [esp]
        cld
        call before_close
        test eax, eax
        jz deferred
        frstor [esp]
        add esp, 108
        popad
        popfd
        jmp dword ptr [g_close_trampoline]
    deferred:
        frstor [esp]
        add esp, 108
        popad
        popfd
        mov eax, 0
        ret
    }
}
__declspec(naked) void boot_detour() {
    __asm {
        pushfd
        pushad
        sub esp, 108
        fnsave [esp]
        cld
        call before_boot
        test eax, eax
        jz cancelled
        frstor [esp]
        add esp, 108
        popad
        popfd
        jmp dword ptr [g_boot_trampoline]
    cancelled:
        frstor [esp]
        add esp, 108
        popad
        popfd
        mov eax, 0
        ret
    }
}
__declspec(naked) void before_continuation() {
    __asm {
        pushfd
        pushad
        sub esp, 108
        fnsave [esp]
        cld
        call finish_close
        frstor [esp]
        add esp, 108
        popad
        popfd
        cmp dword ptr ds:[0x006445f7], 0 // displaced CMP; the native JZ consumes THESE flags
        ret
    }
}
__declspec(naked) void shutdown_detour() {
    __asm {
        pushfd
        pushad
        sub esp, 108
        fnsave [esp]
        cld
        call before_shutdown
        test eax, eax
        jz deferred
        frstor [esp]
        add esp, 108
        popad
        popfd
        call dword ptr [g_shutdown_trampoline]
        pushfd
        pushad
        sub esp, 108
        fnsave [esp]
        cld
        call after_shutdown
        frstor [esp]
        add esp, 108
        popad
        popfd
        ret
    deferred:
        frstor [esp]
        add esp, 108
        popad
        popfd
        ret
    }
}
// clang-format on

template <size_t N>
bool replace_call(uintptr_t site, const uint8_t (&expected)[N], const void *target) {
    static_assert(N >= 5);
    uint8_t replacement[N];
    std::memset(replacement, 0x90, sizeof(replacement));
    replacement[0]         = 0xe8;
    const int32_t relative = static_cast<int32_t>(reinterpret_cast<uintptr_t>(target) - (site + 5));
    std::memcpy(replacement + 1, &relative, sizeof(relative));
    return mh::hook::patch_bytes_guarded(site, expected, replacement, sizeof(replacement));
}
} // namespace

bool install() {
    if (g_installed) return true;
    constexpr uint8_t init[]    = {0x2e, 0xff, 0x15, 0x2c, 0x04, 0x07, 0x01};
    constexpr uint8_t create[]  = {0x2e, 0xff, 0x15, 0xbc, 0x03, 0x07, 0x01};
    constexpr uint8_t destroy[] = {0x2e, 0xff, 0x15, 0xc0, 0x03, 0x07, 0x01};
    constexpr uint8_t proceed[] = {0x83, 0x3d, 0xf7, 0x45, 0x64, 0x00, 0x00};
    constexpr uint8_t enter[]   = {0x83, 0x3d, 0x3f, 0x46, 0x64, 0, 0, 0x74, 0x0d, 0xb8, 0x1b, 0x46, 0x64, 0, 0x50, 0x2e, 0xff, 0x15, 0xc8, 3, 7, 1};
    constexpr uint8_t leave[]   = {0x83, 0x3d, 0x3f, 0x46, 0x64, 0, 0, 0x74, 0x0d, 0xb8, 0x1b, 0x46, 0x64, 0, 0x50, 0x2e, 0xff, 0x15, 0x30, 4, 7, 1};
    // The boot and info players only read immutable media. Exclusive readers
    // collide across same-host instances after the boot lease has ended, entering
    // the native CD/error modal from Human selection. Keep denying writers.
    constexpr uint8_t exclusive_read[] = {0x6a, OF_SHARE_EXCLUSIVE};
    constexpr uint8_t shared_read[]    = {0x6a, OF_SHARE_DENY_WRITE};
    uint8_t           removed_lock[sizeof(enter)];
    std::memset(removed_lock, 0x90, sizeof(removed_lock));
    // The required caller aborts on any refusal before game entry; never run a
    // partially installed ownership protocol. Primitives adjudicate entry owners.
    if (!mh::hook::install_trampoline(CLOSE, reinterpret_cast<void *>(&close_detour), &g_close_trampoline, 8,
                                      mh::hook::entry_claim::exclusive, "AVI lifetime close") ||
        !mh::hook::install_trampoline(0x004c29d8, reinterpret_cast<void *>(&boot_detour), &g_boot_trampoline, 8,
                                      mh::hook::entry_claim::exclusive, "AVI lifetime boot") ||
        !mh::hook::install_trampoline(0x004b6ca2, reinterpret_cast<void *>(&shutdown_detour), &g_shutdown_trampoline, 8,
                                      mh::hook::entry_claim::exclusive, "AVI lifetime shutdown") ||
        !mh::hook::install_trampoline(0x004a0524, reinterpret_cast<void *>(&window_proc), &g_wndproc_trampoline, 8,
                                      mh::hook::entry_claim::exclusive, "AVI lifetime close deferral") ||
        !replace_call(0x004c30e5u, init, reinterpret_cast<void *>(&initialize_lock)) ||
        !replace_call(0x004c400du, create, reinterpret_cast<void *>(&create_worker)) ||
        !replace_call(0x004c23b3u, destroy, reinterpret_cast<void *>(&release_worker_lock)) ||
        !replace_call(0x004c3391u, proceed, reinterpret_cast<void *>(&before_continuation)) ||
        !mh::hook::patch_bytes_guarded(0x004c3245u, enter, removed_lock, sizeof(enter)) ||
        !mh::hook::patch_bytes_guarded(0x004c3292u, leave, removed_lock, sizeof(leave)) ||
        !mh::hook::patch_bytes_guarded(0x004cd07cu, exclusive_read, shared_read, sizeof(shared_read))) return false;
    g_installed = true;
    seam_log("; avi-lifetime armed: read-shared media; exact worker rundown before native context/free; main owns lock deletion\n");
    return true;
}
} // namespace mh::avi_lifecycle
