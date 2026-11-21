#pragma once

#include "../../mh_common/participant_boundary.h"
#include "state/host_api.h"
#include "lockstep/turn_engine.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>

namespace mh::lockstep::participant {
namespace pb = mh::participant_boundary;

[[noreturn]] inline void fail(const char *why) {
    std::fprintf(stderr, "libmh participant boundary: %s\n", why);
    std::abort();
}
inline uint64_t boundary(pb::operation op, uint64_t argument = 0) {
    const auto port = mh::host().participant_body_boundary;
    if (!port) fail("missing required body authority");
    return port(uint32_t(op), argument);
}
inline int32_t live_admit(const void *data, uint32_t size) {
    const auto port = mh::host().participant_removal_admit;
    if (!port) fail("missing required admission port");
    const int32_t result = port(data, size);
    if (result < 0) fail("invalid removal admission");
    return result;
}
inline void poll() { boundary(pb::operation::poll); }

template <typename T>
T copy(const void *bytes, uint32_t size) {
    static_assert(std::is_trivially_copyable_v<T>);
    static_assert(sizeof(T) <= pb::PAYLOAD_CAPACITY);
    if (!bytes || size != sizeof(T)) fail("invalid owned removal payload");
    T result;
    std::memcpy(&result, bytes, sizeof(result));
    return result;
}
inline pb::admission submit(pb::admit_fn admit, const pb::request &request) {
    if (!admit) fail("missing detail admission policy");
    const int32_t result = admit(&request, sizeof(request));
    if (result < 0 || result > int32_t(pb::admission::applied_stop)) fail("removal admission refused");
    return static_cast<pb::admission>(result);
}

inline bool human(const player_profile *players, int32_t index) {
    if (!players || index < 0 || index >= MAX_PLAYERS) return false;
    const auto flags = players[index].status_flags;
    return (flags & (PLAYER_ALIVE | PLAYER_HUMAN)) == (PLAYER_ALIVE | PLAYER_HUMAN);
}
inline int survivors(const player_profile *players, int32_t removed) {
    int result = 0;
    for (int32_t i = 0; i < MAX_PLAYERS; ++i)
        if (i != removed && human(players, i)) ++result;
    return result;
}
inline bool continuing(const player_profile *players, int32_t removed, uint16_t local) {
    return removed != local && human(players, removed) && survivors(players, removed) > 1;
}
template <class T>
pb::request request_for(uint32_t index, double horizon, const T &payload,
                        pb::execute_fn execute, pb::validate_fn validate = nullptr,
                        pb::validate_fn terminal = nullptr) {
    static_assert(std::is_trivially_copyable_v<T> && sizeof(T) <= pb::PAYLOAD_CAPACITY);
    pb::request r{};
    r.struct_size  = sizeof(r);
    r.slot_index   = index;
    r.horizon_bits = pb::bits(horizon);
    r.execute      = execute;
    r.validate     = validate;
    r.terminal     = terminal;
    r.payload_size = sizeof(payload);
    r.payload      = &payload;
    return r;
}

} // namespace mh::lockstep::participant
