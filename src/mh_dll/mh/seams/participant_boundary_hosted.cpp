#include "seams/participant_boundary_hosted.h"
#include "addr/mh_calls.gen.h"
#include "addr/mh_regions.gen.h"
#include "addr/mh_structs.gen.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mh::participant_boundary::hosted {
namespace {
authority g_owner;
bool      g_new_match      = false;
bool      g_session_seen   = false;
uint32_t  g_terminal_scope = 0;

[[noreturn]] void fail(const char *why) {
    std::fprintf(stderr, "participant boundary: %s\n", why);
    std::abort();
}
bool readable(mh::state::region_id rid, uint32_t minimum) {
#ifdef MH_SPINE_IN_IMAGE
    // The hosted selftest executable has no mapped game. Its world must be explicitly bound.
    if (!mh::state::is_bound(rid)) return false;
#endif
    // mh.dll's native-only registry deliberately seeds valid stock bases with bound=false.
    return mh::state::live_base(rid) != 0 && mh::state::live_size(rid) >= minimum;
}
view read_world(void *) {
    using namespace mh::state;
    if (!readable(RID_STRAT_GAME_CLOCK, sizeof(double)) ||
        !readable(RID_STRAT_SIM_STEP_INTERVAL, sizeof(double)) || !readable(RID_GAME_SESSION_MODE, 1))
        return {0, 0, false, false};
    return {*ptr<const double>(RID_STRAT_GAME_CLOCK), *ptr<const double>(RID_STRAT_SIM_STEP_INTERVAL),
            *ptr<const uint8_t>(RID_GAME_SESSION_MODE) == 3, true};
}
} // namespace

bool       initialize() { return g_owner.ready() || g_owner.initialize(read_world, nullptr); }
authority &owner() { return g_owner; }
void       begin_session() {
    if (!g_owner.ready()) fail("session entered before authority initialization");
    g_owner.end_match();
    g_new_match    = true;
    g_session_seen = true;
}
void observe_session_begin() {
    // A native begin may re-enter the continuing match; that is not permission to discard input.
    const view v = read_world(nullptr);
    if (!g_session_seen || (v.valid && !v.multiplayer)) begin_session();
}
void end_session() {
    g_new_match = g_session_seen = false;
    g_owner.end_match();
}
void prepare_frame() {
    if (!g_new_match) return;
    const view v = read_world(nullptr);
    if (v.valid && v.multiplayer && !g_owner.body_active()) {
        g_owner.new_match();
        g_new_match = false;
    }
}
void invalidate_world() {
    g_new_match = false; // an imported clock is PRE-body, not a completed empty prefix
    g_owner.invalidate_world();
}
void poll() {
    const uint32_t result = g_owner.poll();
    if (!(result & POLL_STOP) && g_owner.receive_ready())
        mh::call::llm_net_lockstep_dispatch();
}
uint64_t participant_body_boundary(uint32_t op, uint64_t argument) {
    if (!g_owner.ready()) fail("required host authority is not initialized");
    switch (static_cast<operation>(op)) {
        case operation::begin: prepare_frame(); return g_owner.body_begin();
        case operation::poll: poll(); return 0;
        case operation::world_invalidate: invalidate_world(); return 0;
        case operation::end_match: begin_session(); return 0;
        default: return g_owner.boundary(op, argument);
    }
}
int32_t participant_removal_admit(const void *data, uint32_t size) {
    if (!data || size != sizeof(request)) return int32_t(admission::invalid);
    request r;
    std::memcpy(&r, data, sizeof(r));
    if (g_terminal_scope && !r.received) return int32_t(admission::execute_now);
    return int32_t(g_owner.admit(r));
}
bool removal_ready(int32_t side) {
    const view v = read_world(nullptr);
    if (!v.valid) fail("removal observed an unbound world");
    if (!v.multiplayer) return true;
    const int32_t index = mh::call::llm_strat_player_by_side_id(side);
    if (index < 0 || index >= 8) return true;
    if (g_owner.has_removal(uint32_t(index))) return false;
    using namespace mh::state;
    if (!readable(RID_STRAT_PLAYERS, 8 * sizeof(mh::game::mh_llm_strat_player_profile)) ||
        !readable(RID_NET_PEER_HORIZON, 8 * sizeof(double)) || !readable(RID_PLAYERSIDE, sizeof(uint16_t)))
        fail("removal roster is unavailable");
    const auto *players = ptr<const mh::game::mh_llm_strat_player_profile>(RID_STRAT_PLAYERS);
    if ((players[index].status_flags & 6u) != 6u || index == *ptr<const uint16_t>(RID_PLAYERSIDE))
        return true;
    unsigned survivors = 0;
    for (int i = 0; i < 8; ++i)
        if (i != index && (players[i].status_flags & 6u) == 6u) ++survivors;
    return survivors <= 1 || g_owner.can_apply(bits(ptr<const double>(RID_NET_PEER_HORIZON)[index]));
}
void remove_terminal(int32_t side) {
    struct scope {
        scope() { ++g_terminal_scope; }
        ~scope() { --g_terminal_scope; }
    } immediate;
    mh::call::llm_net_player_remove(side);
}
} // namespace mh::participant_boundary::hosted
