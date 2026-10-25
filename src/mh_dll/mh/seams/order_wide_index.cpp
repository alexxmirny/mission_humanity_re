//
// seams/order_wide_index.cpp -- O1c `[net] order_wide_index`: the RETAIL CARRIER (configuration (1), no libmh.dll;
// libmh's twin is orders/order_queue.cpp + orders/order_index_codec.h).
//
// THE DEFECT. The scheduled (replicated) order lane stores every identity ushort of a record as its low byte
// DUPLICATED (0x012c -> 0x2c2c); both receivers mask `& 0xff`; llm_strat_order_integrity_check demands hi == lo.
// A building order for roster index 300 was therefore applied to building 44 on every peer -- a silent mis-target
// no determinism oracle can see. The raised building roster (500) needs 12 bits.
//
// THE FORMAT. Only unit_index (+0x08) is widened, to 12 bits, as a strict SUPERSET of the vanilla encoding:
//     encode(v): lo = v & 0xff; hi = lo ^ ((v >> 8) & 0x0f)        (v < 256 -> hi == lo, the vanilla word)
//     decode(w): v  = lo | (((hi ^ lo) & 0x0f) << 8)
//     check(w):  ((hi ^ lo) & 0xf0) == 0                           (4 of the 8 check bits survive)
// owner_and_kind / param0 / order_code keep the vanilla rule. Wire, STAGING, PENDING and every hash are byte-identical
// to retail for every index < 256.
//
// THE FOUR SITES (in-place byte patches, no caves; assembled and disassembled with keystone/capstone):
//   (E) llm_strat_order_stage_scheduled 0x00466242, 13 bytes: the unit_index encode.
//   (V) llm_strat_order_integrity_check 0x004661a0, 34 bytes: the field selector; field 0 checks 4 bits.
//   (L) llm_strat_order_schedule        0x0046643a, 27 bytes: the local PENDING decode (unit_index only).
//   (R) llm_strat_order_pending_enqueue 0x004667b4, 24 bytes: the remote decode (all four fields).
// The VAs stay raw constexprs here and are deliberately NOT in the mh_addrs manifest: a manifest data entry renumbers
// the hash rids and stales every recorded fixture.
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "seams/order_wide_index.h"
#include "hook/patch.h"    // patch_bytes_guarded
#include "hook/promoted.h" // promoted_owner_of
#include "caps/orders.h"   // the order layer: the live pending base, the rebind hook
#include "net_internal.h"  // seam_log

namespace {

constexpr uintptr_t SITE_ENCODE = 0x00466242u; // llm_strat_order_stage_scheduled
constexpr uintptr_t SITE_CHECK  = 0x004661a0u; // llm_strat_order_integrity_check
constexpr uintptr_t SITE_LOCAL  = 0x0046643au; // llm_strat_order_schedule
constexpr uintptr_t SITE_REMOTE = 0x004667b4u; // llm_strat_order_pending_enqueue

// mov edx,[ebp-0x18]; xor dh,dh; mov eax,[ebp-0x18]; shl eax,8; or edx,eax
const uint8_t ENCODE_EXPECT[13] = {0x8b, 0x55, 0xe8, 0x30, 0xf6, 0x8b, 0x45, 0xe8, 0xc1, 0xe0, 0x08, 0x09, 0xc2};
// mov edx,[ebp-0x18]; and dh,0x0f; xor dh,dl; 5 x nop
const uint8_t ENCODE_REPL[13] = {0x8b, 0x55, 0xe8, 0x80, 0xe6, 0x0f, 0x30, 0xd6, 0x90, 0x90, 0x90, 0x90, 0x90};

// the vanilla field selector: cmp [ebp-0x24],1; jb ..; cmp ..,1; jbe ..; cmp ..,2; je ..; jmp ..; cmp ..,0; jne ..;
// mov eax,[ebp+0x10]; mov [ebp-0x20],eax; jmp ..
const uint8_t CHECK_EXPECT[34] = {0x83, 0x7d, 0xdc, 0x01, 0x72, 0x0e, 0x83, 0x7d, 0xdc, 0x01, 0x76, 0x16,
                                  0x83, 0x7d, 0xdc, 0x02, 0x74, 0x18, 0xeb, 0x1e, 0x83, 0x7d, 0xdc, 0x00,
                                  0x75, 0x18, 0x8b, 0x45, 0x10, 0x89, 0x45, 0xe0, 0xeb, 0x16};
// cmp [ebp-0x24],0; jne L1; mov eax,[ebp+0x10]; xor ah,al; and ah,0xf0; xor ah,al; mov [ebp-0x20],eax; jmp 0x4661d8;
// L1: cmp [ebp-0x24],2; je 0x4661ca; ja 0x4661d2; 5 x nop  (falls through to field 1 at 0x4661c2)
const uint8_t CHECK_REPL[34] = {0x83, 0x7d, 0xdc, 0x00, 0x75, 0x0f, 0x8b, 0x45, 0x10, 0x30, 0xc4, 0x80,
                                0xe4, 0xf0, 0x30, 0xc4, 0x89, 0x45, 0xe0, 0xeb, 0x23, 0x83, 0x7d, 0xdc,
                                0x02, 0x74, 0x0f, 0x77, 0x15, 0x90, 0x90, 0x90, 0x90, 0x90};

// THE LOCAL SITE EMBEDS THE PENDING BASE (PT-CAP9-U5). The 27 bytes carry two disp32 = &PENDING[0] + 8 (the unit_index field). With the
// order layer ON the pending array lives in `.mhcapo`, so the bytes are REBUILT from the live base -- here, never in the cap_v023_orders
// manifest, whose generator excludes these two dwords (0x0046643d / 0x00466451) for exactly this reason. Two forms, by whether the O1c fix
// is in force; each takes the pending base:
//   vanilla: mov di,[esi+B+8]; and edi,0xff; imul esi,esi,0x44 (re-imul); mov [esi+B+8],di
//   o1c:     push eax; mov ax,[esi+B+8]; xor ah,al; and ah,0x0f; mov [esi+B+8],ax; pop eax; 6 x nop
constexpr uint32_t PENDING_STOCK = 0x00bb9e80u;
constexpr int      LOCAL_LEN     = 27;

void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

void local_vanilla(uint32_t pending_base, uint8_t (&o)[LOCAL_LEN]) {
    static const uint8_t T[LOCAL_LEN] = {0x66, 0x8b, 0xbe, 0x88, 0x9e, 0xbb, 0x00, 0x81, 0xe7, 0xff, 0x00, 0x00, 0x00, 0x6b,
                                         0x35, 0xa8, 0x01, 0x5d, 0x00, 0x44, 0x66, 0x89, 0xbe, 0x88, 0x9e, 0xbb, 0x00};
    memcpy(o, T, LOCAL_LEN);
    put32(o + 3, pending_base + 8);
    put32(o + 23, pending_base + 8);
}
void local_o1c(uint32_t pending_base, uint8_t (&o)[LOCAL_LEN]) {
    static const uint8_t T[LOCAL_LEN] = {0x50, 0x66, 0x8b, 0x86, 0x88, 0x9e, 0xbb, 0x00, 0x30, 0xc4, 0x80, 0xe4, 0x0f, 0x66,
                                         0x89, 0x86, 0x88, 0x9e, 0xbb, 0x00, 0x58, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
    memcpy(o, T, LOCAL_LEN);
    put32(o + 4, pending_base + 8);
    put32(o + 16, pending_base + 8);
}

// What the local site holds now: 0 = not ours (promoted away / mismatched), 1 = vanilla bytes, 2 = the O1c bytes.
int g_local_form = 0;

uint32_t live_pending_base() { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(mh::caps::orders::base(mh::caps::orders::PENDING))); }

// caps::orders::toggle() calls this with the pending base it is leaving and the one it is entering (both in the caller's quiescent window).
bool local_rebind(uint32_t from, uint32_t to) {
    if (g_local_form == 0) return true; // the site is not ours to rebuild (libmh's twin owns the body, or the install was refused)
    uint8_t cur[LOCAL_LEN], nxt[LOCAL_LEN];
    if (g_local_form == 2) {
        local_o1c(from, cur);
        local_o1c(to, nxt);
    } else {
        local_vanilla(from, cur);
        local_vanilla(to, nxt);
    }
    return mh::hook::patch_bytes_guarded(SITE_LOCAL, cur, nxt, LOCAL_LEN);
}

// and word [ebp+0x12],0xff; and word [ebp+0x10],0xff; and word [ebp+0x14],0xff; and word [ebp+0x16],0xff
const uint8_t REMOTE_EXPECT[24] = {0x66, 0x81, 0x65, 0x12, 0xff, 0x00, 0x66, 0x81, 0x65, 0x10, 0xff, 0x00,
                                   0x66, 0x81, 0x65, 0x14, 0xff, 0x00, 0x66, 0x81, 0x65, 0x16, 0xff, 0x00};
// mov byte [ebp+0x13],0; mov ax,[ebp+0x10]; xor ah,al; and ah,0x0f; mov [ebp+0x11],ah; mov byte [ebp+0x15],0;
// mov byte [ebp+0x17],0
const uint8_t REMOTE_REPL[24] = {0xc6, 0x45, 0x13, 0x00, 0x66, 0x8b, 0x45, 0x10, 0x30, 0xc4, 0x80, 0xe4,
                                 0x0f, 0x88, 0x65, 0x11, 0xc6, 0x45, 0x15, 0x00, 0xc6, 0x45, 0x17, 0x00};

const char *const DISPLACED = "DISPLACED by promotion (libmh's twin carries it)";

} // namespace

void install_order_wide_index(bool on) {
    // PT-CAP9-U5: the local site is also the order layer's: whichever form it holds (vanilla or O1c), its pending disp32 follows the live
    // pending base. Register the rebuild hook either way, and note the vanilla form when the fix is off, so a toggle can re-aim it.
    mh::caps::orders::set_local_rebind(&local_rebind);
    if (!on) {
        if (!mh::hook::promoted_owner_of(SITE_LOCAL)) {
            uint8_t van[LOCAL_LEN];
            local_vanilla(live_pending_base(), van);
            g_local_form = memcmp(reinterpret_cast<const void *>(SITE_LOCAL), van, LOCAL_LEN) == 0 ? 1 : 0;
        }
        seam_log("; order wide index OFF ([net] order_wide_index=0): retail -- the scheduled order lane carries "
                 "unit_index in 8 bits, so building index 256..499 is applied to index & 0xff (the reproduction arm)\n");
        return;
    }
    const char *e = DISPLACED, *v = DISPLACED, *l = DISPLACED, *r = DISPLACED;
    if (!mh::hook::promoted_owner_of(SITE_ENCODE))
        e = mh::hook::patch_bytes_guarded(SITE_ENCODE, ENCODE_EXPECT, ENCODE_REPL, (int)sizeof(ENCODE_REPL))
                ? "PATCHED"
                : "MISMATCHED";
    if (!mh::hook::promoted_owner_of(SITE_CHECK))
        v = mh::hook::patch_bytes_guarded(SITE_CHECK, CHECK_EXPECT, CHECK_REPL, (int)sizeof(CHECK_REPL))
                ? "PATCHED"
                : "MISMATCHED";
    if (!mh::hook::promoted_owner_of(SITE_LOCAL)) {
        uint8_t van[LOCAL_LEN], fix[LOCAL_LEN];
        local_vanilla(live_pending_base(), van);
        local_o1c(live_pending_base(), fix);
        if (mh::hook::patch_bytes_guarded(SITE_LOCAL, van, fix, LOCAL_LEN)) {
            l            = "PATCHED";
            g_local_form = 2;
        } else {
            l = "MISMATCHED";
        }
    }
    if (!mh::hook::promoted_owner_of(SITE_REMOTE))
        r = mh::hook::patch_bytes_guarded(SITE_REMOTE, REMOTE_EXPECT, REMOTE_REPL, (int)sizeof(REMOTE_REPL))
                ? "PATCHED"
                : "MISMATCHED";
    char m[640];
    wsprintfA(m,
              "; order wide index ON ([net] order_wide_index=1): encode @00466242 %s; check @004661A0 %s; "
              "local @0046643A %s; remote @004667B4 %s%s\n",
              e, v, l, r,
              (e[0] == 'M' || v[0] == 'M' || l[0] == 'M' || r[0] == 'M')
                  ? " -- THIS RUN DOES NOT HAVE THE FIX at a MISMATCHED site"
                  : "");
    seam_log(m);
}
