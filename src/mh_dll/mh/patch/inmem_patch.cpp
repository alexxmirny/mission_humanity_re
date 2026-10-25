#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "hook/promoted.h" // C1: a site inside a body we promoted must not be written
#include "patch/inmem_patch.h"

namespace mh::patch {
namespace {

// MAX_SECTIONS / MAX_SITE_LEN moved to the header at fork F4C-GATE so the compiled-in set can be
// asserted against them off-rig; they are used unqualified below exactly as before.

void (*g_log)(const char *);

// PT-CAP8: a refusal's one-line detail is ALWAYS kept in apply_result::detail and logged unless the caller asked for quiet.
void rlog(apply_result &r, bool quiet, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(r.detail, sizeof(r.detail), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (!quiet && g_log) g_log(r.detail);
}

void plog(const char *fmt, ...) {
    if (!g_log) return;
    char    b[400];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(b, sizeof(b), _TRUNCATE, fmt, ap);
    va_end(ap);
    g_log(b);
}

// ---- the production host ------------------------------------------------------------------------

void *win_reserve_at(uint32_t va, uint32_t size) {
    return VirtualAlloc(reinterpret_cast<void *>(va), size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}
void *win_reserve_any(uint32_t size) {
    return VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}
void win_release(void *p, uint32_t) { VirtualFree(p, 0, MEM_RELEASE); }
bool win_protect(void *p, uint32_t size, uint32_t want, uint32_t *old_out) {
    DWORD old = 0;
    if (!VirtualProtect(p, size, want, &old)) return false;
    if (old_out) *old_out = old;
    return true;
}
uint8_t *win_at(uint32_t va) { return reinterpret_cast<uint8_t *>(static_cast<uintptr_t>(va)); }

bool win_query(const void *p, uint32_t *region_end, uint32_t *prot) {
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
    *region_end = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize);
    *prot       = mbi.Protect;
    return true;
}

const patch_host  WIN_HOST = {&win_reserve_at, &win_reserve_any, &win_release, &win_protect, &win_at, &win_query};
const patch_host *g_host   = &WIN_HOST;

// A PE section's characteristics as the page protection the loader would have given it. Only the
// combinations the manifests use are named; anything else gets the read-only answer rather than a
// silently-executable page.
uint32_t prot_of(uint32_t chars) {
    const bool exec  = (chars & 0x20000000u) != 0; // IMAGE_SCN_MEM_EXECUTE
    const bool write = (chars & 0x80000000u) != 0; // IMAGE_SCN_MEM_WRITE
    if (exec && write) return PAGE_EXECUTE_READWRITE;
    if (exec) return PAGE_EXECUTE_READ;
    if (write) return PAGE_READWRITE;
    return PAGE_READONLY;
}

// Rewrite, IN PLACE, every placement-sensitive dword in one blob for where that blob actually sits.
bool relocate_blob(const inmem_manifest &m, bool in_section, int owner, uint32_t blob_va,
                   const uint32_t *actual_base, uint8_t *blob, uint32_t len) {
    for (int i = 0; i < m.ref_count; ++i) {
        const inmem_ref &r = m.refs[i];
        if ((r.in_section != 0) != in_section || r.owner != owner) continue;
        if (r.off + 4u > len) return false;
        // F4C-COMP. abs32_to_section writes the ADDRESS, the other two write a DISPLACEMENT to it.
        // The split is deliberate and it is the whole of the new kind: the caphike family's sites
        // are bare operand dwords whose instruction starts before the patch VA, so `blob_va + off +
        // 4` -- the end-of-instruction every rel32 encoding puts the immediate last of -- is not the
        // end of anything. Subtracting it would produce a plausible number pointing nowhere.
        if (r.kind == ref_kind::abs32_to_section) {
            const uint32_t abs = actual_base[r.target_section] + r.target;
            memcpy(blob + r.off, &abs, 4);
            continue;
        }
        const uint32_t target = (r.kind == ref_kind::rel32_to_section)
                                    ? actual_base[r.target_section] + r.target
                                    : r.target;
        const int32_t  rel    = static_cast<int32_t>(target - (blob_va + r.off + 4u));
        memcpy(blob + r.off, &rel, 4);
    }
    return true;
}

// Does any site of `m` land in the first `entry_window` bytes of body `b`? That is the only part of
// a body a TRAMPOLINE install contests; everything past it composes (hook/promoted.h, the Q6 rule).
bool body_entry_window_hit(const inmem_manifest &m, const inmem_body &b, uint32_t window) {
    for (int i = 0; i < m.site_count; ++i) {
        const inmem_site &s = m.sites[i];
        // Overlap, not containment: a site STARTING before the entry can still reach into the
        // window, and the caphike sites are 4-byte operands carved out of longer instructions.
        if (s.va + s.len > b.entry && s.va < b.entry + window) return true;
    }
    return false;
}

// ---- PT-CAP8: BATCHED PAGE PROTECTION ---------------------------------------------------------------
//
// The per-site protect/restore the applier started with (4 VirtualProtect + 1 FlushInstructionCache
// per site) is ~20,000 syscalls at the v0.2.2 relocation manifest's 3,939 sites. The writes are instead
// done over PAGE RUNS: the pages the sites touch, merged where adjacent and split where the page
// protection changes (.text R-X into .data RW-: one VirtualProtect `old` value cannot restore both),
// made RWX once, written, and restored once. ONE FlushInstructionCache covers [lowest site, end of the
// highest).
struct page_run {
    uint32_t lo, hi; // page-aligned [lo, hi)
    uint32_t old;    // the protection to restore
};

constexpr uint32_t PAGE = 0x1000u;

int cmp_run(const void *a, const void *b) {
    const page_run *x = static_cast<const page_run *>(a), *y = static_cast<const page_run *>(b);
    return x->lo < y->lo ? -1 : (x->lo > y->lo ? 1 : 0);
}

// The runs covering `sites[idx[0..n))` (or sites[0..n) when idx is null). Heap-allocated; the caller
// frees. *fail is set on an allocation failure.
int build_runs(const inmem_site *sites, const int *idx, int n, page_run **out, bool *fail) {
    *out  = nullptr;
    *fail = false;
    if (n <= 0) return 0;
    page_run *raw = static_cast<page_run *>(malloc(sizeof(page_run) * static_cast<size_t>(n)));
    if (!raw) {
        *fail = true;
        return 0;
    }
    for (int i = 0; i < n; ++i) {
        const inmem_site &s = sites[idx ? idx[i] : i];
        raw[i].lo           = s.va & ~(PAGE - 1);
        raw[i].hi           = (s.va + s.len + PAGE - 1) & ~(PAGE - 1);
        raw[i].old          = 0;
    }
    qsort(raw, static_cast<size_t>(n), sizeof(page_run), &cmp_run);
    int m = 0;
    for (int i = 0; i < n; ++i) {
        if (m && raw[i].lo <= raw[m - 1].hi) {
            if (raw[i].hi > raw[m - 1].hi) raw[m - 1].hi = raw[i].hi;
        } else {
            raw[m++] = raw[i];
        }
    }
    if (!g_host->query) {
        *out = raw;
        return m;
    }
    // Split at protection changes.
    int       cap  = m * 4 + 8;
    page_run *outr = static_cast<page_run *>(malloc(sizeof(page_run) * static_cast<size_t>(cap)));
    if (!outr) {
        free(raw);
        *fail = true;
        return 0;
    }
    int k = 0;
    for (int i = 0; i < m; ++i) {
        uint32_t p = raw[i].lo;
        while (p < raw[i].hi) {
            uint32_t end = 0, prot = 0;
            if (!g_host->query(g_host->at(p), &end, &prot) || end <= p) {
                // Unknown region: keep the remainder as ONE run restored from protect()'s own `old`.
                end  = raw[i].hi;
                prot = 0;
            }
            if (end > raw[i].hi) end = raw[i].hi;
            if (k == cap) {
                cap *= 2;
                page_run *g = static_cast<page_run *>(realloc(outr, sizeof(page_run) * static_cast<size_t>(cap)));
                if (!g) {
                    free(outr);
                    free(raw);
                    *fail = true;
                    return 0;
                }
                outr = g;
            }
            outr[k].lo  = p;
            outr[k].hi  = end;
            outr[k].old = prot;
            ++k;
            p = end;
        }
    }
    free(raw);
    *out = outr;
    return k;
}

// Make every run writable. On a failure the runs already opened are restored and the failing run's
// index is returned; -1 = all open. `old` of a run whose region was unknown is filled from protect().
int open_runs(page_run *runs, int n) {
    for (int i = 0; i < n; ++i) {
        uint32_t old = 0;
        if (!g_host->protect(g_host->at(runs[i].lo), runs[i].hi - runs[i].lo, PAGE_EXECUTE_READWRITE, &old)) {
            for (int k = 0; k < i; ++k) {
                uint32_t dummy = 0;
                g_host->protect(g_host->at(runs[k].lo), runs[k].hi - runs[k].lo, runs[k].old, &dummy);
            }
            return i;
        }
        if (runs[i].old == 0) runs[i].old = old;
    }
    return -1;
}

void close_runs(const page_run *runs, int n) {
    for (int i = 0; i < n; ++i) {
        uint32_t dummy = 0;
        g_host->protect(g_host->at(runs[i].lo), runs[i].hi - runs[i].lo, runs[i].old, &dummy);
    }
}

// The first site (by index) that lies in run `r`, for a refusal message. -1 if none.
int site_in_run(const inmem_site *sites, int n, const int *idx, const page_run &r) {
    for (int i = 0; i < n; ++i) {
        const inmem_site &s = sites[idx ? idx[i] : i];
        if (s.va + s.len > r.lo && s.va < r.hi) return idx ? idx[i] : i;
    }
    return -1;
}

void flush_span(const inmem_site *sites, const int *idx, int n) {
    uint32_t lo = 0xFFFFFFFFu, hi = 0;
    for (int i = 0; i < n; ++i) {
        const inmem_site &s = sites[idx ? idx[i] : i];
        if (s.va < lo) lo = s.va;
        if (s.va + s.len > hi) hi = s.va + s.len;
    }
    if (hi > lo) FlushInstructionCache(GetCurrentProcess(), g_host->at(lo), hi - lo);
}

// ---- PT-CAP8: THE APPLIED-PLACEMENT RECORD (overlay_applied) -------------------------------------
struct applied_rec {
    const inmem_manifest *m;
    uint32_t              actual_base[4];
    bool                  sorted; // sites strictly ascending by VA -> binary search is valid
};
constexpr int MAX_APPLIED = 4;
applied_rec   g_applied[MAX_APPLIED];
int           g_applied_n = 0;

bool sites_sorted(const inmem_manifest &m) {
    for (int i = 1; i < m.site_count; ++i)
        if (m.sites[i].va <= m.sites[i - 1].va) return false;
    return true;
}

bool match_masked(const uint8_t *cur, const inmem_site &s) {
    for (uint32_t k = 0; k < s.len; ++k)
        if (s.mask[k] && (cur[k] & s.mask[k]) != (s.expect[k] & s.mask[k])) return false;
    return true;
}

uint64_t qpc_now() {
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return static_cast<uint64_t>(v.QuadPart);
}
uint32_t qpc_micros_since(uint64_t t0) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    if (!f.QuadPart) return 0;
    return static_cast<uint32_t>(((qpc_now() - t0) * 1000000ull) / static_cast<uint64_t>(f.QuadPart));
}

} // namespace

void set_host(const patch_host *h) { g_host = h ? h : &WIN_HOST; }
void set_logger(void (*fn)(const char *)) { g_log = fn; }

int verify_sites(const inmem_manifest &m) {
    for (int i = 0; i < m.site_count; ++i) {
        const inmem_site &s   = m.sites[i];
        const uint8_t    *cur = g_host->at(s.va);
        for (uint32_t k = 0; k < s.len; ++k)
            if (s.mask[k] && (cur[k] & s.mask[k]) != (s.expect[k] & s.mask[k])) return i;
    }
    return -1;
}

bool relocate_site(const inmem_manifest &m, int site, const uint32_t *actual_base, uint8_t *out) {
    const inmem_site &s = m.sites[site];
    memcpy(out, s.repl, s.len);
    return relocate_blob(m, false, site, s.va, actual_base, out, s.len);
}

bool relocate_section(const inmem_manifest &m, int section, const uint32_t *actual_base, uint8_t *out) {
    const inmem_section &sec = m.sections[section];
    if (!sec.data_len) return true;
    memcpy(out, sec.data, sec.data_len);
    return relocate_blob(m, true, section, actual_base[section], actual_base, out, sec.data_len);
}

apply_result apply(const inmem_manifest &m) { return apply(m, nullptr, 0); }

apply_result apply(const inmem_manifest &m, toggle_state *ts, uint32_t flags) {
    apply_result r                      = {};
    r.refused_site                      = -1;
    void          *placed[MAX_SECTIONS] = {};
    uint8_t       *staged               = nullptr;
    uint32_t      *staged_off           = nullptr;
    page_run      *runs                 = nullptr;
    int            nruns                = 0;
    const uint64_t t0                   = qpc_now();
    const bool     quiet                = (flags & APPLY_QUIET_OK) != 0;

    if (m.section_count > MAX_SECTIONS) {
        r.refusal = "more sections than the applier can track";
        return r;
    }
    if (m.toggle && (!ts || m.site_count > MAX_TOGGLE_SITES)) {
        r.refusal = !ts ? "a toggle manifest needs a toggle_state" : "more toggle sites than toggle_state holds";
        return r;
    }

    // ---- PHASE A: refuse before touching anything ------------------------------------------------
    //
    // C1 first, bytes second, and the order is the point -- hook/detour.h's detour_refusal orders its
    // three reasons the same way. A site inside a body this run PROMOTED fails the byte compare too,
    // so letting the bytes answer first reports a displaced patch as a wrong build (dead-ends G68).
    for (int i = 0; i < m.site_count; ++i) {
        if (const char *owner = mh::hook::promoted_owner_of(m.sites[i].va)) {
            rlog(r, quiet, "; [inmem] %s REFUSED: site %d (%08X) is inside %s, which is PROMOTED in this run -- "
                           "those bytes never execute, so the fix must be carried by our implementation.\n",
                 m.name, i, (unsigned)m.sites[i].va, owner);
            r.refusal      = "a site is inside a promoted body";
            r.refused_site = i;
            return r;
        }
    }
    const int bad = verify_sites(m);
    if (bad >= 0) {
        rlog(r, quiet, "; [inmem] %s NOT applied: site %d (%08X) does not hold the expected original bytes "
                       "(already patched, or not this build)\n",
             m.name, bad, (unsigned)m.sites[bad].va);
        r.refusal      = "expected-bytes guard";
        r.refused_site = bad;
        return r;
    }
    for (int i = 0; i < m.site_count; ++i)
        if (m.sites[i].len > MAX_SITE_LEN || (m.toggle && m.sites[i].len > MAX_TOGGLE_LEN)) {
            r.refusal      = "site longer than the applier's staging buffer";
            r.refused_site = i;
            return r;
        }

    // ---- PHASE B: place and fill the sections ----------------------------------------------------
    for (int i = 0; i < m.section_count; ++i) {
        const inmem_section &sec = m.sections[i];
        void                *p   = g_host->reserve_at(sec.preferred_va, sec.vsize);
        // APPLY_QUIET_OK (the PT-CAP8 callers): the baked VA is not expected to be free in a live process and a relocatable
        // manifest does not care; the occupant diagnostic below is only worth a line when the fallback fails too.
        if (!p && !(quiet && m.refs_complete)) {
            // WHY the baked VA was refused, in the one line that answers it. mhpatch's cave lands at
            // the exe's next-free VA, which in memory is one page past the image -- and that is
            // precisely where the loader and the CRT put the first things they reserve. Naming the
            // occupant here is what turns "VirtualAlloc failed" into a cave-placement STRATEGY.
            MEMORY_BASIC_INFORMATION mbi = {};
            if (VirtualQuery(reinterpret_cast<void *>(sec.preferred_va), &mbi, sizeof(mbi))) {
                char who[MAX_PATH] = "";
                if (mbi.Type == MEM_IMAGE)
                    GetModuleFileNameA((HMODULE)mbi.AllocationBase, who, sizeof(who));
                plog("; [inmem] %s: the baked cave VA %08X is not free -- state=%s type=%s base=%08X "
                     "size=%08X protect=%08X %s\n",
                     m.name, (unsigned)sec.preferred_va,
                     mbi.State == MEM_FREE ? "FREE" : (mbi.State == MEM_COMMIT ? "COMMIT" : "RESERVE"),
                     mbi.Type == MEM_IMAGE ? "IMAGE" : (mbi.Type == MEM_MAPPED ? "MAPPED" : "PRIVATE"),
                     (unsigned)(uintptr_t)mbi.AllocationBase, (unsigned)mbi.RegionSize,
                     (unsigned)mbi.Protect, who);
            }
        }
        if (!p && m.refs_complete) p = g_host->reserve_any(sec.vsize);
        if (!p) {
            rlog(r, quiet, "; [inmem] %s NOT applied: no cave at %08X%s\n", m.name, (unsigned)sec.preferred_va,
                 m.refs_complete ? " and the anywhere-fallback failed too"
                                 : " and this manifest is not relocatable (refs_complete=false)");
            r.refusal = m.refs_complete ? "cave allocation failed"
                                        : "baked cave VA unavailable, manifest not relocatable";
            goto unwind;
        }
        placed[i]        = p;
        r.actual_base[i] = (uint32_t)(uintptr_t)p;
        // THE ADDRESS IS THE RESULT, NOT THE REQUEST. VirtualAlloc with a non-null lpAddress rounds
        // DOWN to the 64 KB allocation granularity, so an exact-VA request can succeed at an address
        // that is not the one asked for -- and a manifest whose relocation data is incomplete may
        // not silently accept that, because its cave's internal rel32s would then be wrong with
        // nothing to say so.
        if (r.actual_base[i] != sec.preferred_va) {
            if (!m.refs_complete) {
                rlog(r, quiet, "; [inmem] %s NOT applied: the cave landed at %08X, not its baked %08X, and this "
                               "manifest is not relocatable (refs_complete=false)\n",
                     m.name, (unsigned)r.actual_base[i], (unsigned)sec.preferred_va);
                r.refusal = "baked cave VA unavailable, manifest not relocatable";
                goto unwind;
            }
            r.relocated = true;
        }
    }
    // Fill AFTER every placement is known: a rel32 from one section into another needs both bases.
    for (int i = 0; i < m.section_count; ++i) {
        const inmem_section &sec = m.sections[i];
        uint8_t             *dst = static_cast<uint8_t *>(placed[i]);
        if (sec.data_len) {
            memcpy(dst, sec.data, sec.data_len);
            if (!relocate_blob(m, true, i, r.actual_base[i], r.actual_base, dst, sec.data_len)) {
                r.refusal = "a section ref names an offset outside its data (generator bug)";
                goto unwind;
            }
        }
        uint32_t old = 0;
        if (!g_host->protect(dst, sec.vsize, prot_of(sec.chars), &old)) {
            r.refusal = "cave protect failed";
            goto unwind;
        }
    }

    // ---- PHASE C: the writes ---------------------------------------------------------------------
    //
    // BATCHED, and still all-or-nothing. Every site's bytes are relocated into a staging buffer FIRST
    // (a ref that names an offset outside its blob refuses before a single byte moves), then every page
    // run is made writable -- a protect that fails restores the runs already opened and refuses with
    // nothing written -- and only then do the writes happen, over all runs at once. Nothing runs
    // between the open and the writes (DllMain, one thread). A TOGGLE manifest stops after the open:
    // it proves the runs CAN be opened, stages its ON bytes, and writes nothing.
    {
        uint32_t total = 0;
        staged_off     = static_cast<uint32_t *>(malloc(sizeof(uint32_t) * (size_t)(m.site_count + 1)));
        if (!staged_off) {
            r.refusal = "out of memory staging the site bytes";
            goto unwind;
        }
        for (int i = 0; i < m.site_count; ++i) {
            staged_off[i] = total;
            total += m.sites[i].len;
        }
        staged_off[m.site_count] = total;
        staged                   = static_cast<uint8_t *>(malloc(total ? total : 1));
        if (!staged) {
            r.refusal = "out of memory staging the site bytes";
            goto unwind;
        }
        for (int i = 0; i < m.site_count; ++i) {
            if (!relocate_site(m, i, r.actual_base, staged + staged_off[i])) {
                r.refusal      = "a site ref names an offset outside its bytes (generator bug)";
                r.refused_site = i;
                goto unwind;
            }
        }
        bool fail = false;
        nruns     = build_runs(m.sites, nullptr, m.site_count, &runs, &fail);
        if (fail) {
            r.refusal = "out of memory building the page runs";
            goto unwind;
        }
        const int open_fail = open_runs(runs, nruns);
        if (open_fail >= 0) {
            const int si = site_in_run(m.sites, m.site_count, nullptr, runs[open_fail]);
            rlog(r, quiet, "; [inmem] %s NOT applied: site %d (%08X) cannot be made writable\n", m.name, si,
                 si >= 0 ? (unsigned)m.sites[si].va : (unsigned)runs[open_fail].lo);
            r.refusal      = "site protect failed";
            r.refused_site = si;
            nruns          = 0; // nothing left open (open_runs restored what it opened)
            goto unwind;
        }
        r.protect_runs = nruns;
        if (m.toggle) {
            for (int i = 0; i < m.site_count; ++i) {
                memcpy(ts->on_bytes[i], staged + staged_off[i], m.sites[i].len);
                // the live bytes now are the originals (the expected-bytes guard passed): save them whole
                memcpy(ts->off_bytes[i], g_host->at(m.sites[i].va), m.sites[i].len);
            }
            ts->m = &m;
            memcpy(ts->actual_base, r.actual_base, sizeof(ts->actual_base));
            ts->staged = true;
            ts->on     = false;
            close_runs(runs, nruns);
        } else {
            for (int i = 0; i < m.site_count; ++i)
                memcpy(g_host->at(m.sites[i].va), staged + staged_off[i], m.sites[i].len);
            close_runs(runs, nruns);
            flush_span(m.sites, nullptr, m.site_count);
        }
        nruns = 0;
    }

    // ---- PHASE D: REGISTER (F4C-COMP / Q6) -------------------------------------------------------
    //
    // Only now, and only on the success path: a refused manifest wrote nothing, so registering its
    // bodies would refuse later claimants on behalf of bytes that are not there -- the mirror image
    // of the silence this interlock exists to end, and just as wrong.
    {
        const uint32_t window = (uint32_t)mh::hook::entry_window_bytes();
        for (int i = 0; i < m.body_count; ++i) {
            const inmem_body &b = m.bodies[i];
            mh::hook::note_patched_body(b.entry, b.end, b.name, m.name,
                                        body_entry_window_hit(m, b, window));
        }
    }
    if (!m.toggle && g_applied_n < MAX_APPLIED) {
        applied_rec &a = g_applied[g_applied_n++];
        a.m            = &m;
        memcpy(a.actual_base, r.actual_base, sizeof(a.actual_base));
        a.sorted = sites_sorted(m);
    }

    free(runs);
    free(staged);
    free(staged_off);
    r.applied = true;
    r.micros  = qpc_micros_since(t0);
    if (!quiet)
        plog("; [inmem] %s APPLIED: %d site(s), %d section(s), %d body(ies) registered with the "
             "interlock%s%s\n",
             m.name, m.site_count, m.section_count, m.body_count,
             m.toggle ? " (limit layer: staged, written on toggle)" : "",
             r.relocated ? " -- cave RELOCATED off its baked VA" : "");
    return r;

unwind:
    if (nruns) close_runs(runs, nruns);
    free(runs);
    free(staged);
    free(staged_off);
    for (int i = 0; i < m.section_count; ++i)
        if (placed[i]) g_host->release(placed[i], m.sections[i].vsize);
    memset(r.actual_base, 0, sizeof(r.actual_base));
    r.applied   = false;
    r.relocated = false;
    r.micros    = qpc_micros_since(t0);
    return r;
}

bool toggle(toggle_state &ts, bool on, int *bad_site, const char **why) {
    if (bad_site) *bad_site = -1;
    if (why) *why = nullptr;
    auto refuse = [&](const char *w, int site) {
        if (bad_site) *bad_site = site;
        if (why) *why = w;
        if (site >= 0 && ts.m)
            plog("; [inmem] %s toggle %s REFUSED: site %d (%08X) %s\n", ts.m->name, on ? "ON" : "OFF", site,
                 (unsigned)ts.m->sites[site].va, w);
        return false;
    };
    if (!ts.staged || !ts.m) return refuse("toggle_state was never staged by apply()", -1);
    const inmem_manifest &m = *ts.m;

    // Verify first: every site is at one of its two images. Collect the ones that need a write.
    int need[MAX_TOGGLE_SITES];
    int nn = 0;
    for (int i = 0; i < m.site_count; ++i) {
        const inmem_site &s      = m.sites[i];
        const uint8_t    *cur    = g_host->at(s.va);
        const bool        is_off = match_masked(cur, s);
        const bool        is_on  = memcmp(cur, ts.on_bytes[i], s.len) == 0;
        if (!is_off && !is_on)
            return refuse("holds bytes that are neither its vanilla nor its raised image (a foreign writer)", i);
        // OFF is satisfied only by the EXACT saved original (wildcards included); a masked-equal image whose
        // wildcard bytes drifted is restored.
        const bool exact_off = memcmp(cur, ts.off_bytes[i], s.len) == 0;
        if ((on && !is_on) || (!on && !exact_off)) need[nn++] = i;
    }
    if (nn == 0) {
        ts.on = on;
        return true;
    }
    page_run *runs = nullptr;
    bool      fail = false;
    const int n    = build_runs(m.sites, need, nn, &runs, &fail);
    if (fail) return refuse("out of memory building the page runs", -1);
    const int of = open_runs(runs, n);
    if (of >= 0) {
        const int si = site_in_run(m.sites, nn, need, runs[of]);
        free(runs);
        return refuse("cannot be made writable", si >= 0 ? si : need[0]);
    }
    for (int k = 0; k < nn; ++k) {
        const int         i = need[k];
        const inmem_site &s = m.sites[i];
        uint8_t          *t = g_host->at(s.va);
        if (on) {
            memcpy(t, ts.on_bytes[i], s.len);
        } else {
            // OFF restores the EXACT bytes saved before the first ON, wildcard bytes included.
            memcpy(t, ts.off_bytes[i], s.len);
        }
    }
    close_runs(runs, n);
    flush_span(m.sites, need, nn);
    free(runs);
    ts.on = on;
    return true;
}

int toggle_current(const toggle_state &ts) {
    if (!ts.staged || !ts.m) return -1;
    const inmem_manifest &m   = *ts.m;
    int                   off = 0, on = 0;
    for (int i = 0; i < m.site_count; ++i) {
        const uint8_t *cur = g_host->at(m.sites[i].va);
        if (memcmp(cur, ts.off_bytes[i], m.sites[i].len) == 0) ++off;
        else if (memcmp(cur, ts.on_bytes[i], m.sites[i].len) == 0) ++on;
        else return -1;
    }
    return off == m.site_count ? 0 : (on == m.site_count ? 1 : -1);
}

void overlay_applied(uint32_t va, uint32_t len, uint8_t *inout) {
    for (int a = 0; a < g_applied_n; ++a) {
        const applied_rec    &ar = g_applied[a];
        const inmem_manifest &m  = *ar.m;
        int                   i  = 0;
        if (ar.sorted) {
            // first site that could still reach `va`: site.va + MAX_SITE_LEN > va
            int            lo = 0, hi = m.site_count;
            const uint32_t floor_va = va > (uint32_t)MAX_SITE_LEN ? va - (uint32_t)MAX_SITE_LEN : 0u;
            while (lo < hi) {
                const int mid = (lo + hi) / 2;
                if (m.sites[mid].va < floor_va) lo = mid + 1;
                else hi = mid;
            }
            i = lo;
        }
        for (; i < m.site_count; ++i) {
            const inmem_site &s = m.sites[i];
            if (ar.sorted && s.va >= va + len) break;
            if (s.va + s.len <= va || s.va >= va + len) continue;
            uint8_t buf[MAX_SITE_LEN];
            if (!relocate_site(m, i, ar.actual_base, buf)) continue;
            const uint32_t lo = s.va > va ? s.va : va;
            const uint32_t hi = (s.va + s.len) < (va + len) ? (s.va + s.len) : (va + len);
            memcpy(inout + (lo - va), buf + (lo - s.va), hi - lo);
        }
    }
}

void reset_applied_for_test() { g_applied_n = 0; }

bool write_parity_dump(const inmem_manifest &m, const apply_result &r, const char *path) {
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    char  line[8192];
    DWORD wrote = 0;
    auto  put   = [&](const char *s) { WriteFile(h, s, (DWORD)strlen(s), &wrote, nullptr); };

    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "# mh in-memory static-patch parity dump v1\n"
                "manifest %s\nsha256 %s\napplied %d\nrelocated %d\nrefusal %s\n",
                m.name, m.sha256, r.applied ? 1 : 0, r.relocated ? 1 : 0, r.refusal ? r.refusal : "-");
    put(line);

    // Every extent is READ BACK FROM THE LIVE IMAGE, never echoed from the manifest: a dump that
    // printed what it meant to write would agree with the on-disk file by construction and would
    // prove nothing about what is in the process.
    for (int i = 0; r.applied && i < m.section_count; ++i) {
        const inmem_section &sec = m.sections[i];
        const uint8_t       *p   = reinterpret_cast<const uint8_t *>((uintptr_t)r.actual_base[i]);
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "section %d %s preferred=%08X actual=%08X vsize=%08X datalen=%08X\n", i, sec.name,
                    (unsigned)sec.preferred_va, (unsigned)r.actual_base[i], (unsigned)sec.vsize,
                    (unsigned)sec.data_len);
        put(line);
        int n = _snprintf_s(line, sizeof(line), _TRUNCATE, "extent section %d %08X ", i, (unsigned)r.actual_base[i]);
        for (uint32_t k = 0; k < sec.data_len && n + 3 < (int)sizeof(line); ++k)
            n += _snprintf_s(line + n, sizeof(line) - (size_t)n, _TRUNCATE, "%02x", p[k]);
        _snprintf_s(line + n, sizeof(line) - (size_t)n, _TRUNCATE, "\n");
        put(line);
        // The virtual tail must be zero, exactly as the loader would leave an appended section's.
        // Reported as a verdict rather than dumped -- it is up to 4 KB of zeros.
        bool zero = true;
        for (uint32_t k = sec.data_len; k < sec.vsize; ++k)
            if (p[k]) {
                zero = false;
                break;
            }
        _snprintf_s(line, sizeof(line), _TRUNCATE, "tail section %d %08X %08X allzero=%d\n", i,
                    (unsigned)(r.actual_base[i] + sec.data_len), (unsigned)(sec.vsize - sec.data_len),
                    zero ? 1 : 0);
        put(line);
    }
    for (int i = 0; r.applied && i < m.site_count; ++i) {
        const inmem_site &s = m.sites[i];
        const uint8_t    *p = reinterpret_cast<const uint8_t *>((uintptr_t)s.va);
        int               n = _snprintf_s(line, sizeof(line), _TRUNCATE, "extent site %d %08X ", i, (unsigned)s.va);
        for (uint32_t k = 0; k < s.len && n + 3 < (int)sizeof(line); ++k)
            n += _snprintf_s(line + n, sizeof(line) - (size_t)n, _TRUNCATE, "%02x", p[k]);
        _snprintf_s(line + n, sizeof(line) - (size_t)n, _TRUNCATE, "\n");
        put(line);
    }
    put("end\n");
    CloseHandle(h);
    return true;
}

} // namespace mh::patch
