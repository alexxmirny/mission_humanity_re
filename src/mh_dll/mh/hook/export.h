//
// hook/export.h -- install a generated ENTRY thunk over a game function (P0-EXPORT).
//
// The inverse direction of hook/watcall.h + addr/mh_calls.gen.h. Those let our C++ CALL the game;
// this lets a plain C++ function BE a game function: `tools/gen_dll_exports.py` emits a naked entry
// thunk that unmarshals the Watcom register/stack arguments into a cdecl call of your C++ body and
// applies the right callee stack cleanup on the way out. This header is the arming half.
//
// Layering: `hook/` carries no feature knowledge, so it cannot reach for the net seam's logger.
// Instead a logger is INSTALLED into it once at init (`set_export_logger`) and a failed arm reports
// itself through that. A replacement that does not arm must be loud -- a silent no-arm looks exactly
// like a working replacement whose body never mattered.
//
#pragma once
#include <cstdint>

namespace mh::hook {

enum class export_result {
    ok,
    bad_entry,      // the 8 entry bytes are not what we generated against: wrong build, or hooked
    install_failed, // VirtualProtect / write failed
    entry_owned,    // a DLL detour already owns this entry -- REBIND it, do not patch a second time
};

const char *export_result_str(export_result r);

// Install `thunk` over `target`, but only if the first 8 bytes there still equal `expect_entry8`.
//
// The guard is per-function expected BYTES rather than hook/detour.h's WATCOM_PROLOGUE constant,
// because installing a detour rewrites 8 bytes and the generator knows exactly which 8 it compiled
// against. That covers leaf helpers with no frame prologue (w_strlen opens `PUSH EDX; MOV EDX,EAX`)
// which a prologue check would refuse outright, and it is a tighter guard besides: it pins THIS
// function rather than "some Watcom function". A mismatch means a different build or an entry
// something else already hooked -- either way, do not write.
//
// On anything but `ok` the installed logger (if any) gets "; [export] <name> NOT armed -- <reason>".
export_result install_export(uintptr_t target, void *thunk, const char *name, uint64_t expect_entry8);

// Convenience: true iff the arm succeeded. Same logging.
bool install_export_ok(uintptr_t target, void *thunk, const char *name, uint64_t expect_entry8);

// Install the sink that failed arms report through (e.g. the net seam's seam_log). Until this is
// called, failures are still returned but not logged.
void set_export_logger(void (*fn)(const char *));

} // namespace mh::hook
