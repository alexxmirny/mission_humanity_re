//
// patch/inmem_patch.h -- apply an mhpatch manifest to the LOADED image instead of to a file.
//
// F1E (the fork plan supplementary ruling). Today src/patcher/mhpatch.py rewrites mh.exe on
// disk: guarded byte writes plus an APPENDED SECTION that hosts a code cave or a relocated array.
// In the fork the distribution is a stock retail exe, so the same manifest has to be carried here
// and applied at DllMain: VirtualAlloc stands in for the appended section, and the guarded write is
// the one hook/patch.h already performs.
//
// THE THREE THINGS THAT MAKE THIS DIFFERENT FROM `memcpy` AND WHY EACH IS A STRUCTURE BELOW.
//
//  1. A CAVE HAS A BAKED VA. mhpatch's add_data_section asserts `vaddr == the input's next-free VA`,
//     and gen_no_cd_manifest.py assembles the cave's `jmp` back relative to it -- so a manifest is
//     valid for exactly ONE image layout. In memory the same constraint reappears as "VirtualAlloc
//     must hand back that exact address". It normally can (mh.exe is non-ASLR, the baked VA is one
//     page past the image end, and DllMain for a load-time dependency runs before the process has
//     allocated much), but "normally" is not a guard, so the applier asks for the exact VA, and only
//     accepts a different one if the manifest's relocation data is COMPLETE (see 2).
//
//  2. WHICH DWORDS ARE RELATIVE IS NOT IN THE MANIFEST. The JSON holds bytes; it does not say that
//     dword +59 of the cave is a rel32 to 0x004c3848 and dword +1 of the patch is a rel32 to the
//     cave. tools/gen_inmem_manifest.py recovers that by DECODING both blobs with capstone and
//     emitting one `inmem_ref` per branch that leaves its blob. `refs_complete` is the generator's
//     proof that it decoded everything; without it a move is refused rather than guessed at.
//
//  3. NOTHING MAY BE HALF-APPLIED. A manifest is verified in full (every expected-bytes guard, every
//     section placement) before the first byte is written, and a placement made during a run that
//     then refuses is released. A half-applied cave manifest is a `jmp` into unmapped memory.
//
// The applier writes ONLY through the host table below, which production fills with the real Win32
// calls and the offline selftest fills with its own -- the same seam, and for the same reason, as
// hook/detour.h's page_ops: the interesting arms are refusals, and a refusal over a real game VA
// cannot be provoked on a machine where the write would succeed.
//
#pragma once
#include <cstdint>

namespace mh::patch {

// How a placement-sensitive dword inside a blob is re-derived when its blob moves.
enum class ref_kind : uint8_t {
    // target = sections[target_section].actual_base + target (a rel32 from a patch into a cave)
    rel32_to_section,
    // target = the absolute image VA in `target` (the cave's rel32 back into the game)
    rel32_to_image,
    // F4C-COMP. The four bytes at `off` are an ABSOLUTE VA naming declared section
    // `target_section` at byte offset `target`; at placement the applier writes
    // `actual_base[target_section] + target` there, little-endian. NOT a displacement from the
    // blob -- and that is the whole difference from the two above, which is why it needs its own
    // kind rather than a flag: the caphike/relocate family's sites are BARE 4-byte operands carved
    // out of instructions that start BEFORE the patch VA, so there is no instruction end to
    // subtract from. 4,076 of grand_all_caphike_storagecap_EN's 7,770 sites are exactly one of
    // these. Spec + the generator's totality proof: docs/inmem-patching.md.
    abs32_to_section,
};

struct inmem_ref {
    uint8_t in_section; // 1 = the dword lives in sections[owner].data; 0 = in sites[owner].repl
    // Index into sections[] or sites[]. SIXTEEN BITS, and the width is a measurement rather than a
    // round number: the caphike family reaches site index 7,769 (grand_all_caphike_storagecap_EN),
    // so at uint8_t every ref past site 255 silently named the WRONG BLOB -- a relocation written
    // into someone else's bytes with nothing to notice. It was uint8_t while exactly one
    // 1-site manifest compiled in; the generator refused to widen MANIFESTS past that entry until
    // this field could hold the value (gen_inmem_manifest.py's applier-prerequisite interlock,
    // which PARSES this declaration).
    uint16_t owner;
    uint16_t off; // byte offset of the sensitive dword within that blob
    ref_kind kind;
    uint8_t  target_section; // rel32_to_section / abs32_to_section only
    uint32_t target;         // section-relative offset, or an absolute image VA
};

// One ORIGINAL FUNCTION BODY that a manifest writes bytes inside (F4C-COMP / fork ruling Q6).
//
// WHY THE MANIFEST CARRIES THIS AT ALL, rather than the applier looking it up. A site is a bare VA;
// "which function contains it" is Ghidra's knowledge, and the DLL's only runtime extent table is
// mh::addr::promotable_ranges -- the 401 bodies the tree can PROMOTE, which is a different and much
// smaller set than the 2,892 the manifests actually land in. An applier restricted to that table
// could not NAME the containing function of most sites, and Q6's rule is that collisions are refused
// BY NAME. So the generator joins each site against tools/data/en_functions.json and emits the
// bodies here, exactly as gen_dll_patches.py already does for the promotable set.
//
// `end` is INCLUSIVE, matching mh::hook::owner_range so the two tables answer containment the same
// way. A site whose VA no function covers contributes no row (and is reported by the generator).
struct inmem_body {
    const char *name;
    uint32_t    entry;
    uint32_t    end;
};

// One appended section: a code cave, or the zero-init host for a relocated array.
struct inmem_section {
    const char    *name;
    uint32_t       preferred_va; // the VA the manifest was baked for
    uint32_t       vsize;
    const uint8_t *data;     // nullptr for a pure zero-init section
    uint32_t       data_len; // bytes of `data`; the rest of vsize is zero
    uint32_t       chars;    // the PE section characteristics, mapped to a page protection
};

// One guarded byte patch.
struct inmem_site {
    uint32_t       va;
    const uint8_t *expect; // original bytes
    const uint8_t *mask;   // 0xff = must match, 0x00 = mhpatch's `xx` wildcard
    const uint8_t *repl;
    uint32_t       len;
};

struct inmem_manifest {
    const char          *name;
    const char          *sha256; // the build the manifest was baked against; informational here
    const inmem_section *sections;
    int                  section_count;
    const inmem_site    *sites;
    int                  site_count;
    const inmem_ref     *refs;
    int                  ref_count;
    const inmem_body    *bodies; // the original bodies the sites land in (F4C-COMP)
    int                  body_count;
    bool                 refs_complete; // false => the cave may only be placed at its baked VA
    bool                 toggle;        // PT-CAP8: a caps_limits manifest -- verified and staged, written only on a toggle
};

// THE APPLIER'S TWO HARD CAPS. Public since fork F4C-GATE, because the compile list widened from
// one hand-picked manifest to a derived set and these decide whether a manifest can be applied at
// all: a section over the fourth is refused outright, and a site longer than the staging buffer
// refuses AFTER the cave is placed. Both are silent-until-tripped, so `net_selftest patchtest`
// asserts every compiled-in manifest against them rather than trusting that today's corpus fits.
// (Measured 2026-09-13: the shipped set's worst is 2 sections and a 13-byte site.)
constexpr int MAX_SECTIONS = 4;  // == apply_result::actual_base
constexpr int MAX_SITE_LEN = 64; // the staging buffer for one site's relocated bytes

// One entry of the COMPILE LIST -- the manifests built into this DLL, generated into
// patch/manifests.gen.h from every src/patcher manifest whose `class` is "shipped" (fork ruling Q7).
//
// THEY ARE ALTERNATIVES, NOT A BATCH, and the `[patch] manifest=<name>` key that selects one is the
// consequence rather than a convenience. Every manifest bakes its cave at the image's next-free VA
// and the caphike family's site sets nest (grand_all_* subsumes the pool manifests), so a second
// apply in one process would fail the first one's already-written bytes at the expected-bytes guard
// -- a correct refusal for a reason that reads like a build mismatch. One per run, named.
struct compiled_manifest {
    const char           *name; // the manifest's own name, as `[patch] manifest=` spells it
    const inmem_manifest *m;
};

// What one apply() did, in the detail the parity dump needs. `actual_base[i]` is where section i
// really landed (== preferred_va on the normal path).
struct apply_result {
    bool        applied;        // every site written
    const char *refusal;        // nullptr on success; otherwise why NOTHING was written
    int         refused_site;   // the site index a byte/ownership refusal names, else -1
    uint32_t    actual_base[4]; // per section
    bool        relocated;      // any section did not land on its preferred VA
    uint32_t    micros;         // PT-CAP8: wall time of the whole apply() in microseconds (QueryPerformanceCounter)
    int         protect_runs;   // PT-CAP8: page runs the batched write pass protected (0 for a toggle manifest)
    char        detail[240];    // PT-CAP8: the refusal's one-line detail (the line apply() logs unless APPLY_QUIET_OK), else empty
};

// The page/memory operations, injectable so the refusal arms can be driven off-rig. Production
// leaves this null, which selects the real Win32 calls.
struct patch_host {
    void *(*reserve_at)(uint32_t va, uint32_t size); // exact-VA reserve+commit; null if unavailable
    void *(*reserve_any)(uint32_t size);             // anywhere; used only when refs_complete
    void (*release)(void *p, uint32_t size);
    bool (*protect)(void *p, uint32_t size, uint32_t want, uint32_t *old_out);
    uint8_t *(*at)(uint32_t va); // image VA -> writable pointer. Production: identity.
    // PT-CAP8 (optional -- null means "assume a page run is uniform"): the page protection of the
    // region that starts at `p` and the first byte past the uniform region. The batched write pass
    // merges adjacent pages into runs and must restore EACH page to the protection it had; a run that
    // crosses from .text (R-X) into .data (RW-) cannot be restored from one VirtualProtect `old` value.
    bool (*query)(const void *p, uint32_t *region_end, uint32_t *prot);
};
void set_host(const patch_host *h);

// Route the applier's diagnostics somewhere. Production points this at seam_log.
void set_logger(void (*fn)(const char *));

// Apply `m` to the loaded image. ALL-OR-NOTHING: a failed expected-bytes guard, a contested site
// (hook/patch.h's C1 interlock -- a body we promoted), or a cave that cannot be placed leaves the
// image exactly as it was, with `refusal` naming which. Idempotent in the useful direction: run
// against an image that already carries the patch and the guard mismatches, so it refuses.
//
// ON SUCCESS IT REGISTERS (F4C-COMP / Q6). Every body in `m.bodies` is recorded with the C1
// interlock (mh::hook::note_patched_body), together with whether any site of this manifest lands in
// the ENTRY WINDOW those bodies' first bytes occupy. The patcher arms BEFORE every detour and byte
// patch (G68's order, MH_Core_Arm), so registration is the only way a LATER claimant can learn that
// bytes in the body it is about to take are already carried by a manifest -- and it is what lets
// hook/detour.cpp refuse that claimant BY NAME instead of by a byte compare that can only say
// "wrong build". The reverse direction needs nothing new: a body promoted BEFORE the patcher runs is
// already refused above.
apply_result apply(const inmem_manifest &m);

// Verify, without writing: every site's current bytes against its expect/mask. Exposed because the
// refusal is the interesting half and a test must be able to ask for it without a target to corrupt.
// Returns -1 if all sites match, else the index of the first that does not.
int verify_sites(const inmem_manifest &m);

// Resolve a blob's bytes AS THEY WOULD BE WRITTEN at a given placement -- the relocation arithmetic
// of (2) above, isolated so it can be asserted directly. `out` must hold at least `len` bytes.
// Returns false if a ref names an offset outside the blob (a generator bug).
bool relocate_site(const inmem_manifest &m, int site, const uint32_t *actual_base, uint8_t *out);
bool relocate_section(const inmem_manifest &m, int section, const uint32_t *actual_base, uint8_t *out);


// ---- PT-CAP8: THE TOGGLE LAYER AND THE APPLIED-BYTES OVERLAY ---------------------------------------
//
// A manifest with `toggle` set is the v0.2.2 LIMIT layer: every site holds either its exact vanilla
// bytes (OFF, the manifest's `expect`) or its raised bytes (ON, the relocated `repl`). apply() on such a
// manifest places its sections and fills them, verifies every site still holds the vanilla bytes and
// registers the bodies -- and writes NO site. It stages each site's ON image here instead, and
// toggle() flips the whole set at a QUIESCENT point (docs/inmem-patching.md "The limit layer").
constexpr int MAX_TOGGLE_SITES = 256; // the limits manifest has 51, the AI scratch manifest (PT-CAP11) 165
constexpr int MAX_TOGGLE_LEN   = 32;  // its longest site is a 12-byte walk backstop

struct toggle_state {
    const inmem_manifest *m;
    uint32_t              actual_base[4];
    bool                  staged; // apply() filled on_bytes
    bool                  on;     // the last successful toggle() target
    uint8_t               on_bytes[MAX_TOGGLE_SITES][MAX_TOGGLE_LEN];
    // PT-CAP9-U8b: the EXACT original bytes of every site, full length INCLUDING the wildcard (mask 0) bytes,
    // captured by apply() before any write. OFF restores these, not `expect`-under-mask: a wildcard byte is
    // not ours to ignore on the way back, because the ON image overwrites it (a rel32 displacement inside a
    // masked instruction) and "keep whatever is there" kept the ON image.
    uint8_t off_bytes[MAX_TOGGLE_SITES][MAX_TOGGLE_LEN];
};

enum : uint32_t {
    // Do not log the success line OR a refusal's detail (apply_result::detail still carries it): the caller states the
    // outcome in its own words. Also skips the baked-VA occupant diagnostic of a relocatable manifest.
    APPLY_QUIET_OK = 1u,
};

// Same contract as apply() above, plus the toggle layer. A manifest with `m.toggle` REQUIRES `ts`
// (a null `ts` refuses). `flags` is a mask of APPLY_*.
apply_result apply(const inmem_manifest &m, toggle_state *ts, uint32_t flags = 0);

// Switch a staged toggle manifest to ON (`on` true) or back to OFF. Every site's CURRENT bytes must be
// either its vanilla image or its ON image -- anything else is a foreign writer, and the call REFUSES
// (writes nothing, returns false, `*bad_site` names the site). A site already at the target is a no-op.
// One batched protect/write/restore and one icache flush. On success `ts.on == on`.
bool toggle(toggle_state &ts, bool on, int *bad_site = nullptr, const char **why = nullptr);

// Which target a toggle manifest's sites are CURRENTLY in: 0 = every site holds its EXACT saved original
// (every byte, wildcards included), 1 = every site ON,
// -1 = mixed or foreign (the parity dump and capstest ask; toggle() itself tolerates mixed).
int toggle_current(const toggle_state &ts);

// Copy, over `inout[0..len)` (which holds what the caller THINKS is at `va`), the bytes the applier
// WROTE into any applied non-toggle manifest's sites overlapping [va, va+len). This is the compose
// primitive for a DLL byte patch whose `expect` names bytes a cap manifest has since relocated
// (hook/patch.h patch_bytes_guarded_caps). The bytes are re-derived from the manifest and the recorded
// placement; nothing is stored per site.
void overlay_applied(uint32_t va, uint32_t len, uint8_t *inout);

// Forget every recorded placement (selftest only: the production applier never un-applies).
void reset_applied_for_test();

// The parity dump: every extent this applier wrote, READ BACK FROM THE LIVE IMAGE, in the format
// tools/check_inmem_patch_parity.py compares against the mhpatch-produced file. Written only when
// the spike is armed with a dump path. Returns false if the file could not be written.
bool write_parity_dump(const inmem_manifest &m, const apply_result &r, const char *path);

} // namespace mh::patch
