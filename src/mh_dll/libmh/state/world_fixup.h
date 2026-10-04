//
// state/world_fixup.h -- the FIXUP TRAILER of a world blob (tracker mp:X3a).
//
// WHAT IT IS FOR. A world blob carries every bound region VERBATIM (world_snapshot.h note 1). For a
// live mh.exe -> mh.exe import that is right for every dword EXCEPT one whose value is PROCESS-LOCAL:
//   (H) a heap / private-memory address (the .TLO buffer, the framebuffer, a COM device, an open
//       dialog the sender allocated), and
//   (D) an address inside one of OUR ASLR'd DLL images (mh.dll, libmh.dll, mh_harness, mh_net, ...),
//       or inside a region we RELOCATED into a DLL's .bss (the AI island, the reloc arena).
// A pointer from one STOCK region into another is the same value in both processes -- mh.exe is fixed
// at 0x00400000 -- so that class (the seven UI widget-list heads the old banner worried about) is
// already safe live and needs nothing. The population is therefore not derivable at build time and
// not visible to the importer: only the CAPTURING process knows where its own heap and DLLs are. So
// the capture classifies every 4-step dword of every carried block against ITS OWN address map and
// writes the result down as data: this trailer.
//
// THE HASHED BYTES ARE NEVER TOUCHED, by capture or by import. A dword inside a HASH_REGIONS slice is
// skipped (and counted in hashed_skipped): were an unmasked hashed slice to hold a process-local
// pointer the two peers' lockstep hashes would differ on every step. The trailer therefore cannot
// move SNAPIMP's columns, which is what lets it be additive.
//
// LAYOUT. It sits at `nav_offset + nav_len` and is measured by the header word that used to be
// `pad_` (blob_header::fixup_len; 0 == absent, which is every blob recorded before it existed --
// hence NO format bump and no re-record). Little-endian, every field naturally aligned.
//
// CLASSES (fixup_entry::kind):
//   REGION   -- the value points INTO a carried region that lives at a non-stock address in the
//               sender. The receiver writes live_base(pointee) + arg2.
//   REBASE   -- the value points into an image module. The receiver writes its own module base + rva,
//               after an identity check (name + TimeDateStamp + SizeOfImage).
//   PRESERVE -- process-local memory with no description in the blob. The receiver keeps its OWN
//               dword at that (block, offset) -- including NULL.
// A view-owned holder (MF_VIEW without MF_SAVE/MF_HASH: presentation state) turns REGION and REBASE
// into PRESERVE: the receiver owns its screen, and rebasing would adopt the sender's open dialog.
//
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "addr/mh_regions.gen.h"

namespace mh::state::world {

// ---- regions the importer ZEROES and rebuilds anyway -------------------------------------------
//
// One list for both halves: state/spine.cpp clears these on import (llm_map_region_pool_reset would
// otherwise free() the RECORDING process's heap nodes), and the capture-side classifier skips them
// (MAP_REGION_GRID alone is ~59k heap pointers, all destined to be zeroed). The GRID is not in the
// array because it is cleared cell by cell through the sim view, but it is zeroed all the same --
// is_zeroed_on_import() answers for both.
inline constexpr mh::state::region_id ZEROED_ON_IMPORT[] = {
    mh::state::RID_MAP_REGION_BY_INDEX,         // 4096 llm_map_region*, by region index -- all pointer
    mh::state::RID_MAP_REGION_LIST_HEAD,        // the active-region list head
    mh::state::RID_MAP_REGION_POOL_FREE_HEAD,   // the allocator's free list head
    mh::state::RID_STRAT_PATH_JOB_RESULT_TABLE, // 100 heap pointers; pathfinder_init refills them
};
// ---- OS handles: process-local by NATURE, invisible to an address map ---------------------------
//
// An HWND / HANDLE is a small integer from the kernel's handle table, not an address, so no VirtualQuery
// walk can classify it. Carried verbatim it silently retargets the importer at the SENDER's object:
// `hWnd_00824fe8` is the main window, WinMain's pump calls RedrawWindow(hWnd) to make WM_PAINT and thus
// every frame, and an imported HWND redraws the other process's window while this one's message loop
// sits in WaitMessage forever (measured 2026-09-29: two of three live imports froze the importer at the
// first frame after the hold, main thread in win32u under WinMain). The capture therefore emits a
// PRESERVE entry for each of these regardless of value.
inline constexpr mh::state::region_id OS_HANDLE_REGIONS[] = {
    mh::state::RID_HWND_00824FE8, // the game's main window
};
inline bool is_os_handle_region(mh::state::region_id r) {
    for (const mh::state::region_id z : OS_HANDLE_REGIONS)
        if (z == r) return true;
    return false;
}

inline bool is_zeroed_on_import(mh::state::region_id r) {
    if (r == mh::state::RID_MAP_REGION_GRID) return true;
    for (const mh::state::region_id z : ZEROED_ON_IMPORT)
        if (z == r) return true;
    return false;
}

// ---- mp:X3c: regions a RESYNC import must NOT take from the blob ---------------------------------
//
// A world blob carries this peer's IDENTITY and TRANSPORT state along with the sim: it is a byte
// copy of the sender's bound regions. mp_snapshot never noticed because it HELD the sim; a resync
// imports into a session that keeps running, and a client that took the host's copy of these would
// believe it is the host's seat and would resume a barrier / send buffer that is not its own.
//
// The importer therefore saves these regions' bytes before world::import() and restores them right
// after fixup_apply() (state/spine.cpp), the same place and same "no window" discipline as the
// process-handle preserve pass.
//
// DERIVED FROM READERS, NOT FROM THE MANIFEST (dead-ends G197). Two invariants are asserted by
// `worldtest` arm G:
//   (1) no entry backs a hash slice that is in the STATE verdict (an entry whose only slices are
//       `excluded` -- the clock family, ls_horizon, peer_horizon, order_staging -- is the design:
//       those are per-peer by nature and the verdict already ignores them);
//   (2) order_pending / order_pending_arr are NOT here: they are the INPUT QUEUE and are imported.
//
// RE-3 (mp:X3c): _G_LLM_GAME_SESSION_MODE is a 4-byte REGION. The "1623 bytes" is the SAVE
// BLOCK that starts there (save_table.gen.h SLICED_BLOCKS: 53 runs over 49 other regions plus gaps).
// The world blob carries one block per REGION, so it carries the mode dword and nothing else from
// that block; each neighbour is judged below on its own readers.
inline constexpr mh::state::region_id RESYNC_KEEP_LOCAL[] = {
    // -- identity: who THIS peer is --
    mh::state::RID_PLAYERSIDE,              // local player index; session_begin_multi seeds it from NET_LOCAL_PLAYER_SLOT
    mh::state::RID_STRAT_LOCAL_PLAYER_SLOT, // the strategic-side local slot (planet_session_begin writes it)
    mh::state::RID_STRAT_PLAYER_RACE,       // Players[PlayerSide].race: the LOCAL player's race (voice/UI/HUD readers)
    mh::state::RID_NET_LOCAL_PLAYER_INDEX,  // transport identity
    mh::state::RID_NET_IS_HOST,             // host/client role
    mh::state::RID_NET_LOCAL_PLAYER_SLOT,   // the MP stack's own local slot
    mh::state::RID_LOBBY_LOCAL_SLOT_INDEX,  // lobby seat of this peer
    // -- transport: this peer's own wire state --
    mh::state::RID_NET_SEND_BUF,              // outbound batch under construction
    mh::state::RID_NET_SEND_BUF_CURSOR,       //   ... and its cursor
    mh::state::RID_NET_LOCKSTEP_STATUS_FLAGS, // per-peer barrier status
    mh::state::RID_NET_RESYNC_IN_PROGRESS,    // retail mode-8 barrier latch (not ours to inherit)
    mh::state::RID_NET_SYNC_WAIT_ACTIVE,
    mh::state::RID_NET_LOCKSTEP_SYNC_RETRY_COUNTDOWN,
    mh::state::RID_NET_LOCKSTEP_SYNC_WAIT_ELAPSED,
    mh::state::RID_NET_LOCKSTEP_RESYNC_TRIGGER_COUNT,
    mh::state::RID_NET_LOCKSTEP_STALL_NAG_COUNT,
    mh::state::RID_STRAT_LOCKSTEP_STALL_COUNT,
    mh::state::RID_NET_LOCKSTEP_PEER_TIMEOUT_ELAPSED,
    // -- horizons: the live values the catch-up mirrors (design section 2) --
    mh::state::RID_STRAT_LOCKSTEP_HORIZON,
    mh::state::RID_STRAT_LOCKSTEP_COMMITTED_HORIZON,
    mh::state::RID_NET_PEER_HORIZON,
    mh::state::RID_STRAT_LOCKSTEP_STEP_SIZE,
    mh::state::RID_STRAT_LOCKSTEP_STEP_MULT, // session-start pacing config (reset writes, enter_gameplay_sync reads)
    // -- this peer's own proposals --
    mh::state::RID_STRAT_ORDER_STAGING,
    mh::state::RID_STRAT_ORDER_STAGING_COUNT,
    // -- frame clocks: wall-clock derived, per-peer frame rate --
    mh::state::RID_TOTAL_GAME_TIME,
    mh::state::RID_LAST_GAME_TIME,
    mh::state::RID_CURRENT_GAME_TIME,
    mh::state::RID_STRAT_FRAME_TIME_RING,
    mh::state::RID_STRAT_FPS_ESTIMATE,
    mh::state::RID_STRAT_SIM_STEP_INTERVAL,
    mh::state::RID_GAME_SPEED,
    mh::state::RID_GAME_TIME_DELTA,
    // -- presentation: this peer's own screen and options --
    mh::state::RID_STRAT_CAM_PAN_TARGET_COL, // camera (R1: the view must not jump to the host's)
    mh::state::RID_STRAT_CAM_PAN_TARGET_ROW,
    mh::state::RID_CHAT_TARGET_MASK,      // chat recipients: local UI choice
    mh::state::RID_STRAT_SHOW_UNIT_FLAGS, // local display option
};
inline constexpr int RESYNC_KEEP_LOCAL_COUNT =
    static_cast<int>(sizeof(RESYNC_KEEP_LOCAL) / sizeof(RESYNC_KEEP_LOCAL[0]));

inline bool is_resync_keep_local(mh::state::region_id r) {
    for (const mh::state::region_id k : RESYNC_KEEP_LOCAL)
        if (k == r) return true;
    return false;
}

// The keep-local x hash-verdict invariant, over an arbitrary list (so the selftest can mutate it).
// Returns the number of (region, slice) pairs where a keep-local region backs a hash slice that is NOT
// excluded from the state verdict. 0 is the only acceptable answer for the real list.
//
// KEYED BY REGION ID, NOT BY ADDRESS, on purpose: a hash slice is a window into its region
// (HASH_REGIONS[].rid/offset/len), and REGIONS[].base is 0 in a libmh build (MH_STOCK_BASE), so an
// address-overlap test would either be vacuous or report everything. Registry regions are disjoint,
// so "same rid" is the whole overlap relation.
inline int keep_local_verdict_conflicts(const mh::state::region_id *list, int n, const char **first_region = nullptr,
                                        const char **first_slice = nullptr) {
    int bad = 0;
    for (int i = 0; i < n; ++i) {
        for (int h = 0; h < mh::state::HASH_REGION_COUNT; ++h) {
            const auto &hs = mh::state::HASH_REGIONS[h];
            if (hs.excluded || hs.rid != list[i]) continue;
            if (bad == 0 && first_region) *first_region = mh::state::REGIONS[list[i]].name;
            if (bad == 0 && first_slice) *first_slice = hs.name;
            ++bad;
        }
    }
    return bad;
}

// The keep-local scratch: the bytes of every RESYNC_KEEP_LOCAL region, taken before the byte engine
// runs and put back right after the fixup. Sized from the registry at compile time, so it cannot be
// outgrown silently (~22 KB, dominated by the 20400-byte order_staging array). Callers keep it static.
constexpr uint32_t keep_local_capacity() {
    uint32_t t = 0;
    for (const mh::state::region_id r : RESYNC_KEEP_LOCAL) t += mh::state::REGIONS[r].size;
    return t;
}
struct keep_local_store {
    uint8_t  data[keep_local_capacity()];
    uint32_t off[RESYNC_KEEP_LOCAL_COUNT];
    uint32_t len[RESYNC_KEEP_LOCAL_COUNT];
};
void keep_local_save(keep_local_store &k, uint32_t *out_regions, uint32_t *out_bytes);
void keep_local_restore(const keep_local_store &k);

// mp:X3c (RE-1): reset THIS peer's selection after a resync import (control groups, UI-selected
// building, click-select target, every unit's ctrl_group_id). Defined in state/spine.cpp (it needs the
// sim store). See the note there.
void reset_local_selection();

inline constexpr uint32_t FIXUP_MAGIC   = 'M' | ('H' << 8) | ('F' << 16) | ('X' << 24);
inline constexpr uint16_t FIXUP_VERSION = 1;

inline constexpr uint32_t FIXUP_MAX_MODULES = 64;
inline constexpr uint32_t FIXUP_MAX_ENTRIES = 65536;
inline constexpr uint32_t FIXUP_MAX_BLOCKS  = 1024; // >= WORLD_SNAPSHOT_BLOCK_COUNT (asserted in the .cpp)

inline constexpr uint8_t FIXUP_REGION   = 1;
inline constexpr uint8_t FIXUP_REBASE   = 2;
inline constexpr uint8_t FIXUP_PRESERVE = 3;

inline constexpr uint32_t FIXUP_MOD_OURS = 1u << 0;

struct fixup_trailer_header { // 32 bytes
    uint32_t magic;           // FIXUP_MAGIC
    uint16_t version;         // FIXUP_VERSION
    uint16_t module_count;    // <= FIXUP_MAX_MODULES
    uint32_t entry_count;     // <= FIXUP_MAX_ENTRIES
    uint32_t checksum;        // FNV-1a-32 over modules[] + entries[]
    uint32_t hashed_skipped;  // capture diagnostics, outside the checksum's meaning
    uint32_t unmapped;        //   (a value >= 0x10000 landing in no mapped range: an integer, mostly)
    uint32_t zeroed_skipped_blocks;
    uint32_t reserved; // 0
};
struct fixup_module {  // 40 bytes
    char     name[28]; // lowercase basename, NUL-padded ("mh.dll")
    uint32_t time_date_stamp, size_of_image;
    uint32_t flags; // FIXUP_MOD_*
};
struct fixup_entry { // 16 bytes, sorted by (block, offset)
    uint16_t block;  // index into WORLD_SNAPSHOT_BLOCKS
    uint8_t  kind;   // FIXUP_REGION / _REBASE / _PRESERVE
    uint8_t  module; // REBASE only
    uint32_t offset; // byte offset inside the block; offset + 4 <= block len
    uint32_t arg;    // REGION: pointee rid;  REBASE: rva
    uint32_t arg2;   // REGION: offset in the pointee
};
static_assert(sizeof(fixup_trailer_header) == 32, "the fixup trailer is a file format");
static_assert(sizeof(fixup_module) == 40, "the fixup trailer is a file format");
static_assert(sizeof(fixup_entry) == 16, "the fixup trailer is a file format");

inline constexpr size_t FIXUP_MAX_TRAILER_BYTES =
    sizeof(fixup_trailer_header) + FIXUP_MAX_MODULES * sizeof(fixup_module) +
    static_cast<size_t>(FIXUP_MAX_ENTRIES) * sizeof(fixup_entry);

// ---- inline readers, header-only so a consumer (the harness's capture log line) needs no new symbol --
inline uint32_t fixup_checksum(const void *modules, size_t nmod, const void *entries, size_t nent) {
    uint32_t       h = 2166136261u;
    const uint8_t *p = static_cast<const uint8_t *>(modules);
    for (size_t i = 0; i < nmod * sizeof(fixup_module); ++i) h = (h ^ p[i]) * 16777619u;
    p = static_cast<const uint8_t *>(entries);
    for (size_t i = 0; i < nent * sizeof(fixup_entry); ++i) h = (h ^ p[i]) * 16777619u;
    return h;
}

// Entry counts by class, straight out of a written trailer. `t` may be unaligned.
struct fixup_summary {
    uint32_t entries = 0, region = 0, rebase = 0, preserve = 0;
    uint32_t hashed_skipped = 0, unmapped = 0, zeroed_skipped_blocks = 0, modules = 0;
    bool     valid = false;
};
inline fixup_summary fixup_summarize(const void *trailer, size_t len) {
    fixup_summary        s;
    fixup_trailer_header h;
    if (trailer == nullptr || len < sizeof(h)) return s;
    std::memcpy(&h, trailer, sizeof(h));
    if (h.magic != FIXUP_MAGIC || h.version != FIXUP_VERSION) return s;
    if (h.module_count > FIXUP_MAX_MODULES || h.entry_count > FIXUP_MAX_ENTRIES) return s;
    const size_t need =
        sizeof(h) + h.module_count * sizeof(fixup_module) + h.entry_count * sizeof(fixup_entry);
    if (need > len) return s;
    const uint8_t *e =
        static_cast<const uint8_t *>(trailer) + sizeof(h) + h.module_count * sizeof(fixup_module);
    for (uint32_t i = 0; i < h.entry_count; ++i) {
        fixup_entry fe;
        std::memcpy(&fe, e + i * sizeof(fe), sizeof(fe));
        if (fe.kind == FIXUP_REGION) ++s.region;
        else if (fe.kind == FIXUP_REBASE) ++s.rebase;
        else if (fe.kind == FIXUP_PRESERVE) ++s.preserve;
    }
    s.entries               = h.entry_count;
    s.hashed_skipped        = h.hashed_skipped;
    s.unmapped              = h.unmapped;
    s.zeroed_skipped_blocks = h.zeroed_skipped_blocks;
    s.modules               = h.module_count;
    s.valid                 = true;
    return s;
}

// ---- the CAPTURING process's address map -------------------------------------------------------
enum : uint8_t { AM_PRIVATE = 1,
                 AM_IMAGE   = 2 };

struct addr_range {
    uint32_t base, size;
    uint8_t  kind;   // AM_*
    uint8_t  is_exe; // an AM_IMAGE range of the process's own exe (fixed base: never rebased)
    uint16_t module; // AM_IMAGE: index into addr_map::mod, 0xFFFF == unnamed
};
struct addr_module {
    fixup_module m;
    uint32_t     alloc_base;
};

inline constexpr int ADDR_MAP_MAX_RANGES  = 32768;
inline constexpr int ADDR_MAP_MAX_MODULES = 512;

// Heap-allocate one (about 0.5 MB): `new addr_map`.
struct addr_map {
    int         nrange = 0, nmod = 0;
    bool        overflow = false; // the walker ran out of room: a capture over it refuses
    addr_range  range[ADDR_MAP_MAX_RANGES];
    addr_module mod[ADDR_MAP_MAX_MODULES];

    void clear() {
        nrange   = 0;
        nmod     = 0;
        overflow = false;
    }
    // Builders, also used by the offline arms to synthesise a process. Return the module index.
    int               add_module(const char *lower_name, uint32_t alloc_base, uint32_t stamp, uint32_t size_of_image,
                                 bool ours);
    void              add_range(uint32_t base, uint32_t size, uint8_t kind, int module = 0xFFFF, bool is_exe = false);
    void              finish(); // sort + merge; call once after the last add_range
    const addr_range *find(uint32_t v) const;
};

// A map with NO mapped ranges: every dword classifies as "none". For offline arms whose arena is
// pattern-filled (their dwords are noise, not pointers) and which are not about the trailer.
const addr_map *empty_addr_map();

// Walk the real address space with VirtualQuery. False if the platform has none or it overflowed.
bool build_real_addr_map(addr_map &m);

// ---- a module's identity in THIS process, for the import side ---------------------------------
struct module_identity {
    uint32_t base, time_date_stamp, size_of_image;
};
// Returns false if `lower_name` is not loaded. Null resolver == the real one (GetModuleHandleA + PE).
struct module_resolver {
    bool (*fn)(void *ctx, const char *lower_name, module_identity *out);
    void *ctx;
};

// ---- which arm is importing (tooling:TL-LIBREF-FIXUP-HOST) --------------------------------------
//
// The trailer's OURS entries name the SENDER's images, and a same-named image loaded here with another
// identity is a mixed-build pair: refused, because a REBASE would land in a different layout. That is
// right for the case the trailer exists for -- a live mh.exe -> mh.exe import, both hosting libmh.dll.
// It is wrong for exactly one image in exactly one arm: the STANDALONE libmh.dll (MH_LIBMH_BUILD) runs
// with no game, so a trailer recorded by the game names the HOSTED libmh.dll -- a different product
// that shares the file name with the image doing the import. That entry cannot be "our" module in this
// process, so it is treated as a foreign module of a different build: its REBASEs degrade to PRESERVE,
// as for any foreign or unloaded module. Only the importing image itself is exempt; every other OURS
// entry (mh.dll, mh_harness.dll, ...) that is loaded with another identity still refuses.
//
// AND IN THAT ARM PRESERVE CARRIES THE BLOB'S VALUE. PRESERVE means "the receiver keeps its OWN
// process-local object here" -- a live peer has its own heap buffers, dialogs and function tables. The
// standalone arm starts from a poisoned arena and owns none of them; its import pipeline (spine.cpp's
// pathfinder/zeroing steps, libref_host's G_TEXT restamp) is built on the sender's verbatim values.
// Measured on the -v2 fixtures: applying PRESERVE there diverged replay-v2 at step 8 and faulted both
// all-AI replays; carrying the values replays all three 5000/5000. So in the standalone arm PRESERVE
// (and every degraded REBASE) leaves the imported dword alone; REGION and resolved REBASE still apply,
// and the trailer is still validated in full.
struct import_arm {
    bool     standalone = false; // this image is the standalone arm (no game in the process)
    uint32_t self_base  = 0;     // the importing image's own base; a resolved module at this base is "self"
};
// The real arm: standalone == MH_LIBMH_BUILD, self_base == the image this code is linked into.
import_arm real_import_arm();

// ---- the import-side plan ----------------------------------------------------------------------
struct fixup_plan {
    bool           present        = false;
    bool           standalone     = false; // import_arm::standalone: PRESERVE carries the blob's value
    uint32_t       count          = 0;
    uint32_t       hashed_skipped = 0, unmapped = 0, zeroed_skipped_blocks = 0;
    uint32_t       module_count = 0;
    char           module_name[FIXUP_MAX_MODULES][28];
    uint32_t       module_base[FIXUP_MAX_MODULES];
    uint8_t        module_ok[FIXUP_MAX_MODULES];           // 1 == resolved with a matching identity
    uint8_t        module_self_foreign[FIXUP_MAX_MODULES]; // 1 == OURS, but the standalone importer itself
    const uint8_t *entries = nullptr;                      // into the blob (may be unaligned); valid across the import
    uint32_t       saved[FIXUP_MAX_ENTRIES];               // the receiver's own dword, read BEFORE the byte engine
    uint8_t        eff[FIXUP_MAX_ENTRIES];                 // effective kind after degrading
    // per-holder tallies, indexed by block, for the log
    uint32_t blk_region[FIXUP_MAX_BLOCKS], blk_rebase[FIXUP_MAX_BLOCKS], blk_preserve[FIXUP_MAX_BLOCKS],
        blk_changed[FIXUP_MAX_BLOCKS];
};

struct fixup_stats {
    bool     present = false;
    uint32_t entries = 0, region = 0, rebase = 0, preserve = 0, degraded = 0, changed = 0;
    uint32_t carried        = 0;                                           // standalone arm: PRESERVE entries left at the blob's value
    uint32_t changed_region = 0, changed_rebase = 0, changed_preserve = 0; // `changed` split by class
    uint32_t hashed_skipped = 0, unmapped = 0, modules = 0;
};

// Capture: classify every carried dword of the blob's block payload and append the trailer at `out`
// (capacity `cap`). `blob` is the block payload as already written. Returns 0, or a negative code the
// caller maps to WORLD_ERR_FIXUP_CAPTURE.
int fixup_capture(const uint8_t *blob, uint8_t *out, size_t cap, size_t *out_len, const addr_map &map);

// Import, step 0a: validate EVERYTHING and save the live dwords. Writes nothing to the world. An
// absent trailer (or a blob the byte engine will refuse anyway) yields an empty plan and returns 0.
// Returns 0, or WORLD_ERR_FIXUP (-23) with the world untouched.
// Null `arm` == the hosted arm (the X3a semantics). The import path passes real_import_arm().
int fixup_prepare(const void *blob, size_t n, fixup_plan &plan, const module_resolver *res = nullptr,
                  const import_arm *arm = nullptr);

// Import, step 2: FIRST statement after world::import(). Writes REGION / REBASE / PRESERVE dwords.
void fixup_apply(fixup_plan &plan, fixup_stats *out = nullptr);

// One-line "modules=a,b,c" helper for the log.
size_t fixup_module_list(const fixup_plan &plan, char *buf, size_t cap);

} // namespace mh::state::world
