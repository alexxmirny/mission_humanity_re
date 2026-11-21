#include "strategic_input_native.h"
#include "strategic_keyboard_native.h"
#include "mh_order_tasks.h"
#include "addr/mh_addrs.gen.h"
#include "addr/mh_calls.gen.h"
#include "addr/mh_structs.gen.h"
#include "include/mh_uidrive_export.h"
#include "include/mh_chatinput_export.h"
#include "seams/net_internal.h"
#include "ui/player_strings.h"
#include <cstring>

namespace mh::seams::strategic_input_native {
namespace b = mh::strategic_bindings;
namespace {
template <class T>
T read(uintptr_t p) {
    T v;
    std::memcpy(&v, reinterpret_cast<const void *>(p), sizeof v);
    return v;
}
constexpr uintptr_t MAIN_RETURN = 0x00441bfdu;
constexpr uintptr_t EDIT_LO = 0x004bbd30u, EDIT_HI = 0x004bc089u;
constexpr uintptr_t CHAT_MODE = 0x005d01d8u;
bool                physical_down[256]{};
bool                popped_repeat = false, ready = false, main_pop_active = false;
uint8_t             current_modifiers = 0;
uint8_t             modal_scan[256]{};
bool                modal_released[256]{};
bool                modal_blocked[256]{};
unsigned            modal_keys            = 0;
bool                modal_release_pending = false;
uintptr_t           modal_screen          = 0;
b::context          modal_owner           = b::context::none;
// Slot identity: raw records/layout stay authoritative and are never rewritten.
struct key_proof {
    uint32_t timestamp         = 0;
    uint16_t physical_plus_one = 0;
};
key_proof fallback_fresh[128]{};
unsigned  fallback_pending = 0;
// A pre-ring break can overtake DOWNs already in the native ring. Fence only
// those exact records, not newer makes at/after its captured WRITE boundary.
key_proof discarded_make[128]{};
bool      popped_discarded = false;
struct delivery {
    uint32_t scan = 0, type = 0;
};
void emit(void *opaque, size_t action, b::phase phase) {
    auto      *out  = static_cast<delivery *>(opaque);
    uint32_t   scan = 0, type = 0;
    const auto result = strategic_keyboard_native::dispatch(action, phase, scan, type);
    if (out && phase == b::phase::pressed) {
        if (result == strategic_keyboard_native::delivery::token) {
            out->scan = scan;
            out->type = type;
        } else if (result == strategic_keyboard_native::delivery::not_main) {
            out->scan = b::actions()[action].native_scan;
            out->type = 0x100u;
        }
    }
}
uint8_t levels() {
    return uint8_t(((physical_down[0x1d] || physical_down[0x9d]) ? b::ctrl : 0) |
                   ((physical_down[0x38] || physical_down[0xb8]) ? b::alt : 0) |
                   ((physical_down[0x2a] || physical_down[0x36]) ? b::shift : 0));
}
bool contains(const mh::game::mh_llm_ui_widget_list *list, uintptr_t widget) {
    if (!list || !list->children) return false;
    for (unsigned i = 0; i < 256 && list->children[i]; ++i)
        if (reinterpret_cast<uintptr_t>(list->children[i]) == widget) return true;
    return false;
}
bool wait_consumer() {
    return read<uint8_t>(mh::addr::_G_LLM_GAME_MODE) == 3 && MH_UIDrive_ActiveScreen() == mh::addr::_G_LLM_UI_WGT_LIST_LOCKSTEP_SYNC;
}
b::context modal_context() {
    if (read<uint8_t>(mh::addr::_G_LLM_GAME_MODE) != 3) return b::context::none;
    const auto *list = reinterpret_cast<const mh::game::mh_llm_ui_widget_list *>(uintptr_t(MH_UIDrive_ActiveScreen()));
    if (!list) return b::context::none;
    if (wait_consumer()) return b::context::wait;
    // Tutorial welcome is installed before its first menu_enter_gameplay, so the real
    // IN_MAIN_MENU flag is still1. This is an explicit measured tutorial-only exception.
    if (read<int32_t>(mh::addr::_G_LLM_GAME_TUTORIAL_STEP) != 0 &&
        (contains(list, 0x0065102fu) || contains(list, 0x00650fa7u))) return b::context::tutorial;
    // menu_enter_gameplay4b8116 clears this; return-to-main4b941e sets it. The
    // GAMEPLAY_HUD parent pointer is deliberately NOT a session test: main menu reuses it.
    if (read<int32_t>(0x00603f70u) != 0) return b::context::none;
    if (contains(list, 0x0065135fu)) return b::context::game_menu;
    if (contains(list, 0x0065142bu)) return b::context::options;
    if (contains(list, 0x006514b3u)) return b::context::info;
    if (contains(list, 0x0065098bu)) return b::context::planet;
    if (contains(list, 0x00650a57u)) return b::context::saveload;
    if (contains(list, 0x006511c7u)) return b::context::diplomacy;
    if (contains(list, 0x0065102fu) || contains(list, 0x00650fa7u)) return b::context::tutorial;
    if (contains(list, 0x00650befu) || contains(list, 0x00650c33u)) return b::context::outcome;
    return b::context::none;
}
void sync_modal_keys(b::context ctx) {
    const uintptr_t screen = ctx == b::context::none ? 0 : uintptr_t(MH_UIDrive_ActiveScreen());
    if (ctx != modal_owner || screen != modal_screen) {
        cancel_modal_gesture();
        modal_owner  = ctx;
        modal_screen = screen;
    }
}
void feedback(mh::ui::Str key, unsigned count) {
    wchar_t text[mh::ui::STR_MAX];
    wsprintfW(text, mh::ui::tr(key), count);
    auto         *queue = reinterpret_cast<volatile int32_t *>(mh::addr::_G_LLM_STRAT_FLOATING_MSG_QUEUE_ACTIVE);
    const int32_t old   = *queue;
    *queue              = 0;
    mh::call::llm_ui_print_floating_msg_cyan(text);
    *queue = old;
}
} // namespace
bool main_context_active() {
    const uint8_t mode = read<uint8_t>(mh::addr::_G_LLM_GAME_MODE);
    if (mode == 2) return true;
    // EN tutorial driver004bae7d calls the same strategic input pump in mode3
    // while its menu list is null and no async callback owns the tutorial.
    // Actual dialogs remain modal; a bare mode3 is never a main-input permit.
    return mode == 3 && read<int32_t>(mh::addr::_G_LLM_GAME_TUTORIAL_STEP) != 0 &&
           read<uintptr_t>(mh::addr::_G_LLM_UI_MENU_ASYNC_CALLBACK_B) == 0 &&
           MH_UIDrive_ActiveScreen() == 0;
}
bool initialize() {
    // The core-arm owns the one immutable load before harness/replay. Attaching observers must
    // not reinitialize the facade (which correctly refuses a second profile owner).
    ready = strategic_input::effective_profile() != nullptr;
    return ready;
}
uint8_t modifiers() { return current_modifiers; }
void    capture_pointer_modifiers() {
    // Expanded-map pre-input runs before strategic keyboard dequeue. Read pending real modifier
    // records without consuming/dispatching them, so same-frame key+mouse has world-identical
    // semantics and journal/keyhold records need no synthetic KEYSTATE substitution.
    const uint8_t scans[] = {0x1d, 0x9d, 0x38, 0xb8, 0x2a, 0x36};
    bool          down[6];
    for (unsigned n = 0; n < 6; ++n) down[n] = physical_down[scans[n]];
    const uint32_t write = read<uint32_t>(mh::addr::_G_LLM_INPUT_KEY_WRITE_IDX) & 127u;
    for (uint32_t slot = read<uint32_t>(mh::addr::_G_LLM_INPUT_KEY_READ_IDX) & 127u; slot != write; slot = (slot + 1) & 127u) {
        const auto *event = reinterpret_cast<const uint32_t *>(mh::addr::_G_LLM_INPUT_KEY_EVENTS + slot * 0x38u);
        if (!(event[1] & 0x180u)) continue;
        const uint8_t scan      = b::native_physical(event[0], event[1]);
        const auto   &discarded = discarded_make[slot];
        if ((event[1] & 0x100u) && discarded.physical_plus_one == unsigned(scan) + 1u && discarded.timestamp == event[8]) continue;
        for (unsigned n = 0; n < 6; ++n)
            if (scans[n] == scan) down[n] = (event[1] & 0x100u) != 0;
    }
    current_modifiers = uint8_t(((down[0] || down[1]) ? b::ctrl : 0) | ((down[2] || down[3]) ? b::alt : 0) | ((down[4] || down[5]) ? b::shift : 0));
    strategic_input::update_modifiers(current_modifiers);
}
bool held(b::role_index role) {
    const auto *p = strategic_input::effective_profile();
    return p && b::role_held(*p, role, current_modifiers);
}
unsigned button_mask(b::role_index role) {
    const auto *p = strategic_input::effective_profile();
    return p && size_t(role) < b::role_count ? p->role_values[size_t(role)] : 0;
}
void begin_main_pop() { main_pop_active = true; }
bool observing() { return ready; }
void observe_dequeue(const uint32_t *event, uintptr_t caller) {
    if (!ready || !event) return;
    strategic_input::note_input_record();
    const uint8_t  physical  = b::native_physical(event[0], event[1]);
    const bool     down      = (event[1] & 0x100u) != 0;
    const uint32_t slot      = read<uint32_t>(mh::addr::_G_LLM_INPUT_KEY_READ_IDX) & 127u;
    auto          &discarded = discarded_make[slot];
    popped_discarded         = down && discarded.physical_plus_one == unsigned(physical) + 1u && discarded.timestamp == event[8];
    discarded                = {};
    auto      &fresh         = fallback_fresh[slot];
    const bool fresh_make    = down && fresh.physical_plus_one == unsigned(physical) + 1u && fresh.timestamp == event[8];
    if (fresh.physical_plus_one) {
        fresh = {};
        --fallback_pending;
    }
    if (popped_discarded) {
        // Its actual break was already reconciled before ring production.
        // Neither physical nor semantic ownership may reacquire from this DOWN.
        // The actual consumer neutralizes only its private copied payload.
        popped_repeat = false;
        return;
    }
    if (!down || fresh_make) modal_blocked[physical] = false;
    popped_repeat           = down && physical_down[physical] && !fresh_make;
    physical_down[physical] = down;
    if (!down && modal_scan[physical]) {
        modal_released[physical] = true;
        modal_release_pending    = true;
    }
    current_modifiers = levels();
    strategic_input::update_modifiers(current_modifiers);
    // Releases must reach their semantic owner even if chat/menu/wait owns the ring today.
    // Main's local-record hook handles its event exactly once after native copied it.
    if (!main_pop_active && caller != MAIN_RETURN) {
        if (wait_consumer()) {
            // Only the indicator catalog row belongs to WAIT. Other strategic keys stay out,
            // and the wait/chat/menu drain receives the original unmodified event afterwards.
            strategic_input::context_changed(b::context::wait, emit, nullptr);
            strategic_input::key_event(event[0], event[1], current_modifiers, popped_repeat, emit, nullptr);
        } else if (!down)
            strategic_input::key_event(event[0], event[1], current_modifiers, popped_repeat, emit, nullptr);
    }
}
void main_record(uint32_t *event) {
    main_pop_active = false;
    struct record_trace {
        uint32_t *event;
        uint32_t  original[4];
        bool      active, discard_valid, discarded;
        record_trace(uint32_t *p, bool valid, bool discard)
            : event(p), active(chat_input_trace::enabled() && p && p[1] != 0),
              discard_valid(valid && p), discarded(discard) {
            if (active) {
                original[0] = p[0];
                original[1] = p[1];
                original[2] = p[2];
                original[3] = p[8];
            }
        }
        ~record_trace() {
            if (active)
                chat_input_trace::main_record(original[0], original[1], original[2], original[3],
                                              event, discard_valid, discarded);
        }
    } trace(event, ready, popped_discarded);
    if (!ready || !event) return;
    if (popped_discarded) {
        event[0] = event[1] = event[2] = 0;
        return;
    }
    if (read<uint32_t>(CHAT_MODE) != 0) {
        strategic_input::context_changed(b::context::none, emit, nullptr);
        return;
    }
    if (!main_context_active()) {
        strategic_input::context_changed(b::context::none, emit, nullptr);
        event[0] = event[1] = event[2] = 0;
        return;
    }
    strategic_input::context_changed(b::context::main, emit, nullptr);
    delivery   result;
    const bool claimed = strategic_input::key_event(event[0], event[1], current_modifiers, popped_repeat, emit, &result);
    // Shift+Enter console remains a reference-only fixed native operation. No other raw native
    // key fallback: an action moved/unbound in the profile must not still fire its old scan.
    if (!claimed && (event[1] & 0x100u) && (b::native_physical(event[0], event[1]) & 0x7fu) == 0x1c && (current_modifiers & b::shift)) return;
    event[0] = result.scan;
    event[1] = result.type;
    event[2] = 0;
}
// Preserve the native pause erasure policy in one pass: move the exact raw
// records AND their two sidecar identities together, then publish WRITE.
uint32_t erase_key_record(uint32_t i) {
    const uint32_t w    = read<uint32_t>(mh::addr::_G_LLM_INPUT_KEY_WRITE_IDX);
    const uint32_t last = (w + 127u) & 127u;
    if (fallback_fresh[i].physical_plus_one) --fallback_pending;
    for (uint32_t j = i; j != last; j = (j + 1u) & 127u) {
        const uint32_t next = (j + 1u) & 127u;
        std::memcpy(reinterpret_cast<void *>(mh::addr::_G_LLM_INPUT_KEY_EVENTS + j * 0x38u),
                    reinterpret_cast<const void *>(mh::addr::_G_LLM_INPUT_KEY_EVENTS + next * 0x38u), 0x38u);
        fallback_fresh[j] = fallback_fresh[next];
        discarded_make[j] = discarded_make[next];
    }
    // Moved fresh proofs stay counted; only the erased record retires above.
    fallback_fresh[last]                                                         = {};
    discarded_make[last]                                                         = {};
    *reinterpret_cast<volatile uint32_t *>(mh::addr::_G_LLM_INPUT_KEY_WRITE_IDX) = last;
    return i;
}
void key_ring_replaced() {
    // Successful snapshot import replaced raw records/cursors, not merely
    // the world/context. Neither old table proves anything about that queue.
    std::memset(fallback_fresh, 0, sizeof fallback_fresh);
    std::memset(discarded_make, 0, sizeof discarded_make);
    fallback_pending = 0;
    popped_discarded = false;
}
// Original WndProc's exact input-tap boundary, independent of the renderer.
// Call the original ENTRY, preserving harness producer suppression there.
void wndproc_record(void *hwnd, uint32_t message, uint32_t wparam, int32_t lparam) {
    const uint32_t lp       = uint32_t(lparam);
    const uint8_t  physical = uint8_t(((lp >> 16) & 127u) | ((lp & 0x01000000u) ? 128u : 0u));
    const bool     fresh    = ready && !read<uintptr_t>(0x0066155cu) &&
                       (message == 0x100u || message == 0x104u) && !(lp & 0x40000000u) && modal_blocked[physical];
    const uint32_t before = fresh ? read<uint32_t>(mh::addr::_G_LLM_INPUT_KEY_WRITE_IDX) & 127u : 0;
    mh::call::llm_input_wndproc_tap(hwnd, message, wparam, lparam);
    if (!fresh || read<uintptr_t>(0x0066155cu) ||
        (read<uint32_t>(mh::addr::_G_LLM_INPUT_KEY_WRITE_IDX) & 127u) != ((before + 1u) & 127u)) return;
    const uintptr_t record = mh::addr::_G_LLM_INPUT_KEY_EVENTS + before * 0x38u;
    const uint32_t  scan = read<uint32_t>(record), type = read<uint32_t>(record + 4);
    const uint32_t  expected_type = 0x100u | ((lp & 0x01000000u) ? 0x200u : 0u);
    if (scan != ((lp >> 16) & 127u) || type != expected_type) return;
    auto &proof = fallback_fresh[before];
    if (!proof.physical_plus_one) ++fallback_pending;
    proof.timestamp         = read<uint32_t>(record + 0x20);
    proof.physical_plus_one = uint16_t(unsigned(physical) + 1u);
}
void discarded_key_release(uint32_t scan, uint32_t type) {
    if (!ready || (type & 0x100u) || !(type & 0x80u)) return;
    const uint8_t physical = b::native_physical(scan, type);
    // The DI break was discarded BEFORE native ring production. Its older
    // queued makes must not acquire AFTER the release is reconciled here.
    // Snapshot the real boundary once; never clear/reorder any raw record.
    const uint32_t first = read<uint32_t>(mh::addr::_G_LLM_INPUT_KEY_READ_IDX) & 127u;
    const uint32_t last  = read<uint32_t>(mh::addr::_G_LLM_INPUT_KEY_WRITE_IDX) & 127u;
    for (uint32_t slot = first; slot != last; slot = (slot + 1u) & 127u) {
        const uintptr_t record      = mh::addr::_G_LLM_INPUT_KEY_EVENTS + slot * 0x38u;
        const uint32_t  queued_scan = read<uint32_t>(record), queued_type = read<uint32_t>(record + 4);
        if ((queued_type & 0x100u) && b::native_physical(queued_scan, queued_type) == physical)
            discarded_make[slot] = {read<uint32_t>(record + 0x20), uint16_t(unsigned(physical) + 1u)};
    }
    physical_down[physical] = false;
    modal_blocked[physical] = false;
    if (modal_scan[physical]) {
        modal_released[physical] = true;
        modal_release_pending    = true;
    }
    current_modifiers = levels();
    strategic_input::update_modifiers(current_modifiers);
    // This real break never enters the ring. Release ownership once here,
    // without enqueueing a duplicate or invoking a native widget.
    strategic_input::key_event(scan, type, current_modifiers, false, emit, nullptr);
}
bool modal_record(uint32_t *event, uintptr_t outer_return) {
    if (!ready || !event) return false;
    if (popped_discarded) {
        // Native scheduling/rearm still consumes the original100 edge/timers;
        // only the freshly copied semantic payload is neutralized.
        event[0] = event[2] = 0;
        return true;
    }
    // H1 has JUST dequeued into KEYREC. Preserve that fresh payload at BOTH
    // cancellation sites while retiring old publication/native selection.
    auto cancel_record_gesture = [&]() {
        const uint32_t scan = event[0], character = event[2];
        cancel_modal_gesture();
        event[0] = scan;
        event[2] = character;
    };
    auto sync_record_context = [&](b::context ctx) {
        const uint32_t scan = event[0], character = event[2];
        sync_modal_keys(ctx);
        event[0] = scan;
        event[2] = character;
    };
    if (wait_consumer()) {
        sync_record_context(b::context::none);
        return false;
    }
    if ((outer_return >= EDIT_LO && outer_return < EDIT_HI) || outer_return != modal_widget_return) {
        sync_record_context(b::context::none);
        strategic_input::context_changed(b::context::none, emit, nullptr);
        return false;
    }
    const b::context ctx = modal_context();
    sync_record_context(ctx);
    strategic_input::context_changed(ctx, emit, nullptr);
    if (ctx == b::context::none) return false;
    const uint8_t physical = b::native_physical(event[0], event[1]);
    const bool    down     = (event[1] & 0x100u) != 0;
    uint32_t      scan     = 0;
    if (down) {
        if (modal_released[physical]) {
            cancel_record_gesture();
            sync_modal_keys(ctx);
        }
        const bool repeated = popped_repeat || modal_blocked[physical];
        if (!repeated || modal_scan[physical]) {
            delivery result;
            strategic_input::key_event(event[0], event[1], current_modifiers, popped_repeat, emit, &result);
            scan = result.scan;
            if (modal_scan[physical] && scan != modal_scan[physical]) scan = 0;
            if (scan && !modal_scan[physical]) {
                modal_scan[physical] = uint8_t(scan);
                ++modal_keys;
            }
        }
    } else if (event[1] & 0x80u) {
        // Semantic release was observed once at the shared pop. The existing
        // native widget separately needs its paired mapped physical release.
        scan = modal_scan[physical];
        if (scan) {
            modal_scan[physical]     = 0;
            modal_released[physical] = false;
            --modal_keys;
        }
    }
    event[0] = scan;
    event[2] = 0;
    // Consumed records keep physical edges/timers for native pump rearm.
    if (!scan) return true;
    // Catalog target scans are native FULL codes, not physical binding E0.
    event[1] = (event[1] & ~0x380u) | (down ? 0x100u : 0x80u);
    // H1 must supply real translation/fallback: ASCII-mode widgets reject a
    // zero character BEFORE checking their Enter/Escape default flags.
    return false;
}
int filter_modal_pump(int result) {
    if (result && read<uint8_t>(0x006542b5u) == 2 &&
        !read<uint32_t>(0x00654302u) && !read<uint32_t>(0x00654306u)) return 0;
    return result;
}
void cancel_modal_gesture(bool reset_provenance) {
    // Focus/world reset retires old fresh-make evidence. Ordinary context
    // setup may follow a NEW queued fresh make and must preserve its proof.
    if (reset_provenance && fallback_pending) {
        std::memset(fallback_fresh, 0, sizeof fallback_fresh);
        fallback_pending = 0;
    }
    // Discard fences remain until their actual ring slots are popped. Reset
    // does not erase raw queued records and must not let their makes revive.
    if (!modal_keys && !modal_release_pending && !modal_screen && modal_owner == b::context::none) return;
    if (modal_keys || modal_screen) {
        constexpr uintptr_t KEY_SELECTED                    = 0x006542a5u;
        *reinterpret_cast<volatile uint32_t *>(0x006542cau) = 0;
        *reinterpret_cast<volatile uint32_t *>(0x006542d2u) = 0;
        *reinterpret_cast<volatile uint32_t *>(0x00654302u) = 0;
        *reinterpret_cast<volatile uint32_t *>(0x00654306u) = 0;
        const uintptr_t selected                            = read<uintptr_t>(KEY_SELECTED);
        if (selected && read<uintptr_t>(mh::addr::_G_LLM_UI_WIDGET_SELECTED) == selected)
            *reinterpret_cast<volatile uintptr_t *>(mh::addr::_G_LLM_UI_WIDGET_SELECTED) = 0;
        *reinterpret_cast<volatile uintptr_t *>(KEY_SELECTED) = 0;
    }
    for (unsigned p = 0; p < 256; ++p) {
        if (modal_scan[p] && physical_down[p] && !modal_released[p]) modal_blocked[p] = true;
    }
    std::memset(modal_scan, 0, sizeof modal_scan);
    std::memset(modal_released, 0, sizeof modal_released);
    modal_keys            = 0;
    modal_release_pending = false;
    modal_screen          = 0;
    modal_owner           = b::context::none;
}
void focus_lost() {
    cancel_modal_gesture(true);
    std::memset(physical_down, 0, sizeof physical_down);
    current_modifiers = 0;
    popped_repeat     = false;
    strategic_input::focus_lost(emit, nullptr);
    cancel_pointer_gesture();
}
void on_present() {
    if (!ready) return;
    // GAME_RUNNING is the real WM_ACTIVATE consumer's focus contract, not GetAsyncKeyState.
    if (!read<int32_t>(0x005d5fe8u)) {
        focus_lost();
        return;
    }
    const uint8_t    mode = read<uint8_t>(mh::addr::_G_LLM_GAME_MODE);
    const bool       main = main_context_active();
    const b::context ctx  = !main && mode == 3 ? modal_context() : b::context::none;
    sync_modal_keys(ctx == b::context::wait ? b::context::none : ctx);
    if (modal_release_pending) {
        modal_release_pending = false;
        for (unsigned p = 0; p < 256; ++p) {
            if (modal_scan[p] && modal_released[p]) {
                cancel_modal_gesture();
                break;
            }
        }
    }
    if (!main) cancel_pointer_gesture();
    if (!main || read<uint32_t>(CHAT_MODE) != 0)
        strategic_input::context_changed(ctx, emit, nullptr);
}
void refused(unsigned reason, unsigned count) {
    if (!count) return;
    using mh::ui::Str;
    switch (mh::order_tasks::refusal(reason)) {
        case mh::order_tasks::refusal::stale_actor: feedback(Str::ORDER_QUEUE_ACTOR_UNAVAILABLE, count); break;
        case mh::order_tasks::refusal::stale_world: feedback(Str::ORDER_QUEUE_WORLD_UNAVAILABLE, count); break;
        case mh::order_tasks::refusal::full: feedback(Str::ORDER_QUEUE_FULL, count); break;
        case mh::order_tasks::refusal::after_patrol: feedback(Str::ORDER_QUEUE_AFTER_PATROL, count); break;
        case mh::order_tasks::refusal::degenerate_cycle: feedback(Str::ORDER_QUEUE_DEGENERATE, count); break;
        case mh::order_tasks::refusal::unavailable_target: feedback(Str::ORDER_QUEUE_TARGET, count); break;
        default: feedback(Str::ORDER_QUEUE_UNSUPPORTED, count); break;
    }
}
void blocked(unsigned, unsigned count) {
    if (count) feedback(mh::ui::Str::ORDER_QUEUE_BLOCKED, count);
}
} // namespace mh::seams::strategic_input_native
