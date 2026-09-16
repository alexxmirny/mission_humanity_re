//
// en_guard.h -- EN-only build gate. The DLL targets /eng/mh.exe exclusively (2026-07-22
// refactor); every seam module calls mh::en_build_ok() at init and
// arms NOTHING if the loaded exe is not the English build (RU support was dropped with
// mh_port -- the last RU-capable DLL lives in git history before that refactor).
//
#pragma once

namespace mh {

// True iff the loaded exe carries the Watcom `55 89 e5` prologue at three well-separated
// EN function entries (SEH-guarded probe; result cached). On any other image (RU, patched
// beyond recognition, foreign exe) every subsystem stays a ship-safe no-op -- the per-hook
// expected-bytes guards remain as the second safety layer.
bool en_build_ok();

} // namespace mh
