//
// desync/scene_dump.cpp -- see scene_dump.h and scene_rename.h. Compiled with scene_rename.h force-included,
// so `world` / `nav` below are mh.dll's PRIVATE copy of libmh's world-snapshot code, not the contract's.
//
#include "desync/scene_dump.h"

#include "caps/caps.h"   // PT-CAP8: canon_scatter after an import
#include "caps/orders.h" // PT-CAP9-U7c: finish_scatter after the MHOV apply
#include "caps/ovf.h"    // PT-CAP8 S2: the MHOV trailer after the fixup trailer

#include <windows.h>

#include <cstring>

#include "addr/mh_regions.gen.h"
#include "addr/mh_world_snapshot.gen.h"
#include "sim/libtrans/sim_lt_map_region_pool.h"
#include "state/nav_trailer.h"
#include "state/region_runtime.h"
#include "state/world_fixup.h"
#include "state/world_snapshot.h"

namespace {
mh::desync::scene::nav_views g_views = {};
// The allocator of the import in progress: nav::import(buf, n) below has no parameter to carry it.
void *(*g_alloc)(uint32_t) = nullptr;
void (*g_release)(void *)  = nullptr;
} // namespace

#ifdef MH_NAV_CAPTURE_ONLY
// The private copy of nav_trailer.cpp is built without the sim tree (MH_NAV_CAPTURE_ONLY), so it has no
// sim_store to read the region pool through and leaves nav::capture to its host: bound here over the four
// objects desync_watch.cpp handed in.
namespace mh::state::nav {
int capture(void *buf, size_t cap, size_t *out_len) {
    if (!g_views.list_head) return NAV_ERR_ARG;
    capture_views v;
    v.list_head = g_views.list_head;
    v.free_head = g_views.free_head;
    v.by_index  = g_views.by_index;
    v.grid      = g_views.grid;
    return capture_with(buf, cap, out_len, v);
}

// The scene replay's pool rebuild: the SAME body libmh runs (nav_trailer.cpp import_walk), over the four live
// objects, through the GAME'S allocator (the original llm_map_region_pool_reset frees these nodes with
// utils_free, so a CRT malloc here would be a cross-allocator free).
int import(const void *buf, size_t n) {
    if (!g_views.list_head || !g_alloc || !g_release) return NAV_ERR_ARG;
    const mh::sim::lt_map_region_pool_calls c = {g_alloc, g_release};
    capture_views                           v;
    v.list_head = g_views.list_head;
    v.free_head = g_views.free_head;
    v.by_index  = g_views.by_index;
    v.grid      = g_views.grid;
    return import_with(buf, n, c, v);
}
} // namespace mh::state::nav
#endif // MH_NAV_CAPTURE_ONLY (the hosted selftest build links libmh's own nav::capture instead)

namespace mh::desync::scene {

void set_nav_views(const nav_views &v) { g_views = v; }

uint8_t *capture(double game_clock, uint32_t step, size_t *len, int *rc) {
    namespace w = mh::state::world;
    // PT-CAP8 S2: with the raise ON the overflow records ride behind the fixup trailer (OFF: not a byte more).
    const size_t need = w::capture_capacity() + (mh::caps::raised() ? mh::caps::ovf::max_bytes_world() : 0);
    uint8_t     *blob = static_cast<uint8_t *>(HeapAlloc(GetProcessHeap(), 0, need));
    *len              = 0;
    *rc               = -1;
    if (!blob) return nullptr;
    w::capture_params p;
    p.step = step;
    std::memcpy(&p.game_clock, &game_clock, sizeof(game_clock));
    w::lockstep_hash(p.mask_flags, &p.lockstep_combined, &p.lockstep_state);
    size_t got = 0;
    *rc        = w::capture(blob, need, &got, p);
    if (*rc == w::WORLD_OK && !mh::caps::ovf::append_to_world_blob(blob, &got, need))
        *rc = mh::caps::ovf::world_rc_refused(mh::caps::ovf::OVF_ERR_TOO_SMALL);
    if (*rc != w::WORLD_OK) {
        HeapFree(GetProcessHeap(), 0, blob);
        return nullptr;
    }
    *len = got;
    return blob;
}

void release(uint8_t *blob) {
    if (blob) HeapFree(GetProcessHeap(), 0, blob);
}

// ---- the replay half --------------------------------------------------------------------------------------
//
// libmh/state/spine.cpp import_world_impl, step for step, minus what a configuration-(1) process does not
// need (see scene_dump.h). The helpers are copies of that file's anonymous-namespace ones; the libmh side is
// left untouched so configuration (2) stays byte-for-byte what it was.

namespace {

namespace st = mh::state;

constexpr st::region_id LIVE_PRESERVE_REGIONS[] = {
    st::RID_FILE_PTR,
    st::RID_GFX_DRAW_SURFACE,
    st::RID_TILE_VIS_MAP_PTR,
    st::RID_TACT_LOS_CACHE_PTR,
    st::RID_TACT_FOV_DIR_TABLE_PTR,
    st::RID_MENU_SAVE_NAME_PTR,
    st::RID_PTR_S_MENUBCK1_GFX_00604288,
    st::RID_HWND_00824FE8,
};
constexpr int LIVE_PRESERVE_N        = (int)(sizeof(LIVE_PRESERVE_REGIONS) / sizeof(LIVE_PRESERVE_REGIONS[0]));
constexpr int LIVE_PRESERVE_SCAN_CAP = 512;

inline bool looks_like_process_handle(uint32_t v) {
    return v >= 0x01100000u && v < 0x7ff00000u && (v & 3u) == 0u;
}
struct handle_slot {
    uint16_t block;
    uint32_t live;
};
struct handle_set {
    handle_slot slot[LIVE_PRESERVE_SCAN_CAP];
    int         n;
    bool        overflowed;
};
bool preserve_candidate(int i) {
    const st::region_id r = st::WORLD_SNAPSHOT_BLOCKS[i].rid;
    if (st::WORLD_SNAPSHOT_BLOCKS[i].len != 4u) return false;
    const uint8_t f = st::REGIONS[r].manifests;
    if ((f & st::MF_VIEW) == 0) return false;
    if ((f & (st::MF_SAVE | st::MF_HASH)) != 0) return false;
    return true;
}
bool on_explicit_list(st::region_id r) {
    for (int k = 0; k < LIVE_PRESERVE_N; ++k)
        if (LIVE_PRESERVE_REGIONS[k] == r) return true;
    return false;
}
void live_handles_save(handle_set &out) {
    out.n          = 0;
    out.overflowed = false;
    for (int i = 0; i < st::WORLD_SNAPSHOT_BLOCK_COUNT; ++i) {
        const st::region_id r = st::WORLD_SNAPSHOT_BLOCKS[i].rid;
        if (!preserve_candidate(i) && !on_explicit_list(r)) continue;
        uint32_t v = 0;
        if (!st::read_region_u32(r, &v)) continue;
        if (out.n >= LIVE_PRESERVE_SCAN_CAP) {
            out.overflowed = true;
            break;
        }
        out.slot[out.n].block = (uint16_t)i;
        out.slot[out.n].live  = v;
        ++out.n;
    }
}
void live_handles_restore(const handle_set &saved) {
    for (int k = 0; k < saved.n; ++k) {
        const st::region_id r        = st::WORLD_SNAPSHOT_BLOCKS[saved.slot[k].block].rid;
        uint32_t            imported = 0;
        if (st::world::is_zeroed_on_import(r)) continue;
        if (!st::read_region_u32(r, &imported)) continue;
        const uint32_t live = saved.slot[k].live;
        if (imported == live) continue;
        const bool forced = on_explicit_list(r);
        if (!forced && !(looks_like_process_handle(live) && looks_like_process_handle(imported))) continue;
        st::write_region_u32(r, live);
    }
}

} // namespace

int import_world(const void *blob, size_t n, const import_ops &ops) {
    namespace w = mh::state::world;
    if (!g_views.list_head || !ops.alloc || !ops.release || !ops.pathfinder_init || !ops.setup_dimensions ||
        !ops.pool_reset || !ops.route_step_deltas)
        return w::WORLD_ERR_ARG;
    g_alloc   = ops.alloc;
    g_release = ops.release;
    auto say  = [&](const char *what, int a, int b) {
        if (ops.log) ops.log("; [scene_in] %s (%d, %d)\n", what, a, b);
    };

    // (0) PT-CAP8 S2: an overflow trailer (behind the fixup trailer) is validated BEFORE anything is written: a blob
    // that carries overflow cannot be imported with the raise OFF (the wide arrays' tails would not be addressed), and
    // a damaged trailer refuses with the world untouched.
    const uint8_t *ovf_tr  = nullptr;
    size_t         ovf_len = 0;
    if (n >= sizeof(w::blob_header)) {
        const auto *bh = static_cast<const w::blob_header *>(blob);
        ovf_tr         = mh::caps::ovf::world_trailer(blob, n, bh->nav_offset, bh->nav_len, bh->fixup_len, &ovf_len);
    }
    if (ovf_tr) {
        if (!mh::caps::raised()) {
            if (ops.log) ops.log("; [scene_in] overflow trailer REFUSED: raise OFF (%u bytes)\n", (unsigned)ovf_len);
            return mh::caps::ovf::WORLD_ERR_CAPS_OVF;
        }
        const int orc = mh::caps::ovf::check(ovf_tr, ovf_len);
        if (orc != mh::caps::ovf::OVF_OK) {
            if (ops.log) ops.log("; [scene_in] overflow trailer REFUSED rc=%d\n", orc);
            return mh::caps::ovf::world_rc_refused(orc);
        }
    }

    // (0a) validate the fixup trailer completely and stash the receiver's own dwords; an OURS module of a
    // different build refuses here (-23) with the world untouched.
    static w::fixup_plan plan; // ~330 KB: static, never a stack frame
    const w::import_arm  arm = w::real_import_arm();
    const int            frc = w::fixup_prepare(blob, n, plan, nullptr, &arm);
    if (frc != 0) {
        say("fixup trailer REFUSED rc", frc, 0);
        return frc;
    }
    static handle_set live;
    live_handles_save(live);

    // The byte engine refuses a carried block whose region the registry says is SMALLER than the block
    // (ERR_UNBOUND: "the host never bound it"). In configuration (1) nothing bound anything: live().size is
    // the canonical extent of the symbol (Tree = 44), while the block spans what the manifests touch (Tree
    // = 4400 = REGIONS[].reach), and those bytes ARE at the stock address -- the generated coverage asserts
    // say so. libmh's own bind widens the size to the host's capacity; here the same fact is declared for
    // the duration of the import and put back.
    static uint32_t widened_rid[mh::state::WORLD_SNAPSHOT_BLOCK_COUNT];
    static uint32_t widened_size[mh::state::WORLD_SNAPSHOT_BLOCK_COUNT];
    int             widened = 0;
    for (int i = 0; i < st::WORLD_SNAPSHOT_BLOCK_COUNT; ++i) {
        const auto &b = st::WORLD_SNAPSHOT_BLOCKS[i];
        if (st::is_rebased(b.rid) || st::live_size(b.rid) >= b.len) continue;
        widened_rid[widened]    = (uint32_t)b.rid;
        widened_size[widened++] = st::live_size(b.rid);
        st::live().size[b.rid]  = b.len;
    }
    const int rc = w::import(blob, n); // the byte engine (this copy's session latch is never set)
    for (int k = 0; k < widened; ++k) st::live().size[widened_rid[k]] = widened_size[k];
    say("regions widened for the import", widened, 0);
    if (rc != w::WORLD_OK) {
        say("byte import refused rc", rc, 0);
        if (ops.log) { // name what the registry could not host: the first few carried regions without a base / room
            int shown = 0;
            for (int i = 0; i < st::WORLD_SNAPSHOT_BLOCK_COUNT && shown < 8; ++i) {
                const auto &b = st::WORLD_SNAPSHOT_BLOCKS[i];
                if (st::live_base(b.rid) != 0 && st::live_size(b.rid) >= b.len) continue;
                ops.log("; [scene_in]   region %s: live base %08X size %u, the dump carries %u\n",
                        st::REGIONS[b.rid].name, (unsigned)st::live_base(b.rid), (unsigned)st::live_size(b.rid),
                        (unsigned)b.len);
                ++shown;
            }
        }
        return rc;
    }
    // (2) FIRST statement after the byte engine: put every process-local dword where THIS process needs it.
    w::fixup_stats fs;
    w::fixup_apply(plan, &fs);
    live_handles_restore(live);
    say("fixup entries/changed", (int)fs.entries, (int)fs.changed);

    // (1) neutralise the imported nav graph / path-result pointers (they are the RECORDING process's heap).
    for (const st::region_id r : w::ZEROED_ON_IMPORT) st::clear_region(r);
    {
        struct cell {
            void    *region;
            uint32_t flags;
        };
        cell *grid = static_cast<cell *>(g_views.grid);
        for (int i = 0; i < 256 * 256; ++i) grid[i].region = nullptr;
    }
    // (2') the heap blocks `general` points at, then the torus masks (the original's order).
    ops.pathfinder_init();
    ops.setup_dimensions();
    // (4) pool_reset over the two empty lists; its tail clobbers two CARRIED scalars, so bracket them.
    uint32_t last_index = 0, alloc_ctr = 0;
    st::read_region_u32(st::RID_G_LAST_MAP_INDEX, &last_index);
    st::read_region_u32(st::RID_COUNTER, &alloc_ctr);
    ops.pool_reset();
    st::write_region_u32(st::RID_G_LAST_MAP_INDEX, last_index);
    st::write_region_u32(st::RID_COUNTER, alloc_ctr);
    // (5) the route step-delta table, then (6) the nav decomposition from the trailer.
    ops.route_step_deltas();
    const int nrc = w::import_nav(blob, n);
    if (nrc != w::WORLD_OK) say("nav import refused rc", nrc, 0);
    // PT-CAP8 (D1): the byte engine wrote the STOCK extents of the seven raised arrays -- the canonical mirror. Publish it to the
    // wide arrays the game addresses (and zero their tails). A no-op unless the cap relocation is applied.
    mh::caps::canon_scatter();
    if (ovf_tr) { // PT-CAP8 S2: the tails were zeroed by the scatter; the carried overflow records go in on top
        const int orc = mh::caps::ovf::apply(ovf_tr, ovf_len);
        say("overflow records applied rc", orc, mh::caps::ovf::record_count(ovf_tr, ovf_len));
        mh::caps::orders::finish_scatter(); // PT-CAP9-U7c: the order counts the stock import carried come back with their tail records, or stay clamped (logged)
        if (orc != mh::caps::ovf::OVF_OK) return mh::caps::ovf::world_rc_refused(orc);
    } else {
        mh::caps::orders::finish_scatter();
    }
    return nrc;
}

} // namespace mh::desync::scene
