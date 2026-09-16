//
// hook/watcall.h -- calling-convention bridges from the MSVC DLL into Watcom __watcall functions.
//
// mh.exe is Watcom-built: many functions take their first args in EAX/EDX/EBX/ECX rather than on
// the stack. MSVC can't express __watcall, so we bridge with tiny inline-asm shims. This header
// covers the common single-pointer-in-EAX shape used by the map/lobby/session helpers; naked
// register-marshalling detours for the multi-arg transport stubs stay local to net_seams (their
// register contracts are one-off and tied to specific hook sites).
//
#pragma once
#include <cstdint>

namespace mh::hook {

// Call a __watcall function taking a single pointer arg in EAX; returns EAX. EBX/ESI/EDI/EBP are
// callee-preserved under __watcall, so no manual save is needed; EAX/ECX/EDX are volatile (and
// MSVC treats them volatile across an __asm block too).
int call_watcall1(uintptr_t fn, void *a);

// Call a __watcall function taking 2 args in EAX/EDX (e.g. llm_ui_widget_layout_resolve_position(
// widget EAX, list EDX)); returns EAX. Both arg regs are volatile, so no save/restore needed. (P2)
int call_watcall2(uintptr_t fn, void *a, void *b);

// Call a __watcall function taking 3 args in EAX/EDX/EBX (e.g. llm_lobby_announce_line(name, fmt,
// src)); returns EAX. EBX is a __watcall ARG register (so the callee clobbers it) AND cdecl callee-
// saved here, so the shim saves/restores it. (U16)
int call_watcall3(uintptr_t fn, void *a, void *b, void *c);

} // namespace mh::hook
