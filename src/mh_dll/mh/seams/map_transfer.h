#pragma once
//
// map_transfer.h -- THE MAP DOWNLOAD (tracker mp:X2, plan D7 + section 6 answer 9).
//
// THE PROBLEM, stated as the thing that can go wrong rather than as the feature. Two peers agree on
// a map by NAME. A map is a file in `Maps\`, players trade them, editors save over them, and a name
// is not an identity: `Cold War.mpm` on one machine and `Cold War.mpm` on another can be different
// bytes, and nothing in the lobby could tell. Both peers then load "the map", build different
// terrain, and the lockstep diverges on step 1 with a desync report that names neither the map nor
// the cause. The other half of the same problem is the easy half: a joiner who does not have the
// map at all cannot join at all, which at least fails loudly.
//
// SO THE MAP IS ADDRESSED BY ITS CONTENT. `SESSION_INFO` (v5) carries the SHA-256 of the host's map
// FILE, truncated to eight bytes, plus its length; a joiner reports in its `JOIN` (v5) the hash of
// whatever it holds under that name; and the host arms a transfer over channel C for any admitted
// joiner whose hash disagrees. The retail protocol had a map-chunk request shape of its own (frame
// types 0x0f/0x10/0x11) and it is dead at the wire, so it is the PRECEDENT for this existing at all
// and not the code -- the transfer runs on mp:X1's snapshot pipeline, which already does chunking,
// a committed hash vector, re-request and resume.
//
// ---- THE THREE RULES THAT MAKE THIS SAFE ---------------------------------------------------------
//
// 1. A DOWNLOAD NEVER OVERWRITES A LOCAL FILE -- OR APPEARS IN THE PLAYER'S MAP LIST. It is stored
//    as `<stem>.<16 hex>.<ext>` (`mh_net_proto::map_stored_name`) in `mh_dl\`, which is NOT the map
//    directory and not under it; see the DL_DIR_DEFAULT block below, where the picker's own folder
//    scan is the reason. The player's own `Cold War.mpm` is still there, byte for byte, after a
//    match played on somebody else's, and their map list is the one they left. That is not
//    politeness: a map a player edited is work, and a multiplayer session silently replacing it
//    would be data loss caused by joining a game.
//
// 2. THE LOAD RESOLVES BY HASH, NOT BY NAME -- and it resolves WITHOUT CHANGING WHAT THE GAME
//    THINKS THE MAP IS CALLED. This is the constraint that shapes the whole file. `current_map_data`
//    is copied into `cfg::final::struct::Planet[31]` at session begin, and `planets` IS a determinism
//    hash region (mh_regions.gen.h, RID_PLANETS, not excluded, and `emit_planets` hashes the record's
//    string fields as ordinary bytes). So a peer that renamed its map in memory to the hash-qualified
//    form would hash a different `planets` slice than its opponent and the determinism gate would go
//    red at step 0 -- with a "desync" that is really a filename. The redirect therefore happens at
//    the FILE OPEN: `utils_open_file` is replaced, and a request for the base name under which we
//    hold a redirect is served the stored file instead. Every byte of game state stays identical on
//    both peers; only the directory entry differs.
//
// 3. THE HOST CANNOT START WHILE A JOINER IS STILL DOWNLOADING. Not as a courtesy -- a peer that
//    entered the match without the map would either fail to load it or load its own and desync, and
//    both are worse than waiting. The refusal is visible: the lobby's status line names the peer.
//
// ---- WHAT "THE BYTES THIS PEER WOULD LOAD" MEANS, AND WHY IT IS NOT "THE FILE IN `Maps\`" ---------
//
// `cfg::ReadMapFile` and `map::ReadMap` both resolve a map name through `rsr::GetResourseFilePtr`
// FIRST and fall back to a loose file only when the resource packs do not hold it. So the identity
// of a map name on a given install is the resource-pack copy when there is one. A host whose map
// comes out of `mh.rsr` therefore makes NO CLAIM (all-zero hash): a stock map is the same bytes on
// every install of the same build by construction, and claiming a loose file's hash while loading
// the pack's would be a claim about the wrong bytes. This is also why the item's scope says "custom
// maps included" -- the custom ones, loose in `Maps\`, are the population that needed solving.
//
// A joiner whose pack DOES hold a name the host claims loosely is the one case a download cannot
// rescue, because the pack shadows any loose file whatever it is called. That peer is BLOCKED by
// name rather than quietly played with the wrong map.
//
// ---- WHY THE REQUEST IS A JOIN FIELD AND NOT A FRAME ----------------------------------------------
//
// See `JOIN_REQUEST_FORMAT` v5 in mh_net_proto/session_info.h. Short version: "which map bytes do
// you hold" is true exactly once, at the moment a peer asks to be admitted, and the host must know
// it before it can answer "may anyone Start". A separate frame would be a second thing to lose, to
// retry and to reconcile. The COMPLETION report is the same field in a re-sent JOIN, which is also
// why the host's "this peer is done" is an assertion at the RECEIVER: the host believes the transfer
// landed because the peer said what it now holds, never because the sender finished pushing.
//
#ifndef MH_SEAMS_MAP_TRANSFER_H
#define MH_SEAMS_MAP_TRANSFER_H

#include <stddef.h>
#include <stdint.h>

#include "mh_net_proto/session_info.h" // SessionInfo / MAP_HASH_BYTES / map_stored_name

namespace mh {
namespace seams {
namespace maps {

// ---- the file layer: small, and deliberately callable with no game and no network ---------------
//
// Every one of these is reachable from `net_selftest.exe maptest`, which compiles this TU like every
// other seam TU. They take their directory as an argument for exactly that reason: the suite points
// them at a scratch directory in %TEMP% and proves the naming, the refusals and the round trip with
// real files, so the rig is left to prove the INTEGRATION rather than the mechanism.

// The directory a map of this name loads from, by the loader's own extension rule (`.mpm` loads
// loose from `Maps\`; anything else from the `Dane\` campaign path). Returns a static string.
const char *dir_for(const char *name);

// SHA-256 the file at `<dir><name>` and truncate to the wire's eight bytes. False if it is not
// there or cannot be read; `*out_size` is the file's length on success.
bool file_hash(const char *dir, const char *name, uint8_t out[mh_net_proto::MAP_HASH_BYTES],
               uint32_t *out_size);

// Read `<dir><name>` whole. Returns a VirtualAlloc'd buffer the caller releases with free_bytes(),
// or null. Refuses anything past `cap_bytes` (the snapshot pipeline's blob cap) rather than
// allocating it: a "map" of 40 MB is a mistake, not a map.
uint8_t *read_file(const char *dir, const char *name, uint32_t cap_bytes, uint32_t *out_len);
void     free_bytes(uint8_t *p);

// WHERE A DOWNLOAD LANDS, AND WHY IT IS NOT IN `Maps\` AT ALL. The stored NAME is
// `<stem>.<16 hex>.<ext>` -- that is the tracker's requirement and what makes the file
// content-addressed -- but the file goes in `mh_dl\` beside the executable, NOT in the map
// directory and NOT in a subdirectory of it. That is measured, and the first two guesses were both
// wrong:
//
//   * BESIDE THE PLAYER'S MAPS is wrong because the picker would list it. The map list would grow a
//     second, hash-suffixed "Blue Monday" the player never made; on the local rig, whose lanes SHARE
//     one `Maps` by symlink (tools/make_lane.py, LINKED_DIRS), that extra row also changes what every
//     other scenario's picker defaults to.
//   * A SUBDIRECTORY OF `Maps\` is wrong for a sharper reason, and it cost a rig run to find:
//     `llm_mp_mappicker_populate_list` (EN 0x004c0aca) scans `Maps\` for SUB-FOLDERS FIRST
//     (`llm_fs_scan_dir` with the folder callback `llm_mp_mappicker_add_folder`) and only then for
//     `*.mpm`. The picker supports map folders by design, so a download directory becomes the FIRST
//     ROW of the list and the pre-selected one -- the host script's `clickl Ok` then descended into
//     it and the lobby never opened. "The picker does not recurse" was an assumption; the folder
//     callback is the code.
//
// So downloads live outside the scanned tree entirely, and `resolve`/`store` report a PATH RELATIVE
// TO THE WORKING DIRECTORY rather than a name relative to a map directory. The redirect then
// substitutes the whole path instead of splicing a basename, which is also simpler: a downloaded map
// is not "in the maps folder under another name", it is somewhere else, and the only thing that ever
// needs to know is the one file-open call that asks for it.
constexpr const char *DL_DIR_DEFAULT = "mh_dl\\";
// Room for the download directory + the longest stored name.
constexpr size_t STORED_PATH_CAP = mh_net_proto::MAP_STORED_NAME_CAP + 64;

// The download directory, as a prefix ending in a backslash. `set_dl_dir` exists for `maptest`,
// which points the whole file layer at a scratch directory in %TEMP%; nothing in the game calls it.
const char *dl_dir();
void        set_dl_dir(const char *dir_with_trailing_slash);

// Where a wanted content hash can be found locally, if anywhere.
enum class Resolve {
    Base,    // the file at the base name already IS this content -- open it, redirect nothing
    Stored,  // a previous download holds it (`mh_dl\<stem>.<hex>.<ext>`) -- redirect the base there
    Missing, // nothing local holds it -- it must be fetched
};

// Decide, by HASHING the candidates rather than by trusting their names. `out_path` receives the
// PATH to open, relative to the working directory: `<dir><base>` for Base, `<dl_dir()><stored>` for
// Stored, untouched for Missing. `skip_base` suppresses the base-name candidate: the UI harness
// needs a client that behaves as though it does not hold the content, and making that a DECISION
// rather than a file move is what lets the scenario run against a shared `Maps` directory.
Resolve resolve(const char *dir, const char *base,
                const uint8_t want[mh_net_proto::MAP_HASH_BYTES], char *out_path, size_t cap,
                bool skip_base = false);

// Write `bytes` as the stored name for (base, hash) into `dl_dir()`, and REFUSE to write anything
// whose content does not hash to `hash` -- a delivered blob is checked here, at the last moment
// before it becomes a file, and not only by the transfer that carried it. `out_path` receives the
// path written. False on any refusal; nothing is left behind on the failing paths.
bool store(const char *base, const uint8_t hash[mh_net_proto::MAP_HASH_BYTES], const void *bytes,
           uint32_t len, char *out_path, size_t cap);

// ---- the OPEN redirect (rule 2 above) -----------------------------------------------------------
//
// One entry, because one lobby picks one map. Registering a second replaces the first; clearing is
// what a lobby exit does. `redirect_apply` is the pure half of the replaced `utils_open_file`: given
// the path the game asked for, it answers the path to actually open -- which is `in` itself
// whenever nothing is registered or the basename does not match.
void        redirect_set(const char *base, const char *stored);
void        redirect_clear();
const char *redirect_apply(const char *in, char *scratch, size_t cap);

// ---- the seam proper ----------------------------------------------------------------------------

// Arm the `utils_open_file` replacement. Idempotent; called from MH_Core_Arm (net_seams.cpp).
void install();

// One lobby frame, from on_lobby_dispatch (net_seams.cpp). Drives the host's per-peer state machine
// and the client's download, repaints the status-line notice, and opens or closes the Start gate.
void lobby_tick(int is_host);

// S2 (net_discovery.cpp): stamp our own map's content claim into the advert we are about to send.
void host_fill_advert(mh_net_proto::SessionInfo &si);

// S4 (net_discovery.cpp), host side: an admitted JOIN reported what that peer holds. `player_name`
// is for the lobby notice; `map_hash` is the peer's own measurement of its own file.
void host_on_join(int sender, const char *player_name,
                  const uint8_t map_hash[mh_net_proto::MAP_HASH_BYTES]);

// U12 (net_discovery.cpp), host side: that peer left the lobby -- forget its download state, so a
// departed peer can never hold the Start gate shut.
void host_on_leave(int sender);

// THE START GATE. True while some admitted joiner's download is incomplete; `out_peer` receives that
// peer's name. Read by the lobby tick (which closes the button) and by nothing else -- a second
// caller would be a second answer.
bool host_start_blocked(char *out_peer, int cap);

// WHICH peer the next transfer is for, or -1 for nobody. Split out of the pump so the CHOICE is a
// function the offline suite can drive: "a joiner already holding the content transfers nothing" is
// a claim about this returning -1, and proving it on the rig alone would mean reading an absence.
int host_next_peer_needing_map();

// THE OFFLINE DOOR, and it exists for the reason `reset_session_latch_for_test` does one layer down:
// the host's claim is normally read out of `current_map_data`, a fixed VA inside mh.exe, so every
// decision that depends on it would be unreachable in a process with no game in it. This sets the
// same three fields directly. Never called from a game path -- `net_selftest.exe maptest` is its
// only caller, and the host tick overwrites it from the live map name on the next frame.
void host_set_claim_for_test(const char   *map_name,
                             const uint8_t hash[mh_net_proto::MAP_HASH_BYTES], uint32_t size);

// S3 (net_discovery.cpp), client side: a host advert arrived. Records what map content this lobby
// requires and decides, once, whether we already hold it.
void client_on_advert(const mh_net_proto::SessionInfo &rec);

// The JOIN field (net_discovery.cpp), client side: the hash WE hold for the advertised map, or false
// for "nothing that matches" (which encodes as the all-zero no-claim).
bool client_my_hash(uint8_t out[mh_net_proto::MAP_HASH_BYTES]);

// Client side: a stored download owes the host a re-JOIN (the completion report -- see the header's
// note on why the JOIN field IS the protocol). One-shot; net_discovery.cpp drains it from the lobby
// tick because that TU owns the stored host record and the player name the JOIN carries.
bool client_take_rejoin();

// U2 (net_discovery.cpp), client side: the host's FLAG_START arrived. Samples the base map file one
// last time, so a log carries the bytes of the player's own file AFTER the match it was not used in.
void client_on_start();

// Both sides: the lobby is over (entered, cancelled or lost). Drops the per-lobby state.
void session_reset();

} // namespace maps
} // namespace seams
} // namespace mh

#endif // MH_SEAMS_MAP_TRANSFER_H
