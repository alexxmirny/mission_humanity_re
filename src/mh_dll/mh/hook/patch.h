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

} // namespace mh::hook
