//
// state/world_fixup.cpp -- see world_fixup.h for why this exists.
//
// Three halves: the CAPTURE-side classifier (fixup_capture + the address map), the IMPORT-side
// validator (fixup_prepare, which writes nothing) and the IMPORT-side applier (fixup_apply).
//
#include "state/world_fixup.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#endif

#include "addr/mh_world_snapshot.gen.h"
#include "state/region_runtime.h"
#include "state/world_snapshot.h"

namespace mh::state::world {

using mh::state::blob::snapshot_block;

static_assert(FIXUP_MAX_BLOCKS >= static_cast<uint32_t>(WORLD_SNAPSHOT_BLOCK_COUNT),
              "the per-holder tallies are indexed by block");

// ---- the address map --------------------------------------------------------------------------

int addr_map::add_module(const char *lower_name, uint32_t alloc_base, uint32_t stamp,
                         uint32_t size_of_image, bool ours) {
    if (nmod >= ADDR_MAP_MAX_MODULES) {
        overflow = true;
        return 0xFFFF;
    }
    addr_module &am = mod[nmod];
    std::memset(&am, 0, sizeof(am));
    std::strncpy(am.m.name, lower_name, sizeof(am.m.name) - 1);
    am.m.time_date_stamp = stamp;
    am.m.size_of_image   = size_of_image;
    am.m.flags           = ours ? FIXUP_MOD_OURS : 0u;
    am.alloc_base        = alloc_base;
    return nmod++;
}

void addr_map::add_range(uint32_t base, uint32_t size, uint8_t kind, int module, bool is_exe) {
    if (size == 0) return;
    if (nrange >= ADDR_MAP_MAX_RANGES) {
        overflow = true;
        return;
    }
    addr_range &r = range[nrange++];
    r.base        = base;
    r.size        = size;
    r.kind        = kind;
    r.is_exe      = is_exe ? 1 : 0;
    r.module      = static_cast<uint16_t>(module);
}

static int cmp_range(const void *a, const void *b) {
    const uint32_t x = static_cast<const addr_range *>(a)->base;
    const uint32_t y = static_cast<const addr_range *>(b)->base;
    return x < y ? -1 : (x > y ? 1 : 0);
}

void addr_map::finish() {
    if (nrange > 1) std::qsort(range, static_cast<size_t>(nrange), sizeof(addr_range), cmp_range);
    int w = 0;
    for (int i = 0; i < nrange; ++i) {
        if (w > 0) {
            addr_range &p = range[w - 1];
            if (p.kind == range[i].kind && p.module == range[i].module && p.is_exe == range[i].is_exe &&
                uint64_t(p.base) + p.size == range[i].base) {
                p.size += range[i].size;
                continue;
            }
        }
        range[w++] = range[i];
    }
    nrange = w;
}

const addr_range *addr_map::find(uint32_t v) const {
    int lo = 0, hi = nrange - 1;
    while (lo <= hi) {
        const int         mid = (lo + hi) / 2;
        const addr_range &r   = range[mid];
        if (v < r.base) hi = mid - 1;
        else if (uint64_t(v) >= uint64_t(r.base) + r.size) lo = mid + 1;
        else return &r;
    }
    return nullptr;
}

const addr_map *empty_addr_map() {
    static addr_map *m = nullptr;
    if (m == nullptr) {
        m = new addr_map;
        m->clear();
    }
    return m;
}

#ifdef _WIN32
namespace {

struct pe_identity {
    uint32_t stamp = 0, size_of_image = 0;
    bool     ok = false;
};

// The in-memory PE identity of the image mapped at `base`. Committed image pages are readable, but
// the header is still validated before every dereference.
pe_identity read_pe(uint32_t base) {
    pe_identity    id;
    const uint8_t *p = reinterpret_cast<const uint8_t *>(static_cast<uintptr_t>(base));
    __try {
        const IMAGE_DOS_HEADER *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(p);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x1000) return id;
        const IMAGE_NT_HEADERS32 *nt = reinterpret_cast<const IMAGE_NT_HEADERS32 *>(p + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return id;
        id.stamp         = nt->FileHeader.TimeDateStamp;
        id.size_of_image = nt->OptionalHeader.SizeOfImage;
        id.ok            = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        id.ok = false;
    }
    return id;
}

void lower_basename(const char *path, char *out, size_t cap) {
    const char *b = path;
    for (const char *q = path; *q; ++q)
        if (*q == '\\' || *q == '/') b = q + 1;
    size_t i = 0;
    for (; b[i] && i + 1 < cap; ++i) {
        char c = b[i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        out[i] = c;
    }
    out[i] = 0;
}

// The directory part of `path` compared case-insensitively.
bool same_dir(const char *a, const char *b) {
    auto dirlen = [](const char *s) {
        size_t n = std::strlen(s);
        while (n > 0 && s[n - 1] != '\\' && s[n - 1] != '/') --n;
        return n;
    };
    const size_t na = dirlen(a), nb = dirlen(b);
    return na == nb && _strnicmp(a, b, na) == 0;
}

} // namespace

bool build_real_addr_map(addr_map &m) {
    m.clear();
    char exe_path[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
    const uint32_t exe_base = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr)));

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uint64_t limit = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
    if (limit > 0xFFFFFFFFull) limit = 0xFFFFFFFFull;

    uint32_t last_ab  = 0;
    int      last_mod = 0xFFFF;
    uint64_t addr     = 0x10000;
    while (addr < limit) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(addr)), &mbi, sizeof(mbi)) == 0)
            break;
        const uint64_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        uint64_t       size = mbi.RegionSize;
        if (base + size > 0x100000000ull) size = 0x100000000ull - base;
        if (mbi.State == MEM_COMMIT) {
            if (mbi.Type == MEM_IMAGE) {
                const uint32_t ab = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(mbi.AllocationBase));
                if (ab != last_ab) {
                    last_ab  = ab;
                    last_mod = 0xFFFF;
                    if (ab != exe_base) {
                        char path[MAX_PATH] = {0};
                        if (GetModuleFileNameA(reinterpret_cast<HMODULE>(static_cast<uintptr_t>(ab)), path,
                                               MAX_PATH) != 0) {
                            char name[28];
                            lower_basename(path, name, sizeof(name));
                            const pe_identity id = read_pe(ab);
                            if (id.ok)
                                last_mod = m.add_module(name, ab, id.stamp, id.size_of_image,
                                                        same_dir(path, exe_path));
                        }
                    }
                }
                m.add_range(static_cast<uint32_t>(base), static_cast<uint32_t>(size), AM_IMAGE, last_mod,
                            ab == exe_base);
            } else {
                m.add_range(static_cast<uint32_t>(base), static_cast<uint32_t>(size), AM_PRIVATE);
            }
        }
        const uint64_t next = base + size;
        if (next <= addr) break;
        addr = next;
    }
    m.finish();
    return !m.overflow;
}

import_arm real_import_arm() {
    import_arm a;
#ifdef MH_LIBMH_BUILD
    a.standalone = true;
#endif
    HMODULE self = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&real_import_arm), &self))
        a.self_base = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self));
    return a;
}

static bool real_resolve(void *, const char *lower_name, module_identity *out) {
    const HMODULE h = GetModuleHandleA(lower_name);
    if (h == nullptr) return false;
    const uint32_t    base = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(h));
    const pe_identity id   = read_pe(base);
    if (!id.ok) return false;
    out->base            = base;
    out->time_date_stamp = id.stamp;
    out->size_of_image   = id.size_of_image;
    return true;
}
#else
bool build_real_addr_map(addr_map &m) {
    m.clear();
    return false;
}
import_arm real_import_arm() {
    import_arm a;
#ifdef MH_LIBMH_BUILD
    a.standalone = true;
#endif
    return a;
}
static bool real_resolve(void *, const char *, module_identity *) {
    return false;
}
#endif

// ---- helpers ----------------------------------------------------------------------------------

namespace {

bool view_owned(region_id r) {
    const uint8_t f = mh::state::REGIONS[r].manifests;
    return (f & mh::state::MF_VIEW) != 0 && (f & (mh::state::MF_SAVE | mh::state::MF_HASH)) == 0;
}

struct span {
    uint32_t  base, len;
    region_id rid;
};

bool cmp_span_less(const span &a, const span &b) {
    return a.base < b.base;
}

// The carried blocks' LIVE spans, sorted by base. Blocks may nest or alias (a hashed alias of a unit
// table, say), so a lookup scans back over a bounded window rather than trusting the last base.
struct span_index {
    span *s = nullptr;
    int   n = 0;

    ~span_index() { std::free(s); }
    bool build() {
        s = static_cast<span *>(std::malloc(sizeof(span) * WORLD_SNAPSHOT_BLOCK_COUNT));
        if (s == nullptr) return false;
        for (int i = 0; i < WORLD_SNAPSHOT_BLOCK_COUNT; ++i) {
            const snapshot_block &b  = WORLD_SNAPSHOT_BLOCKS[i];
            const uint32_t        lb = live_base(b.rid);
            if (lb == 0 || b.len == 0) continue;
            s[n++] = {lb, b.len, b.rid};
        }
        for (int i = 1; i < n; ++i) { // insertion sort: ~830 entries, once per capture
            const span v = s[i];
            int        j = i - 1;
            while (j >= 0 && cmp_span_less(v, s[j])) {
                s[j + 1] = s[j];
                --j;
            }
            s[j + 1] = v;
        }
        return true;
    }
    const span *find(uint32_t v) const {
        int lo = 0, hi = n - 1, idx = -1;
        while (lo <= hi) {
            const int mid = (lo + hi) / 2;
            if (s[mid].base <= v) {
                idx = mid;
                lo  = mid + 1;
            } else hi = mid - 1;
        }
        for (int k = idx, seen = 0; k >= 0 && seen < 16; --k, ++seen)
            if (uint64_t(v) < uint64_t(s[k].base) + s[k].len) return &s[k];
        return nullptr;
    }
};

uint32_t rd32(const uint8_t *p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

} // namespace

// ---- capture ----------------------------------------------------------------------------------

int fixup_capture(const uint8_t *blob, uint8_t *out, size_t cap, size_t *out_len, const addr_map &map) {
    if (map.overflow) return -1;
    span_index spans;
    if (!spans.build()) return -1;
    fixup_entry *ent = static_cast<fixup_entry *>(std::malloc(sizeof(fixup_entry) * FIXUP_MAX_ENTRIES));
    if (ent == nullptr) return -1;

    int used_mod[ADDR_MAP_MAX_MODULES];
    int nused = 0;
    for (int i = 0; i < ADDR_MAP_MAX_MODULES; ++i) used_mod[i] = -1;
    fixup_module mods[FIXUP_MAX_MODULES];

    uint32_t n_ent = 0, hashed_skipped = 0, unmapped = 0, zeroed_blocks = 0;
    int      rc = 0;

    size_t off = sizeof(blob_header);
    for (int bi = 0; bi < WORLD_SNAPSHOT_BLOCK_COUNT && rc == 0; ++bi) {
        const snapshot_block &b       = WORLD_SNAPSHOT_BLOCKS[bi];
        const uint8_t        *payload = blob + off + 8u;
        off += 8u + b.len;
        if (is_zeroed_on_import(b.rid)) {
            ++zeroed_blocks;
            continue;
        }
        const bool holder_view = view_owned(b.rid);
        // this block's hashed windows, BY ADDRESS. A block is a save-table RUN (`reach` bytes from the
        // region's base), not the region: GAME_SESSION_MODE is a 4-byte region whose block is 1623 bytes
        // and covers the whole `Players` table. Matching hash slices by rid alone missed every hashed
        // neighbour inside the run, so a player name that read as a module address was REBASEd and the
        // importer's `players` slice no longer matched the host's. Every slice overlapping the run counts.
        uint32_t hs[512], hl[512];
        int      nh = 0;
        {
            const int64_t blk_lo = static_cast<int64_t>(live_base(b.rid));
            const int64_t blk_hi = blk_lo + static_cast<int64_t>(b.len);
            for (int h = 0; h < HASH_REGION_COUNT && rc == 0; ++h) {
                const int64_t lo = static_cast<int64_t>(mh::state::hash_base(h));
                const int64_t hi = lo + static_cast<int64_t>(HASH_REGIONS[h].len);
                if (hi <= blk_lo || lo >= blk_hi) continue;
                if (nh >= 512) { // an unmasked hashed window would be unsafe: refuse, never truncate
                    rc = -1;
                    break;
                }
                const int64_t a = lo > blk_lo ? lo : blk_lo;
                const int64_t z = hi < blk_hi ? hi : blk_hi;
                hs[nh]          = static_cast<uint32_t>(a - blk_lo);
                hl[nh]          = static_cast<uint32_t>(z - a);
                ++nh;
            }
        }
        if (rc != 0) break;
        const bool os_handle = is_os_handle_region(b.rid);
        for (uint32_t o = 0; o + 4u <= b.len; o += 4u) {
            const uint32_t v = rd32(payload + o);
            if (v < 0x10000u && !os_handle) continue;
            if (os_handle) { // an OS handle is not an address: forced PRESERVE, whatever it holds
                if (n_ent >= FIXUP_MAX_ENTRIES) {
                    rc = -1;
                    break;
                }
                fixup_entry &e = ent[n_ent++];
                e.block        = static_cast<uint16_t>(bi);
                e.kind         = FIXUP_PRESERVE;
                e.module       = 0;
                e.offset       = o;
                e.arg = e.arg2 = 0;
                continue;
            }
            uint8_t  kind = 0, module = 0;
            uint32_t arg = 0, arg2 = 0;
            int      am_idx = -1;
            if (const span *sp = spans.find(v)) {
                if (!is_rebased(sp->rid)) continue; // stock: the same value on every host
                kind = FIXUP_REGION;
                arg  = static_cast<uint32_t>(sp->rid);
                arg2 = v - sp->base;
            } else if (const addr_range *r = map.find(v)) {
                if (r->kind == AM_IMAGE) {
                    if (r->is_exe) continue; // mh.exe is not ASLR'd
                    if (r->module == 0xFFFF) kind = FIXUP_PRESERVE;
                    else {
                        kind   = FIXUP_REBASE;
                        am_idx = r->module;
                        arg    = v - map.mod[r->module].alloc_base;
                    }
                } else kind = FIXUP_PRESERVE;
            } else {
                ++unmapped;
                continue;
            }
            if (holder_view && kind != FIXUP_PRESERVE) {
                kind   = FIXUP_PRESERVE;
                am_idx = -1;
                arg = arg2 = 0;
            }
            bool hashed = false;
            for (int k = 0; k < nh; ++k)
                if (o < hs[k] + hl[k] && o + 4u > hs[k]) hashed = true;
            if (hashed) {
                ++hashed_skipped;
                continue;
            }
            if (kind == FIXUP_REBASE) {
                if (used_mod[am_idx] < 0) {
                    if (nused >= static_cast<int>(FIXUP_MAX_MODULES)) {
                        rc = -1;
                        break;
                    }
                    mods[nused]      = map.mod[am_idx].m;
                    used_mod[am_idx] = nused++;
                }
                module = static_cast<uint8_t>(used_mod[am_idx]);
            }
            if (n_ent >= FIXUP_MAX_ENTRIES) {
                rc = -1;
                break;
            }
            fixup_entry &e = ent[n_ent++];
            e.block        = static_cast<uint16_t>(bi);
            e.kind         = kind;
            e.module       = module;
            e.offset       = o;
            e.arg          = arg;
            e.arg2         = arg2;
        }
    }
    if (rc == 0) {
        const size_t total = sizeof(fixup_trailer_header) + nused * sizeof(fixup_module) +
                             static_cast<size_t>(n_ent) * sizeof(fixup_entry);
        if (total > cap) rc = -1;
        else {
            fixup_trailer_header h;
            std::memset(&h, 0, sizeof(h));
            h.magic                 = FIXUP_MAGIC;
            h.version               = FIXUP_VERSION;
            h.module_count          = static_cast<uint16_t>(nused);
            h.entry_count           = n_ent;
            h.checksum              = fixup_checksum(mods, static_cast<size_t>(nused), ent, n_ent);
            h.hashed_skipped        = hashed_skipped;
            h.unmapped              = unmapped;
            h.zeroed_skipped_blocks = zeroed_blocks;
            std::memcpy(out, &h, sizeof(h));
            std::memcpy(out + sizeof(h), mods, nused * sizeof(fixup_module));
            std::memcpy(out + sizeof(h) + nused * sizeof(fixup_module), ent,
                        static_cast<size_t>(n_ent) * sizeof(fixup_entry));
            *out_len = total;
        }
    }
    std::free(ent);
    return rc;
}

// ---- import: validate + stash -----------------------------------------------------------------

int fixup_prepare(const void *blob, size_t n, fixup_plan &plan, const module_resolver *res,
                  const import_arm *arm) {
    plan.present        = false;
    plan.count          = 0;
    plan.module_count   = 0;
    plan.hashed_skipped = plan.unmapped = plan.zeroed_skipped_blocks = 0;
    plan.entries                                                     = nullptr;
    std::memset(plan.blk_region, 0, sizeof(plan.blk_region));
    std::memset(plan.blk_rebase, 0, sizeof(plan.blk_rebase));
    std::memset(plan.blk_preserve, 0, sizeof(plan.blk_preserve));
    std::memset(plan.blk_changed, 0, sizeof(plan.blk_changed));

    // A blob the byte engine will refuse anyway is not this function's to diagnose: return an empty
    // plan and let import() report its own code.
    if (blob == nullptr || n < sizeof(blob_header)) return 0;
    blob_header h;
    std::memcpy(&h, blob, sizeof(h));
    if (std::memcmp(h.base.magic, MAGIC, sizeof(h.base.magic)) != 0 || h.base.format != FORMAT) return 0;
    if (h.fixup_len == 0) return 0;

    const uint8_t *d = static_cast<const uint8_t *>(blob);
    if (h.nav_offset == 0 || uint64_t(h.nav_offset) + h.nav_len + h.fixup_len > n) return WORLD_ERR_FIXUP;
    if (h.fixup_len < sizeof(fixup_trailer_header)) return WORLD_ERR_FIXUP;
    const uint8_t *t = d + h.nav_offset + h.nav_len;

    fixup_trailer_header th;
    std::memcpy(&th, t, sizeof(th));
    if (th.magic != FIXUP_MAGIC || th.version != FIXUP_VERSION) return WORLD_ERR_FIXUP;
    if (th.module_count > FIXUP_MAX_MODULES || th.entry_count > FIXUP_MAX_ENTRIES) return WORLD_ERR_FIXUP;
    const size_t want = sizeof(th) + th.module_count * sizeof(fixup_module) + th.entry_count * sizeof(fixup_entry);
    if (want != h.fixup_len) return WORLD_ERR_FIXUP;
    const uint8_t *mp = t + sizeof(th);
    const uint8_t *ep = mp + th.module_count * sizeof(fixup_module);
    if (fixup_checksum(mp, th.module_count, ep, th.entry_count) != th.checksum) return WORLD_ERR_FIXUP;

    // ---- modules: resolve and identity-check. Nothing here writes to the world.
    module_resolver        def = {&real_resolve, nullptr};
    const module_resolver *rs  = (res != nullptr && res->fn != nullptr) ? res : &def;
    const import_arm       ia  = arm != nullptr ? *arm : import_arm{};
    plan.standalone            = ia.standalone;
    for (uint32_t k = 0; k < th.module_count; ++k) {
        fixup_module fm;
        std::memcpy(&fm, mp + k * sizeof(fm), sizeof(fm));
        fm.name[sizeof(fm.name) - 1] = 0;
        std::memcpy(plan.module_name[k], fm.name, sizeof(fm.name));
        plan.module_base[k]         = 0;
        plan.module_ok[k]           = 0;
        plan.module_self_foreign[k] = 0;
        module_identity id;
        if (rs->fn(rs->ctx, fm.name, &id)) {
            if (id.time_date_stamp == fm.time_date_stamp && id.size_of_image == fm.size_of_image) {
                plan.module_base[k] = id.base;
                plan.module_ok[k]   = 1;
            } else if ((fm.flags & FIXUP_MOD_OURS) != 0) {
                // The standalone importer's own image under the sender's libmh name: a different
                // product, not a mixed build -- foreign (see import_arm).
                if (ia.standalone && ia.self_base != 0 && id.base == ia.self_base)
                    plan.module_self_foreign[k] = 1;
                else
                    return WORLD_ERR_FIXUP; // a mixed-build pair: loud, and the world is untouched
            }
        }
        // not loaded, or a foreign module of a different build: degrade to PRESERVE (module_ok == 0)
    }
    plan.module_count = th.module_count;

    // ---- entries: validate ALL of them before anything is saved.
    for (uint32_t i = 0; i < th.entry_count; ++i) {
        fixup_entry e;
        std::memcpy(&e, ep + i * sizeof(e), sizeof(e));
        if (e.block >= WORLD_SNAPSHOT_BLOCK_COUNT) return WORLD_ERR_FIXUP;
        const snapshot_block &b = WORLD_SNAPSHOT_BLOCKS[e.block];
        if (uint64_t(e.offset) + 4u > b.len) return WORLD_ERR_FIXUP;
        if (e.kind < FIXUP_REGION || e.kind > FIXUP_PRESERVE) return WORLD_ERR_FIXUP;
        if (e.kind == FIXUP_REGION) {
            if (e.arg >= static_cast<uint32_t>(mh::state::RID_COUNT)) return WORLD_ERR_FIXUP;
            const region_id pr      = static_cast<region_id>(e.arg);
            bool            carried = false;
            for (int q = 0; q < WORLD_SNAPSHOT_BLOCK_COUNT; ++q)
                if (WORLD_SNAPSHOT_BLOCKS[q].rid == pr) {
                    carried = true;
                    break;
                }
            if (!carried || live_base(pr) == 0) return WORLD_ERR_FIXUP;
            if (uint64_t(e.arg2) + 4u > live_size(pr)) return WORLD_ERR_FIXUP;
        } else if (e.kind == FIXUP_REBASE) {
            if (e.module >= th.module_count) return WORLD_ERR_FIXUP;
        }
    }

    // ---- validated. Save the receiver's own dword for every PRESERVE / degraded entry.
    plan.entries = ep;
    plan.count   = th.entry_count;
    for (uint32_t i = 0; i < th.entry_count; ++i) {
        fixup_entry e;
        std::memcpy(&e, ep + i * sizeof(e), sizeof(e));
        uint8_t eff = e.kind;
        if (e.kind == FIXUP_REBASE && !plan.module_ok[e.module]) eff = 4; // degraded to PRESERVE
        plan.eff[i]   = eff;
        plan.saved[i] = 0;
        if (eff == FIXUP_PRESERVE || eff == 4)
            mh::state::read_region_u32_at(WORLD_SNAPSHOT_BLOCKS[e.block].rid, e.offset, &plan.saved[i]);
    }
    plan.hashed_skipped        = th.hashed_skipped;
    plan.unmapped              = th.unmapped;
    plan.zeroed_skipped_blocks = th.zeroed_skipped_blocks;
    plan.present               = true;
    return 0;
}

// ---- import: apply ----------------------------------------------------------------------------

void fixup_apply(fixup_plan &plan, fixup_stats *out) {
    fixup_stats st;
    st.present        = plan.present;
    st.hashed_skipped = plan.hashed_skipped;
    st.unmapped       = plan.unmapped;
    st.modules        = plan.module_count;
    for (uint32_t i = 0; plan.present && i < plan.count; ++i) {
        fixup_entry e;
        std::memcpy(&e, plan.entries + i * sizeof(e), sizeof(e));
        const region_id rid = WORLD_SNAPSHOT_BLOCKS[e.block].rid;
        uint32_t        v   = 0;
        const uint8_t   eff = plan.eff[i];
        if (eff == FIXUP_REGION) {
            v = live_base(static_cast<region_id>(e.arg)) + e.arg2;
            ++plan.blk_region[e.block];
            ++st.region;
        } else if (eff == FIXUP_REBASE) {
            v = plan.module_base[e.module] + e.arg;
            ++plan.blk_rebase[e.block];
            ++st.rebase;
        } else {
            v = plan.saved[i];
            ++plan.blk_preserve[e.block];
            ++st.preserve;
            if (eff == 4) ++st.degraded;
            if (plan.standalone) { // no receiver-owned object exists here: keep the blob's value
                ++st.carried;
                ++st.entries;
                continue;
            }
        }
        ++st.entries;
        uint32_t cur = 0;
        if (!mh::state::read_region_u32_at(rid, e.offset, &cur)) continue;
        if (cur != v) {
            ++st.changed;
            if (eff == FIXUP_REGION) ++st.changed_region;
            else if (eff == FIXUP_REBASE) ++st.changed_rebase;
            else ++st.changed_preserve;
            ++plan.blk_changed[e.block];
            mh::state::write_region_u32_at(rid, e.offset, v);
        }
    }
    if (out != nullptr) *out = st;
}

size_t fixup_module_list(const fixup_plan &plan, char *buf, size_t cap) {
    size_t w = 0;
    if (cap == 0) return 0;
    buf[0] = 0;
    for (uint32_t k = 0; k < plan.module_count; ++k) {
        const int r = std::snprintf(buf + w, cap - w, "%s%s%s", k ? "," : "", plan.module_name[k],
                                    plan.module_self_foreign[k] ? "(foreign:standalone-self)" : "");
        if (r < 0 || static_cast<size_t>(r) >= cap - w) break;
        w += static_cast<size_t>(r);
    }
    return w;
}

// ---- mp:X3c: the keep-local pass ---------------------------------------------------------------

void keep_local_save(keep_local_store &k, uint32_t *out_regions, uint32_t *out_bytes) {
    uint32_t at = 0, nr = 0;
    for (int i = 0; i < RESYNC_KEEP_LOCAL_COUNT; ++i) {
        const uint32_t got = mh::state::save_region_bytes(RESYNC_KEEP_LOCAL[i], k.data + at);
        k.off[i]           = at;
        k.len[i]           = got;
        at += got;
        if (got) ++nr;
    }
    *out_regions = nr;
    *out_bytes   = at;
}

void keep_local_restore(const keep_local_store &k) {
    for (int i = 0; i < RESYNC_KEEP_LOCAL_COUNT; ++i)
        mh::state::restore_region_bytes(RESYNC_KEEP_LOCAL[i], k.data + k.off[i], k.len[i]);
}

} // namespace mh::state::world
