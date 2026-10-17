//
// desync/replay_record.h -- the field-report replay inputs (2026-10-14): the two recordings that make
// a FIELD REPORT replayable without the test harness, written by mh.dll beside its order recording
// (order_capture.h) in the session folder.
//
// WHY. A shipped report carried mh_rec_orders.bin.gz + mh_rec_clock.bin.gz (mp:Q3) but not the match's
// starting state, and tools/replay_match_segment.py needs three inputs: orders, clock and the step-1
// state ("the seed"). mh_harness.dll's seed (mh_match_seed.bin) and per-step hashes do not ship (the
// release has no harness), so without these two files a field report can neither be replayed nor
// have its replay judged against the field run.
//
// WHAT IS WRITTEN (both default ON, `[desync] seed_record` / `[desync] hash_record_every`):
//   mh_rec_seed.bin     the step-1 state over every hash region in manifest order, PERSIST mode -- the
//                       harness's seed blob format (harness.cpp seed_dump_to), so `seed_mode=1` injects
//                       it unchanged. Taken at match step 1's PRE-BODY boundary, the point the harness
//                       takes mh_match_seed.bin. Gzipped right after (mh_rec_seed.bin.gz). Its size is
//                       the manifest's (~2.8 MB raw: every region is a fixed static array, so it does
//                       not grow with the map or the player count); compressed it is a few hundred KB.
//   mh_rec_hash.txt     a header line, then `<match step> <state hash, 16 hex>` every N steps (N =
//                       hash_record_every, default 256): the FNV state fold over the non-excluded
//                       regions with the harness's default masks -- the `state` column of
//                       mh_harness.log (hash kind 1), so a replay's own log is compared with it line for
//                       line. ~5 ms per checkpoint (one 2.8 MB walk), i.e. ~20 us per step amortised.
//
// SEGMENTS (hotfix rc9.4, 2026-10-15). A session in which the game REPLACES its state wholesale -- a
// save load, a campaign planet transition (the planet-file restore or a first-visit generation), the
// return from a tactical mission (the planet file the tactical entry wrote), a new campaign -- cannot be
// replayed past that point from the step-1 seed. So every such boundary is NOTED by the seam that sees
// it (seams/replay_boundary.cpp, before the state is replaced: the hash, the clock and the source file's
// content hash at that instant), and at the first sim step after it this module writes:
//   mh_rec_seed_<n>.bin(.gz)  segment n's seed, same format, same off-thread gzip (n = 1, 2, ...; the
//                             step-1 seed is segment 0);
//   mh_rec_segments.txt       one `segment <n> step=<k> kind=<...> ...` line per segment (segment 0 at
//                             step 1): k is the first step of segment n on the SESSION's own step axis
//                             (the one mh_rec_orders / mh_rec_clock / mh_rec_hash use), so the replay
//                             cuts those streams at k. The same line goes to mh_net.log.
//   mh_rec_hash.txt           a checkpoint at step k (the post-boundary hash), so a segment replay is
//                             judged from its first step.
// A boundary the seams did not see (a promoted SwitchToPlanet in configuration (2), a world resync) is
// still cut, by the per-step POLL: the planet index changed or the game clock went backwards. Its line
// says `kind=unhooked` and carries no pre-boundary hash (there was no instant to take it at).
//
// THE STEP AXIS is order_capture.h's: step k = the k-th sim step that ran with the session folder open
// (= the harness segment's match step). Its own counter, so it records whether or not the order
// recording is on (a harness-armed run that owns the order stream still gets a seed and a hash file).
//
// THREADING. The step runs on the sim thread; a boundary may be noted from another (the planet
// transition runs under its own mutex) -- the pending boundary is guarded. The seed and hash lines go to
// the async log sink as reliable binary records; the gzip runs on a background thread that waits for the
// sink (state_compress.h).
//
// NO ADDRESSES HERE (tools/check_sim_addresses.py: desync_watch.cpp is the only binder). The seed, the
// hash, the clock and the planet index are produced by callbacks the binder passes in.
//
#pragma once
#include <cstdint>

namespace mh::desync::replay_rec {

inline constexpr char SEED_NAME[]     = "mh_rec_seed.bin";
inline constexpr char HASH_NAME[]     = "mh_rec_hash.txt";
inline constexpr char SEGMENTS_NAME[] = "mh_rec_segments.txt";
// The SCENE (field-report scene dump): the whole bound world (829 regions + the heap nav graph, the
// libmh world blob, FORMAT 2) at the same instant as each seed, so a report replays without any save.
// mh_rec_scene.bin(.gz) for segment 0, mh_rec_scene_<n>.bin(.gz) for segment n. ~8 MB raw, ~300 KB gzipped.
inline constexpr char SCENE_NAME[] = "mh_rec_scene.bin";
// Scenes per session (a boundary past the cap is logged `scene=none(cap)`): bounds a session at ~5 MB of scenes.
inline constexpr int SCENES_MAX         = 16;
inline constexpr int HASH_EVERY_DEFAULT = 256;
// Segment seeds per session past the step-1 seed. A boundary past the cap is still logged (its line says
// `seed=none (cap)`); ~100 KB gz each, so the cap bounds a session at ~6.4 MB of seeds.
inline constexpr int SEGMENTS_MAX = 64;

using log_fn = void (*)(const char *fmt, ...);

// What replaced the state. `unhooked` = the per-step poll saw it (no seam did).
enum class boundary : int { session_start = 0,
                            save_load,
                            planet_transition,
                            tact_return,
                            new_game,
                            unhooked };
const char *boundary_name(boundary b);

struct hooks {
    // Write the seed blob to `path` through the sink (create + appends). Returns bytes handed over.
    // `segment` (0 = the step-1 seed) and `label` (boundary_name) name it for the sanity scan; `gen` is
    // the session folder's generation, `step` the segment's first step, `segments_path` the segments file.
    uint64_t (*seed)(const char *path, int segment, const char *label, unsigned long gen, uint32_t step,
                     const char *segments_path);
    // The state hash at this instant (hash kind 1, the harness's `state` column).
    uint64_t (*hash)();
    // The configuration number this process runs in (1 = no libmh spine, 2 = libmh bound) -- the replay
    // must run in the same one. May be null (then 0 = unknown).
    int (*configuration)();
    // The strategic game clock (seconds) and the current planet index. May be null (then no poll).
    double (*clock)();
    int (*planet)();
    // Write the scene blob to `path` through the sink (create). Returns bytes handed over, 0 = none. May be null.
    uint64_t (*scene)(const char *path, uint32_t step);
};

// Install time. `seed_on` = [desync] seed_record, `hash_every` = [desync] hash_record_every (0 = off).
void configure(int seed_on, int hash_every, const hooks &h, log_fn log);
bool enabled();
void set_compress(bool on);     // tests: keep the raw files
void set_scene_record(bool on); // `[desync] scene_record` (default on)

// Once per sim step at the PRE-BODY boundary -- the same arguments as orders::step.
void step(const char *session_dir, unsigned long gen, bool settled);

// A seam saw a wholesale state replacement BEFORE it happens (any thread). `what` names the source
// (the save name, "planet 5", ...), `file` is the file the state is about to be read from (its size and
// FNV-1a 64 are logged; null or "" = none), `planet` the planet index involved (-1 = n/a). Ignored with
// no session folder open (the step-1 seed of the session that opens later covers it). A second note before the next step
// keeps the first one's pre-boundary hash and clock (the state the session last ran on) and takes the
// later kind/file (what the session will run on).
void note_boundary(boundary kind, const char *what, const char *file, int planet);

// The session's close (the session layer's rollups).
void session_close(const char *why);

// The current file's step (1 = the first recorded step), 0 when no session folder is open. The door
// watch (seams/door_watch.cpp) stamps its lines with it.
uint32_t current_step();
// Segments written to the open session (0 = only the step-1 seed). Tests.
int segments();

// FNV-1a 64 + size of a file (0/0 when it cannot be read). Pure file I/O; exposed for tests.
bool file_fnv64(const char *path, uint64_t *fnv, uint64_t *bytes);

} // namespace mh::desync::replay_rec
