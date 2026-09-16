//
// sim/sim_stack_guard.h -- llm_stack_capacity_guard_0x20 @0x004219b7, as a libmh-internal no-op.
//
// LIFT-TABLE S5 (2026-09-09). This was a `host-callback:platform` entry, i.e. a REQUIRED service a
// standalone host had to implement or trap on. It is not a service at all. The original body is 38
// bytes and its whole content is `utils_assert_stack_capacity(0x20)` -- the Watcom `__STK` probe the
// compiler emits at the head of nearly every function in this image (see the `PUSH n / CALL
// 0x004cf46f` prologue on any of them). It touches no OS, no device and no game state, and asks one
// question: does the NATIVE CALL STACK of the running process have headroom.
//
// A host cannot answer that question for libmh, because it is not a question about libmh's caller --
// it is a question about the frame the probe is standing in, and under a C++ reimplementation that
// frame is MSVC's, not Watcom's. So the honest disposition is neither "the host implements it" nor
// "the host binds a no-op and hopes": libmh owns it, and owning it means doing nothing, because our
// compiler emits its own stack checks.
//
// THE MEMBERS STAY, and that is deliberate. Its seven sites sit on the not-found / impassable arms
// of four pathfinding bodies, and the offline suites count those calls to prove control flow REACHED
// those arms (a `return` immediately after one is the caller's, not the probe's). Deleting the
// calls-struct members would delete that evidence along with the entry.
//
#pragma once

namespace mh::sim {

// Bound where `llm_stack_capacity_guard_0x20` used to be. Empty by derivation, not by omission.
inline void stack_capacity_guard_noop() {}

} // namespace mh::sim
