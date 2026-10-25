//
// inmem_patch_selftest.cpp -- the in-memory static-patch applier, off the rig (F1E).
//
// WHY A UNIT TEST AND NOT A RIG RUN, the same argument interlock_selftest makes one file over: the
// applier's whole contract is about what does NOT happen. A refused manifest leaves the image byte
// for byte as it was, and a run where the refusal never fired looks exactly the same from outside.
// The rig run proves the POSITIVE half (and does: the parity dump is compared byte-for-byte against
// mhpatch's on-disk output by tools/check_inmem_patch_parity.py); the negative half -- a guarded
// byte that moved, a cave VA that is taken, a site inside a promoted body -- cannot be provoked on a
// real game image without shipping a deliberately broken build.
//
// So the applier writes only through an injectable host table, exactly as hook/detour.h's page_ops
// does and for exactly that reason, and this file supplies one whose `at()` maps the manifest's real
// VAs into a buffer it owns. The MANIFEST under test is a committed, GENERATED one -- not a
// synthetic stand-in -- so the arithmetic arms below are assertions about real manifest bytes, and
// the generator cannot drift away from the applier without saying so here.
//
// IT IS NOW A FIXTURE RATHER THAN THE COMPILED-IN MANIFEST (fork F4C-GATE). Q7 reclassified
// no_cd_EN as `reimplemented` -- seams/standalone.cpp already trampolines the body it patches -- so
// it is no longer in mh.dll's compile list, which is the three EN caphike/relocate manifests. This
// file keeps it because the applier's NEGATIVE half needs a manifest that can be reasoned about
// completely: one site, one containing body, and an initialised cave whose trailing `jmp` back into
// the game is a known address. The shipped alternatives are 75 to 7,770 sites across up to 530
// bodies, which would turn every assertion below from a named fact into self-consistency. The
// positive, at-scale half is the parity oracle's (tools/check_inmem_patch_parity.py), which runs
// over every shipped manifest -- offline in lint, and live on the rig.
//
#include "patch/inmem_patch.h"
#include "patch/manifests.gen.h" // the SHIPPED set -- arm (9) asserts it against the applier's caps

#include "manifest_no_cd_en.fixture.gen.h"

#include "hook/detour.h"
#include "hook/promoted.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace mh::patch;
namespace M = mh::patch::manifest_no_cd_en;

namespace {

int g_checks = 0, g_fails = 0;

void ck(bool ok, const char *what) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL: %s\n", what);
    }
}

int32_t rel32_at(const uint8_t *p, uint32_t off) {
    int32_t v;
    memcpy(&v, p + off, 4);
    return v;
}

// ---- the fake host --------------------------------------------------------------------------
//
// `at()` windows the single site into a buffer. IMG_LO is chosen so the manifest's own site VA
// lands inside it, which is what lets the REAL manifest drive the apply arms.
constexpr uint32_t IMG_LO   = 0x004C3700u;
constexpr uint32_t IMG_SIZE = 0x100u;

uint8_t g_img[IMG_SIZE];
uint8_t g_cave[0x1000];

bool g_deny_exact = false; // reserve_at refuses -> the relocation / refusal arms
bool g_deny_any   = false;
int  g_n_reserve = 0, g_n_release = 0;
bool g_cave_live = false;

void *fake_reserve_at(uint32_t va, uint32_t size) {
    ++g_n_reserve;
    if (g_deny_exact || size > sizeof(g_cave)) return nullptr;
    (void)va;
    memset(g_cave, 0, sizeof(g_cave)); // VirtualAlloc hands back zeroed pages; so must the fake
    g_cave_live = true;
    return g_cave;
}
void *fake_reserve_any(uint32_t size) {
    ++g_n_reserve;
    if (g_deny_any || size > sizeof(g_cave)) return nullptr;
    memset(g_cave, 0, sizeof(g_cave));
    g_cave_live = true;
    return g_cave;
}
void fake_release(void *, uint32_t) {
    ++g_n_release;
    g_cave_live = false;
}
bool fake_protect(void *, uint32_t, uint32_t, uint32_t *old_out) {
    if (old_out) *old_out = 0x40;
    return true;
}
uint8_t *fake_at(uint32_t va) { return g_img + (va - IMG_LO); }

const patch_host FAKE = {&fake_reserve_at, &fake_reserve_any, &fake_release, &fake_protect, &fake_at};

// A pristine window: the site holds its expected original bytes, everything else is a pattern that
// is neither 0xE9 nor a NOP, so "nothing else was written" is checkable.
void arm_image() {
    memset(g_img, 0xCC, sizeof(g_img));
    const inmem_site &s = M::SITES[0];
    memcpy(g_img + (s.va - IMG_LO), s.expect, s.len);
}
bool image_clean_outside_site() {
    const inmem_site &s   = M::SITES[0];
    const uint32_t    off = s.va - IMG_LO;
    for (uint32_t i = 0; i < IMG_SIZE; ++i) {
        if (i >= off && i < off + s.len) continue;
        if (g_img[i] != 0xCC) return false;
    }
    return true;
}
bool site_still_original() {
    const inmem_site &s = M::SITES[0];
    return memcmp(g_img + (s.va - IMG_LO), s.expect, s.len) == 0;
}

void reset_counters() {
    g_n_reserve = g_n_release = 0;
    g_deny_exact = g_deny_any = false;
    g_cave_live               = false;
}

// The applier's diagnostic channel, captured. "REFUSED BY NAME" (fork ruling Q6) is a property of
// the LINE, not of the `refusal` string -- that one is a short category the caller switches on --
// so the only way to assert it is to read what the run would have written into mh_net.log.
char g_last_log[1024];
void capture_log(const char *s) { strncpy_s(g_last_log, sizeof(g_last_log), s, _TRUNCATE); }
bool logged(const char *needle) { return strstr(g_last_log, needle) != nullptr; }

} // namespace


// ---- PT-CAP8: a WIDE fake host ------------------------------------------------------------------
//
// The arms below drive the BATCHED applier, the toggle layer and the overlay over the REAL v0.2.2 cap
// manifests (3,941 + 51 sites between 0x41129b and 0x4ee14b). The image is a heap window over
// 0x00400000..0x004F0000 (the sites' span, filled with a pattern that is neither a vanilla byte nor a
// raised one, then seeded with each site's `expect`), sections are calloc'd, and every protect call is
// recorded so the batching is a measurement, not a claim.
namespace wide {

constexpr uint32_t LO   = 0x00400000u;
constexpr uint32_t SIZE = 0x000F0000u;

uint8_t *img = nullptr;

struct pcall {
    uint32_t off, size, prot;
};
pcall    g_calls[8192];
int      g_ncalls      = 0;  // image-range protect calls only
int      g_sec_protect = 0;  // protects of section memory (the caves / arrays)
int      g_fail_at     = -1; // fail the Nth image-range protect call (0-based); -1 = never
int      g_alloc_fail  = -1;
void    *g_sections[8];
uint32_t g_sec_size[8];
int      g_nsec     = 0;
uint32_t g_split_va = 0; // query(): the region containing LO.. ends here with prot 0x20; the rest is 0x04

void *res_at(uint32_t, uint32_t size) {
    if (g_nsec >= 8) return nullptr;
    void *p              = calloc(size, 1);
    g_sections[g_nsec]   = p;
    g_sec_size[g_nsec++] = size;
    return p;
}
void *res_any(uint32_t size) { return res_at(0, size); }
void  rel(void *p, uint32_t) {
    for (int i = 0; i < g_nsec; ++i)
        if (g_sections[i] == p) {
            free(p);
            g_sections[i] = nullptr;
        }
}
bool in_img(const void *p) { return p >= img && p < img + SIZE; }
bool prot(void *p, uint32_t size, uint32_t want, uint32_t *old_out) {
    if (!in_img(p)) {
        ++g_sec_protect;
        if (old_out) *old_out = 0x04;
        return true;
    }
    const uint32_t off = (uint32_t)((uint8_t *)p - img);
    if (g_fail_at == g_ncalls) {
        ++g_ncalls;
        return false;
    }
    if (g_ncalls < 8192) g_calls[g_ncalls] = {off, size, want};
    ++g_ncalls;
    if (old_out) *old_out = (g_split_va && LO + off >= g_split_va) ? 0x04u : 0x20u;
    return true;
}
uint8_t *at(uint32_t va) { return img + (va - LO); }
bool     query(const void *p, uint32_t *end, uint32_t *pr) {
    const uint32_t va = LO + (uint32_t)((const uint8_t *)p - img);
    if (!g_split_va) {
        *end = LO + SIZE;
        *pr  = 0x20;
        return true;
    }
    if (va < g_split_va) {
        *end = g_split_va;
        *pr  = 0x20;
    } else {
        *end = LO + SIZE;
        *pr  = 0x04;
    }
    return true;
}
const patch_host HOST = {&res_at, &res_any, &rel, &prot, &at, &query};

void reset() {
    g_ncalls = g_sec_protect = g_nsec = 0;
    g_fail_at = g_alloc_fail = -1;
    g_split_va               = 0;
    memset(g_sections, 0, sizeof(g_sections));
}
void free_sections() {
    for (int i = 0; i < g_nsec; ++i) free(g_sections[i]), g_sections[i] = nullptr;
    g_nsec = 0;
}
// Fill the window with a pattern and seed every site of `m` with its vanilla bytes.
void seed(const inmem_manifest &m) {
    memset(img, 0xCC, SIZE);
    for (int i = 0; i < m.site_count; ++i) memcpy(at(m.sites[i].va), m.sites[i].expect, m.sites[i].len);
}
bool image_equals(const uint8_t *snap) { return memcmp(img, snap, SIZE) == 0; }

} // namespace wide

// The synthetic 400-site manifest the batching arm uses: four groups of 100 4-byte sites, each group
// inside ONE page, groups 0x20000 apart (so exactly four runs).
namespace synth {
constexpr int  N = 400;
uint8_t        expect_b[N][4], mask_b[N][4], repl_b[N][4];
inmem_site     sites[N];
inmem_manifest man;
void           build() {
    for (int i = 0; i < N; ++i) {
        const uint32_t grp = (uint32_t)(i / 100), idx = (uint32_t)(i % 100);
        for (int k = 0; k < 4; ++k) {
            expect_b[i][k] = (uint8_t)(0x11 + k);
            mask_b[i][k]   = 0xff;
            repl_b[i][k]   = (uint8_t)(0xA0 + (i & 0xf));
        }
        sites[i] = {0x00410000u + grp * 0x20000u + idx * 0x20u, expect_b[i], mask_b[i], repl_b[i], 4u};
    }
    man = {"synthetic_400", "0", nullptr, 0, sites, N, nullptr, 0, nullptr, 0, true, false};
}
} // namespace synth

int run_patchtest() {
    printf("== in-memory static-patch selftest (F1E: the applier + its refusals) ==\n");
    const inmem_manifest &m = M::MANIFEST;
    set_host(&FAKE);
    set_logger(&capture_log);

    // ---- (1) THE MANIFEST'S OWN ARITHMETIC. The generator recovered which dwords are relative by
    // decoding both blobs; the check that it recovered them CORRECTLY is that re-deriving them at
    // the BAKED placement reproduces mhpatch's bytes exactly. A ref at a wrong offset, or naming a
    // wrong target, fails here and nowhere else -- the apply arms below would happily write the
    // wrong rel32 and report success.
    ck(m.section_count == 1 && m.site_count == 1, "the spike manifest is one site and one cave");
    ck(m.refs_complete, "the generator proved it decoded every branch (refs_complete)");
    {
        const uint32_t baked[4]  = {M::SECTIONS[0].preferred_va, 0, 0, 0};
        uint8_t        site[8]   = {};
        uint8_t        cave[128] = {};
        ck(relocate_site(m, 0, baked, site), "relocate_site accepts the baked placement");
        ck(memcmp(site, M::SITES[0].repl, M::SITES[0].len) == 0,
           "at the BAKED cave VA the relocated site bytes are mhpatch's bytes, unchanged");
        ck(relocate_section(m, 0, baked, cave), "relocate_section accepts the baked placement");
        ck(memcmp(cave, M::SECTIONS[0].data, M::SECTIONS[0].data_len) == 0,
           "...and so are the relocated cave bytes -- the ref table is the identity where it must be");
    }

    // The two references, stated as the facts the disc-check RE states: the patch is a JMP into the
    // cave, and the cave's last instruction is a JMP back into llm_cd_locate_and_open_audio's
    // shared return-0 epilogue. If either ever stops being true the manifest changed shape.
    {
        const inmem_site &s = M::SITES[0];
        ck(s.len == 5 && s.repl[0] == 0xE9, "the site patch is a 5-byte E9");
        ck(s.va + 5 + (uint32_t)rel32_at(s.repl, 1) == M::SECTIONS[0].preferred_va,
           "the site JMP targets the cave's baked base");
        const inmem_section &sec = M::SECTIONS[0];
        ck(sec.data[sec.data_len - 5] == 0xE9, "the cave's last instruction is a 5-byte E9");
        const uint32_t tail_va = sec.preferred_va + sec.data_len - 5;
        ck(tail_va + 5 + (uint32_t)rel32_at(sec.data, sec.data_len - 4) == 0x004C3848u,
           "...and it rejoins the original return-0 epilogue at 0x004c3848");
    }

    // ---- (2) THE RELOCATION ARITHMETIC. The cave's baked VA is the exe's next-free VA, which in
    // memory is an ALLOCATION REQUEST that can be declined. Everything the applier would then do is
    // this arithmetic, so it is asserted directly rather than only through a host that happens to
    // hand back a different address.
    {
        const uint32_t moved[4]  = {M::SECTIONS[0].preferred_va + 0x20000u, 0, 0, 0};
        uint8_t        site[8]   = {};
        uint8_t        cave[128] = {};
        relocate_site(m, 0, moved, site);
        relocate_section(m, 0, moved, cave);
        const inmem_site &s = M::SITES[0];
        ck(s.va + 5 + (uint32_t)rel32_at(site, 1) == moved[0],
           "a moved cave: the site JMP follows it");
        const inmem_section &sec     = M::SECTIONS[0];
        const uint32_t       tail_va = moved[0] + sec.data_len - 5;
        ck(tail_va + 5 + (uint32_t)rel32_at(cave, sec.data_len - 4) == 0x004C3848u,
           "a moved cave: its JMP back still lands on the SAME game address");
        ck(memcmp(site, s.repl, s.len) != 0 && memcmp(cave, sec.data, sec.data_len) != 0,
           "...and both blobs really did change -- a no-op relocation would pass the two above");
    }

    // ---- (3) THE POSITIVE ARM. Everything below drives the real applier over the real manifest.
    reset_counters();
    arm_image();
    {
        const apply_result r = apply(m);
        ck(r.applied && r.refusal == nullptr, "a pristine image accepts the manifest");
        ck(g_n_reserve == 1 && r.actual_base[0] == (uint32_t)(uintptr_t)g_cave,
           "the cave was placed by the exact-VA request, not the fallback");
        // The fake host cannot hand back 0x010f0000 itself, so this run is a RELOCATED one and the
        // checkable property is consistency, not byte-identity -- byte-identity at the baked
        // placement is arm (1)'s, and in the live image it is the parity tool's.
        ck(r.relocated, "...and the applier reports the placement it got, not the one it asked for");
        ck(M::SITES[0].va + 5 + (uint32_t)rel32_at(g_img + (M::SITES[0].va - IMG_LO), 1) == r.actual_base[0],
           "...the written site JMP points at where the cave really is");
        ck(image_clean_outside_site(), "...and NOTHING outside the site was touched");
        ck(memcmp(g_cave, M::SECTIONS[0].data, 58) == 0,
           "...the cave holds its data, everything up to the trailing JMP unchanged");
        ck(r.actual_base[0] + M::SECTIONS[0].data_len +
                   (uint32_t)rel32_at(g_cave, M::SECTIONS[0].data_len - 4) ==
               0x004C3848u,
           "...and its trailing JMP still rejoins the game at 0x004c3848");
        bool tail_zero = true;
        for (uint32_t k = M::SECTIONS[0].data_len; k < M::SECTIONS[0].vsize; ++k)
            if (g_cave[k]) tail_zero = false;
        ck(tail_zero, "...and the cave's virtual tail is zero, as a loader-filled section's would be");
        ck(g_n_release == 0, "...nothing was released on the success path");
    }

    // ---- (4) THE NEGATIVE CASE: ONE GUARDED BYTE MOVED.
    //
    // This is the arm the done_when names. The mismatch is put at the LAST byte of the expectation
    // on purpose: a guard that compared only the first byte (or only the opcode) would pass, and
    // that is the plausible way to get this wrong.
    reset_counters();
    arm_image();
    g_img[M::SITES[0].va - IMG_LO + M::SITES[0].len - 1] ^= 0x01;
    {
        ck(verify_sites(m) == 0, "a moved guarded byte is REPORTED by verify_sites, by site index");
        const apply_result r = apply(m);
        ck(!r.applied, "...and the manifest is refused");
        ck(r.refusal != nullptr && strstr(r.refusal, "expected-bytes") != nullptr,
           "...naming the expected-bytes guard, not some later failure");
        ck(r.refused_site == 0, "...and naming WHICH site");
        ck(g_img[M::SITES[0].va - IMG_LO] == M::SITES[0].expect[0] && g_img[M::SITES[0].va - IMG_LO + 1] != 0xB1,
           "...the site was NOT written");
        ck(image_clean_outside_site(), "...nothing else was written either");
        ck(g_n_reserve == 0, "...and the cave was never even allocated -- the guard runs FIRST");
    }

    // The first byte alone moving must refuse too: the site's own opcode is the byte most likely to
    // be already-patched, and "already patched" is exactly the state this guard exists to detect.
    reset_counters();
    arm_image();
    memcpy(g_img + (M::SITES[0].va - IMG_LO), M::SITES[0].repl, M::SITES[0].len); // pretend mhpatch ran
    {
        const apply_result r = apply(m);
        ck(!r.applied && r.refused_site == 0,
           "an image that ALREADY carries the static patch is refused, not double-applied");
    }

    // ---- (5) THE WILDCARD IS REAL. mhpatch's `xx` means "do not compare"; a guard that compared it
    // anyway would silently refuse every manifest that uses one (the caphike family does).
    {
        uint8_t exp[5], msk[5];
        memcpy(exp, M::SITES[0].expect, 5);
        memcpy(msk, M::SITES[0].mask, 5);
        msk[4]           = 0x00; // the last byte becomes a wildcard
        inmem_site     s = {M::SITES[0].va, exp, msk, M::SITES[0].repl, 5};
        inmem_manifest w = m;
        w.sites          = &s;
        w.site_count     = 1;
        reset_counters();
        arm_image();
        g_img[s.va - IMG_LO + 4] = 0x77; // differs, but under a wildcard
        ck(verify_sites(w) == -1, "a byte under an `xx` wildcard does not have to match");
        const apply_result r = apply(w);
        ck(r.applied, "...and the manifest applies");
    }

    // ---- (5b) F4C-COMP: THE abs32_to_section REF KIND.
    //
    // The kind the caphike/relocate family is made of -- 99.9% of the corpus's 46,294 sites. It is
    // driven off a SYNTHETIC manifest rather than the compiled-in one because no_cd_EN contains no
    // absolute (MANIFESTS stays at one entry until F4C-GATE), and the arithmetic has to be asserted
    // before a 7,770-site manifest is allowed to depend on it.
    //
    // THE POINT OF THE NEGATIVE HALF: a rel32 and an abs32 at the same offset produce two DIFFERENT
    // dwords, and each looks entirely plausible on its own. The one way to get this wrong is to
    // route the new kind through relocate_blob's existing rel32 arithmetic, which would write
    // `base + target - (blob_va + off + 4)` -- so that exact value is computed here and REQUIRED to
    // be absent.
    {
        // A 4-byte whole-blob absolute: the bytes ARE the pointer (0x010f0079 = cave base + 0x79).
        static const uint8_t    A_EXPECT[] = {0x11, 0xdc, 0xe1, 0x00};
        static const uint8_t    A_MASK[]   = {0xff, 0xff, 0xff, 0xff};
        static const uint8_t    A_REPL[]   = {0x79, 0x00, 0x0f, 0x01};
        static const inmem_site A_SITES[]  = {{0x004C3710u, A_EXPECT, A_MASK, A_REPL, 4u}};
        static const inmem_ref  A_REFS[]   = {{0, 0, 0u, ref_kind::abs32_to_section, 0, 0x79u}};
        inmem_manifest          a          = m;
        a.sites                            = A_SITES;
        a.site_count                       = 1;
        a.refs                             = A_REFS;
        a.ref_count                        = 1;
        a.bodies                           = nullptr;
        a.body_count                       = 0;

        const uint32_t baked[4] = {M::SECTIONS[0].preferred_va, 0, 0, 0};
        uint8_t        out[8]   = {};
        ck(relocate_site(a, 0, baked, out), "an abs32_to_section site relocates");
        ck(memcmp(out, A_REPL, 4) == 0,
           "at the BAKED base the abs32 bytes are mhpatch's bytes, unchanged");

        const uint32_t moved[4] = {0x20000000u, 0, 0, 0};
        ck(relocate_site(a, 0, moved, out), "...and at a moved base too");
        uint32_t got = 0;
        memcpy(&got, out, 4);
        ck(got == moved[0] + 0x79u,
           "an abs32_to_section ref writes base + target as an ABSOLUTE, little-endian");
        const uint32_t as_rel = moved[0] + 0x79u - (A_SITES[0].va + 0u + 4u);
        ck(got != as_rel,
           "...and NOT the displacement the two rel32 kinds would have written (the wrong-base "
           "negative: routing the new kind through the old arithmetic is the plausible way to "
           "get this wrong, and it produces a number that looks just as reasonable)");

        // A ref at a nonzero offset -- the operand-embedded shape, where the opcode IS in the blob.
        static const uint8_t    B_EXPECT[] = {0xa1, 0x11, 0xdc, 0xe1, 0x00, 0x90};
        static const uint8_t    B_MASK[]   = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
        static const uint8_t    B_REPL[]   = {0xa1, 0x79, 0x00, 0x0f, 0x01, 0x90};
        static const inmem_site B_SITES[]  = {{0x004C3720u, B_EXPECT, B_MASK, B_REPL, 6u}};
        static const inmem_ref  B_REFS[]   = {{0, 0, 1u, ref_kind::abs32_to_section, 0, 0x79u}};
        inmem_manifest          b          = a;
        b.sites                            = B_SITES;
        b.refs                             = B_REFS;
        ck(relocate_site(b, 0, moved, out), "an operand-embedded abs32 relocates");
        memcpy(&got, out + 1, 4);
        ck(got == moved[0] + 0x79u && out[0] == 0xA1 && out[5] == 0x90,
           "...its dword follows the base and the opcode bytes around it are untouched");
    }

    // ---- (5c) F4C-COMP: inmem_ref::owner IS SIXTEEN BITS.
    //
    // The caphike family reaches site index 7,769. At uint8_t, ref.owner == 300 matched site 44 --
    // silently, writing a relocation into another site's bytes. This arm is the width, asserted as
    // BEHAVIOUR: 300 sites, a ref owned by the LAST one, and the aliased site must be untouched.
    {
        constexpr int     N = 301;
        static uint8_t    expect[N][4], mask[N][4], repl[N][4];
        static inmem_site many[N];
        for (int i = 0; i < N; ++i) {
            for (int k = 0; k < 4; ++k) {
                expect[i][k] = (uint8_t)(0x11 + k);
                mask[i][k]   = 0xff;
                repl[i][k]   = 0x00;
            }
            many[i] = {0x004C3700u + (uint32_t)i * 4u, expect[i], mask[i], repl[i], 4u};
        }
        static const inmem_ref far_ref[] = {{0, 300, 0u, ref_kind::abs32_to_section, 0, 0x11u}};
        inmem_manifest         w         = m;
        w.sites                          = many;
        w.site_count                     = N;
        w.refs                           = far_ref;
        w.ref_count                      = 1;
        w.bodies                         = nullptr;
        w.body_count                     = 0;

        const uint32_t base[4] = {0x20000000u, 0, 0, 0};
        uint8_t        out[8]  = {};
        uint32_t       got     = 0;
        ck(relocate_site(w, 300, base, out), "site 300 relocates");
        memcpy(&got, out, 4);
        ck(got == base[0] + 0x11u, "a ref whose owner is 300 reaches SITE 300");
        memset(out, 0, sizeof(out));
        ck(relocate_site(w, 300 & 0xFF, base, out), "site 44 relocates");
        memcpy(&got, out, 4);
        ck(got == 0u,
           "...and site 44 -- what `owner` aliased to at uint8_t -- is left ALONE, which is the "
           "whole reason the field had to widen before the caphike family could compile in");
    }

    // ---- (6) C1: A SITE INSIDE A BODY THIS RUN PROMOTED.
    //
    // The reason this is checked BEFORE the bytes, and asserted as an ordering rather than merely as
    // a refusal: a promoted entry has an E9 over it, so it fails the byte compare too, and a checker
    // that asked bytes first would report every displaced static patch as a wrong build. That
    // inversion is a DISPLACED FIX read as a wrong build, and hook/detour.h's detour_refusal already orders its three
    // reasons for it.
    {
        using mh::hook::note_promoted;
        using mh::hook::owner_range;
        using mh::hook::set_owner_table;
        static const owner_range RANGES[] = {{"llm_cd_locate_and_open_audio", 0x004C371Bu, 0x004C384Fu, "cd"}};
        set_owner_table(RANGES, 1);
        reset_counters();
        arm_image();
        ck(apply(m).applied, "before the promotion the same manifest still applies");

        note_promoted(0x004C371Bu);
        reset_counters();
        arm_image();
        const apply_result r = apply(m);
        ck(!r.applied, "a site inside a PROMOTED body is refused");
        ck(r.refusal != nullptr && strstr(r.refusal, "promoted") != nullptr,
           "...and the refusal says PROMOTED, so the fix is known to need carrying in our body");
        ck(site_still_original() && image_clean_outside_site(), "...having written nothing");
        ck(g_n_reserve == 0, "...and allocated nothing");
        // F4C-COMP, and this is the done_when clause: BY NAME. The site is at entry+0x2f, not at
        // the entry, so an interlock keyed on entry equality would have let it through -- what
        // makes the refusal correct is that promoted_owner_of resolves the CONTAINING FUNCTION.
        ck(M::SITES[0].va != RANGES[0].entry && M::SITES[0].va > RANGES[0].entry &&
               M::SITES[0].va <= RANGES[0].end,
           "the planted site is INSIDE the promoted body and is not its entry");
        ck(logged("llm_cd_locate_and_open_audio"),
           "...and the refusal line NAMES the containing function, not just the address");
        ck(logged(m.name), "...and the manifest");

        // ORDERING, driven the only way it can be: make the bytes wrong TOO. The answer must still
        // be the promotion.
        arm_image();
        g_img[M::SITES[0].va - IMG_LO] = 0x90;
        const apply_result r2          = apply(m);
        ck(r2.refusal != nullptr && strstr(r2.refusal, "promoted") != nullptr,
           "a promoted site whose bytes ALSO mismatch reports the PROMOTION, not the bytes (G68)");
        // A promotion has no unnote in production, so the registry must be cleared or every arm
        // after this one is refused for a reason it was not testing.
        mh::hook::registry_reset_for_test();
        set_owner_table(nullptr, 0);
    }

    // ---- (7) THE CAVE VA IS A REQUEST, NOT A GUARANTEE.
    //
    // mhpatch asserts the section lands at the input's next-free VA; in memory that becomes
    // "VirtualAlloc must hand back exactly this address". It normally does -- non-ASLR image, one
    // page past the image end, DllMain before the process has allocated much -- but a manifest whose
    // relocation data is INCOMPLETE may not silently take a different address, because its cave's
    // internal rel32s would then point at the wrong place with nothing to say so.
    {
        inmem_manifest nofix = m;
        nofix.refs_complete  = false;
        reset_counters();
        arm_image();
        g_deny_exact         = true;
        const apply_result r = apply(nofix);
        ck(!r.applied, "a non-relocatable manifest is refused when its baked cave VA is unavailable");
        ck(r.refusal != nullptr && strstr(r.refusal, "not relocatable") != nullptr,
           "...and says so, rather than blaming the bytes");
        ck(g_n_reserve == 1, "...without trying the anywhere-fallback");
        ck(site_still_original(), "...and without writing the site");
    }
    {
        reset_counters();
        arm_image();
        g_deny_exact         = true; // relocatable: the fallback is allowed and the refs carry the move
        const apply_result r = apply(m);
        ck(r.applied && r.relocated, "a RELOCATABLE manifest accepts a different cave address");
        ck(g_n_reserve == 2, "...having asked for the exact VA first");
        const uint32_t base = r.actual_base[0];
        ck(M::SITES[0].va + 5 + (uint32_t)rel32_at(g_img + (M::SITES[0].va - IMG_LO), 1) == base,
           "...the written site JMP points at where the cave REALLY is");
        ck(base + M::SECTIONS[0].data_len + (uint32_t)rel32_at(g_cave, M::SECTIONS[0].data_len - 4) ==
               0x004C3848u,
           "...and the cave's JMP back still lands on the same game address");
    }
    {
        reset_counters();
        arm_image();
        g_deny_exact = g_deny_any = true;
        const apply_result r      = apply(m);
        ck(!r.applied && !g_cave_live, "no cave at all -> refused, with nothing left allocated");
        ck(site_still_original() && image_clean_outside_site(), "...and nothing written");
    }

    // ---- (8) F4C-COMP: THE REGISTRATION, AND WHAT IT REFUSES AFTERWARDS.
    //
    // Q6's composition rule. The patcher arms BEFORE every detour (G68's order, MH_Core_Arm), so the
    // promoted case above is the only one it can refuse on its own; everything else has not happened
    // yet. Registration is how the LATER claimant learns, and the two answers below are different
    // ON PURPOSE -- which is the measured part of this item, not a preference:
    //
    //   a JMP install (and every promotion, which is one) redirects the entry, so the whole body is
    //   dead and the manifest's bytes with it            -> REFUSED
    //   a TRAMPOLINE steals 8 entry bytes and the body runs on, so a site past them is untouched
    //                                                    -> ALLOWED, registration standing
    //
    // The second is not a hypothetical: no_cd_EN patches +0x2f into llm_cd_locate_and_open_audio
    // while seams/standalone.cpp trampolines that same entry, and they have composed correctly since
    // F1E. A body-wide refusal would disarm the no-CD fallback to protect bytes it never touches.
    {
        using namespace mh::hook;
        registry_reset_for_test();
        reset_counters();
        arm_image();
        const apply_result r = apply(m);
        ck(r.applied, "the manifest applies, so its bodies may be registered");
        ck(m.body_count == 1 && patched_body_count() == 1,
           "one body registered -- the generator joined the site against en_functions.json");

        const char *fn   = nullptr;
        const char *mani = patched_body_of(M::SITES[0].va, &fn);
        ck(mani != nullptr && strcmp(mani, m.name) == 0,
           "the registry names the MANIFEST holding the site's bytes");
        ck(fn != nullptr && strcmp(fn, m.bodies[0].name) == 0, "...and the containing FUNCTION");
        ck(patched_body_of(m.bodies[0].entry) != nullptr && patched_body_of(m.bodies[0].end) != nullptr,
           "the whole body is covered, entry and inclusive end alike");
        ck(patched_body_of(m.bodies[0].end + 1) == nullptr,
           "...and one byte past it is not -- the registry is an extent, not a neighbourhood");

        const uint32_t entry = m.bodies[0].entry;
        ck(detour_refusal(entry, entry_claim::exclusive, 0, /*body_dies=*/true) ==
               refuse_reason::patched,
           "a JMP install over that entry is REFUSED: it would make every patched byte dead");
        ck(detour_refusal(entry, entry_claim::rebind, 0, /*body_dies=*/true) == refuse_reason::patched,
           "...and entry_claim::rebind does NOT exempt it -- a promotion is a rebind claim, and a "
           "promotion is the most destructive thing that can happen to a static patch");
        ck(detour_refusal(entry, entry_claim::exclusive, 0, /*body_dies=*/false) == refuse_reason::none,
           "a TRAMPOLINE over the same entry is ALLOWED: the site is at +0x2f, past the 8 stolen "
           "bytes, and the body still runs (the measured no_cd_EN / standalone.cpp composition)");
        ck(M::SITES[0].va >= entry + (uint32_t)entry_window_bytes(),
           "...and that is true because of WHERE the site is, asserted rather than assumed");
        ck(patched_entry_window_of(entry) == nullptr, "the entry window is uncontested");

        // A site INSIDE the window is the other half, and without it the arm above only proves the
        // trampoline answer is "always yes".
        static const uint8_t    W_EXPECT[] = {0x55, 0x89, 0xe5, 0x83};
        static const uint8_t    W_MASK[]   = {0xff, 0xff, 0xff, 0xff};
        static const uint8_t    W_REPL[]   = {0x90, 0x90, 0x90, 0x90};
        static const inmem_site W_SITES[]  = {{0x004C371Bu + 2u, W_EXPECT, W_MASK, W_REPL, 4u}};
        static const inmem_body W_BODIES[] = {{"llm_cd_locate_and_open_audio", 0x004C371Bu, 0x004C385Bu}};
        inmem_manifest          w          = m;
        w.name                             = "a_window_manifest";
        w.sites                            = W_SITES;
        w.site_count                       = 1;
        w.bodies                           = W_BODIES;
        w.body_count                       = 1;
        w.sections                         = nullptr;
        w.section_count                    = 0;
        w.refs                             = nullptr;
        w.ref_count                        = 0;
        registry_reset_for_test();
        memset(g_img, 0xCC, sizeof(g_img));
        memcpy(g_img + (W_SITES[0].va - IMG_LO), W_EXPECT, 4);
        ck(apply(w).applied, "a manifest patching the first bytes of a body applies");
        ck(patched_entry_window_of(0x004C371Bu) != nullptr,
           "...and its body registers with the ENTRY WINDOW contested");
        ck(detour_refusal(0x004C371Bu, entry_claim::exclusive, 0, /*body_dies=*/false) ==
               refuse_reason::patched,
           "so NOW a trampoline over that entry is refused too -- the stolen bytes are the patch");
        ck(detour_refusal(0x004C3800u, entry_claim::exclusive, 0, /*body_dies=*/false) ==
               refuse_reason::none,
           "...while an unrelated entry is unaffected");

        // A REFUSED manifest must register NOTHING: refusing later claimants on behalf of bytes
        // that were never written is the same silence as not refusing on behalf of bytes that were.
        registry_reset_for_test();
        memset(g_img, 0xCC, sizeof(g_img)); // the guard now mismatches
        ck(!apply(w).applied, "a manifest whose guard fails is refused");
        ck(patched_body_count() == 0, "...and registers NOTHING");
        registry_reset_for_test();
    }

    // ---- (9) F4C-GATE: THE COMPILE LIST, AT ITS REAL WIDTH.
    //
    // Everything above drives ONE small fixture. The set mh.dll actually carries is three EN
    // caphike/relocate manifests -- up to 7,770 sites, 4,260 refs, 530 bodies and two sections -- and
    // the applier's caps are all of the silent-until-tripped kind: a section past the fourth refuses
    // outright, a site longer than the staging buffer refuses AFTER the cave is placed, an owner
    // index past the blob count writes a relocation into someone else's bytes. None of those can be
    // provoked by the fixture, and a lane would only show the refusal, not which cap.
    //
    // This is the OFF-RIG half of "the arms hold at the new width". The other half is the parity
    // oracle, which compares each of these against mhpatch's own output (offline in lint, live on
    // the rig) -- here we only assert that what was compiled in is INSIDE the applier's limits.
    {
        ck(mh::patch::COMPILED_COUNT >= 1, "at least one manifest is compiled in");
        for (int i = 0; i < mh::patch::COMPILED_COUNT; ++i) {
            const compiled_manifest &e  = mh::patch::COMPILED[i];
            const inmem_manifest    &cm = *e.m;
            ck(strcmp(e.name, cm.name) == 0, "the index name matches the manifest's own");
            for (int k = 0; k < i; ++k)
                ck(strcmp(mh::patch::COMPILED[k].name, e.name) != 0,
                   "...and no two compiled-in manifests share a name (the ini selects BY name)");
            // refs_complete is not decoration here: every one of these bakes its cave at the image's
            // next-free VA, which in a live process is already taken, so EVERY shipped manifest is
            // applied relocated or not at all.
            ck(cm.refs_complete, "a compiled-in manifest carries complete relocation metadata");
            ck(cm.section_count <= MAX_SECTIONS, "...its sections fit apply_result::actual_base");
            ck(cm.body_count <= mh::hook::MAX_PATCHED_BODIES,
               "...its bodies fit the C1 patched-body registry");
            bool len_ok = true, owner_ok = true;
            for (int s = 0; s < cm.site_count; ++s)
                if (cm.sites[s].len > (uint32_t)MAX_SITE_LEN) len_ok = false;
            for (int r = 0; r < cm.ref_count; ++r) {
                const inmem_ref &rr = cm.refs[r];
                const int        n  = rr.in_section ? cm.section_count : cm.site_count;
                if (rr.owner >= n) owner_ok = false;
                if (rr.kind != ref_kind::rel32_to_image && rr.target_section >= cm.section_count)
                    owner_ok = false;
            }
            ck(len_ok, "...every site fits the relocation staging buffer");
            ck(owner_ok,
               "...and every ref names a blob and a target section that EXIST -- the uint16_t owner "
               "field's whole point, at the 7,769 the caphike family actually reaches");
        }
    }


    // ====================================================================================================
    // ---- (10) PT-CAP8 SLICE C: the BATCHED applier, the TOGGLE layer and the OVERLAY ------------------
    // ====================================================================================================
    //
    // Over a wide fake host (see namespace `wide`): a heap window over the cap manifests' whole span.
    wide::img     = static_cast<uint8_t *>(malloc(wide::SIZE));
    uint8_t *snap = static_cast<uint8_t *>(malloc(wide::SIZE));
    set_host(&wide::HOST);
    reset_applied_for_test();
    mh::hook::registry_reset_for_test();

    // ---- (10a) BATCHING IS A MEASUREMENT: 400 sites in four pages -> four runs -> 2 x runs protects ----
    {
        synth::build();
        wide::reset();
        wide::seed(synth::man);
        const apply_result r = apply(synth::man);
        ck(r.applied && r.refusal == nullptr, "(10a) a 400-site manifest over four pages applies");
        ck(r.protect_runs == 4, "...as exactly FOUR page runs, not 400 sites");
        ck(wide::g_ncalls <= 2 * r.protect_runs && wide::g_ncalls == 8,
           "...with 2 protect calls per run (open RWX + restore) -- 8 in all, where the per-site applier spent 1,600");
        bool all = true;
        for (int i = 0; i < synth::N; ++i)
            all = all && memcmp(wide::at(synth::sites[i].va), synth::repl_b[i], 4) == 0;
        ck(all, "...and every one of the 400 sites holds its replacement");
        bool restored = true;
        for (int i = 4; i < 8; ++i) restored = restored && wide::g_calls[i].prot == 0x20u; // the restores put back what query()/protect() reported
        ck(restored, "...and the runs were restored to the protection they had");
        ck(r.micros < 5u * 1000u * 1000u, "...and apply() reports its own wall time (micros) -- a sanity bound, not a budget");
        reset_applied_for_test();
    }

    // ---- (10b) ALL-OR-NOTHING ACROSS RUNS: the third run cannot be opened ------------------------------
    {
        wide::reset();
        wide::seed(synth::man);
        memcpy(snap, wide::img, wide::SIZE);
        wide::g_fail_at      = 2; // the third image-range protect call = opening run 3
        const apply_result r = apply(synth::man);
        ck(!r.applied && r.refusal != nullptr && strstr(r.refusal, "protect") != nullptr,
           "(10b) a run that cannot be made writable refuses the whole manifest");
        ck(r.refused_site >= 200 && r.refused_site < 300, "...naming a site that lives in the failing (third) run");
        ck(wide::image_equals(snap), "...and NOT ONE byte was written, including the two runs that opened fine");
        ck(wide::g_ncalls == 5 && wide::g_calls[3].prot == 0x20u && wide::g_calls[4].prot == 0x20u,
           "...and the two runs it had opened were put back (two restore calls follow the failed one)");
        ck(logged("cannot be made writable"), "...with the refusal line saying why");
        reset_applied_for_test();
    }

    // ---- (10c) PAGE RUNS SPLIT WHERE THE PROTECTION CHANGES --------------------------------------------
    {
        // Two adjacent pages, one site each, a protection boundary between them: 2 runs (not 1), each
        // restored to ITS OWN protection (0x20 below the boundary, 0x04 above).
        static const uint8_t    E[] = {1, 2, 3, 4};
        static const uint8_t    M[] = {255, 255, 255, 255};
        static const uint8_t    R[] = {9, 9, 9, 9};
        static const inmem_site S[] = {{0x00480ffcu, E, M, R, 4u}, {0x00481004u, E, M, R, 4u}};
        inmem_manifest          m   = {"two_pages", "0", nullptr, 0, S, 2, nullptr, 0, nullptr, 0, true, false};
        wide::reset();
        wide::seed(m);
        wide::g_split_va     = 0x00481000u;
        const apply_result r = apply(m);
        ck(r.applied && r.protect_runs == 2, "(10c) two adjacent pages with a protection boundary between them are TWO runs");
        ck(wide::g_ncalls == 4 && wide::g_calls[2].prot == 0x20u && wide::g_calls[3].prot == 0x04u,
           "...and each is restored to the protection ITS pages had (R-X below the boundary, RW above) -- one value would have broken one of them");
        reset_applied_for_test();
    }

    // ---- (10d) THE REAL RELOCATION MANIFEST, AT SCALE ---------------------------------------------------
    {
        const inmem_manifest &m = *CAPS_RELOC;
        wide::reset();
        wide::seed(m);
        const apply_result r = apply(m, nullptr, APPLY_QUIET_OK);
        ck(r.applied && r.refusal == nullptr, "(10d) the real cap_v022_reloc_EN manifest applies over the fake image");
        ck(r.relocated && r.actual_base[0] != 0 && r.actual_base[1] != 0 && wide::g_nsec == 2,
           "...placing both of its sections (the fake host cannot hand back 0x010f0000, so it is a relocated run)");
        ck(r.protect_runs > 0 && r.protect_runs <= 100 && wide::g_ncalls == 2 * r.protect_runs,
           "...in at most 99 page runs with 2 protects each -- where the old per-site path made ~20,000 syscalls");
        bool bytes_ok = true;
        for (int i = 0; i < m.site_count; ++i) {
            uint8_t want[MAX_SITE_LEN];
            relocate_site(m, i, r.actual_base, want);
            if (memcmp(wide::at(m.sites[i].va), want, m.sites[i].len) != 0) {
                bytes_ok = false;
                break;
            }
        }
        ck(bytes_ok, "...and every one of its 3,941 sites holds exactly relocate_site()'s bytes at the placement it got");
        ck(m.sites[0].va == 0x0041129bu && m.sites[m.site_count - 1].va == 0x004ee14bu && wide::LO <= 0x0041129bu,
           "...over the 0x41129b..0x4ee14b span the planted window covers");

        // ---- (10e) THE OVERLAY returns what the applier WROTE ----
        {
            const int         k = 1000; // some site in the middle
            const inmem_site &s = m.sites[k];
            uint8_t           buf[MAX_SITE_LEN + 8];
            memset(buf, 0xAA, sizeof(buf));
            overlay_applied(s.va - 2, s.len + 4, buf);
            uint8_t want[MAX_SITE_LEN];
            relocate_site(m, k, r.actual_base, want);
            bool site_part = memcmp(buf + 2, want, s.len) == 0;
            ck(site_part, "(10e) overlay_applied copies the bytes the applier wrote over the site");
            // bytes outside every site stay as the caller had them
            uint32_t va0 = 0;
            for (int i = 0; i + 1 < m.site_count; ++i)
                if (m.sites[i + 1].va > m.sites[i].va + m.sites[i].len + 8u) {
                    va0 = m.sites[i].va + m.sites[i].len + 2u;
                    break;
                }
            uint8_t gap[4] = {0xAA, 0xAA, 0xAA, 0xAA};
            overlay_applied(va0, 4, gap);
            ck(gap[0] == 0xAA && gap[1] == 0xAA && gap[2] == 0xAA && gap[3] == 0xAA,
               "...and a span between sites is left exactly as the caller had it");
            // a partial overlap: the last byte of one site and the bytes after it
            const inmem_site &s2 = m.sites[500];
            uint8_t           part[4];
            memset(part, 0x55, sizeof(part));
            overlay_applied(s2.va + s2.len - 1, 4, part);
            uint8_t w2[MAX_SITE_LEN];
            relocate_site(m, 500, r.actual_base, w2);
            ck(part[0] == w2[s2.len - 1], "...a partial overlap copies exactly the overlapping byte(s)");
        }
        wide::free_sections();
        reset_applied_for_test();
    }

    // ---- (10f) THE TOGGLE LAYER over the REAL limit manifest ---------------------------------------------
    {
        const inmem_manifest &m = *CAPS_LIMITS;
        wide::reset();
        wide::seed(m);
        memcpy(snap, wide::img, wide::SIZE);
        static toggle_state ts;
        memset(&ts, 0, sizeof(ts));
        ck(m.toggle, "(10f) the limit manifest is a toggle manifest");
        {
            const apply_result nots = apply(m);
            ck(!nots.applied && nots.refusal != nullptr && strstr(nots.refusal, "toggle_state") != nullptr,
               "...and a toggle manifest REFUSES to apply without a toggle_state");
        }
        const apply_result r = apply(m, &ts, APPLY_QUIET_OK);
        ck(r.applied && ts.staged && !ts.on, "...applies: placed, verified, staged, OFF");
        ck(wide::image_equals(snap), "...and wrote NO site -- the image is byte-for-byte the vanilla one");
        ck(toggle_current(ts) == 0, "...toggle_current() reads every site as vanilla");

        ck(toggle(ts, true), "(10g) toggle ON succeeds");
        ck(ts.on && toggle_current(ts) == 1, "...every site is now its raised image");
        bool on_ok = true;
        for (int i = 0; i < m.site_count; ++i) on_ok = on_ok && memcmp(wide::at(m.sites[i].va), ts.on_bytes[i], m.sites[i].len) == 0;
        ck(on_ok, "...byte-for-byte the relocated `repl` the manifest carries");
        ck(*wide::at(0x004562e3u) == 0x7f, "...a loop-bound imm8 (map_FillDefaults 0x4562e3) is 0x7f");
        ck(*wide::at(0x00412441u) == 0xE9, "...a widened bound (0x412441) is a JMP into the cave");
        {
            int32_t rel32;
            memcpy(&rel32, wide::at(0x00412442u), 4);
            ck(0x00412441u + 5u + (uint32_t)rel32 == r.actual_base[0], "...whose target is the limit section's first cave, at the placement it got");
        }
        ck(toggle(ts, true), "toggling ON when ON is a successful no-op");

        ck(toggle(ts, false), "toggle OFF succeeds");
        ck(!ts.on && toggle_current(ts) == 0, "...every site is vanilla again");
        ck(wide::image_equals(snap), "...and the image is BYTE-IDENTICAL to the vanilla one it started as (OFF -> ON -> OFF restores exact `expect`)");

        // ---- (10h) a FOREIGN byte refuses, writing nothing ----
        ck(toggle(ts, true), "(10h) back ON for the foreign-byte arm");
        uint8_t *foreign = wide::at(m.sites[5].va);
        foreign[0] ^= 0xFF; // neither the vanilla nor the raised image
        memcpy(snap, wide::img, wide::SIZE);
        int         bad = -1;
        const char *why = nullptr;
        ck(!toggle(ts, false, &bad, &why), "a site holding neither image REFUSES the toggle");
        ck(bad == 5 && why != nullptr && strstr(why, "foreign") != nullptr, "...naming the site and the cause");
        ck(wide::image_equals(snap), "...and wrote nothing (the other 50 sites stay raised)");
        ck(logged("site 5") && logged(m.name), "...and the log line names the site and the manifest");
        foreign[0] ^= 0xFF;
        ck(toggle(ts, false), "...and once the foreign writer is gone the toggle works");
        wide::free_sections();
    }

    // ---- (10f2) PT-CAP9-U5: the ORDER layer is a toggle over the real manifest too ----------------------------
    {
        const inmem_manifest &m = *CAPS_ORDERS;
        wide::reset();
        wide::seed(m);
        memcpy(snap, wide::img, wide::SIZE);
        static toggle_state ts;
        memset(&ts, 0, sizeof(ts));
        ck(m.toggle && m.site_count <= MAX_TOGGLE_SITES && m.section_count == 2, "(10f2) the order manifest is a toggle with two sections that fits toggle_state");
        const apply_result r = apply(m, &ts, APPLY_QUIET_OK);
        ck(r.applied && ts.staged && !ts.on && wide::image_equals(snap), "...applies and stages WITHOUT writing a single site");
        ck(toggle(ts, true) && toggle_current(ts) == 1, "...toggle ON: every site is its raised image");
        // the wipe at 0x00466153 is now `inc dword [ovf_queue]` (ff 05 <abs32 into .mhcapo>), 4 nops after it
        const uint8_t *wp = wide::at(0x00466153u);
        uint32_t       cell;
        memcpy(&cell, wp + 2, 4);
        ck(wp[0] == 0xff && wp[1] == 0x05 && cell == r.actual_base[0] + 4u && wp[6] == 0x90 && wp[9] == 0x90, "...the enqueue WIPE became `inc dword [.mhcapo+4]` + nops (at the section's actual placement)");
        // the quota site: call into .mhcapr, then test eax,eax
        const uint8_t *qp = wide::at(0x00466232u);
        int32_t        rel;
        memcpy(&rel, qp + 1, 4);
        ck(qp[0] == 0xe8 && 0x00466232u + 5u + (uint32_t)rel == r.actual_base[1] && qp[5] == 0x85 && qp[6] == 0xc0, "...the sender bound became `call .mhcapq; test eax,eax`");
        // the thunk is `jmp dword ptr [.mhcapo+0]`
        uint32_t       tcell;
        const uint8_t *th = static_cast<const uint8_t *>(wide::g_sections[1]);
        memcpy(&tcell, th + 2, 4);
        ck(th[0] == 0xff && th[1] == 0x25 && tcell == r.actual_base[0], "...and the thunk jumps through the section's first dword");
        uint32_t lim;
        memcpy(&lim, wide::at(0x004660bbu), 4);
        ck(lim == 4096u, "...the queue bound is 4096");
        ck(toggle(ts, false) && toggle_current(ts) == 0 && wide::image_equals(snap), "...OFF restores the image BYTE-IDENTICALLY");
        wide::free_sections();
    }

    // ---- (10f3) PT-CAP9-U8b: WILDCARD bytes round-trip EXACTLY, for every toggle manifest -------------------
    // The units-limit manifest has sites whose mask is 0 on a byte the ON image rewrites (a rel32 displacement
    // inside a masked instruction). OFF used to write back only the mask-0xff bytes, so those bytes kept the ON
    // image and toggle_current() still read the site as OFF. The seed gives every wildcard byte a value that is
    // NOT the manifest's `expect` byte, as a real image does.
    {
        const inmem_manifest *const TM[] = {CAPS_LIMITS, CAPS_AI, CAPS_UNITS_LIMITS, CAPS_ORDERS};
        for (const inmem_manifest *tm : TM) {
            const inmem_manifest &m = *tm;
            wide::reset();
            wide::seed(m);
            int wild_sites = 0;
            for (int i = 0; i < m.site_count; ++i) {
                bool w = false;
                for (uint32_t b = 0; b < m.sites[i].len; ++b)
                    if (!m.sites[i].mask[b]) {
                        wide::at(m.sites[i].va)[b] = (uint8_t)((0x5A ^ (i * 7 + (int)b)) | 0x01); // arbitrary live byte
                        w                          = true;
                    }
                wild_sites += w ? 1 : 0;
            }
            memcpy(snap, wide::img, wide::SIZE);
            static toggle_state ts;
            memset(&ts, 0, sizeof(ts));
            const apply_result r = apply(m, &ts, APPLY_QUIET_OK);
            printf("   (10f3) %s: %d sites, %d with wildcard bytes\n", m.name, m.site_count, wild_sites);
            ck(r.applied && ts.staged && wide::image_equals(snap) && toggle_current(ts) == 0, "(10f3) a toggle manifest with live wildcard bytes applies, wrote nothing, reads OFF");
            ck(toggle(ts, true) && toggle_current(ts) == 1, "...ON");
            ck(toggle(ts, false) && toggle_current(ts) == 0 && wide::image_equals(snap), "...OFF restores the image BYTE-IDENTICALLY, wildcard bytes included");
            ck(toggle(ts, true) && toggle_current(ts) == 1 && toggle(ts, false) && wide::image_equals(snap), "...ON -> OFF -> ON -> OFF is stable");
            // a planted corrupt wildcard byte: toggle_current must not call the site OFF
            int      pw = -1;
            uint32_t wb = 0;
            for (int i = 0; i < m.site_count && pw < 0; ++i)
                for (uint32_t b = 0; b < m.sites[i].len; ++b)
                    if (!m.sites[i].mask[b]) {
                        pw = i;
                        wb = b;
                        break;
                    }
            if (pw >= 0) {
                wide::at(m.sites[pw].va)[wb] ^= 0x40;
                ck(toggle_current(ts) == -1, "...a corrupt WILDCARD byte is detected: toggle_current() is not OFF");
                ck(toggle(ts, false) && wide::image_equals(snap) && toggle_current(ts) == 0, "...and OFF restores the original byte");
            }
            wide::free_sections();
        }
    }

    // ---- (10i) the compile-list caps, for the two cap manifests --------------------------------------------
    {
        const inmem_manifest *const CM[] = {CAPS_RELOC, CAPS_LIMITS, CAPS_AI, CAPS_ORDERS};
        for (const inmem_manifest *cm : CM) {
            ck(cm->refs_complete, "(10i) a cap manifest carries complete relocation metadata");
            ck(cm->section_count <= MAX_SECTIONS && cm->body_count <= mh::hook::MAX_PATCHED_BODIES, "...fits the applier's sections and the C1 registry");
            bool len_ok = true, sorted = true, owner_ok = true;
            for (int s = 0; s < cm->site_count; ++s) {
                if (cm->sites[s].len > (uint32_t)MAX_SITE_LEN) len_ok = false;
                if (s && cm->sites[s].va <= cm->sites[s - 1].va + cm->sites[s - 1].len - 1u) sorted = false;
            }
            for (int r = 0; r < cm->ref_count; ++r) {
                const inmem_ref &rr = cm->refs[r];
                const int        n  = rr.in_section ? cm->section_count : cm->site_count;
                if (rr.owner >= n) owner_ok = false;
                if (rr.kind != ref_kind::rel32_to_image && rr.target_section >= cm->section_count) owner_ok = false;
            }
            ck(len_ok && sorted && owner_ok, "...sites <= 64 bytes, strictly ascending and non-overlapping (the overlay binary-searches them), every ref names a blob that exists");
        }
        ck(CAPS_LIMITS->site_count <= MAX_TOGGLE_SITES, "...the limit manifest fits toggle_state's site table");
        bool tlen = true;
        for (int s = 0; s < CAPS_LIMITS->site_count; ++s) tlen = tlen && CAPS_LIMITS->sites[s].len <= (uint32_t)MAX_TOGGLE_LEN;
        ck(tlen, "...and its longest site fits toggle_state's per-site staging");
        ck(!CAPS_RELOC->toggle && CAPS_LIMITS->toggle, "...the reloc layer is written at boot, the limit layer only on a toggle");
        ck(CAPS_AI->toggle && CAPS_AI->site_count <= MAX_TOGGLE_SITES, "...the AI scratch layer is a toggle that fits toggle_state's site table (PT-CAP11)");
        bool ai_len = true;
        for (int s = 0; s < CAPS_AI->site_count; ++s) ai_len = ai_len && CAPS_AI->sites[s].len <= (uint32_t)MAX_TOGGLE_LEN;
        ck(ai_len, "...and its sites fit the per-site staging");
    }

    // ---- (10j) the overlay on a SMALL manifest: relocated bytes, binary search, the unsorted fallback -----
    {
        static const uint8_t       E[]   = {0x11, 0xdc, 0xe1, 0x00};
        static const uint8_t       M[]   = {0xff, 0xff, 0xff, 0xff};
        static const uint8_t       R[]   = {0x79, 0x00, 0x0f, 0x01};
        static const inmem_site    S[]   = {{0x00450010u, E, M, R, 4u}, {0x00450030u, E, M, R, 4u}};
        static const inmem_ref     F[]   = {{0, 0, 0u, ref_kind::abs32_to_section, 0, 0x79u}, {0, 1, 0u, ref_kind::abs32_to_section, 0, 0x7au}};
        static const inmem_section SEC[] = {{".t", 0x01100000u, 0x1000u, nullptr, 0u, 0xC0000040u}};
        inmem_manifest             m     = {"overlay_small", "0", SEC, 1, S, 2, F, 2, nullptr, 0, true, false};
        wide::reset();
        wide::seed(m);
        reset_applied_for_test();
        const apply_result r = apply(m, nullptr, APPLY_QUIET_OK);
        ck(r.applied, "(10j) a two-site manifest with a section applies");
        uint8_t buf[8];
        memset(buf, 0xAA, sizeof(buf));
        overlay_applied(0x00450032u, 6, buf); // overlaps the SECOND site's last two bytes
        uint32_t base_plus = r.actual_base[0] + 0x7au;
        uint8_t  wantb[4];
        memcpy(wantb, &base_plus, 4);
        ck(buf[0] == wantb[2] && buf[1] == wantb[3] && buf[2] == 0xAA, "...the overlay returns the RELOCATED bytes (base + target), not the manifest's baked ones");
        wide::free_sections();
        reset_applied_for_test();
    }

    free(wide::img);
    free(snap);
    wide::img = nullptr;
    set_host(&FAKE);
    mh::hook::registry_reset_for_test();

    set_host(nullptr);
    set_logger(nullptr);
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
