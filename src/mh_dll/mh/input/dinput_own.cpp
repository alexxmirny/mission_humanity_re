//
// dinput_own.cpp -- PT-INPUT1: mh.dll's own DirectInput 5 on Raw Input ([input] backend=own).
//
// ---- WHY -----------------------------------------------------------------------------------------
//
// The game reads the mouse and the keyboard through DirectInput 5, loaded at run time. On modern
// Windows DI5 is a compatibility layer that turns a hypervisor's or RDP's ABSOLUTE pointer into
// relative elements the game integrates and clamps -- the cursor pins at the screen edge and small
// counts truncate to nothing in the game's remainder-less per-element divide (measured 2026-08-24: 27%
// of traced frames at a clamp, 38% of event-frames with no motion). The only fix so far was a
// third-party wrapper (dinputto8) plus a magic divisor ([input] mouse_div=51). Owning the device
// removes both: absolute pointers are converted to the exact delta the game needs, relative ones keep
// their remainder.
//
// ---- THE SEAM: the game's own two-function DINPUT.DLL loader (the owned ddraw's shape) -----------
//
// llm_input_dinput_acquire (0x004d0894, EAX = the HINSTANCE) LoadLibraryA's "dinput.dll" into
// 0x0066154c, GetProcAddress'es DirectInputCreateA into 0x00661550, calls it as
// DirectInputCreateA(hinst, 0x0500, &0x00661554, NULL) and bumps the refcount at 0x00661558;
// llm_input_dinput_release (0x004d0904) decrements and, at zero, Releases the IDirectInput and
// FreeLibrary's. Every DirectInput object descends from 0x00661554: the keyboard init (0x004d0948) and
// the mouse create (0x004d0b88) are its only CreateDevice callers. Both loader functions are replaced
// whole (install_jmp) after an exact-bytes check of both entries (mh::exp::entry_*).
//
// ---- WHAT IS IMPLEMENTED: exactly the slots the binary calls ------------------------------------
//
// Read from the listing (2026-09-28): IDirectInputA Release (+0x08) and CreateDevice (+0x0c, with
// GUID_SysKeyboard 0x004d0730 / GUID_SysMouse 0x004d0720); IDirectInputDeviceA Release (+0x08),
// SetProperty (+0x18: DIPROP_BUFFERSIZE 32 keyboard / 256 mouse, DIPROP_AXISMODE = REL for the
// mouse), Acquire (+0x1c), Unacquire (+0x20), GetDeviceData (+0x28, cbObjectData 16, flags 0; the
// keyboard poll 0x004d0a48 asks for ONE element per call and loops, the mouse poll 0x004d0cc4 for 256
// into 0x00e65510), SetDataFormat (+0x2c, c_dfDIKeyboard 0x004f5d70 / c_dfDIMouse 0x004f5e00) and
// SetCooperativeLevel (+0x34: keyboard 6 = NONEXCLUSIVE|FOREGROUND; mouse 5 = EXCLUSIVE|FOREGROUND when
// llm_wnd_is_fullscreen, else 6). The ImGui overlay's drain calls GetDeviceData too. Every other slot is
// a real method -- the vtable shape is the SDK's dinput.h at DIRECTINPUT_VERSION 0x0500 -- that logs
// "unsupported vtable slot" once and returns DIERR_UNSUPPORTED (done_when (2) counts those lines).
//
// ---- RAW INPUT CAPTURE: at message RETRIEVAL, not in the window procedure -------------------------
//
// SetCooperativeLevel registers Raw Input (usage page 1, mouse 2 / keyboard 6) on the game window, flags
// 0: no RIDEV_INPUTSINK (input arrives only while the window is foreground -- what DISCL_FOREGROUND
// asks for anyway) and no RIDEV_NOLEGACY (WM_MOUSE* / WM_KEY* keep flowing, which the ImGui overlay and
// the game's own message fallback need). WM_INPUT is read by a thread-local WH_GETMESSAGE hook when the
// game's loop REMOVES the message from the queue, i.e. before ANY window procedure sees it. That order
// is the point: the owned ddraw's subclass and the ImGui overlay's subclass both sit on the window
// procedure, and the overlay decides "this input is mine" by draining the DI buffers before it forwards
// a message. Captured at retrieval, a packet is already in the device buffer when the overlay looks --
// the same position a system DirectInput event has (its hook fills the buffer before any message is
// dispatched). Captured inside a subclass instead, it would enter the buffer AFTER the outer overlay had
// drained and before the game's tap polled, i.e. it would leak through the overlay. No subclass is
// added, so the chain order of the other two does not change. Per message the cost is one comparison;
// per WM_INPUT, one GetRawInputData and a queue append.
//
// ---- MOUSE ---------------------------------------------------------------------------------------
//
//   * RELATIVE packets (a physical mouse) pass through as X / Y elements with the divisor's remainder
//     carried to the next packet (dinput_convert.h carry_step), so no motion is eaten whatever
//     [input] mouse_div says. The game's own doubling above the threshold stays: that is its feel.
//   * ABSOLUTE packets (MOUSE_MOVE_ABSOLUTE -- a hypervisor's pointer, RDP) are mapped to the game pixel
//     under the pointer (virtual desktop -> screen -> client -> the owned ddraw's image rect, or the
//     client scale when the owned ddraw is not mapping) and become a TARGET; each GetDeviceData walks
//     the game's live accumulator (_G_LLM_INPUT_MOUSE_LAST_X/Y) to it in steps of at most the doubling
//     threshold, each encoded as step * divisor, so the game's IDIV and doubling land it exactly. No
//     clamp residency, no truncation, no mouse_div.
//   * Buttons 0..2 and the wheel become their c_dfDIMouse elements, dwData 0x80 / 0 and the delta.
//   * DISCL_EXCLUSIVE is recorded but changes nothing: WM_MOUSE* keep flowing (ImGui is fed from
//     them), and the owned ddraw already clips the OS pointer to the image while the game is active.
//
// ---- KEYBOARD ------------------------------------------------------------------------------------
//
// RAWKEYBOARD make code -> DIK (E0 -> |0x80; Pause 0xC5; the fake-shift halves dropped), make / break
// -> dwData 0x80 / 0, dwTimeStamp from the message time (GetTickCount-based, the epoch the game
// subtracts), buffer = the DIPROP_BUFFERSIZE the game set (32). Auto-repeat makes are dropped (DI5 has
// none; the raw WM_KEYDOWN fallback's repeat was U25). On focus loss and on Unacquire every held key
// gets a synthetic key-up, delivered by the next GetDeviceData even though the device is no longer
// acquired, so no key sticks across Alt-Tab.
//
// ---- ACQUISITION ---------------------------------------------------------------------------------
//
// DISCL_FOREGROUND: Acquire succeeds (DI_OK, S_FALSE when already) only while the game window is the
// foreground window, else DIERR_OTHERAPPHASPRIO. Every GetDeviceData checks the foreground; losing it
// unacquires, discards the buffer, queues the synthetic key-ups and returns DIERR_INPUTLOST once, then
// DIERR_NOTACQUIRED -- exactly the two codes the game's polls answer with Acquire-and-retry.
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "mh_ini_gate.h" // RL2: the ship gate every ini read goes through
#define DIRECTINPUT_VERSION 0x0500
#include <dinput.h>
#include <stdint.h>
#include <string.h>

#include "input/dinput_own.h"
#include "input/dinput_convert.h"
#include "gfx/ddraw_own.h"          // owned_client_to_game / owned_game_to_client (the scaled window)
#include "addr/mh_export.gen.h"     // mh::exp::entry_llm_input_dinput_{acquire,release} -- the arm guards
#include "addr/mh_addrs.gen.h"      // the input globals the conversion reads
#include "include/mh_run_context.h" // mh_proc_path
#include "include/mh_log_sink.h"    // LOG1: async log sink
#include "config/ini_read.h"        // read_ini_string
#include "hook/detour.h"            // install_jmp
#include "en_guard.h"               // EN-only build gate

#pragma comment(lib, "user32.lib")

namespace mh::input {

namespace {

// ---- addresses (EN) -----------------------------------------------------------------------------
// The loader globals are raw VAs with provenance (the fixture-fingerprint trap: a manifest DATA entry
// becomes a hash region). The rest are already manifest entries.
constexpr uintptr_t ADDR_DI_MODULE   = 0x0066154cu; // HMODULE of dinput.dll (the loader's "loaded" test)
constexpr uintptr_t ADDR_DI_CREATE   = 0x00661550u; // DirectInputCreateA proc
constexpr uintptr_t ADDR_DI_OBJECT   = 0x00661554u; // IDirectInputA*
constexpr uintptr_t ADDR_DI_REFCOUNT = 0x00661558u; // loader refcount
constexpr uintptr_t ADDR_DI_KEYBOARD = 0x0066155cu; // _G_LLM_DI_KEYBOARD_DEVICE
constexpr uintptr_t ADDR_DI_MOUSE    = mh::addr::_G_LLM_DI_MOUSE_DEVICE;
constexpr uintptr_t ADDR_LAST_X      = mh::addr::_G_LLM_INPUT_MOUSE_LAST_X;
constexpr uintptr_t ADDR_LAST_Y      = mh::addr::_G_LLM_INPUT_MOUSE_LAST_Y;
constexpr uintptr_t ADDR_BOX_W       = 0x00e69da4u; // llm_input_mouse_init: the clamp box width
constexpr uintptr_t ADDR_BOX_H       = 0x00e69da8u; // ... and height
constexpr uintptr_t ADDR_DIVISOR     = mh::addr::_G_LLM_INPUT_DI_MOUSE_DIVISOR;
constexpr uintptr_t ADDR_THRESHOLD   = mh::addr::_G_LLM_INPUT_DI_MOUSE_ACCEL_THRESHOLD;
constexpr uintptr_t ADDR_KEY_EVENTS  = mh::addr::_G_LLM_INPUT_KEY_EVENTS; // stride 0x38: scan +0, type +4
constexpr uintptr_t ADDR_KEY_WRITE   = mh::addr::_G_LLM_INPUT_KEY_WRITE_IDX;

template <class T>
T &at(uintptr_t a) { return *reinterpret_cast<T *>(a); }

// ---- log: mh_input.log (mh_net.log's arm sequence is baseline-diffed) --------------------------

char          g_log_path[MAX_PATH];
unsigned long g_log_gen = 0;

void in_log(const char *fmt, ...) {
    mh_proc_path(g_log_path, MAX_PATH, "%smh_input.log", &g_log_gen);
    char    line[600];
    va_list ap;
    va_start(ap, fmt);
    wvsprintfA(line, fmt, ap);
    va_end(ap);
    int n = lstrlenA(line);
    if (n < (int)sizeof(line) - 2) {
        line[n++] = '\n';
        line[n]   = 0;
    }
    mh_logq_write(g_log_path, line, lstrlenA(line));
}

long g_unsupported = 0; // distinct unsupported slots reached (each logged once)

// One line per slot, the first time it is reached. The phrase is what done_when (2) greps for.
// clang-format off
#define MH_DI_UNSUPPORTED(what)                                                                   \
    do {                                                                                          \
        static bool once_ = false;                                                                \
        if (!once_) {                                                                             \
            once_ = true;                                                                         \
            ++g_unsupported;                                                                      \
            in_log("; [input] unsupported vtable slot " what " -- returned DIERR_UNSUPPORTED");   \
        }                                                                                         \
        return DIERR_UNSUPPORTED;                                                                 \
    } while (0)
// clang-format on

// ---- configuration ------------------------------------------------------------------------------

bool            g_active       = false;
bool            g_trace_m      = false; // [input] mouse_trace: per-element dwData lines + the size histogram
bool            g_trace_k      = false; // [input] key_trace: every key-ring event the game receives
HMODULE         g_self         = nullptr;
int             g_focus_ovr    = -1;      // harness_focus
long            g_raw          = 0;       // Raw Input packets captured (OS + harness)
long            g_harness      = 0;       // ... of which injected by the harness
uint32_t        g_seq          = 0;       // DI sequence numbers, one counter for both devices (as DI does)
mouse_filter_fn g_mouse_filter = nullptr; // set_mouse_filter: the ImGui overlay's

// ---- element trace (both backends) --------------------------------------------------------------
//
// The dinputto8 question (why the DI8 wrapper fixed the VM mouse was inferred from aggregate counts,
// never from the elements themselves): what size are the individual elements the game divides? One line per non-empty
// mouse poll, `X+12 Y-4 B0:80 Z+120`, plus a histogram of |dwData| over the X/Y elements every 5 s.

struct size_hist {
    long polls = 0, els = 0, xy = 0;
    long b[10] = {}; // |d| 0, 1, 2-3, 4-7, 8-15, 16-31, 32-63, 64-127, 128-255, 256+
    long sum   = 0;
};
size_hist g_hist;
DWORD     g_hist_ms         = 0;
long      g_hist_logged_els = -1;

void hist_add(int32_t d) {
    const uint32_t a = (uint32_t)(d < 0 ? -d : d);
    int            k = 0;
    if (a == 0) k = 0;
    else {
        k          = 1;
        uint32_t t = a;
        while (t > 1 && k < 9) {
            t >>= 1;
            ++k;
        }
    }
    ++g_hist.b[k];
    ++g_hist.xy;
    g_hist.sum += (long)a;
}

void hist_flush(const char *src, bool force) {
    const DWORD now = GetTickCount();
    if (!force && now - g_hist_ms < 5000) return;
    g_hist_ms = now;
    if (g_hist.els == g_hist_logged_els) return;
    g_hist_logged_els = g_hist.els;
    in_log("; [di-hist] src=%s polls=%ld elements=%ld xy=%ld mean|d|=%ld.%02ld per-poll=%ld.%02ld |d| buckets "
           "0:%ld 1:%ld 2-3:%ld 4-7:%ld 8-15:%ld 16-31:%ld 32-63:%ld 64-127:%ld 128-255:%ld 256+:%ld",
           src, g_hist.polls, g_hist.els, g_hist.xy, g_hist.xy ? g_hist.sum / g_hist.xy : 0,
           g_hist.xy ? (g_hist.sum * 100 / g_hist.xy) % 100 : 0, g_hist.polls ? g_hist.els / g_hist.polls : 0,
           g_hist.polls ? (g_hist.els * 100 / g_hist.polls) % 100 : 0, g_hist.b[0], g_hist.b[1], g_hist.b[2],
           g_hist.b[3], g_hist.b[4], g_hist.b[5], g_hist.b[6], g_hist.b[7], g_hist.b[8], g_hist.b[9]);
}

// `src` names the producer; `els` are the elements the game (or the overlay's drain) just received.
void trace_mouse_elements(const char *src, const di_element *els, DWORD n) {
    if (!g_trace_m || n == 0) return;
    ++g_hist.polls;
    g_hist.els += (long)n;
    for (DWORD i = 0; i < n; ++i)
        if (els[i].ofs == MOFS_X || els[i].ofs == MOFS_Y) hist_add((int32_t)els[i].data);
    char line[480];
    int  len = wsprintfA(line, "; [di-el] %s n=%lu t=%lu", src, n, els[0].timestamp);
    for (DWORD i = 0; i < n; ++i) {
        const di_element &e = els[i];
        const int32_t     d = (int32_t)e.data;
        if (len > (int)sizeof(line) - 24) {
            len += wsprintfA(line + len, " +%lu more", n - i);
            break;
        }
        switch (e.ofs) {
            case MOFS_X: len += wsprintfA(line + len, " X%c%ld", d < 0 ? '-' : '+', d < 0 ? -d : d); break;
            case MOFS_Y: len += wsprintfA(line + len, " Y%c%ld", d < 0 ? '-' : '+', d < 0 ? -d : d); break;
            case MOFS_Z: len += wsprintfA(line + len, " Z%c%ld", d < 0 ? '-' : '+', d < 0 ? -d : d); break;
            default: len += wsprintfA(line + len, " B%lu:%lx", e.ofs - MOFS_B0, e.data); break;
        }
    }
    in_log("%s", line);
    hist_flush(src, false);
}

// ---- the game window ----------------------------------------------------------------------------

HWND  g_hwnd      = nullptr; // the window SetCooperativeLevel named (the Raw Input target)
HHOOK g_msg_hook  = nullptr;
DWORD g_hook_tid  = 0;
bool  g_reg_mouse = false, g_reg_kbd = false;

bool window_focused(HWND h) {
    if (g_focus_ovr >= 0) return g_focus_ovr != 0;
    if (!h) return false;
    const HWND fg = GetForegroundWindow();
    return fg && (fg == h || fg == GetAncestor(h, GA_ROOT));
}

void register_raw(USHORT usage, HWND h, bool on) {
    RAWINPUTDEVICE rid;
    rid.usUsagePage = 0x01;
    rid.usUsage     = usage;
    rid.dwFlags     = on ? 0 : RIDEV_REMOVE;
    rid.hwndTarget  = on ? h : nullptr;
    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid)))
        in_log("; [input] RegisterRawInputDevices(usage %u, %s) FAILED (%lu)", (unsigned)usage, on ? "add" : "remove",
               GetLastError());
    else
        in_log("; [input] Raw Input %s: usage page 1 usage %u -> hwnd=%p (flags 0: no INPUTSINK, no NOLEGACY)",
               on ? "registered" : "removed", (unsigned)usage, (void *)h);
}

// ---- the devices --------------------------------------------------------------------------------

const GUID GUID_SYS_MOUSE    = {0x6F1D2B60, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
const GUID GUID_SYS_KEYBOARD = {0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
const GUID IID_DI_A          = {0x89521360, 0xAA8A, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
const GUID IID_DIDEV_A       = {0x5944E680, 0xC92E, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
const GUID IID_UNK           = {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

HRESULT qi_self(IUnknown *self, REFIID riid, void **out, const GUID &own_iid, const char *what) {
    if (!out) return E_POINTER;
    if (riid == IID_UNK || riid == own_iid) {
        self->AddRef();
        *out = self;
        return S_OK;
    }
    *out = nullptr;
    ++g_unsupported;
    in_log("; [input] unsupported vtable slot %s::QueryInterface for {%08lx-...} -- returned E_NOINTERFACE", what,
           riid.Data1);
    return E_NOINTERFACE;
}

struct OwnDevice;
OwnDevice *g_mouse = nullptr; // the newest device of each kind (the one Raw Input feeds)
OwnDevice *g_kbd   = nullptr;

struct key_raw {
    uint8_t  dik;
    uint8_t  down;
    uint32_t time;
};

struct OwnDevice final : IDirectInputDeviceA {
    LONG  ref       = 1;
    bool  is_mouse  = false;
    bool  format    = false; // SetDataFormat seen
    bool  acquired  = false;
    bool  lost      = false; // report DIERR_INPUTLOST once after a focus loss
    bool  fg_only   = true;  // DISCL_FOREGROUND
    DWORD coop      = 0;
    DWORD bufsize   = 0; // DIPROP_BUFFERSIZE; 0 = unbuffered
    HWND  hwnd      = nullptr;
    long  delivered = 0;

    // mouse
    mouse_queue mq;
    // keyboard
    static constexpr int KCAP = 256;
    key_raw              kq[KCAP];
    int                  kh = 0, kt = 0, kn = 0;
    bool                 k_overflow = false;
    key_tracker          keys;
    uint8_t              synth[256]; // pending synthetic key-ups (delivered even while unacquired)
    int                  nsynth = 0;

    explicit OwnDevice(bool mouse) : is_mouse(mouse) {}

    bool is_current() const {
        return is_mouse ? at<void *>(ADDR_DI_MOUSE) == (const void *)this : at<void *>(ADDR_DI_KEYBOARD) == (const void *)this;
    }

    void push_key(uint8_t dik, bool down, uint32_t time) {
        if (!keys.accept(dik, down)) return; // auto-repeat
        const int cap = bufsize > 0 && bufsize < (DWORD)KCAP ? (int)bufsize : KCAP;
        if (kn >= cap) {
            k_overflow = true;
            return;
        }
        kq[kt] = {dik, (uint8_t)(down ? 1 : 0), time};
        kt     = (kt + 1) % KCAP;
        ++kn;
    }

    // Release every held key: the ups wait in `synth` for the next GetDeviceData.
    void release_held(const char *why) {
        if (is_mouse) return;
        uint8_t   ks[256];
        const int n = keys.release_all(ks, 256);
        for (int i = 0; i < n && nsynth < 256; ++i) synth[nsynth++] = ks[i];
        if (n) in_log("; [input] keyboard: %d held key(s) released by synthetic key-up (%s)", n, why);
    }

    void lose(const char *why) {
        if (!acquired) return;
        acquired = false;
        lost     = true;
        if (is_mouse) mq.clear();
        else kh = kt = kn = 0;
        release_held(why);
    }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override {
        return qi_self(this, riid, out, IID_DIDEV_A, "IDirectInputDeviceA");
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG r = InterlockedDecrement(&ref);
        if (r == 0) {
            if (g_mouse == this) {
                g_mouse = nullptr;
                if (g_reg_mouse) register_raw(2, nullptr, false), g_reg_mouse = false;
            }
            if (g_kbd == this) {
                g_kbd = nullptr;
                if (g_reg_kbd) register_raw(6, nullptr, false), g_reg_kbd = false;
            }
            in_log("; [input] %s device released (%ld elements delivered)", is_mouse ? "mouse" : "keyboard", delivered);
            delete this;
        }
        return (ULONG)r;
    }

    // IDirectInputDeviceA -- the slots the game calls
    HRESULT STDMETHODCALLTYPE SetProperty(REFGUID prop, LPCDIPROPHEADER ph) override {
        const uintptr_t id = (uintptr_t)&prop;
        if (!ph || ph->dwHeaderSize != sizeof(DIPROPHEADER)) return DIERR_INVALIDPARAM;
        if (id == 1) { // DIPROP_BUFFERSIZE
            if (acquired) return DIERR_ACQUIRED;
            bufsize = ((const DIPROPDWORD *)ph)->dwData;
            return DI_OK;
        }
        if (id == 2) { // DIPROP_AXISMODE
            const DWORD mode = ((const DIPROPDWORD *)ph)->dwData;
            if (mode != DIPROPAXISMODE_REL)
                in_log("; [input] mouse DIPROP_AXISMODE=%lu requested -- only relative is produced", mode);
            return DI_OK;
        }
        in_log("; [input] SetProperty(%lu) not implemented", (unsigned long)id);
        MH_DI_UNSUPPORTED("IDirectInputDeviceA::SetProperty(other property)");
    }

    HRESULT STDMETHODCALLTYPE Acquire() override {
        if (!format) return DIERR_INVALIDPARAM;
        if (fg_only && !window_focused(hwnd)) return DIERR_OTHERAPPHASPRIO;
        if (acquired) return S_FALSE;
        acquired = true;
        lost     = false;
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE Unacquire() override {
        if (!acquired) return DI_NOEFFECT;
        acquired = false;
        if (is_mouse) mq.clear();
        else kh = kt = kn = 0;
        release_held("Unacquire");
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDeviceData(DWORD cb, LPDIDEVICEOBJECTDATA rgdod, LPDWORD inout, DWORD flags) override {
        if (!inout) return DIERR_INVALIDPARAM;
        if (cb != sizeof(di_element)) {
            static bool once = false;
            if (!once) once = true, in_log("; [input] GetDeviceData cbObjectData=%lu refused (DI5 is 16)", cb);
            return DIERR_INVALIDPARAM;
        }
        if (flags & DIGDD_PEEK) MH_DI_UNSUPPORTED("IDirectInputDeviceA::GetDeviceData(DIGDD_PEEK)");
        if (!bufsize) return DIERR_NOTBUFFERED;
        di_element *out = reinterpret_cast<di_element *>(rgdod);
        const int   cap = out ? (int)(*inout > 0x7fffffff ? 0x7fffffff : *inout) : 0x7fffffff;
        *inout          = 0;
        int n           = 0;

        if (acquired && fg_only && !window_focused(hwnd)) lose("focus lost");
        // Synthetic key-ups first, acquired or not: a key released while we were away must still reach
        // the game, or it sticks.
        while (nsynth > 0 && n < cap) {
            if (out) out[n] = {synth[0], 0, GetTickCount(), ++g_seq};
            memmove(synth, synth + 1, (size_t)--nsynth);
            ++n;
        }
        if (!acquired) {
            *inout = (DWORD)n;
            delivered += n;
            if (n) return DI_OK;
            if (lost) {
                lost = false;
                return DIERR_INPUTLOST;
            }
            return DIERR_NOTACQUIRED;
        }
        bool overflow = false;
        if (is_mouse) {
            mouse_view v;
            v.last_x = at<int>(ADDR_LAST_X);
            v.last_y = at<int>(ADDR_LAST_Y);
            v.box_w  = at<int>(ADDR_BOX_W);
            v.box_h  = at<int>(ADDR_BOX_H);
            v.div    = at<int>(ADDR_DIVISOR);
            v.thr    = at<int>(ADDR_THRESHOLD);
            n += mq.produce(v, out ? out + n : nullptr, cap - n, g_seq);
            overflow = mq.take_overflow();
        } else {
            while (kn > 0 && n < cap) {
                const key_raw &k = kq[kh];
                if (out) out[n] = {k.dik, k.down ? 0x80u : 0u, k.time, ++g_seq};
                kh = (kh + 1) % KCAP;
                --kn;
                ++n;
            }
            overflow   = k_overflow;
            k_overflow = false;
        }
        // Traced BEFORE the filter: the histogram is what the device produced, the dinputto8 question.
        if (is_mouse && out) trace_mouse_elements("own", out, (DWORD)n);
        DWORD got = (DWORD)n;
        if (is_mouse && out && g_mouse_filter && got) g_mouse_filter(out, &got);
        *inout = got;
        delivered += n;
        return overflow ? DI_BUFFEROVERFLOW : DI_OK;
    }

    HRESULT STDMETHODCALLTYPE SetDataFormat(LPCDIDATAFORMAT df) override {
        if (!df) return DIERR_INVALIDPARAM;
        if (acquired) return DIERR_ACQUIRED;
        // c_dfDIMouse's state is a DIMOUSESTATE (16 bytes), c_dfDIKeyboard's a 256-byte array.
        const DWORD want = is_mouse ? 16u : 256u;
        if (df->dwDataSize != want)
            in_log("; [input] %s SetDataFormat dwDataSize=%lu (expected %lu) -- accepted, the elements are fixed",
                   is_mouse ? "mouse" : "keyboard", df->dwDataSize, want);
        format = true;
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND h, DWORD f) override;

    // The rest: real methods of the SDK shape, unsupported.
    HRESULT STDMETHODCALLTYPE GetCapabilities(LPDIDEVCAPS) override { MH_DI_UNSUPPORTED("IDirectInputDeviceA::GetCapabilities"); }
    HRESULT STDMETHODCALLTYPE EnumObjects(LPDIENUMDEVICEOBJECTSCALLBACKA, LPVOID, DWORD) override {
        MH_DI_UNSUPPORTED("IDirectInputDeviceA::EnumObjects");
    }
    HRESULT STDMETHODCALLTYPE GetProperty(REFGUID, LPDIPROPHEADER) override { MH_DI_UNSUPPORTED("IDirectInputDeviceA::GetProperty"); }
    HRESULT STDMETHODCALLTYPE GetDeviceState(DWORD, LPVOID) override { MH_DI_UNSUPPORTED("IDirectInputDeviceA::GetDeviceState"); }
    HRESULT STDMETHODCALLTYPE SetEventNotification(HANDLE) override {
        MH_DI_UNSUPPORTED("IDirectInputDeviceA::SetEventNotification");
    }
    HRESULT STDMETHODCALLTYPE GetObjectInfo(LPDIDEVICEOBJECTINSTANCEA, DWORD, DWORD) override {
        MH_DI_UNSUPPORTED("IDirectInputDeviceA::GetObjectInfo");
    }
    HRESULT STDMETHODCALLTYPE GetDeviceInfo(LPDIDEVICEINSTANCEA) override { MH_DI_UNSUPPORTED("IDirectInputDeviceA::GetDeviceInfo"); }
    HRESULT STDMETHODCALLTYPE RunControlPanel(HWND, DWORD) override { MH_DI_UNSUPPORTED("IDirectInputDeviceA::RunControlPanel"); }
    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE, DWORD, REFGUID) override { MH_DI_UNSUPPORTED("IDirectInputDeviceA::Initialize"); }
};

// ---- Raw Input packets -> the devices -----------------------------------------------------------

// The game pixel under an absolute packet: virtual desktop (or primary screen) -> screen -> client ->
// the owned ddraw's image rect (a scaled window), else the client scaled to the game's clamp box.
bool abs_to_game(bool virtual_desktop, LONG nx, LONG ny, HWND h, int *gx, int *gy) {
    int ox = 0, oy = 0, ew, eh;
    if (virtual_desktop) {
        ox = GetSystemMetrics(SM_XVIRTUALSCREEN);
        oy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        ew = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        eh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    } else {
        ew = GetSystemMetrics(SM_CXSCREEN);
        eh = GetSystemMetrics(SM_CYSCREEN);
    }
    POINT p = {norm_to_pixel(nx, ox, ew), norm_to_pixel(ny, oy, eh)};
    if (!h || !ScreenToClient(h, &p)) return false;
    if (!mh::gfx::owned_client_to_game(&p)) {
        RECT      cr;
        const int bw = at<int>(ADDR_BOX_W), bh = at<int>(ADDR_BOX_H);
        if (GetClientRect(h, &cr) && cr.right > 0 && cr.bottom > 0 && bw > 0 && bh > 0) {
            p.x = (LONG)((long long)p.x * bw / cr.right);
            p.y = (LONG)((long long)p.y * bh / cr.bottom);
        }
    }
    *gx = p.x;
    *gy = p.y;
    return true;
}

// Accept a packet only for the device the game is actually using (a device the harness's
// mouse_absolute knob orphaned would otherwise buffer for ever) and only while it is acquired (DI does
// not buffer an unacquired device). Harness packets are queued regardless of acquisition: a headless
// script injects right after `rawfocus 1`, before the game's next poll has re-acquired.
OwnDevice *live(OwnDevice *d, bool harness) {
    if (!d || !d->is_current()) return nullptr;
    if (!harness && !d->acquired) return nullptr;
    return d;
}

void on_raw_mouse(const RAWMOUSE &m, uint32_t time, bool harness) {
    OwnDevice *d = live(g_mouse, harness);
    if (!d) return;
    ++g_raw;
    const USHORT bf       = m.usButtonFlags;
    const bool   absolute = (m.usFlags & MOUSE_MOVE_ABSOLUTE) != 0;
    if (absolute) {
        // A button-only packet from some absolute sources carries 0,0: not a move to the corner.
        if (!(bf != 0 && m.lLastX == 0 && m.lLastY == 0)) {
            int gx = 0, gy = 0;
            if (abs_to_game((m.usFlags & MOUSE_VIRTUAL_DESKTOP) != 0, m.lLastX, m.lLastY, d->hwnd, &gx, &gy))
                d->mq.push({mouse_raw::abs, gx, gy, time});
        }
    } else if (m.lLastX || m.lLastY) {
        d->mq.push({mouse_raw::rel, m.lLastX, m.lLastY, time});
    }
    static const struct {
        USHORT   down, up;
        uint32_t ofs;
    } BTN[3] = {{RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_LEFT_BUTTON_UP, MOFS_B0},
                {RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_UP, MOFS_B1},
                {RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP, MOFS_B2}};
    for (const auto &b : BTN) {
        if (bf & b.down) d->mq.push({mouse_raw::button, (int32_t)b.ofs, 1, time});
        if (bf & b.up) d->mq.push({mouse_raw::button, (int32_t)b.ofs, 0, time});
    }
    if (bf & RI_MOUSE_WHEEL) d->mq.push({mouse_raw::wheel, (int32_t)(SHORT)m.usButtonData, 0, time});
}

void on_raw_key(const RAWKEYBOARD &k, uint32_t time, bool harness) {
    OwnDevice *d = live(g_kbd, harness);
    if (!d) return;
    ++g_raw;
    const uint8_t dik = scan_to_dik(k.MakeCode, (k.Flags & RI_KEY_E0) != 0, (k.Flags & RI_KEY_E1) != 0, k.VKey);
    if (!dik) return;
    d->push_key(dik, (k.Flags & RI_KEY_BREAK) == 0, time);
}

LRESULT CALLBACK getmsg_proc(int code, WPARAM w, LPARAM l) {
    if (code == HC_ACTION && w == PM_REMOVE) {
        const MSG *msg = (const MSG *)l;
        if (msg->message == WM_INPUT) {
            alignas(8) BYTE buf[128];
            UINT            sz = sizeof(buf);
            if (GetRawInputData((HRAWINPUT)msg->lParam, RID_INPUT, buf, &sz, sizeof(RAWINPUTHEADER)) != (UINT)-1) {
                const RAWINPUT *ri = (const RAWINPUT *)buf;
                if (ri->header.dwType == RIM_TYPEMOUSE) on_raw_mouse(ri->data.mouse, msg->time, false);
                else if (ri->header.dwType == RIM_TYPEKEYBOARD) on_raw_key(ri->data.keyboard, msg->time, false);
            }
        }
    }
    return CallNextHookEx(g_msg_hook, code, w, l);
}

HRESULT STDMETHODCALLTYPE OwnDevice::SetCooperativeLevel(HWND h, DWORD f) {
    if (acquired) return DIERR_ACQUIRED;
    coop    = f;
    fg_only = (f & DISCL_BACKGROUND) == 0;
    hwnd    = h;
    g_hwnd  = h;
    in_log("; [input] %s SetCooperativeLevel(hwnd=%p, 0x%lx = %s|%s)%s", is_mouse ? "mouse" : "keyboard", (void *)h, f,
           (f & DISCL_EXCLUSIVE) ? "EXCLUSIVE" : "NONEXCLUSIVE", (f & DISCL_BACKGROUND) ? "BACKGROUND" : "FOREGROUND",
           (f & DISCL_EXCLUSIVE) ? " -- exclusive recorded; WM_MOUSE* keep flowing (the ImGui overlay reads them)" : "");
    // Raw Input on this window, once per kind (a later device of the same kind re-targets it).
    if (is_mouse) {
        register_raw(2, h, true);
        g_reg_mouse = true;
    } else {
        register_raw(6, h, true);
        g_reg_kbd = true;
    }
    // The retrieval hook, on the window's own thread (the game's single message loop).
    const DWORD tid = h ? GetWindowThreadProcessId(h, nullptr) : GetCurrentThreadId();
    if (!g_msg_hook || g_hook_tid != tid) {
        if (g_msg_hook) UnhookWindowsHookEx(g_msg_hook);
        g_msg_hook = SetWindowsHookExA(WH_GETMESSAGE, &getmsg_proc, nullptr, tid);
        g_hook_tid = tid;
        in_log("; [input] WH_GETMESSAGE hook on thread %lu: %s", tid,
               g_msg_hook ? "installed (WM_INPUT read at retrieval, before any window procedure)" : "FAILED");
    }
    return DI_OK;
}

// ---- IDirectInputA ------------------------------------------------------------------------------

struct OwnDI final : IDirectInputA {
    LONG ref = 1;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override {
        return qi_self(this, riid, out, IID_DI_A, "IDirectInputA");
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG r = InterlockedDecrement(&ref);
        if (r == 0) delete this;
        return (ULONG)r;
    }
    HRESULT STDMETHODCALLTYPE CreateDevice(REFGUID g, LPDIRECTINPUTDEVICEA *out, LPUNKNOWN) override {
        if (!out) return DIERR_INVALIDPARAM;
        *out = nullptr;
        bool mouse;
        if (g == GUID_SYS_MOUSE) mouse = true;
        else if (g == GUID_SYS_KEYBOARD) mouse = false;
        else {
            in_log("; [input] CreateDevice for {%08lx-...} refused -- only SysMouse / SysKeyboard are owned", g.Data1);
            return DIERR_DEVICENOTREG;
        }
        OwnDevice *d              = new OwnDevice(mouse);
        (mouse ? g_mouse : g_kbd) = d;
        *out                      = d;
        in_log("; [input] CreateDevice(%s) -> owned IDirectInputDeviceA %p", mouse ? "GUID_SysMouse" : "GUID_SysKeyboard",
               (void *)d);
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE EnumDevices(DWORD, LPDIENUMDEVICESCALLBACKA, LPVOID, DWORD) override {
        MH_DI_UNSUPPORTED("IDirectInputA::EnumDevices");
    }
    HRESULT STDMETHODCALLTYPE GetDeviceStatus(REFGUID) override { MH_DI_UNSUPPORTED("IDirectInputA::GetDeviceStatus"); }
    HRESULT STDMETHODCALLTYPE RunControlPanel(HWND, DWORD) override { MH_DI_UNSUPPORTED("IDirectInputA::RunControlPanel"); }
    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE, DWORD) override { MH_DI_UNSUPPORTED("IDirectInputA::Initialize"); }
};

long g_creates = 0;

HRESULT WINAPI own_DirectInputCreateA(HINSTANCE, DWORD ver, LPDIRECTINPUTA *out, LPUNKNOWN) {
    if (!out) return DIERR_INVALIDPARAM;
    *out = new OwnDI();
    if (++g_creates == 1) in_log("; [input] DirectInputCreateA(version 0x%04lx) -> owned IDirectInputA (dinput.dll not loaded)", ver);
    return DI_OK;
}

// llm_input_dinput_acquire, replaced: create our IDirectInput on first use, publish a non-null
// module (mh.dll's own handle, never FreeLibrary'd) and our proc, bump the refcount like the original.
void __cdecl own_acquire(HINSTANCE hinst) {
    if (at<void *>(ADDR_DI_OBJECT) == nullptr) {
        at<HMODULE>(ADDR_DI_MODULE) = g_self;
        at<void *>(ADDR_DI_CREATE)  = (void *)&own_DirectInputCreateA;
        if (FAILED(own_DirectInputCreateA(hinst, 0x0500, &at<LPDIRECTINPUTA>(ADDR_DI_OBJECT), nullptr))) {
            at<void *>(ADDR_DI_OBJECT) = nullptr;
            return;
        }
    }
    at<int>(ADDR_DI_REFCOUNT) += 1;
}

// llm_input_dinput_release, replaced: the original's decrement-then-test, Release at zero, and never a
// FreeLibrary (the module handle is mh.dll's).
void __cdecl own_release() {
    int &rc = at<int>(ADDR_DI_REFCOUNT);
    if (--rc == 0) {
        LPDIRECTINPUTA di          = at<LPDIRECTINPUTA>(ADDR_DI_OBJECT);
        at<void *>(ADDR_DI_OBJECT) = nullptr;
        if (di) di->Release();
    }
}

// Both originals are __watcall void(void) that preserve every register (acquire takes the HINSTANCE in
// EAX and keeps EBX/ECX/EDX/EDI; release keeps ECX/EDX/ESI/EDI); the thunks preserve everything.
// clang-format off
__declspec(naked) void acquire_thunk() {
    __asm {
        pushad
        pushfd
        push eax
        call own_acquire
        add esp, 4
        popfd
        popad
        ret
    }
}
__declspec(naked) void release_thunk() {
    __asm {
        pushad
        pushfd
        call own_release
        popfd
        popad
        ret
    }
}
// clang-format on

// ---- the system backend's element trace: a vtable detour on the real device's GetDeviceData --------

typedef HRESULT(__stdcall *gdd_fn)(void *self, DWORD cb, di_element *rgdod, DWORD *inout, DWORD flags);
gdd_fn g_sys_gdd  = nullptr;
void **g_sys_vtbl = nullptr;

HRESULT __stdcall traced_gdd(void *self, DWORD cb, di_element *rgdod, DWORD *inout, DWORD flags) {
    const HRESULT hr = g_sys_gdd(self, cb, rgdod, inout, flags);
    if (SUCCEEDED(hr) && rgdod && inout && self == at<void *>(ADDR_DI_MOUSE) && cb == sizeof(di_element))
        trace_mouse_elements("system", rgdod, *inout);
    return hr;
}

// The keyboard and mouse devices of a real DirectInput share one vtable (one class), so the detour
// filters on `self`. Installed once, when the game's mouse device first exists.
void arm_system_trace() {
    if (g_sys_vtbl || !g_trace_m || g_active) return;
    void *dev = at<void *>(ADDR_DI_MOUSE);
    if (!dev) return;
    void **vtbl = *(void ***)dev;
    // The ImGui overlay hooks the same slot on its first open. Wrapping ITS hook would make it re-install
    // over ours (it recognises only its own pointer), so if the slot already points into mh.dll, stand down.
    HMODULE owner = nullptr, self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)vtbl[10], &owner);
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&arm_system_trace, &self);
    if (owner && owner == self) {
        in_log("; [input] mouse_trace: the system device's GetDeviceData is already hooked inside mh.dll (the ImGui "
               "overlay) -- element trace not installed");
        g_sys_vtbl = vtbl;
        return;
    }
    DWORD old = 0;
    if (!VirtualProtect(&vtbl[10], sizeof(void *), PAGE_READWRITE, &old)) {
        in_log("; [input] mouse_trace: the system device's vtable could not be made writable (%lu)", GetLastError());
        g_sys_vtbl = vtbl; // do not retry every present
        return;
    }
    g_sys_gdd = (gdd_fn)vtbl[10];
    vtbl[10]  = (void *)&traced_gdd;
    VirtualProtect(&vtbl[10], sizeof(void *), old, &old);
    g_sys_vtbl = vtbl;
    in_log("; [input] mouse_trace: system DirectInput GetDeviceData traced (vtable %p, original %p)", (void *)vtbl,
           (void *)g_sys_gdd);
}

// ---- once-per-present -----------------------------------------------------------------------------

void log_modules(const char *when) {
    char        path[MAX_PATH];
    const char *names[] = {"dinput.dll", "dinput8.dll"};
    for (const char *n : names) {
        HMODULE m = GetModuleHandleA(n);
        if (m && GetModuleFileNameA(m, path, sizeof(path))) in_log("; [input] %s: %s LOADED -- %s", when, n, path);
        else in_log("; [input] %s: %s NOT loaded", when, n);
    }
}

void trace_key_ring() {
    static uint32_t s_w = 0xffffffffu;
    const uint32_t  w   = at<uint32_t>(ADDR_KEY_WRITE) & 0x7f;
    if (s_w == 0xffffffffu) {
        s_w = w;
        return;
    }
    while (s_w != w) {
        const uint8_t *e = (const uint8_t *)(ADDR_KEY_EVENTS + (uintptr_t)s_w * 0x38);
        in_log("; [keyring] scan=0x%02x type=0x%03x", (unsigned)*(const uint32_t *)e, (unsigned)*(const uint32_t *)(e + 4));
        s_w = (s_w + 1) & 0x7f;
    }
}

bool parse_backend(const char *v, bool *own) {
    if (lstrcmpiA(v, "own") == 0) return *own = true, true;
    if (lstrcmpiA(v, "system") == 0 || v[0] == 0) return *own = false, true;
    return false;
}

} // namespace

bool owned_dinput_active() { return g_active; }

void on_present() {
    static long s_presents = 0;
    ++s_presents;
    const bool  devices = at<void *>(ADDR_DI_MOUSE) != nullptr || at<void *>(ADDR_DI_KEYBOARD) != nullptr;
    static bool s_first = false, s_later = false;
    if (g_active && devices && !s_first) {
        s_first = true;
        log_modules("first present with a device");
    }
    if (g_active && s_first && !s_later && s_presents >= 600) {
        s_later = true;
        log_modules("present 600");
    }
    if (g_trace_m) {
        arm_system_trace();
        hist_flush(g_active ? "own" : "system", false);
    }
    if (g_trace_k) trace_key_ring();
}

bool install_owned_dinput(const char *ini) {
    static bool done = false;
    if (done) return g_active;
    done = true;

    g_trace_m = mh_ini_get_int("input", "mouse_trace", 0, ini) != 0;
    g_trace_k = mh_ini_get_int("input", "key_trace", 0, ini) != 0;
    char v[32];
    mh::config::read_ini_string("input", "backend", "system", v, sizeof(v), ini);
    bool own = false;
    if (!parse_backend(v, &own)) {
        in_log("; [input] [input] backend=%s not recognised (system | own) -- staying on system", v);
        return false;
    }
    if (!own) return false; // the default: the loader is not touched
    if (!mh::en_build_ok()) return false;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&install_owned_dinput, &g_self);

    // Exact-bytes guard on BOTH entries before writing EITHER: a half-owned loader would hand the
    // original release our IDirectInput and FreeLibrary mh.dll's handle.
    constexpr uintptr_t ACQ = mh::exp::addr_llm_input_dinput_acquire;
    constexpr uintptr_t REL = mh::exp::addr_llm_input_dinput_release;
    if (*(const uint64_t *)ACQ != mh::exp::entry_llm_input_dinput_acquire ||
        *(const uint64_t *)REL != mh::exp::entry_llm_input_dinput_release) {
        in_log("; [input] backend=own NOT armed -- the dinput loader entry bytes differ (wrong build, or hooked)");
        return false;
    }
    if (!mh::hook::install_jmp(ACQ, (const void *)&acquire_thunk, mh::hook::entry_claim::exclusive,
                               "the PT-INPUT1 owned DirectInput (loader acquire)", 0)) {
        in_log("; [input] backend=own NOT armed -- acquire entry refused (see the [interlock] line)");
        return false;
    }
    if (!mh::hook::install_jmp(REL, (const void *)&release_thunk, mh::hook::entry_claim::exclusive,
                               "the PT-INPUT1 owned DirectInput (loader release)", 0)) {
        // Acquire is ours: the ORIGINAL release must never reach FreeLibrary(mh.dll). Pin the refcount.
        at<int>(ADDR_DI_REFCOUNT) += 0x10000000;
        in_log("; [input] backend=own: release entry refused -- refcount pinned so the original never frees");
    }
    g_active = true;
    in_log("; [input] backend=own ARMED: owned DirectInput 5 on Raw Input (loader replaced at %08x/%08x), mouse_trace=%d "
           "key_trace=%d",
           (unsigned)ACQ, (unsigned)REL, g_trace_m ? 1 : 0, g_trace_k ? 1 : 0);
    return true;
}

// ---- the harness --------------------------------------------------------------------------------

bool harness_mouse_abs_game(int gx, int gy) {
    if (!g_active || !g_mouse) return false;
    HWND h = g_mouse->hwnd;
    if (!h) return false;
    POINT p = {gx, gy};
    if (!mh::gfx::owned_game_to_client(&p)) {
        RECT      cr;
        const int bw = at<int>(ADDR_BOX_W), bh = at<int>(ADDR_BOX_H);
        if (GetClientRect(h, &cr) && cr.right > 0 && cr.bottom > 0 && bw > 0 && bh > 0) {
            p.x = (LONG)(((long long)gx * 2 + 1) * cr.right / (2LL * bw));
            p.y = (LONG)(((long long)gy * 2 + 1) * cr.bottom / (2LL * bh));
        }
    }
    ClientToScreen(h, &p);
    RAWMOUSE m;
    ZeroMemory(&m, sizeof(m));
    m.usFlags = MOUSE_MOVE_ABSOLUTE | MOUSE_VIRTUAL_DESKTOP;
    m.lLastX  = pixel_to_norm(p.x, GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_CXVIRTUALSCREEN));
    m.lLastY  = pixel_to_norm(p.y, GetSystemMetrics(SM_YVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN));
    ++g_harness;
    on_raw_mouse(m, GetTickCount(), true);
    return true;
}

bool harness_mouse_rel(int dx, int dy) {
    if (!g_active || !g_mouse) return false;
    RAWMOUSE m;
    ZeroMemory(&m, sizeof(m));
    m.usFlags = MOUSE_MOVE_RELATIVE;
    m.lLastX  = dx;
    m.lLastY  = dy;
    ++g_harness;
    on_raw_mouse(m, GetTickCount(), true);
    return true;
}

bool harness_mouse_button(int button, bool down) {
    if (!g_active || !g_mouse || button < 0 || button > 2) return false;
    static const USHORT DOWN[3] = {RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_DOWN};
    static const USHORT UP[3]   = {RI_MOUSE_LEFT_BUTTON_UP, RI_MOUSE_RIGHT_BUTTON_UP, RI_MOUSE_MIDDLE_BUTTON_UP};
    RAWMOUSE            m;
    ZeroMemory(&m, sizeof(m));
    m.usButtonFlags = down ? DOWN[button] : UP[button];
    ++g_harness;
    on_raw_mouse(m, GetTickCount(), true);
    return true;
}

bool harness_key(uint16_t make, bool e0, bool down) {
    if (!g_active || !g_kbd) return false;
    RAWKEYBOARD k;
    ZeroMemory(&k, sizeof(k));
    k.MakeCode = make;
    k.Flags    = (USHORT)((down ? RI_KEY_MAKE : RI_KEY_BREAK) | (e0 ? RI_KEY_E0 : 0));
    // What the real keyboard driver reports alongside; only Pause / NumLock / 0xFF matter to scan_to_dik.
    k.VKey = (USHORT)MapVirtualKeyA(make | (e0 ? 0xE000 : 0), MAPVK_VSC_TO_VK_EX);
    if (k.VKey == 0) k.VKey = 1;
    ++g_harness;
    on_raw_key(k, GetTickCount(), true);
    return true;
}

void set_mouse_filter(mouse_filter_fn fn) {
    if (fn != g_mouse_filter) in_log("; [input] mouse filter %s (%p)", fn ? "installed" : "removed", (void *)fn);
    g_mouse_filter = fn;
}

void harness_focus(int state) {
    if (g_focus_ovr != state)
        in_log("; [input] harness focus -> %s", state < 0 ? "real foreground" : state ? "focused"
                                                                                      : "LOST");
    g_focus_ovr = state;
}

int query(query_what what) {
    switch (what) {
        case query_what::active: return g_active ? 1 : 0;
        case query_what::mouse_elements: return g_mouse ? (int)g_mouse->delivered : 0;
        case query_what::key_elements: return g_kbd ? (int)g_kbd->delivered : 0;
        case query_what::raw_packets: return (int)g_raw;
        case query_what::unsupported: return (int)g_unsupported;
        case query_what::mouse_acquired: return g_mouse && g_mouse->acquired ? 1 : 0;
        case query_what::key_acquired: return g_kbd && g_kbd->acquired ? 1 : 0;
    }
    return 0;
}

} // namespace mh::input
