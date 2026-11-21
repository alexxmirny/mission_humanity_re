// Destination: src/mh_dll/libmh/sim/sim_order_tasks.cpp
#include "sim/sim_order_tasks.h"
namespace mh::sim::order_tasks_adapter {
// Strategic handlers run synchronously on the simulation thread. Nested calls are scoped;
// no save/snapshot/presentation can query this transient control-return stack.
namespace {
handler_budget_scope *current = nullptr;
}
handler_budget_scope::handler_budget_scope(uint32_t p, uint32_t s) : previous_(current), player_(p), slot_(s) { current = this; }
handler_budget_scope::~handler_budget_scope() {
    if (current == this) current = previous_;
}
void published() { current = nullptr; }
void latch(ot::actor_key actor, ot::control result) {
    if (result == ot::control::hold && current && actor.player == current->player_ && actor.slot == current->slot_)
        current->hold = true;
}
} // namespace mh::sim::order_tasks_adapter
