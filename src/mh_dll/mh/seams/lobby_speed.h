// Host-owned lobby selection and immutable match pacing. One instance, in mh.dll only.
#pragma once

#include <cstdint>

#include "mh_net_proto/gameplay_set.h"

namespace mh::seams::lobby_speed {

bool valid(int percent) noexcept;

// Main thread. Open resets to 100; bind supplies the nonzero SESSION_INFO tag once minted.
// A client opens with the tag of the row it actually JOINs, not whichever advert arrives last.
void open(bool host, std::uint32_t lobby_tag) noexcept;
void bind(std::uint32_t lobby_tag) noexcept;
bool host_cycle() noexcept;
int  display_percent() noexcept;
bool editable() noexcept;

// Receive thread, ONLY after transport verifies this subtype came over its actual host link.
// Rejects malformed/wrong-lobby/non-increasing revisions atomically. Never changes match pacing.
void receive(const unsigned char *bytes, int len) noexcept;
// Main thread: consume pending display and publish host edits/periodic join refreshes.
void tick() noexcept;

// Strict required key, exactly once, INT, one of the five values. No generic default fallback.
bool validate_start(const mh_net_proto::GameplaySet &set) noexcept;
// Main thread: replace the existing registered entry with the UI value (never ini/local_set).
// Failure leaves set untouched. Call before publishing/applying even when there are no peers.
bool overlay_start(mh_net_proto::GameplaySet &set) noexcept;
// Main thread: call AFTER successful gameplay application, before match initialization.
bool freeze_start(const mh_net_proto::GameplaySet &set) noexcept;
// Replay metadata: 0 = native campaign/no match override; otherwise validate and freeze, incl 100.
// Invalid metadata leaves the current owner untouched and must refuse replay entry.
bool replay_enter(int recorded_percent) noexcept;
int  match_percent() noexcept; // 0 = no frozen match; 100 is an active override

// Main thread: immediate post-recompute/import pin; untouched when there is no match owner.
void enforce() noexcept;
// Frozen match > local developer pin > native scalar. Valid for all pacing readers.
double effective_scalar(double native_scalar, double developer_pin = 0.0) noexcept;
// Convert controller inputs/outputs only, NOT fixed simulation steps or explicit game-time pins.
double game_to_wall(double game_value, double scalar) noexcept;
double wall_to_game(double wall_value, double scalar) noexcept;

// Main thread, quiescent real exit/failed entry/standalone save-load/new campaign only.
// Not elimination, spectator continuation, resync, handover, tactical return or planet switch.
void clear() noexcept;

// Bind the promoted live-wrapper post-observer, plus the retail run-after observer when original.
// Both call enforce() in mh.dll; libmh holds only the callback, never a second speed owner.
bool install_native_recompute();

} // namespace mh::seams::lobby_speed

// Harness crossing; exported from the owner image, never linked into mh_harness.dll.
extern "C" __declspec(dllexport) int MH_LobbySpeed_ReplayEnter(int recorded_percent);
