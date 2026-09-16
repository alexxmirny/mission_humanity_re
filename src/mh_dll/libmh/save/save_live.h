//
// save/save_live.h -- SV1-P: the save direction BOUND TO THE LIVE GAME, and promoted.
//
// SV1-DRIVERS left the walk deliberately unbound: `block_io` over an injected file object, `state_io`
// over an injected address space, the workspace over an injected buffer. That is what let a real .sav
// round-trip inside `net_selftest savetest` with no game running. This file is the other end of that
// design -- the three injections resolved against the real process:
//
//   block_io   -> mh::host().vfs_write / vfs_close over the handle vfs_open returned
//   state_io   -> the IDENTITY function; a game address IS a pointer inside the game's process
//   workspace  -> G_LZW_TEMP_DATA + G_LZW_MAX_COMPRESSED_SIZE, i.e. the original's own buffer
//
// plus the one thing the walk itself needed: `write_delegate`, for the two steps that belong to
// callees outside the closure (save_driver.h says which, and why one of them is not what the ledger
// recorded).
//
// WHAT IS PROMOTED, AND WHAT IS NOT. Exactly one entry: SavePlanetToDisk 0x00447bb3, behind
// the save closure key. The LOAD direction and the two container roots are SV1-P-LOAD -- they need nine
// more outward calls and two prototypes Ghidra has not committed, and one variable per run means they
// do not ride along with this.
//
// THE ORACLE IS AN IN-CALL A/B, not a shadow site. docs/save-format.md calls this subsystem
// un-shadowable because a rollback cannot un-write a file; true, and beside the point -- a promotion
// does not need a rollback, it needs the two writers to see the SAME STATE. Under
// `[save] verify=1` the promoted body writes OUR file, renames it aside, calls the ORIGINAL
// through a trampoline so it writes its own, and compares the two byte for byte within the one call.
// No state drift, no rollback, and the file left on disk is the ORIGINAL's -- so a divergence is
// reported and never inflicted on the session.
//
#pragma once
#include <cstdint>
#include "state/host_api.h"

namespace mh::save {

// Where the promoted body's evidence goes (the net seam's seam_log). Without it the A/B is silent,
// which for an oracle is the same as absent.
void set_logger(void (*fn)(const char *));

// Install the promotion. `default_on` is the selector's answer for the SAVE CLOSURE specifically --
// `mh::config::save_walkers_ours()`, i.e. "brokered AND the save closure is owned", which is false in
// every configuration today (config/config.h kSaveClosureOwned). The seams layer passes it in because
// a reimplementation TU may not include a seams header.
//
// `[save] verify=1` selects the A/B form, which installs through a TRAMPOLINE rather than a
// plain entry JMP -- the promoted body has to be able to still call the original, and install_export
// leaves no way to.
//
// Returns 1 if the seam is live, 0 otherwise.
int install_promotion(const char *ini_path, int default_on);

// The LOAD root -- a separate entry with separate evidence, and separately armable (it takes its own
// `default_on`). It shares `[save] verify` deliberately: a run is either verifying or it
// is not, and two independent verify levels would name configurations nobody has measured.
//
// Its oracle is NOT the save side's, because a load emits STATE rather than a file: it loads the same
// file TWICE and compares the state regions the block table names, which works because every block
// reads to a FIXED address. `[save] load_verify_poke=<step index>` is the mutation arm.
int  install_load_promotion(const char *ini_path, int default_on);
bool load_promotion_active();

// The CONTAINER WRITER. Its members are STREAMED through a second file,
// matching the game rather than the driver's arena round-trip model (`member_io` in save_driver.h).
int      install_container_promotion(const char *ini_path, int default_on);
bool     container_promotion_active();
unsigned savegame_now(char *save_name);

// The CONTAINER READER -- llm_game_load 0x004475de. It was once
// recorded here as unpromotable, on the grounds that its apply tail enters
// `llm_strat_time_resync_and_tick` with FPU compare flags live (0x00447b4e FLD / FCOMP / FNSTSW /
// SAHF / CALL) and a generated marshalling thunk would run in between. THAT WAS WRONG: the callee is
// 55 bytes of straight line that never reads EFLAGS, so the sequence is dead code and the root binds
// through the ordinary entry thunk. The correction is kept in save_live.cpp because the lesson --
// read the CALLEE before recording a calling contract as unrepresentable -- outlives the finding.
//
// Its oracle has TWO halves, because a container load produces two things: the state every block
// reads to a fixed address (compared by loading the same file twice, as SV1-P-LOAD does), and the
// per-planet member files it EXTRACTS, which are byte-comparable because that path copies bytes
// verbatim -- the one part of this direction the container WRITER's evidence does not cover.
// `[save] container_load_verify_poke=<step index>` and `container_load_member_poke=<byte+1>` are
// the two mutation arms, one per half.
int  install_container_load_promotion(const char *ini_path, int default_on);
bool container_load_promotion_active();

// Call the container LOAD root for `save_name`, through the game's entry (so the same trigger runs
// the original in an unpromoted run and ours in a promoted one). Returns 1 = success, 0 = failure.
unsigned loadgame_now(char *save_name);

// True once the entry belongs to us. Read by the harness's save trigger, which must not claim to have
// exercised a promotion that never installed.
bool promotion_active();

// True when the A/B is armed. Distinct from promotion_active(): a promoted run with verify OFF is the
// configuration that leaves OUR file on disk, which is what the load-interop half of the acceptance
// test needs, and it must not be mistaken for a verified one.
bool verify_active();

// Call the save root for `planet` in mode `mode` (3 = the mode game::SaveGame and llm_game_load both
// pass). Routed through the game's entry, so with the promotion installed this runs OUR body and with
// it absent this runs the original -- which is what makes it usable as a trigger in both arms.
// Returns the root's own convention: 1 = success, 0 = failure.
unsigned save_planet_now(int planet, unsigned mode);

// The path the last save_planet_now() wrote, or an empty string if it never got that far. Exposed so
// the harness can ARCHIVE each save under a distinct name -- the per-planet path is fixed, so a
// sequence of saves overwrites itself, and the period-2 invariant needs three of them side by side.
const char *last_save_path();

// THE LIVE SAVE'S state_io::resolve -- a game address to the memory it denotes (RI-STATE / ST2).
// Named here rather than left in the .cpp's anonymous namespace so `net_selftest statetest` can pin
// THE FUNCTION THE DRIVER ACTUALLY INSTALLS, not a copy of its logic: the claim under test is that
// the save path follows a region that moved, and a re-implementation of the resolver in the test
// would prove that about the test.
void *identity_resolve(void *ctx, uint32_t addr, uint32_t size);
// SB-HOSTFREE: state_io::resolve_region for the live arm -- one RUN of a decomposed block, by
// region id, because address resolution cannot disambiguate a run inside an overrunning window.
void *region_resolve(void *ctx, uint16_t rid, uint32_t off, uint32_t len);

} // namespace mh::save
