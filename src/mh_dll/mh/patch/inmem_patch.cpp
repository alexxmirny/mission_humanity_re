#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "hook/promoted.h" // C1: a site inside a body we promoted must not be written
#include "patch/inmem_patch.h"

namespace mh::patch {
namespace {

// MAX_SECTIONS / MAX_SITE_LEN moved to the header at fork F4C-GATE so the compiled-in set can be
// asserted against them off-rig; they are used unqualified below exactly as before.

void (*g_log)(const char *);

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

const patch_host  WIN_HOST = {&win_reserve_at, &win_reserve_any, &win_release, &win_protect, &win_at};
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

apply_result apply(const inmem_manifest &m) {
    apply_result r             = {};
    r.refused_site             = -1;
    void *placed[MAX_SECTIONS] = {};

    if (m.section_count > MAX_SECTIONS) {
        r.refusal = "more sections than the applier can track";
        return r;
    }

    // ---- PHASE A: refuse before touching anything ------------------------------------------------
    //
    // C1 first, bytes second, and the order is the point -- hook/detour.h's detour_refusal orders its
    // three reasons the same way. A site inside a body this run PROMOTED fails the byte compare too,
    // so letting the bytes answer first reports a displaced patch as a wrong build (dead-ends G68).
    for (int i = 0; i < m.site_count; ++i) {
        if (const char *owner = mh::hook::promoted_owner_of(m.sites[i].va)) {
            plog("; [inmem] %s REFUSED: site %d (%08X) is inside %s, which is PROMOTED in this run -- "
                 "those bytes never execute, so the fix must be carried by our implementation.\n",
                 m.name, i, (unsigned)m.sites[i].va, owner);
            r.refusal      = "a site is inside a promoted body";
            r.refused_site = i;
            return r;
        }
    }
    const int bad = verify_sites(m);
    if (bad >= 0) {
        plog("; [inmem] %s NOT applied: site %d (%08X) does not hold the expected original bytes "
             "(already patched, or not this build)\n",
             m.name, bad, (unsigned)m.sites[bad].va);
        r.refusal      = "expected-bytes guard";
        r.refused_site = bad;
        return r;
    }

    // ---- PHASE B: place and fill the sections ----------------------------------------------------
    for (int i = 0; i < m.section_count; ++i) {
        const inmem_section &sec = m.sections[i];
        void                *p   = g_host->reserve_at(sec.preferred_va, sec.vsize);
        if (!p) {
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
            plog("; [inmem] %s NOT applied: no cave at %08X%s\n", m.name, (unsigned)sec.preferred_va,
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
                plog("; [inmem] %s NOT applied: the cave landed at %08X, not its baked %08X, and this "
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
    // A DRY PROTECT PASS FIRST, and it is what makes this all-or-nothing WITHOUT per-site rollback
    // storage. Every site is made writable and immediately restored; only if all of them succeeded
    // does the real pass run. Nothing runs between the two passes (DllMain, one thread), so a protect
    // that succeeded once succeeds again -- which is why a manifest with thousands of sites (the
    // caphike family) still needs O(1) state here instead of a saved copy of every original extent.
    for (int i = 0; i < m.site_count; ++i) {
        uint32_t old = 0;
        uint8_t *t   = g_host->at(m.sites[i].va);
        if (!g_host->protect(t, m.sites[i].len, PAGE_EXECUTE_READWRITE, &old)) {
            plog("; [inmem] %s NOT applied: site %d (%08X) cannot be made writable\n", m.name, i,
                 (unsigned)m.sites[i].va);
            r.refusal      = "site protect failed";
            r.refused_site = i;
            goto unwind;
        }
        g_host->protect(t, m.sites[i].len, old, &old);
    }
    for (int i = 0; i < m.site_count; ++i) {
        const inmem_site &s = m.sites[i];
        if (s.len > MAX_SITE_LEN) {
            r.refusal = "site longer than the applier's staging buffer";
            goto unwind;
        }
        uint8_t staged[MAX_SITE_LEN];
        if (!relocate_site(m, i, r.actual_base, staged)) {
            r.refusal = "a site ref names an offset outside its bytes (generator bug)";
            goto unwind;
        }
        uint8_t *t   = g_host->at(s.va);
        uint32_t old = 0;
        g_host->protect(t, s.len, PAGE_EXECUTE_READWRITE, &old);
        memcpy(t, staged, s.len);
        g_host->protect(t, s.len, old, &old);
        FlushInstructionCache(GetCurrentProcess(), t, s.len);
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

    r.applied = true;
    plog("; [inmem] %s APPLIED: %d site(s), %d section(s), %d body(ies) registered with the "
         "interlock%s\n",
         m.name, m.site_count, m.section_count, m.body_count,
         r.relocated ? " -- cave RELOCATED off its baked VA" : "");
    return r;

unwind:
    for (int i = 0; i < m.section_count; ++i)
        if (placed[i]) g_host->release(placed[i], m.sections[i].vsize);
    memset(r.actual_base, 0, sizeof(r.actual_base));
    r.applied   = false;
    r.relocated = false;
    return r;
}

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
