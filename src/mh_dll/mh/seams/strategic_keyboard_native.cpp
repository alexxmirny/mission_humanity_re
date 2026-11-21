#include "strategic_keyboard_native.h"
#include "addr/mh_addrs.gen.h"
#include "addr/mh_calls.gen.h"
#include "include/libmh_host_in.h"
#include <cstring>

namespace mh::seams::strategic_keyboard_native {
namespace b = mh::strategic_bindings;
namespace {
// EN native keyboard ladder / group recall measured 2026-11-07. Noncanonical UI storage;
// do not add it to the state manifest and invalidate deterministic-world recordings.
constexpr uintptr_t chat_mode        = 0x005d01d8u; // uint32: input_mode0; team1/all2/console3
constexpr uintptr_t message_time     = 0x00ae4b90u; // double[30], native K loop at00442653
constexpr uintptr_t message_duration = 0x00500b67u; // double
constexpr uintptr_t message_step     = 0x00500b77u; // double, offset for K
constexpr uintptr_t message_clear    = 0x00500b6fu; // double, offset for Shift+K
constexpr uintptr_t text_ptrs        = 0x0058440cu; // native void*[806]
constexpr uintptr_t zoom_tag         = 0x005d0c2cu; // int32:8 F8,9 F9
constexpr uintptr_t left_held = 0x005d0bbcu, right_held = 0x005d0bc0u, up_held = 0x005d0bc4u, down_held = 0x005d0bc8u;
template <class T>
T   &at(uintptr_t address) { return *reinterpret_cast<T *>(address); }
void release(uint8_t scan) {
    uintptr_t latch = 0;
    switch (scan) {
        case 0x48: latch = up_held; break;
        case 0x50: latch = down_held; break;
        case 0x4b: latch = left_held; break;
        case 0x4d: latch = right_held; break;
        case 0x42:
            if (at<int32_t>(zoom_tag) == 8) at<int32_t>(zoom_tag) = 0;
            break;
        case 0x43:
            if (at<int32_t>(zoom_tag) == 9) at<int32_t>(zoom_tag) = 0;
            break;
    }
    if (latch) at<int32_t>(latch) = 0;
}
void age_messages(bool dismiss) {
    // Preserve the exact early-stop/write ordering of00442653, not a blanket clear of all30 slots.
    bool expired = false;
    for (unsigned i = 0; i < 30 && !expired; ++i) {
        double      &time = at<double>(message_time + i * sizeof(double));
        const double now  = at<double>(mh::addr::TOTAL_GAME_TIME);
        if (time + at<double>(message_duration) < now) expired = true;
        time = now + at<double>(dismiss ? message_clear : message_step);
    }
}
void chat(bool all) {
    if (at<uint8_t>(mh::addr::_G_LLM_GAME_SESSION_MODE) != 3) return;
    if (!all && at<uint8_t>(mh::addr::_G_LLM_CHAT_TARGET_MODE) == 3) {
        mh::call::game_ui_PrintTextMessage(at<void *>(text_ptrs + 0xa3u * sizeof(uint32_t)));
        mh::call::llm_snd_play(0xb3, 100);
    } else {
        at<uint32_t>(chat_mode) = all ? 2u : 1u;
        mh::call::game_SetEvent(LIBMH_EV_CHAT_INPUT_OPEN);
    }
}
} // namespace
delivery dispatch(size_t action, b::phase phase, uint32_t &scan, uint32_t &type) noexcept {
    if (action >= b::action_count) return delivery::not_main;
    const auto &row = b::actions()[action];
    if (std::strcmp(row.contexts, "main") != 0) return delivery::not_main;
    if (phase == b::phase::released) {
        release(row.native_scan);
        return delivery::consumed;
    }
    if (std::strcmp(row.category, "groups") == 0) {
        const int group = int(row.native_scan) - 1;
        if (std::strncmp(row.id, "group_assign_", 13) == 0) mh::call::llm_unit_ctrl_group_flash_observer(group);
        else {
            mh::call::llm_unit_ctrl_group_activate(group);
            if (std::strncmp(row.id, "group_center_", 13) == 0) mh::call::llm_cam_center_on_ctrl_group(group);
            mh::call::game_SetEvent(LIBMH_EV_PANEL_SHOW_INFO);
        }
        mh::call::game_SetEvent(LIBMH_EV_INFO_REFRESH);
        return delivery::consumed;
    }
    switch (b::action_id(action)) {
        case b::action_id::chat_team:
            chat(false);
            return delivery::consumed;
        case b::action_id::chat_all:
            chat(true);
            return delivery::consumed;
        case b::action_id::age_messages: age_messages(false); return delivery::consumed;
        case b::action_id::dismiss_messages: age_messages(true); return delivery::consumed;
        // The runtime owns these locally: expanded-map held state and indicator edge parity.
        case b::action_id::expanded_map:
        case b::action_id::network_indicator: return delivery::consumed;
        default: break;
    }
    // The stock ladder executes all remaining action bodies, preserving their native timing,
    // worker/order admission, camera initial-delay/cross-axis primes, speed gates and side effects.
    // Only its caller's dequeued record changes. There is no physical-key or modifier-array rewrite.
    scan = row.native_scan;
    type = 0x100u;
    return delivery::token;
}
} // namespace mh::seams::strategic_keyboard_native
