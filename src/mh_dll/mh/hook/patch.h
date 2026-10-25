//
// hook/patch.h -- guarded in-place byte patching of mh.exe code.
//
// Used for surgical edits that aren't detours: NOP-ing a call site, flipping a mov's immediate,
// etc. Every write is guarded by an expected-bytes compare so a wrong build / already-patched /
// unexpected-codegen site leaves the exe untouched (ship-safe no-op) instead of corrupting it.
//
#pragma once
#include <cstdint>

namespace mh::hook {

// Overwrite `len` bytes at `target` with `repl` ONLY if the current bytes equal `expect`. On a
// mismatch nothing is written and false is returned (the caller logs "MISMATCH" and stays inert).
// Also returns false if VirtualProtect fails. Flushes the icache on success.
bool patch_bytes_guarded(uintptr_t target, const uint8_t *expect, const uint8_t *repl, int len);

// PT-CAP8: the COMPOSING variant, for a DLL byte patch whose `expect` names bytes the v0.2.2 relocation
// manifest has since rewritten (the three hooks listed in tools/data/cap_compose_allow.json). `expect` is
// the VANILLA image as always; it is laid over with the bytes the applier actually wrote
// (mh::patch::overlay_applied) before the compare, so the guard still means "the bytes here are the ones
// this stub was written against" whether or not the cap layout is relocated. A hook installed through this
// keeps its site; its stub replicates the displaced instruction against mh_caps_base[] instead of the
// literal stock VA. Everything else is patch_bytes_guarded (C1 interlock, protect, icache flush).
bool patch_bytes_guarded_caps(uintptr_t target, const uint8_t *expect, const uint8_t *repl, int len);

} // namespace mh::hook
