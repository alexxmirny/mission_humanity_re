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

    set_host(nullptr);
    set_logger(nullptr);
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
