//
// crt_vendor_selftest.cpp -- `crttest`, second half: the vendored CRT (crt_string.h, crt_math.h,
// crt_heap.h, crt_rand.h, crt_qsort.h) still means what the Watcom code it replaced meant.
//
// WHY THE REFERENCE ARM IS ASSEMBLY AND NOT A SECOND READING OF THE HEADER. A test whose expectation
// was written by reading the C++ it is testing proves that one person read one file twice. Both arms
// carry the same mistake and the suite reports green on it forever. So every reference below is the
// ORIGINAL MACHINE CODE, transcribed instruction by instruction from the disassembly listings
// (tmp/crt, tmp/crt2, tmp/crt3, tmp/cmp) into `__declspec(naked)` bodies, with the source VA on
// essentially every line. The vendored C++ is the SUBJECT; the assembly is the ORACLE. This is the
// same arrangement fp_x87_selftest.cpp uses, and the same reason: an equivalence claim that is
// re-derived every run survives a compiler upgrade, a flag change and a well-meant simplification,
// and a claim recorded in a header comment does not.
//
// WHAT EACH FAMILY IS FOR:
//   RAND    crt_rand.h      -- the cosmetic tactical LCG. Sequence identity over 10000 draws from
//                              equal seeds; a per-draw compare, not a compare of the last one.
//   MATH    crt_math.h      -- llm_sqrt / llm_math_atan / floor, swept at BOTH x87 precision
//                              settings (the harness pins PC=53, the bare default is PC=64 and a
//                              body correct at only one would look fine for months). SWEPT HERE,
//                              THROUGH THE REAL HEADER, AND NOT IN A PROBE: an offline probe of an
//                              x87 replacement reported zero divergences and the same comparison
//                              compiled into mh/fp/x87.h reported 1743 at PC=64, because inlining
//                              and spilling -- not semantics -- decide whether an intermediate
//                              stays 80 bits.
//   STRING  crt_string.h    -- the seven byte/string ops, including the four that are NOT their
//                              standard-library namesakes (utils_str_cmp normalises, utils_str_cmp_ci
//                              folds only A-Z, atoi wraps, strtok keeps its own saved pointer).
//   QSORT   crt_qsort.h     -- the one that matters most. Neither call site depends on "sorted", both
//                              depend on "sorted the way 0x004de8a6 sorts": the PERMUTATION OF EQUAL
//                              ELEMENTS drives wave ranks and group-move commit order. So the sweep
//                              compares the FULL BYTE IMAGE of the array, over duplicate-heavy keys,
//                              across the widths and lengths that straddle the algorithm's own
//                              thresholds (the num<16 shellsort, the 0x1d median-of-3, the 0x2a
//                              ninther, and the width==4-aligned by-value pivot).
//   CMP     the 4 comparators -- each against its own naked reference, over negative and equal keys.
//   HEAP    crt_heap.h      -- NO reference arm and no equivalence claim: the user approved our own
//                              allocator (2026-09-08) because no allocated address reaches the
//                              lockstep hash or a savegame. What is checked is the CONTRACT the
//                              callers branch on, which crt_heap.h enumerates.
//
// THE NEGATIVE ARM. Every case above is an equality, so it is worth nothing unless a real difference
// would show. Case N builds a deliberately-perturbed variant per family and REQUIRES the comparison
// to go red and to name the family. If a sweep ever stops being able to see a difference, N says so
// rather than passing quietly.
//
// ONE DISAGREEMENT THIS FILE FOUND, since found is the point. crt_math.h's floor spelled the
// correction test `!(0.0 <= fpart)`; the assembly's JBE at 0x004daafc is CF|ZF and an UNORDERED
// compare sets both, so the assembly SKIPS the correction on a NaN fraction while that C++ APPLIED
// it -- inverted exactly on the unordered case. FIXED in the header 2026-09-08 to `fpart < 0.0`.
// It was unobservable by value (NaN and both infinities absorb `+ (-1.0)` bit for bit), which is
// why the sweep was green either way and why the checks below pin the VALUE. What actually caught
// it was the NEGATIVE arm: its first perturbation was `fp < 0.0`, and a perturbation that REFUSES
// to diverge is a finding. See the note above the floor checks in sweep_math.
//
// THREE DELIBERATE DEVIATIONS FROM VERBATIM, each flagged where it appears:
//   1. `PUSH ES/FS/GS` and the `PUSH DS / POP ES` pairs are omitted. Win32 is flat, ES == DS already,
//      and they sit before the frame is set up so no EBP-relative offset moves.
//   2. `ENTER 0x144,0` / `ENTER 0x4,0` are spelled `push ebp / mov ebp,esp / sub esp,N`, which is
//      what they do.
//   3. The inert Watcom stack probe (`PUSH n / CALL utils_assert_stack_capacity @0x004cf46f`) is
//      omitted from the four comparators. It XCHGs its argument through EAX, calls __STK, restores
//      EAX and returns cleaning 4 bytes -- so dropping the PUSH and the CALL together leaves ESP and
//      every register unchanged. This is the same omission the translated C++ makes (translator-brief
//      rule 6).
// One further reconstruction, called out at the site: __STOSD @0x004e13a7 was not dumped, so the
// three bytes of FastFillData's dword store are written from the contract the CALLER forces rather
// than copied from a listing.
//
// State the originals read from the CRT's per-thread block does not exist offline. Each such cell is
// given its own file-scope substitute below and named at its use.
//
#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

#include "crt/crt_heap.h"
#include "crt/crt_math.h"
#include "st0_sincos_goldens.h" // LIB-REF-SPLIT: the measured ST0 reference arm
#include "crt/crt_qsort.h"
#include "crt/crt_rand.h"
#include "crt/crt_string.h"

#include "ai/ai_scan_target_sort_cmp.h"                // mh::ai::detail::scan_target_sort_cmp
#include "ai/ai_spiral_table_init.h"                   // mh::ai::detail::spiral_offset_sort_cmp
#include "sim/sim_pathfind_route_leg_group_and_sort.h" // the two group-scratch comparators

namespace {

int g_checks = 0, g_fails = 0;

void ck(bool ok, const char *what) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL: %s\n", what);
    }
}

void ckf(bool ok, const char *fmt, ...) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        char    msg[320];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(msg, sizeof(msg), fmt, ap);
        va_end(ap);
        printf("  FAIL: %s\n", msg);
    }
}

// Doubles are compared BITWISE throughout: NaN != NaN under `==`, and +0.0 == -0.0 under it, and
// both distinctions are exactly what crt_math.h's guard is about.
uint64_t bits(double v) {
    uint64_t b;
    std::memcpy(&b, &v, sizeof(b));
    return b;
}

// ==================================================================================================
//  THE STATE THE ORIGINALS READ FROM THE CRT's PER-THREAD BLOCK
// ==================================================================================================

// llm_rand's seed: thread_data+0x0c. Initial value 1, measured at __InitThreadData @0x004f19a7,
// instruction 0x004f19ce `MOV dword ptr [EBX+0xc],0x1`.
uint32_t g_ref_rand_seed = 1u;

// strtok's saved pointer: thread_data+0x10 (written @0x004dab98 / @0x004dabac).
char *g_ref_strtok_saved = nullptr;

// The Watcom ctype table @0x005073a4, indexed by (c + 1) & 0xff. Only the two bits atoi reads are
// populated, and both were DUMPED FROM THE BINARY 2026-09-08 rather than taken from <cctype>:
// bit 0x02 (leading-skip) for exactly {9,10,11,12,13,32}, bit 0x20 (digit) for exactly {'0'..'9'}.
// The transcription keeps the TABLE LOOKUP rather than substituting a predicate, because the lookup
// is what the assembly does.
unsigned char g_ref_ctype[256];

// The bit table @0x005074a8 that __setbits and strtok index by (c & 7).
unsigned char g_ref_bits[8] = {1, 2, 4, 8, 16, 32, 64, 128};

// DAT_00506d54, the constant floor adds when the fraction is negative. Dumped as exactly -1.0.
double g_ref_floor_bias = -1.0;

void init_ref_tables() {
    std::memset(g_ref_ctype, 0, sizeof(g_ref_ctype));
    const unsigned char SPACE[] = {9, 10, 11, 12, 13, 32};
    for (unsigned char c : SPACE) g_ref_ctype[(c + 1) & 0xff] |= 0x02u;
    for (int c = '0'; c <= '9'; ++c) g_ref_ctype[(c + 1) & 0xff] |= 0x20u;
}

// ==================================================================================================
//  REFERENCE ARM: crt_rand.h
// ==================================================================================================

// llm_rand @0x004da98b. The one substitution: `CALL 0x004da981` returns thread_data+0x0c, i.e. the
// address of the seed, so it becomes a LEA of the local cell.
__declspec(naked) int32_t ref_llm_rand() {
    // clang-format off
    __asm {
        push edx                                    ; 0x004da98b
        lea  eax, g_ref_rand_seed                   ; 0x004da98c CALL 0x004da981 -> &thread_data[0xc]
        test eax, eax                               ; 0x004da991
        jz   L_zero                                 ; 0x004da993  (unreachable for a file-scope cell)
        imul edx, dword ptr [eax], 41C64E6Dh        ; 0x004da995
        add  edx, 3039h                             ; 0x004da99b
        mov  dword ptr [eax], edx                   ; 0x004da9a1
        mov  eax, edx                               ; 0x004da9a3
        shr  eax, 10h                               ; 0x004da9a5
        and  eax, 7FFFh                             ; 0x004da9a8
    L_zero:                                         ; LAB_004da9ad
        pop  edx                                    ; 0x004da9ad
        ret                                         ; 0x004da9ae
    }
    // clang-format on
}

// ==================================================================================================
//  REFERENCE ARM: crt_math.h
// ==================================================================================================

// llm_sqrt's domain guard @0x004da9c0-0x004da9d4, on its own. The arithmetic path below covers the
// NON-ERROR domain only: the error path calls __math87_err @0x004e91d0, which chains through
// __math1err / __math2err / _matherr into the CRT's error machinery and CANNOT be transcribed
// offline. crt_math.h records the derivation of the value that path produces (+0.0 exactly); this
// suite asserts that value directly and proves the PREDICATE that selects it against the assembly.
//
// The double argument sits at [ESP+4] here exactly as it does in the original (both are stack-passed
// after the return address), so the transcribed ESP offsets are already right.
__declspec(naked) int32_t ref_sqrt_is_domain_error(double) {
    // clang-format off
    __asm {
        test byte ptr [esp+0Bh], 80h                ; 0x004da9c0  the SIGN BIT, not `x < 0.0`
        jz   L_ok                                   ; 0x004da9c5
        mov  eax, dword ptr [esp+8]                 ; 0x004da9c7
        and  eax, 7FFFFFFFh                         ; 0x004da9cb
        or   eax, dword ptr [esp+4]                 ; 0x004da9d0  is the MAGNITUDE zero
        jz   L_ok                                   ; 0x004da9d4  -0.0 goes down the FSQRT path
        mov  eax, 1
        ret
    L_ok:
        xor  eax, eax
        ret
    }
    // clang-format on
}

// llm_sqrt @0x004da9c0, the non-error path (LAB_004da9e7).
__declspec(naked) double ref_llm_sqrt(double) {
    // clang-format off
    __asm {
        fld   qword ptr [esp+4]                     ; 0x004da9e7
        fsqrt                                       ; 0x004da9eb
        ret                                         ; 0x004da9ed RET 8 -- ours is __cdecl
    }
    // clang-format on
}

// llm_math_atan @0x004daa52 with IF@ATAN @0x004daa28 inlined. IF@ATAN's `TEST byte ptr
// [0x0066f45c],0x1 / JNZ __fpatan_wrap` guard is NOT transcribable offline -- the byte lives in the
// image, where it reads 0 (checked 2026-09-08), which is the "real x87 present" case. The hardware
// arm is therefore taken unconditionally here, which is the only arm reachable on any machine that
// can run a 2001 Win32 game.
__declspec(naked) double ref_llm_math_atan(double) {
    // clang-format off
    __asm {
        fld    qword ptr [esp+4]                    ; 0x004daa52
        fld1                                        ; 0x004daa28
        fpatan                                      ; 0x004daa33  atan(ST(1)/ST(0)) = atan(x)
        ret                                         ; 0x004daa5b RET 8
    }
    // clang-format on
}

// utils_math_trunc @0x004d0596. ST(0) in, ST(0) out.
__declspec(naked) void ref_utils_math_trunc() {
    // clang-format off
    __asm {
        push eax                                    ; 0x004d0596
        fstcw word ptr [esp]                        ; 0x004d0597 (the 9B wait prefix is FSTCW, not FNSTCW)
        fwait                                       ; 0x004d059b
        push dword ptr [esp]                        ; 0x004d059c
        mov  byte ptr [esp+1], 1Fh                  ; 0x004d059f  RC = 11 truncate, PC = 11 extended
        fldcw word ptr [esp]                        ; 0x004d05a4
        frndint                                     ; 0x004d05a7
        fldcw word ptr [esp+4]                      ; 0x004d05a9  restore BEFORE the store
        fwait                                       ; 0x004d05ad
        lea  esp, [esp+8]                           ; 0x004d05ae
        ret                                         ; 0x004d05b2
    }
    // clang-format on
}

// modf @0x004e9280. Three stack dwords in (x lo, x hi, out), RET 0xc -- callee-cleaned, which is why
// ref_floor below does not adjust ESP after the call.
__declspec(naked) void ref_modf() {
    // clang-format off
    __asm {
        push eax                                    ; 0x004e9280
        fld  qword ptr [esp+8]                      ; 0x004e9281
        fld  st(0)                                  ; 0x004e9285  ST0 = x, ST1 = x
        call ref_utils_math_trunc                   ; 0x004e9287
        fsub st(1), st(0)                           ; 0x004e928c  ST1 = x - trunc(x)
        mov  eax, dword ptr [esp+10h]               ; 0x004e928e
        fstp qword ptr [eax]                        ; 0x004e9292  *out = trunc(x)
        pop  eax                                    ; 0x004e9294
        fwait                                       ; 0x004e9295
        ret  0Ch                                    ; 0x004e9296
    }
    // clang-format on
}

// floor @0x004daadb.
__declspec(naked) double ref_floor(double) {
    // clang-format off
    __asm {
        push ebp                                    ; 0x004daadb
        mov  ebp, esp                               ; 0x004daadc
        push eax                                    ; 0x004daade
        push ebx                                    ; 0x004daadf
        push edx                                    ; 0x004daae0
        sub  esp, 8                                 ; 0x004daae1
        lea  eax, [ebp-14h]                         ; 0x004daae4
        push eax                                    ; 0x004daae7
        mov  edx, dword ptr [ebp+0Ch]               ; 0x004daae8
        push edx                                    ; 0x004daaeb
        mov  ebx, dword ptr [ebp+8]                 ; 0x004daaec
        push ebx                                    ; 0x004daaef
        call ref_modf                               ; 0x004daaf0
        fldz                                        ; 0x004daaf5
        fcompp                                      ; 0x004daaf7  compare 0.0 against the fraction
        fnstsw ax                                   ; 0x004daaf9
        sahf                                        ; 0x004daafb
        jbe  L_skip                                 ; 0x004daafc  NaN sets CF and ZF -> TAKEN
        fld  qword ptr [ebp-14h]                    ; 0x004daafe
        fadd qword ptr g_ref_floor_bias             ; 0x004dab01  FADD DAT_00506d54 (= -1.0)
        fstp qword ptr [ebp-14h]                    ; 0x004dab07
    L_skip:                                         ; LAB_004dab0a
        fld  qword ptr [ebp-14h]                    ; 0x004dab0a
        lea  esp, [ebp-0Ch]                         ; 0x004dab0d
        pop  edx                                    ; 0x004dab10
        pop  ebx                                    ; 0x004dab11
        pop  eax                                    ; 0x004dab12
        pop  ebp                                    ; 0x004dab13
        ret                                         ; 0x004dab14 RET 8
    }
    // clang-format on
}

// ==================================================================================================
//  REFERENCE ARM: crt_string.h
// ==================================================================================================

// __STOSD @0x004e13a7 was NOT dumped, so the four instructions between the markers below are written
// from the contract its CALLER forces rather than copied from a listing: FastFillData saves ECX
// around the call, reads EDX afterwards for the 1..3 trailing bytes, and addresses them off EAX --
// so __STOSD must store ECX dwords of EDX at EAX, advance EAX by 4*ECX, and leave EDX alone. That is
// the whole of what is reconstructed here; everything around it is verbatim.
__declspec(naked) void ref_fast_fill_data() { // EAX = ptr, EDX = value, ECX = size
    // clang-format off
    __asm {
        or   ecx, ecx                               ; 0x004e1370
        jz   L_done                                 ; 0x004e1372  size 0 is a no-op, not a 4 GiB fill
        cmp  byte ptr [eax], dl                     ; 0x004e1374  a read probe, no store
    L_align:                                        ; LAB_004e1376
        test al, 3                                  ; 0x004e1376
        jz   L_dwords                               ; 0x004e1378
        mov  byte ptr [eax], dl                     ; 0x004e137a
        inc  eax                                    ; 0x004e137c
        ror  edx, 8                                 ; 0x004e137d
        dec  ecx                                    ; 0x004e1380
        jnz  L_align                                ; 0x004e1381
    L_dwords:                                       ; LAB_004e1383
        push ecx                                    ; 0x004e1383
        shr  ecx, 2                                 ; 0x004e1384
        ; ---- 0x004e1387 CALL 0x004e13a7 __STOSD -- reconstructed, see the note above ----
        push edi
        mov  edi, eax
        mov  eax, edx
        rep  stosd
        mov  eax, edi
        pop  edi
        ; ---- end of the reconstruction ----
        pop  ecx                                    ; 0x004e138c
        and  ecx, 3                                 ; 0x004e138d
        jz   L_done                                 ; 0x004e1390
        mov  byte ptr [eax], dl                     ; 0x004e1392
        dec  ecx                                    ; 0x004e1394
        jz   L_done                                 ; 0x004e1395
        mov  byte ptr [eax+1], dh                   ; 0x004e1397
        dec  ecx                                    ; 0x004e139a
        jz   L_done                                 ; 0x004e139b
        mov  byte ptr [eax+2], dl                   ; 0x004e139d
    L_done:                                         ; LAB_004e13a0
        ret                                         ; 0x004e13a0
    }
    // clang-format on
}

// utils_fill_data @0x004d1780 with its own register contract (EAX = ptr, EBX = size, DL = byte), so
// __setbits can call it the way the original does.
__declspec(naked) void ref_utils_fill_data_watcall() {
    // clang-format off
    __asm {
        push eax                                    ; 0x004d1780  the PUSH/POP that makes the return
        push ecx                                    ; 0x004d1781  value the ORIGINAL pointer
        mov  dh, dl                                 ; 0x004d1782  broadcast DL over all four bytes
        shl  edx, 8                                 ; 0x004d1784
        mov  dl, dh                                 ; 0x004d1787
        shl  edx, 8                                 ; 0x004d1789
        mov  dl, dh                                 ; 0x004d178c
        mov  ecx, ebx                               ; 0x004d178e  ECX is a BYTE count
        call ref_fast_fill_data                     ; 0x004d1790
        pop  ecx                                    ; 0x004d1795
        pop  eax                                    ; 0x004d1796
        ret                                         ; 0x004d1797
    }
    // clang-format on
}

// The __cdecl face of the same function, for the sweep.
__declspec(naked) void *ref_utils_fill_data(void *, uint32_t, uint8_t) {
    // clang-format off
    __asm {
        push ebx                                    ; entry glue only
        mov  eax, dword ptr [esp+8]                 ; ptr  -> EAX
        mov  ebx, dword ptr [esp+12]                ; size -> EBX
        mov  edx, dword ptr [esp+16]                ; byte -> DL (the broadcast makes bits 8..31 of
                                                    ;   EDX irrelevant -- two SHL 8 shift them out)
        call ref_utils_fill_data_watcall
        pop  ebx
        ret
    }
    // clang-format on
}

// __setbits @0x004e9299. EAX = the 32-byte map, EDX = the delimiter string.
__declspec(naked) void ref_setbits() {
    // clang-format off
    __asm {
        push ebx                                    ; 0x004e9299
        push ecx                                    ; 0x004e929a
        push esi                                    ; 0x004e929b
        mov  esi, eax                               ; 0x004e929c
        mov  ecx, edx                               ; 0x004e929e
        mov  ebx, 20h                               ; 0x004e92a0
        xor  edx, edx                               ; 0x004e92a5
        call ref_utils_fill_data_watcall            ; 0x004e92a7
        jmp  L_test                                 ; 0x004e92ac
    L_body:                                         ; LAB_004e92ae
        xor  eax, eax                               ; 0x004e92ae
        mov  al, dl                                 ; 0x004e92b0
        sar  eax, 3                                 ; 0x004e92b2
        and  dl, 7                                  ; 0x004e92b5
        and  edx, 0FFh                              ; 0x004e92b8
        mov  dl, byte ptr g_ref_bits[edx]           ; 0x004e92be  DAT_005074a8
        mov  dh, byte ptr [esi+eax]                 ; 0x004e92c4
        or   dh, dl                                 ; 0x004e92c7
        inc  ecx                                    ; 0x004e92c9
        mov  byte ptr [esi+eax], dh                 ; 0x004e92ca
    L_test:                                         ; LAB_004e92cd
        mov  dl, byte ptr [ecx]                     ; 0x004e92cd  stops at the delimiter set's NUL,
        test dl, dl                                 ; 0x004e92cf  so '\0' is never itself a delimiter
        jnz  L_body                                 ; 0x004e92d1
        pop  esi                                    ; 0x004e92d3
        pop  ecx                                    ; 0x004e92d4
        pop  ebx                                    ; 0x004e92d5
        ret                                         ; 0x004e92d6
    }
    // clang-format on
}

// utils_w_str_copy @0x004d02d2. THE DECLARED ORDER IS (src, dst) -- EDX is the first parameter for
// this shape, see crt_string.h trap 1.
__declspec(naked) void *ref_utils_w_str_copy(void *, void *) {
    // clang-format off
    __asm {
        mov  edx, dword ptr [esp+4]                 ; entry glue: src -> EDX
        mov  eax, dword ptr [esp+8]                 ;             dst -> EAX
        push ebx                                    ; 0x004d02d2
        push ecx                                    ; 0x004d02d3
        mov  ebx, eax                               ; 0x004d02d4
    L_loop:                                         ; LAB_004d02d6
        mov  cx, word ptr [edx]                     ; 0x004d02d6
        inc  edx                                    ; 0x004d02d9
        inc  edx                                    ; 0x004d02da
        mov  word ptr [eax], cx                     ; 0x004d02db
        inc  eax                                    ; 0x004d02de
        inc  eax                                    ; 0x004d02df
        test cx, cx                                 ; 0x004d02e0
        jnz  L_loop                                 ; 0x004d02e3
        mov  eax, ebx                               ; 0x004d02e5
        pop  ecx                                    ; 0x004d02e7
        pop  ebx                                    ; 0x004d02e8
        ret                                         ; 0x004d02e9
    }
    // clang-format on
}

// utils_concat @0x004d02ea. (dst, src) -- the other way round from its neighbour above.
__declspec(naked) void *ref_utils_concat(void *, void *) {
    // clang-format off
    __asm {
        mov  eax, dword ptr [esp+4]                 ; entry glue: dst -> EAX
        mov  edx, dword ptr [esp+8]                 ;             src -> EDX
        push ebx                                    ; 0x004d02ea
        push ecx                                    ; 0x004d02eb
        mov  ebx, eax                               ; 0x004d02ec
    L_find:                                         ; LAB_004d02ee
        cmp  word ptr [eax], 0                      ; 0x004d02ee
        jz   L_copy                                 ; 0x004d02f2
        inc  eax                                    ; 0x004d02f4
        inc  eax                                    ; 0x004d02f5
        jmp  L_find                                 ; 0x004d02f6
    L_copy:                                         ; LAB_004d02f8
        mov  cx, word ptr [edx]                     ; 0x004d02f8
        inc  edx                                    ; 0x004d02fb
        inc  edx                                    ; 0x004d02fc
        mov  word ptr [eax], cx                     ; 0x004d02fd
        inc  eax                                    ; 0x004d0300
        inc  eax                                    ; 0x004d0301
        test cx, cx                                 ; 0x004d0302
        jnz  L_copy                                 ; 0x004d0305
        mov  eax, ebx                               ; 0x004d0307
        pop  ecx                                    ; 0x004d0309
        pop  ebx                                    ; 0x004d030a
        ret                                         ; 0x004d030b
    }
    // clang-format on
}

// utils_str_cmp @0x004d16d0. THIS READS FOUR BYTES AT A TIME and therefore up to three bytes PAST
// the terminating NUL -- which is why every fixture string in this file lives in a fixed-size padded
// array rather than in a heap block sized to its length. crt_string.h explains why OUR body is a
// byte loop instead; the results are what this sweeps.
__declspec(naked) int32_t ref_utils_str_cmp(char *, char *) {
    // clang-format off
    __asm {
        mov  eax, dword ptr [esp+4]                 ; entry glue
        mov  edx, dword ptr [esp+8]
        push ebx                                    ; 0x004d16d0
        push ecx                                    ; 0x004d16d1
        mov  ebx, eax                               ; 0x004d16d2
        cmp  eax, edx                               ; 0x004d16d4  the identical-pointer early-out
        jz   L_eq                                   ; 0x004d16d6
    L_loop:                                         ; LAB_004d16d8
        mov  eax, dword ptr [ebx]                   ; 0x004d16d8
        mov  ecx, dword ptr [edx]                   ; 0x004d16da
        cmp  ecx, eax                               ; 0x004d16dc
        jnz  L_diff                                 ; 0x004d16de
        not  ecx                                    ; 0x004d16e0
        add  eax, 0FEFEFEFFh                        ; 0x004d16e2  the zero-byte test
        and  eax, ecx                               ; 0x004d16e7
        and  eax, 80808080h                         ; 0x004d16e9
        jnz  L_eq                                   ; 0x004d16ee
        mov  eax, dword ptr [ebx+4]                 ; 0x004d16f0
        mov  ecx, dword ptr [edx+4]                 ; 0x004d16f3
        cmp  ecx, eax                               ; 0x004d16f6
        jnz  L_diff                                 ; 0x004d16f8
        not  ecx                                    ; 0x004d16fa
        add  eax, 0FEFEFEFFh                        ; 0x004d16fc
        and  eax, ecx                               ; 0x004d1701
        and  eax, 80808080h                         ; 0x004d1703
        jnz  L_eq                                   ; 0x004d1708
        mov  eax, dword ptr [ebx+8]                 ; 0x004d170a
        mov  ecx, dword ptr [edx+8]                 ; 0x004d170d
        cmp  ecx, eax                               ; 0x004d1710
        jnz  L_diff                                 ; 0x004d1712
        not  ecx                                    ; 0x004d1714
        add  eax, 0FEFEFEFFh                        ; 0x004d1716
        and  eax, ecx                               ; 0x004d171b
        and  eax, 80808080h                         ; 0x004d171d
        jnz  L_eq                                   ; 0x004d1722
        mov  eax, dword ptr [ebx+0Ch]               ; 0x004d1724
        mov  ecx, dword ptr [edx+0Ch]               ; 0x004d1727
        cmp  ecx, eax                               ; 0x004d172a
        jnz  L_diff                                 ; 0x004d172c
        add  ebx, 10h                               ; 0x004d172e
        add  edx, 10h                               ; 0x004d1731
        not  ecx                                    ; 0x004d1734
        add  eax, 0FEFEFEFFh                        ; 0x004d1736
        and  eax, ecx                               ; 0x004d173b
        and  eax, 80808080h                         ; 0x004d173d
        jz   L_loop                                 ; 0x004d1742
    L_eq:                                           ; LAB_004d1744
        sub  eax, eax                               ; 0x004d1744
        pop  ecx                                    ; 0x004d1746
        pop  ebx                                    ; 0x004d1747
        ret                                         ; 0x004d1748
    L_diff:                                         ; LAB_004d1749
        cmp  al, cl                                 ; 0x004d1749  UNSIGNED byte order sets CF
        jnz  L_sign                                 ; 0x004d174b
        cmp  al, 0                                  ; 0x004d174d
        jz   L_eq                                   ; 0x004d174f
        cmp  ah, ch                                 ; 0x004d1751
        jnz  L_sign                                 ; 0x004d1753
        cmp  ah, 0                                  ; 0x004d1755
        jz   L_eq                                   ; 0x004d1758
        shr  eax, 10h                               ; 0x004d175a
        shr  ecx, 10h                               ; 0x004d175d
        cmp  al, cl                                 ; 0x004d1760
        jnz  L_sign                                 ; 0x004d1762
        cmp  al, 0                                  ; 0x004d1764
        jz   L_eq                                   ; 0x004d1766
        cmp  ah, ch                                 ; 0x004d1768  falls through
    L_sign:                                         ; LAB_004d176a
        sbb  eax, eax                               ; 0x004d176a  CF -> -1, no CF -> 0
        or   al, 1                                  ; 0x004d176c  -> -1 or +1
        pop  ecx                                    ; 0x004d176e
        pop  ebx                                    ; 0x004d176f
        ret                                         ; 0x004d1770
    }
    // clang-format on
}

// utils_str_cmp_ci @0x004de2ae. NOT normalised: it returns the DIFFERENCE of the two lowered bytes,
// zero-extended. The fold is a bare 0x41..0x5a range test, so no locale can move it.
__declspec(naked) int32_t ref_utils_str_cmp_ci(char *, char *) {
    // clang-format off
    __asm {
        mov  eax, dword ptr [esp+4]                 ; entry glue
        mov  edx, dword ptr [esp+8]
        push ebx                                    ; 0x004de2ae
        push ecx                                    ; 0x004de2af
        mov  ebx, eax                               ; 0x004de2b0
    L_loop:                                         ; LAB_004de2b2
        mov  al, byte ptr [ebx]                     ; 0x004de2b2
        xor  ecx, ecx                               ; 0x004de2b4
        mov  ah, byte ptr [edx]                     ; 0x004de2b6
        mov  cl, al                                 ; 0x004de2b8
        cmp  ecx, 41h                               ; 0x004de2ba
        jl   L_b                                    ; 0x004de2bd
        cmp  ecx, 5Ah                               ; 0x004de2bf
        jg   L_b                                    ; 0x004de2c2
        add  al, 20h                                ; 0x004de2c4
    L_b:                                            ; LAB_004de2c6
        xor  ecx, ecx                               ; 0x004de2c6
        mov  cl, ah                                 ; 0x004de2c8
        cmp  ecx, 41h                               ; 0x004de2ca
        jl   L_cmp                                  ; 0x004de2cd
        cmp  ecx, 5Ah                               ; 0x004de2cf
        jg   L_cmp                                  ; 0x004de2d2
        add  ah, 20h                                ; 0x004de2d4
    L_cmp:                                          ; LAB_004de2d7
        cmp  al, ah                                 ; 0x004de2d7
        jnz  L_ret                                  ; 0x004de2d9
        test ah, ah                                 ; 0x004de2db
        jz   L_ret                                  ; 0x004de2dd
        inc  ebx                                    ; 0x004de2df
        inc  edx                                    ; 0x004de2e0
        jmp  L_loop                                 ; 0x004de2e1
    L_ret:                                          ; LAB_004de2e3
        xor  edx, edx                               ; 0x004de2e3
        mov  dl, al                                 ; 0x004de2e5
        mov  al, ah                                 ; 0x004de2e7
        and  eax, 0FFh                              ; 0x004de2e9
        sub  edx, eax                               ; 0x004de2ee
        mov  eax, edx                               ; 0x004de2f0
        pop  ecx                                    ; 0x004de2f2
        pop  ebx                                    ; 0x004de2f3
        ret                                         ; 0x004de2f4
    }
    // clang-format on
}

// atoi @0x004daa6e. Watcom's, which is NOT strtol underneath: wrapping accumulation, no overflow
// detection, no base prefix, and the sign byte sampled ONCE into CL before the '+'/'-' is skipped --
// which is why a lone "-" is 0 and "--5" stops at the second '-'.
__declspec(naked) int32_t ref_atoi(char *) {
    // clang-format off
    __asm {
        mov  eax, dword ptr [esp+4]                 ; entry glue: nptr -> EAX
        push ebx                                    ; 0x004daa6e
        push ecx                                    ; 0x004daa6f
        push edx                                    ; 0x004daa70
    L_ws:                                           ; LAB_004daa71
        mov  dl, byte ptr [eax]                     ; 0x004daa71
        inc  dl                                     ; 0x004daa73
        and  edx, 0FFh                              ; 0x004daa75
        mov  dl, byte ptr g_ref_ctype[edx]          ; 0x004daa7b  DAT_005073a4, index (c+1)&0xff
        and  dl, 2                                  ; 0x004daa81
        and  edx, 0FFh                              ; 0x004daa84
        jz   L_sign                                 ; 0x004daa8a
        inc  eax                                    ; 0x004daa8c
        jmp  L_ws                                   ; 0x004daa8d
    L_sign:                                         ; LAB_004daa8f
        mov  cl, byte ptr [eax]                     ; 0x004daa8f  sampled ONCE, re-tested at the end
        mov  dl, cl                                 ; 0x004daa91
        cmp  edx, 2Bh                               ; 0x004daa93
        jz   L_skip                                 ; 0x004daa96
        cmp  edx, 2Dh                               ; 0x004daa98
        jnz  L_zero                                 ; 0x004daa9b
    L_skip:                                         ; LAB_004daa9d
        inc  eax                                    ; 0x004daa9d
    L_zero:                                         ; LAB_004daa9e
        xor  ebx, ebx                               ; 0x004daa9e
    L_dig:                                          ; LAB_004daaa0
        mov  dl, byte ptr [eax]                     ; 0x004daaa0
        inc  dl                                     ; 0x004daaa2
        and  edx, 0FFh                              ; 0x004daaa4
        mov  dl, byte ptr g_ref_ctype[edx]          ; 0x004daaaa
        and  dl, 20h                                ; 0x004daab0
        and  edx, 0FFh                              ; 0x004daab3
        jz   L_end                                  ; 0x004daab9
        imul ebx, ebx, 0Ah                          ; 0x004daabb  wraps, with no overflow check
        xor  edx, edx                               ; 0x004daabe
        mov  dl, byte ptr [eax]                     ; 0x004daac0
        add  ebx, edx                               ; 0x004daac2
        inc  eax                                    ; 0x004daac4
        sub  ebx, 30h                               ; 0x004daac5
        jmp  L_dig                                  ; 0x004daac8
    L_end:                                          ; LAB_004daaca
        xor  eax, eax                               ; 0x004daaca
        mov  al, cl                                 ; 0x004daacc
        cmp  eax, 2Dh                               ; 0x004daace
        jnz  L_ret                                  ; 0x004daad1
        neg  ebx                                    ; 0x004daad3
    L_ret:                                          ; LAB_004daad5
        mov  eax, ebx                               ; 0x004daad5
        pop  edx                                    ; 0x004daad7
        pop  ecx                                    ; 0x004daad8
        pop  ebx                                    ; 0x004daad9
        ret                                         ; 0x004daada
    }
    // clang-format on
}

// strtok @0x004dab17. The two `CALL llm_crt_get_thread_data` sites become reads/writes of the local
// saved-pointer cell; everything else is verbatim, INCLUDING the quirk at 0x004dab68 -- a run that
// finds only delimiters returns NULL and jumps PAST every write to +0x10, so the saved pointer is
// left as it was.
__declspec(naked) char *ref_strtok(char *, char *) {
    // clang-format off
    __asm {
        push ebx                                    ; entry glue: EBX is callee-saved for our caller
        mov  eax, dword ptr [esp+8]                 ; str   -> EAX
        mov  edx, dword ptr [esp+12]                ; delim -> EDX
        push ebx                                    ; 0x004dab17
        push ecx                                    ; 0x004dab18
        sub  esp, 20h                               ; 0x004dab19  the 32-byte delimiter bitmap
        mov  ebx, eax                               ; 0x004dab1c
        test eax, eax                               ; 0x004dab1e
        jnz  L_have                                 ; 0x004dab20
        mov  ebx, dword ptr [g_ref_strtok_saved]    ; 0x004dab22/28  CALL get_thread_data / [EAX+0x10]
        test ebx, ebx                               ; 0x004dab2b
        jnz  L_have                                 ; 0x004dab2d
    L_null:                                         ; LAB_004dab2f
        xor  eax, eax                               ; 0x004dab2f
        jmp  L_exit                                 ; 0x004dab31
    L_have:                                         ; LAB_004dab36
        mov  eax, esp                               ; 0x004dab36
        call ref_setbits                            ; 0x004dab38
        jmp  L_skip_test                            ; 0x004dab3d
    L_skip_body:                                    ; LAB_004dab3f
        mov  eax, edx                               ; 0x004dab3f
        shr  eax, 3                                 ; 0x004dab41
        xor  ecx, ecx                               ; 0x004dab44
        mov  cl, byte ptr [esp+eax]                 ; 0x004dab46
        mov  eax, edx                               ; 0x004dab49
        and  eax, 7                                 ; 0x004dab4b
        mov  al, byte ptr g_ref_bits[eax]           ; 0x004dab4e
        and  eax, 0FFh                              ; 0x004dab54
        test ecx, eax                               ; 0x004dab59
        jz   L_after_skip                           ; 0x004dab5b
        inc  ebx                                    ; 0x004dab5d
    L_skip_test:                                    ; LAB_004dab5e
        xor  edx, edx                               ; 0x004dab5e
        mov  dl, byte ptr [ebx]                     ; 0x004dab60
        test edx, edx                               ; 0x004dab62
        jnz  L_skip_body                            ; 0x004dab64
    L_after_skip:                                   ; LAB_004dab66
        test edx, edx                               ; 0x004dab66
        jz   L_null                                 ; 0x004dab68  the saved pointer is NOT cleared
        mov  edx, ebx                               ; 0x004dab6a
        jmp  L_scan_test                            ; 0x004dab6c
    L_scan_body:                                    ; LAB_004dab6e
        mov  ecx, eax                               ; 0x004dab6e
        shr  ecx, 3                                 ; 0x004dab70
        mov  cl, byte ptr [esp+ecx]                 ; 0x004dab73
        and  eax, 7                                 ; 0x004dab76
        and  ecx, 0FFh                              ; 0x004dab79
        mov  al, byte ptr g_ref_bits[eax]           ; 0x004dab7f
        and  eax, 0FFh                              ; 0x004dab85
        test ecx, eax                               ; 0x004dab8a
        jz   L_scan_next                            ; 0x004dab8c
        mov  byte ptr [edx], 0                      ; 0x004dab8e
        inc  edx                                    ; 0x004dab91
        mov  dword ptr [g_ref_strtok_saved], edx    ; 0x004dab92/98  [EAX+0x10] = EDX
        jmp  L_ret_ebx                              ; 0x004dab9b
    L_scan_next:                                    ; LAB_004dab9d
        inc  edx                                    ; 0x004dab9d
    L_scan_test:                                    ; LAB_004dab9e
        xor  eax, eax                               ; 0x004dab9e
        mov  al, byte ptr [edx]                     ; 0x004daba0
        test eax, eax                               ; 0x004daba2
        jnz  L_scan_body                            ; 0x004daba4
        mov  dword ptr [g_ref_strtok_saved], 0      ; 0x004daba6/ac  [EAX+0x10] = 0
    L_ret_ebx:                                      ; LAB_004dabb3
        mov  eax, ebx                               ; 0x004dabb3
    L_exit:                                         ; LAB_004dabb5
        add  esp, 20h                               ; 0x004dabb5
        pop  ecx                                    ; 0x004dabb8
        pop  ebx                                    ; 0x004dabb9
        pop  ebx                                    ; entry glue
        ret                                         ; 0x004dabba
    }
    // clang-format on
}

// ==================================================================================================
//  REFERENCE ARM: crt_qsort.h
// ==================================================================================================

// CRT_004de828: swap ECX bytes between [ESI] and [EDI]. `MOVZX EDX,CL` takes the low byte of the
// count BEFORE `SHR ECX,2`, which is n & 3 for every n.
__declspec(naked) void ref_qsort_swap() {
    // clang-format off
    __asm {
        ; 0x004de828 PUSH ES / 0x004de829 PUSH DS / 0x004de82a POP ES -- omitted (deviation 1)
        movzx edx, cl                               ; 0x004de82b
        shr  ecx, 2                                 ; 0x004de82e
        jz   L_bytes                                ; 0x004de831
    L_dwords:                                       ; LAB_004de833
        mov  eax, dword ptr [edi]                   ; 0x004de833
        xchg dword ptr [esi], eax                   ; 0x004de835
        stosd                                       ; 0x004de837
        add  esi, 4                                 ; 0x004de838
        dec  ecx                                    ; 0x004de83b
        jnz  L_dwords                               ; 0x004de83c
    L_bytes:                                        ; LAB_004de83e
        and  dl, 3                                  ; 0x004de83e
        jz   L_done                                 ; 0x004de841
    L_byte:                                         ; LAB_004de843
        mov  al, byte ptr [edi]                     ; 0x004de843
        xchg byte ptr [esi], al                     ; 0x004de845
        stosb                                       ; 0x004de847
        inc  esi                                    ; 0x004de848
        dec  edx                                    ; 0x004de849
        jnz  L_byte                                 ; 0x004de84a
    L_done:                                         ; LAB_004de84c
        ret                                         ; 0x004de84d
    }
    // clang-format on
}

// CRT_004de84e: median of three. EAX = a, EDX = b, EBX = c, ECX = the comparator. Transcribed branch
// for branch INCLUDING the number and order of comparator calls -- a median that agrees on the
// result while calling the comparator a different number of times is a different function, and the
// whole claim of crt_qsort.h is that it is not a different function.
__declspec(naked) void ref_qsort_med3() {
    // clang-format off
    __asm {
        push esi                                    ; 0x004de84e
        push edi                                    ; 0x004de84f
        ; 0x004de850-0x004de853 PUSH ES/FS/GS -- omitted (deviation 1)
        push ebp                                    ; 0x004de855 ENTER 0x4,0 (deviation 2)
        mov  ebp, esp
        sub  esp, 4
        mov  edi, eax                               ; 0x004de859
        mov  esi, edx                               ; 0x004de85b
        mov  dword ptr [ebp-4], ecx                 ; 0x004de85d
        call dword ptr [ebp-4]                      ; 0x004de860  cmp(a, b)
        test eax, eax                               ; 0x004de863
        jle  L_885                                  ; 0x004de865
        mov  edx, ebx                               ; 0x004de867
        mov  eax, edi                               ; 0x004de869
        call dword ptr [ebp-4]                      ; 0x004de86b  cmp(a, c)
        test eax, eax                               ; 0x004de86e
        jle  L_881                                  ; 0x004de870
        mov  edx, ebx                               ; 0x004de872
        mov  eax, esi                               ; 0x004de874
        call dword ptr [ebp-4]                      ; 0x004de876  cmp(b, c)
        test eax, eax                               ; 0x004de879
        jg   L_89b                                  ; 0x004de87b
    L_87d:                                          ; LAB_004de87d
        mov  eax, ebx                               ; 0x004de87d
        jmp  L_89d                                  ; 0x004de87f
    L_881:                                          ; LAB_004de881
        mov  eax, edi                               ; 0x004de881
        jmp  L_89d                                  ; 0x004de883
    L_885:                                          ; LAB_004de885
        mov  edx, ebx                               ; 0x004de885
        mov  eax, edi                               ; 0x004de887
        call dword ptr [ebp-4]                      ; 0x004de889  cmp(a, c)
        test eax, eax                               ; 0x004de88c
        jge  L_881                                  ; 0x004de88e  JGE, not JG -- a tie keeps `a`
        mov  edx, ebx                               ; 0x004de890
        mov  eax, esi                               ; 0x004de892
        call dword ptr [ebp-4]                      ; 0x004de894  cmp(b, c)
        test eax, eax                               ; 0x004de897
        jg   L_87d                                  ; 0x004de899
    L_89b:                                          ; LAB_004de89b
        mov  eax, esi                               ; 0x004de89b
    L_89d:                                          ; LAB_004de89d
        leave                                       ; 0x004de89d
        ; 0x004de89e-0x004de8a2 POP GS/FS/ES -- omitted (deviation 1)
        pop  edi                                    ; 0x004de8a3
        pop  esi                                    ; 0x004de8a4
        ret                                         ; 0x004de8a5
    }
    // clang-format on
}

// qsort @0x004de8a6. The frame after `ENTER 0x144,0` plus the three argument pushes:
//   [EBP-0x150] width   [EBP-0x14c] num    [EBP-0x148] base
//   [EBP-0x144] stk_base[32]                [EBP-0x0c4] stk_num[32]
//   [EBP-0x044] pivot value   [EBP-0x040] off2  [EBP-0x03c] w3   [EBP-0x038] w2
//   [EBP-0x034] end (shellsort)  [EBP-0x030] lo  [EBP-0x02c] end  [EBP-0x028] pivot ptr
//   [EBP-0x024] cur  [EBP-0x020] off  [EBP-0x01c] swap kind  [EBP-0x018] gap  [EBP-0x014] sp
//   [EBP-0x010] compare  [EBP-0x00c] lo_eq  [EBP-0x008] hi_eq  [EBP-0x004] r      (EBX = l)
__declspec(naked) void ref_qsort(void *, uint32_t, uint32_t, void *) {
    // clang-format off
    __asm {
        push ebx                                    ; entry glue: the original's convention leaves
                                                    ;   EBX volatile, ours does not
        mov  eax, dword ptr [esp+8]                 ; base
        mov  edx, dword ptr [esp+12]                ; num
        mov  ecx, dword ptr [esp+20]                ; compare
        mov  ebx, dword ptr [esp+16]                ; width
        push esi                                    ; 0x004de8a6
        push edi                                    ; 0x004de8a7
        ; 0x004de8a8-0x004de8ab PUSH ES/FS/GS -- omitted (deviation 1)
        push ebp                                    ; 0x004de8ad ENTER 0x144,0 (deviation 2)
        mov  ebp, esp
        sub  esp, 144h
        push eax                                    ; 0x004de8b1
        push edx                                    ; 0x004de8b2
        push ebx                                    ; 0x004de8b3
        or   eax, ebx                               ; 0x004de8b4
        mov  dword ptr [ebp-10h], ecx               ; 0x004de8b6
        test al, 3                                  ; 0x004de8b9
        jz   L_8c4                                  ; 0x004de8bb
        mov  eax, 2                                 ; 0x004de8bd
        jmp  L_8cd                                  ; 0x004de8c2
    L_8c4:                                          ; LAB_004de8c4
        cmp  ebx, 4                                 ; 0x004de8c4
        seta al                                     ; 0x004de8c7
        movzx eax, al                               ; 0x004de8ca
    L_8cd:                                          ; LAB_004de8cd
        mov  dword ptr [ebp-1Ch], eax               ; 0x004de8cd
        imul eax, dword ptr [ebp-150h], 3           ; 0x004de8d0
        mov  dword ptr [ebp-3Ch], eax               ; 0x004de8d7
        mov  eax, dword ptr [ebp-150h]              ; 0x004de8da
        add  eax, eax                               ; 0x004de8e0
        mov  dword ptr [ebp-14h], 0                 ; 0x004de8e2
        mov  dword ptr [ebp-38h], eax               ; 0x004de8e9
    L_8ec:                                          ; LAB_004de8ec
        cmp  dword ptr [ebp-14Ch], 1                ; 0x004de8ec
        jbe  L_98a                                  ; 0x004de8f3
        cmp  dword ptr [ebp-14Ch], 10h              ; 0x004de8f9
        jnc  L_9b9                                  ; 0x004de900
        mov  eax, dword ptr [ebp-3Ch]               ; 0x004de906  the gap-3-then-gap-1 shellsort
        mov  dword ptr [ebp-18h], eax               ; 0x004de909
        mov  eax, dword ptr [ebp-14Ch]              ; 0x004de90c
        imul eax, dword ptr [ebp-150h]              ; 0x004de912
        mov  edx, dword ptr [ebp-148h]              ; 0x004de919
        add  edx, eax                               ; 0x004de91f
        mov  dword ptr [ebp-34h], edx               ; 0x004de921
        jmp  L_984                                  ; 0x004de924
    L_926:                                          ; LAB_004de926
        mov  eax, dword ptr [ebp-148h]              ; 0x004de926
        add  eax, dword ptr [ebp-18h]               ; 0x004de92c
        mov  dword ptr [ebp-24h], eax               ; 0x004de92f
        jmp  L_976                                  ; 0x004de932
    L_934:                                          ; LAB_004de934
        mov  ebx, eax                               ; 0x004de934
        jmp  L_958                                  ; 0x004de936
    L_938:                                          ; LAB_004de938
        cmp  dword ptr [ebp-1Ch], 0                 ; 0x004de938
        jz   L_94d                                  ; 0x004de93c
        mov  ecx, dword ptr [ebp-150h]              ; 0x004de93e
        mov  esi, ebx                               ; 0x004de944
        call ref_qsort_swap                         ; 0x004de946
        jmp  L_955                                  ; 0x004de94b
    L_94d:                                          ; LAB_004de94d
        mov  edx, dword ptr [edi]                   ; 0x004de94d
        mov  eax, dword ptr [ebx]                   ; 0x004de94f
        mov  dword ptr [ebx], edx                   ; 0x004de951
        mov  dword ptr [edi], eax                   ; 0x004de953
    L_955:                                          ; LAB_004de955
        sub  ebx, dword ptr [ebp-18h]               ; 0x004de955
    L_958:                                          ; LAB_004de958
        cmp  ebx, dword ptr [ebp-148h]              ; 0x004de958
        jbe  L_970                                  ; 0x004de95e
        mov  edi, ebx                               ; 0x004de960
        sub  edi, dword ptr [ebp-18h]               ; 0x004de962
        mov  edx, ebx                               ; 0x004de965
        mov  eax, edi                               ; 0x004de967
        call dword ptr [ebp-10h]                    ; 0x004de969
        test eax, eax                               ; 0x004de96c
        jg   L_938                                  ; 0x004de96e
    L_970:                                          ; LAB_004de970
        mov  eax, dword ptr [ebp-18h]               ; 0x004de970
        add  dword ptr [ebp-24h], eax               ; 0x004de973
    L_976:                                          ; LAB_004de976
        mov  eax, dword ptr [ebp-24h]               ; 0x004de976
        cmp  eax, dword ptr [ebp-34h]               ; 0x004de979
        jc   L_934                                  ; 0x004de97c
        mov  eax, dword ptr [ebp-38h]               ; 0x004de97e
        sub  dword ptr [ebp-18h], eax               ; 0x004de981
    L_984:                                          ; LAB_004de984
        cmp  dword ptr [ebp-18h], 0                 ; 0x004de984  SIGNED -- the gap runs down to 0
        jg   L_926                                  ; 0x004de988
    L_98a:                                          ; LAB_004de98a
        cmp  dword ptr [ebp-14h], 0                 ; 0x004de98a
        jz   L_epilogue                             ; 0x004de98e  jumps into med3's shared epilogue
        dec  dword ptr [ebp-14h]                    ; 0x004de994
        mov  eax, dword ptr [ebp-14h]               ; 0x004de997
        mov  edx, dword ptr [ebp+eax*4-144h]        ; 0x004de99a
        mov  eax, dword ptr [ebp+eax*4-0C4h]        ; 0x004de9a1
        mov  dword ptr [ebp-148h], edx              ; 0x004de9a8
        mov  dword ptr [ebp-14Ch], eax              ; 0x004de9ae
        jmp  L_8ec                                  ; 0x004de9b4
    L_9b9:                                          ; LAB_004de9b9
        mov  eax, dword ptr [ebp-14Ch]              ; 0x004de9b9
        shr  eax, 1                                 ; 0x004de9bf
        imul eax, dword ptr [ebp-150h]              ; 0x004de9c1
        mov  edi, dword ptr [ebp-148h]              ; 0x004de9c8
        add  edi, eax                               ; 0x004de9ce
        cmp  dword ptr [ebp-14Ch], 1Dh              ; 0x004de9d0  median-of-3 above 29 elements
        jbe  L_a79                                  ; 0x004de9d7
        mov  esi, dword ptr [ebp-14Ch]              ; 0x004de9dd
        dec  esi                                    ; 0x004de9e3
        imul esi, dword ptr [ebp-150h]              ; 0x004de9e4
        mov  eax, dword ptr [ebp-148h]              ; 0x004de9eb
        mov  dword ptr [ebp-30h], eax               ; 0x004de9f1
        add  esi, eax                               ; 0x004de9f4
        cmp  dword ptr [ebp-14Ch], 2Ah              ; 0x004de9f6  the ninther above 42
        jbe  L_a68                                  ; 0x004de9fd
        mov  eax, dword ptr [ebp-14Ch]              ; 0x004de9ff
        mov  edx, dword ptr [ebp-150h]              ; 0x004dea05
        shr  eax, 3                                 ; 0x004dea0b
        imul edx, eax                               ; 0x004dea0e
        mov  ebx, dword ptr [ebp-148h]              ; 0x004dea11
        mov  dword ptr [ebp-20h], edx               ; 0x004dea17
        lea  eax, [edx+edx]                         ; 0x004dea1a
        mov  ecx, dword ptr [ebp-10h]               ; 0x004dea1d
        mov  edx, dword ptr [ebp-148h]              ; 0x004dea20
        mov  dword ptr [ebp-40h], eax               ; 0x004dea26
        add  ebx, eax                               ; 0x004dea29
        mov  eax, dword ptr [ebp-148h]              ; 0x004dea2b
        add  edx, dword ptr [ebp-20h]               ; 0x004dea31
        call ref_qsort_med3                         ; 0x004dea34
        mov  ebx, dword ptr [ebp-20h]               ; 0x004dea39
        mov  ecx, dword ptr [ebp-10h]               ; 0x004dea3c
        mov  dword ptr [ebp-30h], eax               ; 0x004dea3f
        mov  edx, edi                               ; 0x004dea42
        mov  eax, edi                               ; 0x004dea44
        add  ebx, edi                               ; 0x004dea46
        sub  eax, dword ptr [ebp-20h]               ; 0x004dea48
        call ref_qsort_med3                         ; 0x004dea4b
        mov  ecx, dword ptr [ebp-10h]               ; 0x004dea50
        mov  edi, eax                               ; 0x004dea53
        mov  edx, esi                               ; 0x004dea55
        mov  ebx, esi                               ; 0x004dea57
        mov  eax, esi                               ; 0x004dea59
        sub  edx, dword ptr [ebp-20h]               ; 0x004dea5b
        sub  eax, dword ptr [ebp-40h]               ; 0x004dea5e
        call ref_qsort_med3                         ; 0x004dea61
        mov  esi, eax                               ; 0x004dea66
    L_a68:                                          ; LAB_004dea68
        mov  ecx, dword ptr [ebp-10h]               ; 0x004dea68
        mov  eax, dword ptr [ebp-30h]               ; 0x004dea6b
        mov  ebx, esi                               ; 0x004dea6e
        mov  edx, edi                               ; 0x004dea70
        call ref_qsort_med3                         ; 0x004dea72
        mov  edi, eax                               ; 0x004dea77
    L_a79:                                          ; LAB_004dea79
        cmp  dword ptr [ebp-1Ch], 0                 ; 0x004dea79
        jz   L_aa9                                  ; 0x004dea7d
        mov  eax, dword ptr [ebp-148h]              ; 0x004dea7f
        mov  dword ptr [ebp-28h], eax               ; 0x004dea85
        jz   L_a99                                  ; 0x004dea88  DEAD: no instruction between here
                                                    ;   and 0x004dea79 touches ZF, and that CMP said
                                                    ;   "not zero" or the jump above would have gone
        mov  ecx, dword ptr [ebp-150h]              ; 0x004dea8a
        mov  esi, eax                               ; 0x004dea90
        call ref_qsort_swap                         ; 0x004dea92
        jmp  L_ab4                                  ; 0x004dea97
    L_a99:                                          ; LAB_004dea99
        mov  ebx, dword ptr [ebp-148h]              ; 0x004dea99
        mov  edx, dword ptr [eax]                   ; 0x004dea9f
        mov  eax, dword ptr [edi]                   ; 0x004deaa1
        mov  dword ptr [ebx], eax                   ; 0x004deaa3
        mov  dword ptr [edi], edx                   ; 0x004deaa5
        jmp  L_ab4                                  ; 0x004deaa7
    L_aa9:                                          ; LAB_004deaa9
        lea  eax, [ebp-44h]                         ; 0x004deaa9  the by-VALUE pivot: the array is
        mov  dword ptr [ebp-28h], eax               ; 0x004deaac    not disturbed, so the element at
        mov  eax, dword ptr [edi]                   ; 0x004deaaf    `base` takes part in the partition
        mov  dword ptr [ebp-44h], eax               ; 0x004deab1
    L_ab4:                                          ; LAB_004deab4
        mov  eax, dword ptr [ebp-14Ch]              ; 0x004deab4
        dec  eax                                    ; 0x004deaba
        imul eax, dword ptr [ebp-150h]              ; 0x004deabb
        mov  ebx, dword ptr [ebp-148h]              ; 0x004deac2
        mov  dword ptr [ebp-0Ch], ebx               ; 0x004deac8
        lea  edx, [ebx+eax]                         ; 0x004deacb
        mov  dword ptr [ebp-8], edx                 ; 0x004deace
        mov  dword ptr [ebp-4], edx                 ; 0x004dead1
    L_ad4:                                          ; LAB_004dead4
        cmp  ebx, dword ptr [ebp-4]                 ; 0x004dead4
        ja   L_b1e                                  ; 0x004dead7
        mov  edx, dword ptr [ebp-28h]               ; 0x004dead9
        mov  eax, ebx                               ; 0x004deadc
        call dword ptr [ebp-10h]                    ; 0x004deade
        test eax, eax                               ; 0x004deae1
        jg   L_b1e                                  ; 0x004deae3
        jnz  L_b16                                  ; 0x004deae5
        cmp  dword ptr [ebp-1Ch], 0                 ; 0x004deae7
        jz   L_aff                                  ; 0x004deaeb
        mov  ecx, dword ptr [ebp-150h]              ; 0x004deaed
        mov  esi, dword ptr [ebp-0Ch]               ; 0x004deaf3
        mov  edi, ebx                               ; 0x004deaf6
        call ref_qsort_swap                         ; 0x004deaf8
        jmp  L_b0d                                  ; 0x004deafd
    L_aff:                                          ; LAB_004deaff
        mov  edx, dword ptr [ebp-0Ch]               ; 0x004deaff
        mov  ecx, dword ptr [ebp-0Ch]               ; 0x004deb02
        mov  eax, dword ptr [ebx]                   ; 0x004deb05
        mov  edx, dword ptr [edx]                   ; 0x004deb07
        mov  dword ptr [ecx], eax                   ; 0x004deb09
        mov  dword ptr [ebx], edx                   ; 0x004deb0b
    L_b0d:                                          ; LAB_004deb0d
        mov  eax, dword ptr [ebp-150h]              ; 0x004deb0d
        add  dword ptr [ebp-0Ch], eax               ; 0x004deb13
    L_b16:                                          ; LAB_004deb16
        add  ebx, dword ptr [ebp-150h]              ; 0x004deb16
        jmp  L_ad4                                  ; 0x004deb1c
    L_b1e:                                          ; LAB_004deb1e
        cmp  ebx, dword ptr [ebp-4]                 ; 0x004deb1e
        ja   L_b73                                  ; 0x004deb21
        mov  edx, dword ptr [ebp-28h]               ; 0x004deb23
        mov  eax, dword ptr [ebp-4]                 ; 0x004deb26
        call dword ptr [ebp-10h]                    ; 0x004deb29
        test eax, eax                               ; 0x004deb2c
        jl   L_b73                                  ; 0x004deb2e
        jnz  L_b68                                  ; 0x004deb30
        cmp  dword ptr [ebp-1Ch], 0                 ; 0x004deb32
        jz   L_b4b                                  ; 0x004deb36
        mov  ecx, dword ptr [ebp-150h]              ; 0x004deb38
        mov  edi, dword ptr [ebp-8]                 ; 0x004deb3e
        mov  esi, dword ptr [ebp-4]                 ; 0x004deb41
        call ref_qsort_swap                         ; 0x004deb44
        jmp  L_b5f                                  ; 0x004deb49
    L_b4b:                                          ; LAB_004deb4b
        mov  eax, dword ptr [ebp-8]                 ; 0x004deb4b
        mov  edx, dword ptr [ebp-4]                 ; 0x004deb4e
        mov  ecx, dword ptr [ebp-4]                 ; 0x004deb51
        mov  eax, dword ptr [eax]                   ; 0x004deb54
        mov  edx, dword ptr [edx]                   ; 0x004deb56
        mov  dword ptr [ecx], eax                   ; 0x004deb58
        mov  eax, dword ptr [ebp-8]                 ; 0x004deb5a
        mov  dword ptr [eax], edx                   ; 0x004deb5d
    L_b5f:                                          ; LAB_004deb5f
        mov  eax, dword ptr [ebp-150h]              ; 0x004deb5f
        sub  dword ptr [ebp-8], eax                 ; 0x004deb65
    L_b68:                                          ; LAB_004deb68
        mov  eax, dword ptr [ebp-150h]              ; 0x004deb68
        sub  dword ptr [ebp-4], eax                 ; 0x004deb6e
        jmp  L_b1e                                  ; 0x004deb71
    L_b73:                                          ; LAB_004deb73
        cmp  ebx, dword ptr [ebp-4]                 ; 0x004deb73
        ja   L_bb2                                  ; 0x004deb76
        cmp  dword ptr [ebp-1Ch], 0                 ; 0x004deb78
        jz   L_b90                                  ; 0x004deb7c
        mov  ecx, dword ptr [ebp-150h]              ; 0x004deb7e
        mov  edi, dword ptr [ebp-4]                 ; 0x004deb84
        mov  esi, ebx                               ; 0x004deb87
        call ref_qsort_swap                         ; 0x004deb89
        jmp  L_b9e                                  ; 0x004deb8e
    L_b90:                                          ; LAB_004deb90
        mov  edx, dword ptr [ebp-4]                 ; 0x004deb90
        mov  edx, dword ptr [edx]                   ; 0x004deb93
        mov  eax, dword ptr [ebx]                   ; 0x004deb95
        mov  dword ptr [ebx], edx                   ; 0x004deb97
        mov  edx, dword ptr [ebp-4]                 ; 0x004deb99
        mov  dword ptr [edx], eax                   ; 0x004deb9c
    L_b9e:                                          ; LAB_004deb9e
        mov  eax, dword ptr [ebp-150h]              ; 0x004deb9e
        add  ebx, dword ptr [ebp-150h]              ; 0x004deba4
        sub  dword ptr [ebp-4], eax                 ; 0x004debaa
        jmp  L_ad4                                  ; 0x004debad
    L_bb2:                                          ; LAB_004debb2
        mov  eax, dword ptr [ebp-14Ch]              ; 0x004debb2
        imul eax, dword ptr [ebp-150h]              ; 0x004debb8
        mov  edx, dword ptr [ebp-148h]              ; 0x004debbf
        mov  ecx, ebx                               ; 0x004debc5
        add  edx, eax                               ; 0x004debc7
        mov  eax, dword ptr [ebp-0Ch]               ; 0x004debc9
        sub  ecx, dword ptr [ebp-0Ch]               ; 0x004debcc
        sub  eax, dword ptr [ebp-148h]              ; 0x004debcf
        mov  dword ptr [ebp-2Ch], edx               ; 0x004debd5
        cmp  eax, ecx                               ; 0x004debd8  SIGNED
        jge  L_bde                                  ; 0x004debda
        mov  ecx, eax                               ; 0x004debdc
    L_bde:                                          ; LAB_004debde
        test ecx, ecx                               ; 0x004debde
        jbe  L_c11                                  ; 0x004debe0
        mov  edi, ebx                               ; 0x004debe2
        mov  esi, dword ptr [ebp-148h]              ; 0x004debe4
        sub  edi, ecx                               ; 0x004debea
        ; 0x004debec-0x004debee PUSH ES / PUSH DS / POP ES -- omitted (deviation 1)
        movzx edx, cl                               ; 0x004debef
        shr  ecx, 2                                 ; 0x004debf2
        jz   L_c02                                  ; 0x004debf5
    L_bf7:                                          ; LAB_004debf7
        mov  eax, dword ptr [edi]                   ; 0x004debf7
        xchg dword ptr [esi], eax                   ; 0x004debf9
        stosd                                       ; 0x004debfb
        add  esi, 4                                 ; 0x004debfc
        dec  ecx                                    ; 0x004debff
        jnz  L_bf7                                  ; 0x004dec00
    L_c02:                                          ; LAB_004dec02
        and  dl, 3                                  ; 0x004dec02
        jz   L_c10                                  ; 0x004dec05
    L_c07:                                          ; LAB_004dec07
        mov  al, byte ptr [edi]                     ; 0x004dec07
        xchg byte ptr [esi], al                     ; 0x004dec09
        stosb                                       ; 0x004dec0b
        inc  esi                                    ; 0x004dec0c
        dec  edx                                    ; 0x004dec0d
        jnz  L_c07                                  ; 0x004dec0e
    L_c10:                                          ; LAB_004dec10 -- 0x004dec10 POP ES omitted
    L_c11:                                          ; LAB_004dec11
        mov  eax, dword ptr [ebp-2Ch]               ; 0x004dec11
        mov  ecx, dword ptr [ebp-8]                 ; 0x004dec14
        sub  eax, dword ptr [ebp-8]                 ; 0x004dec17
        sub  ecx, dword ptr [ebp-4]                 ; 0x004dec1a
        sub  eax, dword ptr [ebp-150h]              ; 0x004dec1d
        cmp  ecx, eax                               ; 0x004dec23  UNSIGNED
        jc   L_c29                                  ; 0x004dec25
        mov  ecx, eax                               ; 0x004dec27
    L_c29:                                          ; LAB_004dec29
        test ecx, ecx                               ; 0x004dec29
        jbe  L_c59                                  ; 0x004dec2b
        mov  edi, dword ptr [ebp-2Ch]               ; 0x004dec2d
        mov  esi, ebx                               ; 0x004dec30
        sub  edi, ecx                               ; 0x004dec32
        ; 0x004dec34-0x004dec36 PUSH ES / PUSH DS / POP ES -- omitted (deviation 1)
        movzx edx, cl                               ; 0x004dec37
        shr  ecx, 2                                 ; 0x004dec3a
        jz   L_c4a                                  ; 0x004dec3d
    L_c3f:                                          ; LAB_004dec3f
        mov  eax, dword ptr [edi]                   ; 0x004dec3f
        xchg dword ptr [esi], eax                   ; 0x004dec41
        stosd                                       ; 0x004dec43
        add  esi, 4                                 ; 0x004dec44
        dec  ecx                                    ; 0x004dec47
        jnz  L_c3f                                  ; 0x004dec48
    L_c4a:                                          ; LAB_004dec4a
        and  dl, 3                                  ; 0x004dec4a
        jz   L_c58                                  ; 0x004dec4d
    L_c4f:                                          ; LAB_004dec4f
        mov  al, byte ptr [edi]                     ; 0x004dec4f
        xchg byte ptr [esi], al                     ; 0x004dec51
        stosb                                       ; 0x004dec53
        inc  esi                                    ; 0x004dec54
        dec  edx                                    ; 0x004dec55
        jnz  L_c4f                                  ; 0x004dec56
    L_c58:                                          ; LAB_004dec58 -- 0x004dec58 POP ES omitted
    L_c59:                                          ; LAB_004dec59
        mov  esi, dword ptr [ebp-8]                 ; 0x004dec59
        mov  ecx, dword ptr [ebp-2Ch]               ; 0x004dec5c
        mov  edi, ebx                               ; 0x004dec5f
        mov  ebx, dword ptr [ebp-14h]               ; 0x004dec61
        sub  edi, dword ptr [ebp-0Ch]               ; 0x004dec64
        sub  esi, dword ptr [ebp-4]                 ; 0x004dec67
        shl  ebx, 2                                 ; 0x004dec6a
        sub  ecx, esi                               ; 0x004dec6d
        cmp  esi, edi                               ; 0x004dec6f  push the LARGER half
        jc   L_c97                                  ; 0x004dec71
        mov  eax, esi                               ; 0x004dec73
        xor  edx, edx                               ; 0x004dec75
        div  dword ptr [ebp-150h]                   ; 0x004dec77
        xor  edx, edx                               ; 0x004dec7d
        mov  dword ptr [ebp+ebx-0C4h], eax          ; 0x004dec7f
        mov  eax, edi                               ; 0x004dec86
        div  dword ptr [ebp-150h]                   ; 0x004dec88
        mov  dword ptr [ebp+ebx-144h], ecx          ; 0x004dec8e
        jmp  L_cd1                                  ; 0x004dec95
    L_c97:                                          ; LAB_004dec97
        cmp  edi, dword ptr [ebp-150h]              ; 0x004dec97
        jbe  L_98a                                  ; 0x004dec9d
        mov  eax, dword ptr [ebp-148h]              ; 0x004deca3
        xor  edx, edx                               ; 0x004deca9
        mov  dword ptr [ebp+ebx-144h], eax          ; 0x004decab
        mov  eax, edi                               ; 0x004decb2
        div  dword ptr [ebp-150h]                   ; 0x004decb4
        xor  edx, edx                               ; 0x004decba
        mov  dword ptr [ebp+ebx-0C4h], eax          ; 0x004decbc
        mov  eax, esi                               ; 0x004decc3
        div  dword ptr [ebp-150h]                   ; 0x004decc5
        mov  dword ptr [ebp-148h], ecx              ; 0x004deccb
    L_cd1:                                          ; LAB_004decd1
        mov  dword ptr [ebp-14Ch], eax              ; 0x004decd1
        inc  dword ptr [ebp-14h]                    ; 0x004decd7
        jmp  L_8ec                                  ; 0x004decda
    L_epilogue:                                     ; the epilogue Watcom shares with med3, 0x004de89d
        leave
        pop  edi
        pop  esi
        pop  ebx                                    ; entry glue
        ret
    }
    // clang-format on
}

// ==================================================================================================
//  REFERENCE ARM: the four comparators, and the bridge that calls one
// ==================================================================================================
//
// All four are __watcall (a in EAX, b in EDX) and must preserve EBX/ESI/EDI/EBP -- qsort keeps `l`
// in EBX and med3 keeps `c` in EBX across every comparator call, so a comparator that clobbered one
// would corrupt the sort rather than merely return a wrong answer.

__declspec(naked) int32_t call_watcall_cmp(void *, void *, void *) { // (fn, a, b)
    // clang-format off
    __asm {
        mov  eax, dword ptr [esp+8]
        mov  edx, dword ptr [esp+12]
        mov  ecx, dword ptr [esp+4]
        call ecx
        ret
    }
    // clang-format on
}

// FUN_004cbe0c: the group-scratch distance comparator. The SUBTRACTION is the contract, not a
// comparison -- `SUB EDX,[EAX]` wraps, so operands more than INT32_MAX apart return an inverted sign.
__declspec(naked) void ref_cmp_dist() {
    // clang-format off
    __asm {
        push ebp                                    ; 0x004cbe0c
        mov  ebp, esp                               ; 0x004cbe0d
        ; 0x004cbe0f PUSH 0x24 / 0x004cbe14 CALL utils_assert_stack_capacity -- omitted (deviation 3)
        push ebx                                    ; 0x004cbe19
        push ecx                                    ; 0x004cbe1a
        push esi                                    ; 0x004cbe1b
        push edi                                    ; 0x004cbe1c
        sub  esp, 0Ch                               ; 0x004cbe1d
        mov  dword ptr [ebp-1Ch], eax               ; 0x004cbe23
        mov  dword ptr [ebp-18h], edx               ; 0x004cbe26
        mov  eax, dword ptr [ebp-1Ch]               ; 0x004cbe29
        mov  edx, dword ptr [eax]                   ; 0x004cbe2c
        mov  eax, dword ptr [ebp-18h]               ; 0x004cbe2e
        sub  edx, dword ptr [eax]                   ; 0x004cbe31
        mov  dword ptr [ebp-14h], edx               ; 0x004cbe33
        mov  eax, dword ptr [ebp-14h]               ; 0x004cbe36
        lea  esp, [ebp-10h]                         ; 0x004cbe39
        pop  edi                                    ; 0x004cbe3c
        pop  esi                                    ; 0x004cbe3d
        pop  ecx                                    ; 0x004cbe3e
        pop  ebx                                    ; 0x004cbe3f
        pop  ebp                                    ; 0x004cbe40
        ret                                         ; 0x004cbe41
    }
    // clang-format on
}

// FUN_004cbe42: the same over `wave_rank`, offset 8 of llm_strat_group_scratch_member.
__declspec(naked) void ref_cmp_wave_rank() {
    // clang-format off
    __asm {
        push ebp                                    ; 0x004cbe42
        mov  ebp, esp                               ; 0x004cbe43
        ; 0x004cbe45 PUSH 0x24 / 0x004cbe4a CALL utils_assert_stack_capacity -- omitted (deviation 3)
        push ebx                                    ; 0x004cbe4f
        push ecx                                    ; 0x004cbe50
        push esi                                    ; 0x004cbe51
        push edi                                    ; 0x004cbe52
        sub  esp, 0Ch                               ; 0x004cbe53
        mov  dword ptr [ebp-1Ch], eax               ; 0x004cbe59
        mov  dword ptr [ebp-18h], edx               ; 0x004cbe5c
        mov  eax, dword ptr [ebp-1Ch]               ; 0x004cbe5f
        mov  edx, dword ptr [eax+8]                 ; 0x004cbe62
        mov  eax, dword ptr [ebp-18h]               ; 0x004cbe65
        sub  edx, dword ptr [eax+8]                 ; 0x004cbe68
        mov  dword ptr [ebp-14h], edx               ; 0x004cbe6b
        mov  eax, dword ptr [ebp-14h]               ; 0x004cbe6e
        lea  esp, [ebp-10h]                         ; 0x004cbe71
        pop  edi                                    ; 0x004cbe74
        pop  esi                                    ; 0x004cbe75
        pop  ecx                                    ; 0x004cbe76
        pop  ebx                                    ; 0x004cbe77
        pop  ebp                                    ; 0x004cbe78
        ret                                         ; 0x004cbe79
    }
    // clang-format on
}

// llm_strat_ai_scan_target_sort_cmp @0x004ec792: DESCENDING by a SIGNED 16-bit priority_score at +4.
// The two branches share one CMP -- nothing between them touches the flags.
__declspec(naked) void ref_cmp_scan_target() {
    // clang-format off
    __asm {
        ; 0x004ec792 PUSH 0x8 / 0x004ec797 CALL utils_assert_stack_capacity -- omitted (deviation 3)
        push ebx                                    ; 0x004ec79c
        mov  bx, word ptr [eax+4]                   ; 0x004ec79d
        cmp  bx, word ptr [edx+4]                   ; 0x004ec7a1
        jle  L_7ae                                  ; 0x004ec7a5
        mov  eax, 0FFFFFFFFh                        ; 0x004ec7a7
        pop  ebx                                    ; 0x004ec7ac
        ret                                         ; 0x004ec7ad
    L_7ae:                                          ; LAB_004ec7ae
        jge  L_7b7                                  ; 0x004ec7ae
        mov  eax, 1                                 ; 0x004ec7b0
        pop  ebx                                    ; 0x004ec7b5
        ret                                         ; 0x004ec7b6
    L_7b7:                                          ; LAB_004ec7b7
        xor  eax, eax                               ; 0x004ec7b7
        pop  ebx                                    ; 0x004ec7b9
        ret                                         ; 0x004ec7ba
    }
    // clang-format on
}

// LAB_004dc546: the spiral-offset comparator. PARTIALLY TRANSCRIBED, and this is the one place in
// this file where the listing does not exist. Ghidra never made it a function -- it is a bare label
// one instruction before llm_strat_ai_spiral_table_init -- so there is no .asm; the only record is
// the quoted disassembly in the header comment of libmh/ai/ai_spiral_table_init.cpp, which quotes the
// a-half instruction for instruction (0x004dc554-0x004dc561), then says "...same for b into EAX",
// then quotes the compare and the three result VAs. The b-half below therefore MIRRORS the quoted
// a-half rather than being copied from a listing. What IS verbatim is everything the contract turns
// on: MOVSX and not MOVZX (reading the int8 fields unsigned would put every negative offset in the
// wrong ring), the dy-first sum, and the CMP/JGE/JLE ordering that decides -1 / 0 / +1.
__declspec(naked) void ref_cmp_spiral() {
    // clang-format off
    __asm {
        ; 0x004dc546 the inert stack probe -- omitted (deviation 3)
        push ebx
        movsx ebx, byte ptr [eax+1]                 ; 0x004dc554  a->dy, SIGN-extended
        imul ebx, ebx                               ; 0x004dc558
        movsx eax, byte ptr [eax]                   ; 0x004dc55b  a->dx
        imul eax, eax                               ; 0x004dc55e
        add  ebx, eax                               ; 0x004dc561  ra = dy*dy + dx*dx
        movsx ecx, byte ptr [edx+1]                 ; "...same for b into EAX" -- mirrored, not quoted
        imul ecx, ecx
        movsx eax, byte ptr [edx]
        imul eax, eax
        add  eax, ecx                               ; rb
        cmp  ebx, eax                               ; 0x004dc576
        jge  L_ge
        mov  eax, 0FFFFFFFFh                        ; 0x004dc57a  ra < rb
        pop  ebx
        ret
    L_ge:
        jle  L_eq
        mov  eax, 1                                 ; 0x004dc586  ra > rb
        pop  ebx
        ret
    L_eq:
        xor  eax, eax                               ; 0x004dc590  ra == rb
        pop  ebx
        ret
    }
    // clang-format on
}

// The sweep's own comparator pair: key = the FIRST BYTE, so one pair covers every element width the
// qsort sweep uses. The rest of each element is a serial payload, which is what makes a difference
// in the permutation of EQUAL keys visible in the byte image.
int32_t sweep_cmp(void *a, void *b) {
    const int ka = *static_cast<const unsigned char *>(a);
    const int kb = *static_cast<const unsigned char *>(b);
    return ka - kb;
}

__declspec(naked) void ref_sweep_cmp() {
    // clang-format off
    __asm {
        push ebx
        movzx ebx, byte ptr [eax]
        movzx eax, byte ptr [edx]
        sub  ebx, eax
        mov  eax, ebx
        pop  ebx
        ret
    }
    // clang-format on
}

// ==================================================================================================
//  THE NEGATIVE ARM's perturbed variants -- one per family
// ==================================================================================================
//
// Each is a plausible-looking mistake, and case N REQUIRES the corresponding comparison to see it.
// A compare that cannot be shown to fail is not evidence, so if any of these ever stops diverging
// the suite goes red rather than quietly measuring nothing.

uint32_t g_bad_rand_seed = 1u;
int32_t  bad_rand() { // one bit off in the ANSI multiplier
    g_bad_rand_seed = g_bad_rand_seed * 0x41C64E6Eu + 0x3039u;
    return static_cast<int32_t>((g_bad_rand_seed >> 16) & 0x7FFFu);
}

bool bad_sqrt_is_domain_error(double x) { // `x <= 0.0`, the spelling a hurried translation reaches
    return x <= 0.0;                      //   for -- it gets -0.0 wrong
}

double bad_floor(double x) { // the correction constant perturbed: +1.0 where DAT_00506d54 is -1.0
    double ip = 0.0, fp = std::modf(x, &ip);
    if (fp < 0.0) ip += 1.0;
    return ip;
}

int32_t bad_str_cmp(char *a, char *b) { // the raw byte difference, i.e. NOT normalised
    const unsigned char *p = reinterpret_cast<const unsigned char *>(a);
    const unsigned char *q = reinterpret_cast<const unsigned char *>(b);
    for (;; ++p, ++q) {
        if (*p != *q) return static_cast<int32_t>(*p) - static_cast<int32_t>(*q);
        if (*p == 0u) return 0;
    }
}

int32_t bad_atoi(char *s) { // skips only ' ', not the six bytes the ctype table marks
    const unsigned char *p = reinterpret_cast<const unsigned char *>(s);
    while (*p == ' ') ++p;
    const unsigned char sign = *p;
    if (sign == '+' || sign == '-') ++p;
    uint32_t acc = 0u;
    while (*p >= '0' && *p <= '9') acc = acc * 10u + static_cast<uint32_t>(*p++) - 0x30u;
    if (sign == '-') acc = 0u - acc;
    return static_cast<int32_t>(acc);
}

int32_t bad_scan_target_cmp(void *a, void *b) { // reads priority_score UNSIGNED
    uint16_t ka, kb;
    std::memcpy(&ka, static_cast<const uint8_t *>(a) + 4, sizeof(ka));
    std::memcpy(&kb, static_cast<const uint8_t *>(b) + 4, sizeof(kb));
    if (kb < ka) return -1;
    if (ka < kb) return 1;
    return 0;
}

// A MERELY CORRECT sort: stable insertion by the same key. This is the negative arm that matters
// most, because it is exactly the substitution crt_qsort.h exists to refuse -- it produces a sorted
// array with a DIFFERENT permutation of equal elements, which is a lockstep divergence.
void bad_sort(unsigned char *base, uint32_t num, uint32_t width) {
    unsigned char tmp[64];
    for (uint32_t i = 1; i < num; ++i) {
        std::memcpy(tmp, base + static_cast<size_t>(i) * width, width);
        uint32_t j = i;
        while (j > 0 && base[static_cast<size_t>(j - 1) * width] > tmp[0]) {
            std::memcpy(base + static_cast<size_t>(j) * width,
                        base + static_cast<size_t>(j - 1) * width, width);
            --j;
        }
        std::memcpy(base + static_cast<size_t>(j) * width, tmp, width);
    }
}

void *bad_malloc(uint32_t size) { // no size-0 guard: std::malloc(0) may hand back a unique block
    return std::malloc(size == 0u ? 1u : size);
}

// ==================================================================================================
//  FIXTURES
// ==================================================================================================

// Padded, zero-filled and fixed-size ON PURPOSE: ref_utils_str_cmp reads four bytes at a time and so
// up to three bytes past the terminating NUL. That is safe in the original's flat data segment and
// it would not be safe against a heap block sized to the string.
struct StrBuf {
    char b[64];
    explicit StrBuf(const char *s = "") {
        std::memset(b, 0, sizeof(b));
        if (s) {
            size_t n = std::strlen(s);
            if (n > 55) n = 55; // leave room for the dword overread
            std::memcpy(b, s, n);
        }
    }
};

// A byte string built from raw values, so the sweeps can carry bytes above 0x7f -- which is where a
// locale-sensitive substitute for utils_str_cmp_ci would diverge and a C-locale one would not.
StrBuf raw2(unsigned char c0, unsigned char c1) {
    StrBuf s;
    s.b[0] = static_cast<char>(c0);
    s.b[1] = static_cast<char>(c1);
    s.b[2] = '\0';
    return s;
}

alignas(8) unsigned char g_qs_ref[8192];
alignas(8) unsigned char g_qs_ours[8192];
alignas(8) unsigned char g_qs_bad[8192];

// key in byte 0, a unique serial payload in the rest: the payload is what makes a difference in the
// ORDER OF EQUAL KEYS visible in the byte image.
void fill_case(unsigned char *p, uint32_t num, uint32_t width, int dist) {
    for (uint32_t i = 0; i < num; ++i) {
        unsigned char key = 0;
        switch (dist) {
            case 0: key = static_cast<unsigned char>(i * 7u + 3u); break;                  // mostly distinct
            case 1: key = 0x42; break;                                                     // ALL EQUAL
            case 2: key = static_cast<unsigned char>(i % 3u); break;                       // 3 distinct, many dups
            case 3: key = static_cast<unsigned char>(255u - i); break;                     // reverse
            case 4: key = static_cast<unsigned char>((i * 2654435761u >> 24) % 5u); break; // 5 distinct
            case 5: key = static_cast<unsigned char>(i & 1u); break;                       // two values
            default: key = static_cast<unsigned char>(i); break;                           // already ascending
        }
        unsigned char *e = p + static_cast<size_t>(i) * width;
        e[0]             = key;
        for (uint32_t j = 1; j < width; ++j)
            e[j] = static_cast<unsigned char>(i * 31u + j * 7u + 1u);
    }
}

// ==================================================================================================
//  A. RAND -- crt_rand.h
// ==================================================================================================

void run_rand_cases() {
    ck(mh::crt::rand_seed == 1u,
       "A/RAND: the vendored seed starts at 1, the value __InitThreadData writes at 0x004f19ce");
    ck(g_ref_rand_seed == 1u, "A/RAND: ...and so does the reference cell (same premise, stated)");

    // The done_when clause: sequence identity over >= 10000 draws from equal seeds, compared PER
    // DRAW. Comparing only the last one would pass on a generator that diverged and re-converged.
    const uint32_t SEEDS[] = {1u, 0u, 12345u, 0x7fffffffu, 0xffffffffu, 0x41c64e6du};
    for (uint32_t s : SEEDS) {
        mh::crt::llm_srand(s);
        g_ref_rand_seed = s;
        int diff = 0, out_of_range = 0;
        for (int i = 0; i < 10000; ++i) {
            const int32_t ours = mh::crt::llm_rand();
            const int32_t ref  = ref_llm_rand();
            if (ours != ref) ++diff;
            if (ours < 0 || ours > 0x7fff) ++out_of_range;
        }
        ckf(diff == 0, "A/RAND: seed 0x%08X -- 10000 draws, %d differ from the assembly", s, diff);
        ckf(out_of_range == 0, "A/RAND: seed 0x%08X -- %d draws outside [0,0x7fff]", s,
            out_of_range);
    }
    // The seed cell itself must have advanced identically, not merely produced equal outputs (the
    // low 16 bits never reach the result, so a divergence there would hide for a while).
    ckf(mh::crt::rand_seed == g_ref_rand_seed,
        "A/RAND: the seed cells agree after 60000 draws (ours 0x%08X, assembly 0x%08X)",
        mh::crt::rand_seed, g_ref_rand_seed);

    mh::crt::llm_srand(1u);
    g_ref_rand_seed = 1u;
}

// ==================================================================================================
//  B. MATH -- crt_math.h, swept at BOTH x87 precision settings
// ==================================================================================================

uint16_t read_cw() {
    uint16_t cw = 0;
    // clang-format off
    __asm { fnstcw cw }
    // clang-format on
    return cw;
}

// The domain: the boundary and non-finite cases first, then the ordinary ones. This is the list the
// done_when asks for -- 0, -0, denormals both signs, huge, +/-inf and BOTH NaN signs.
const double MATH_EDGE[] = {
    0.0,
    -0.0,
    std::numeric_limits<double>::denorm_min(),
    -std::numeric_limits<double>::denorm_min(),
    std::numeric_limits<double>::min(),
    -std::numeric_limits<double>::min(),
    std::numeric_limits<double>::max(),
    -std::numeric_limits<double>::max(),
    HUGE_VAL,
    -HUGE_VAL,
    std::numeric_limits<double>::quiet_NaN(),
    -std::numeric_limits<double>::quiet_NaN(),
    1.0,
    -1.0,
    0.5,
    -0.5,
    1e-300,
    1e300,
    2147483647.0,
    -2147483648.0,
    4503599627370495.5, // the last double with a fraction
    9007199254740993.0,
};

void sweep_math(const char *label) {
    // ---- the guard predicate, against the assembly's own bit test @0x004da9c0 ----
    int diff_guard = 0;
    for (double v : MATH_EDGE)
        if (mh::crt::detail::sqrt_is_domain_error(v) != (ref_sqrt_is_domain_error(v) != 0))
            ++diff_guard;
    for (int i = -2000; i <= 2000; ++i) {
        const double v = i * 0.125;
        if (mh::crt::detail::sqrt_is_domain_error(v) != (ref_sqrt_is_domain_error(v) != 0))
            ++diff_guard;
    }
    ckf(diff_guard == 0, "%s B/MATH: the sqrt domain guard matches TEST byte [ESP+0xb],0x80 (%d diff)",
        label, diff_guard);
    ck(ref_sqrt_is_domain_error(-0.0) == 0,
       "B/MATH: -0.0 is NOT a domain error in the assembly (premise, not inferred)");
    ck(ref_sqrt_is_domain_error(-std::numeric_limits<double>::quiet_NaN()) == 1,
       "B/MATH: a NEGATIVE NaN is one -- sign bit set and magnitude nonzero (premise)");

    // ---- llm_sqrt. In the domain, bitwise against FSQRT; out of it, against the value crt_math.h
    //      derives from the __math87_err chain, which cannot be transcribed offline.
    int diff_sqrt = 0, diff_err = 0;
    for (double v : MATH_EDGE) {
        if (ref_sqrt_is_domain_error(v)) {
            if (bits(mh::crt::llm_sqrt(v)) != bits(0.0)) ++diff_err;
        } else if (bits(mh::crt::llm_sqrt(v)) != bits(ref_llm_sqrt(v))) {
            ++diff_sqrt;
        }
    }
    for (int i = 0; i <= 4000; ++i) {
        const double v = i * 0.25;
        if (bits(mh::crt::llm_sqrt(v)) != bits(ref_llm_sqrt(v))) ++diff_sqrt;
    }
    ckf(diff_sqrt == 0, "%s B/MATH: llm_sqrt matches FSQRT bit for bit in domain (%d diff)", label,
        diff_sqrt);
    ckf(diff_err == 0, "%s B/MATH: a domain error yields exactly +0.0 (%d wrong)", label, diff_err);
    ck(bits(mh::crt::llm_sqrt(-0.0)) == bits(-0.0),
       "B/MATH: sqrt(-0.0) is -0.0 -- the case `x <= 0.0` would get wrong");

    // ---- llm_math_atan ----
    int diff_atan = 0;
    for (double v : MATH_EDGE)
        if (bits(mh::crt::llm_math_atan(v)) != bits(ref_llm_math_atan(v))) ++diff_atan;
    for (int i = -3000; i <= 3000; ++i) {
        const double v = i * 0.01;
        if (bits(mh::crt::llm_math_atan(v)) != bits(ref_llm_math_atan(v))) ++diff_atan;
    }
    ckf(diff_atan == 0, "%s B/MATH: llm_math_atan matches FLD1/FPATAN bit for bit (%d diff)", label,
        diff_atan);

    // ---- floor, including the values that straddle an integer in both directions ----
    int diff_floor = 0;
    for (double v : MATH_EDGE)
        if (bits(mh::crt::floor(v)) != bits(ref_floor(v))) ++diff_floor;
    const double FRAC[] = {-0.75, -0.5, -0.25, -1e-9, 0.0, 1e-9, 0.25, 0.5, 0.75};
    for (int k = -200; k <= 200; ++k)
        for (double f : FRAC) {
            const double v = k + f;
            if (bits(mh::crt::floor(v)) != bits(ref_floor(v))) ++diff_floor;
        }
    ckf(diff_floor == 0, "%s B/MATH: floor matches modf + the -1.0 correction (%d diff)", label,
        diff_floor);

    // A DISAGREEMENT FOUND WHILE WRITING THIS FILE -- SINCE FIXED IN THE HEADER (2026-09-08), and
    // the reasoning is kept here because these two checks are what stand in for a predicate the
    // value sweep cannot see.
    //
    // crt_math.h USED TO SPELL the correction test `if (!(0.0 <= fpart))`. The assembly's test is
    // `FLDZ / FCOMPP / FNSTSW / SAHF / JBE 0x004dab0a` and JBE is CF|ZF, which an UNORDERED compare
    // sets BOTH of -- so on a NaN fraction the assembly TAKES the branch and SKIPS the correction,
    // while `!(0.0 <= NaN)` is `!false` and APPLIES it. The two are inverted exactly on the
    // unordered case, and the header's own comment argued for the opposite reading; the faithful
    // C++ spelling is the one that comment rejected, `fpart < 0.0`, which is what the header says
    // now.
    //
    // WHY NO CASE HERE CAN FAIL ON IT, which is exactly why the header's comment now carries the
    // derivation instead: the fraction is unordered only when x is NaN or infinite (inf - inf is the
    // indefinite), and NaN and both infinities all ABSORB `+ (-1.0)` bit for bit. The wrong branch
    // produced the right value for every input that can reach it. The checks below pin the VALUE at
    // those inputs, so they hold whichever spelling is in force and go red if the disagreement ever
    // becomes observable -- but a value sweep is structurally unable to police this predicate, and
    // pretending otherwise is how it got in.
    ck(bits(mh::crt::floor(HUGE_VAL)) == bits(HUGE_VAL) &&
           bits(ref_floor(HUGE_VAL)) == bits(HUGE_VAL),
       "B/MATH: floor(+inf) is +inf in both arms -- the unordered branch is unobservable because "
       "infinity absorbs the -1.0 correction");
    {
        const double nan_v = std::numeric_limits<double>::quiet_NaN();
        ck(bits(mh::crt::floor(nan_v)) == bits(ref_floor(nan_v)),
           "B/MATH: floor(NaN) agrees by VALUE even though the two arms take opposite branches -- "
           "see the note above the check");
    }

    // AN INDEPENDENT THIRD ARM for floor, and it is here for a specific reason: both the vendored
    // body and the reference spell the fraction with `fsub st(1), st(0)`, so a mis-assembled FSUB/
    // FSUBR would make the two arms identically wrong and the compare above would still be green.
    // std::floor is a different code path entirely and pins the value for finite inputs.
    int diff_host = 0;
    for (int k = -200; k <= 200; ++k)
        for (double f : FRAC) {
            const double v = k + f;
            if (bits(ref_floor(v)) != bits(std::floor(v))) ++diff_host;
        }
    ckf(diff_host == 0,
        "%s B/MATH: the ASSEMBLY floor also agrees with std::floor on 3609 finite inputs -- the "
        "arm that would catch a mis-assembled FSUB (%d diff)",
        label, diff_host);
    ck(bits(ref_floor(-0.0)) == bits(-0.0), "B/MATH: the assembly floor keeps -0.0 signed");
}

// ---- B2/ST0: the vendored sin/cos, against MEASURED goldens ------------------------------------
//
// The only body in this file whose reference arm is not a transcription of the original, and it
// cannot be: the vendored llm_math_fsin_reduce_loop / llm_math_cos_impl ARE the transcription, so a
// transcribed reference would compare the same instructions with themselves. The reference is
// st0_sincos_goldens.h -- the original bodies' own output, read at their addresses in a hosted
// process by seams/reimpl_probe.cpp and committed (see that header for the capture procedure).
//
// BIT COMPARISON, not ==, so the sign of a zero counts and `0.0 == -0.0` cannot pass a disagreement.
void sweep_st0(int pc, const mh::crt::goldens::st0_row *g) {
    int bad_sin = 0, bad_cos = 0, reduced = 0;
    for (int i = 0; i < mh::crt::ST0_SWEEP_N; ++i) {
        double in;
        std::memcpy(&in, &g[i].in, 8);
        const double s = mh::crt::llm_math_fsin_reduce_loop(in);
        const double c = mh::crt::llm_math_cos_impl(in);
        uint64_t     sb, cb;
        std::memcpy(&sb, &s, 8);
        std::memcpy(&cb, &c, 8);
        if (sb != g[i].sin_bits) ++bad_sin;
        if (cb != g[i].cos_bits) ++bad_cos;
        // |x| >= 2^63 is the ONLY way FSIN/FCOS set C2 and the FPREM reduction loop executes. Count
        // them: this is the band the first capture found 21 disagreements in, and a sweep that
        // stopped covering it would pass while proving only the half that never needed proving.
        double a = in < 0 ? -in : in;
        if (a >= 9223372036854775808.0) ++reduced;
    }
    ckf(bad_sin == 0, "B2/ST0 %s: sin is bit-identical to the original on all %d inputs (%d off)",
        pc == 53 ? "PC=53" : "PC=64", mh::crt::ST0_SWEEP_N, bad_sin);
    ckf(bad_cos == 0, "B2/ST0 %s: cos is bit-identical to the original on all %d inputs (%d off)",
        pc == 53 ? "PC=53" : "PC=64", mh::crt::ST0_SWEEP_N, bad_cos);
    // NON-VACUITY -- a COUNT, not a claim: an instrumentation arm that only ever reports "no
    // divergence" cannot tell a correct body from a sweep that stopped reaching the code. If the
    // sweep ever stops reaching the reduction loop this fails rather than passing quietly.
    ckf(reduced >= 8, "B2/ST0 %s: %d input(s) actually drive the C2/FPREM reduction loop",
        pc == 53 ? "PC=53" : "PC=64", reduced);
}

void run_math_cases() {
    unsigned cur = 0;
    _controlfp_s(&cur, _PC_53, _MCW_PC);
    ck(((read_cw() >> 8) & 3) == 2, "B/MATH: PC=53 is installed (the harness pin_fpu setting)");
    sweep_math("PC=53");
    sweep_st0(53, mh::crt::goldens::ST0_GOLDENS_PC53);
    _controlfp_s(&cur, _PC_64, _MCW_PC);
    ck(((read_cw() >> 8) & 3) == 3, "B/MATH: PC=64 is installed (the bare x87 default)");
    sweep_math("PC=64");
    sweep_st0(64, mh::crt::goldens::ST0_GOLDENS_PC64);
    _controlfp_s(&cur, _PC_53, _MCW_PC); // back to the CRT default for whatever runs next
}

// ==================================================================================================
//  C. STRING -- crt_string.h
// ==================================================================================================

void run_string_cases() {
    // ---- utils_fill_data. Swept over sizes across the dword boundary AND over start offsets, so
    //      FastFillData's align-to-4 head, its dword body and its 1..3-byte tail all run.
    {
        int diff = 0, diff_ret = 0;
        for (uint32_t off = 0; off < 4; ++off) {
            for (uint32_t size = 0; size <= 40; ++size) {
                alignas(4) unsigned char a[64], b[64];
                std::memset(a, 0xCC, sizeof(a));
                std::memset(b, 0xCC, sizeof(b));
                const uint8_t fillv = static_cast<uint8_t>(0x5A + size);
                void         *ra    = mh::crt::utils_fill_data(a + off, size, fillv);
                void         *rb    = ref_utils_fill_data(b + off, size, fillv);
                if (std::memcmp(a, b, sizeof(a)) != 0) ++diff;
                if (ra != a + off || rb != b + off) ++diff_ret;
            }
        }
        ckf(diff == 0, "C/STRING: utils_fill_data matches FastFillData over 164 size/offset pairs "
                       "(%d diff)",
            diff);
        ckf(diff_ret == 0,
            "C/STRING: ...and both return the ORIGINAL pointer, not the end (%d wrong)", diff_ret);
        // The size-0 case is a contract, not an accident: `OR ECX,ECX / JZ` @0x004e1370.
        alignas(4) unsigned char z[8];
        std::memset(z, 0xCC, sizeof(z));
        mh::crt::utils_fill_data(z, 0u, 0x11);
        ck(z[0] == 0xCC, "C/STRING: a zero fill is a no-op, not a 4 GiB one");
    }

    // ---- utils_w_str_copy (src, dst) and utils_concat (dst, src) -- the two adjacent wide helpers
    //      whose parameter orders DISAGREE.
    {
        static const wchar_t *SRC[] = {L"", L"a", L"Reinforcements", L"\x00ff\x0100\xfffe", L"12345678901234567890"};
        int                   diff = 0, diff_ret = 0;
        for (const wchar_t *s : SRC) {
            wchar_t da[64], db[64];
            for (int i = 0; i < 64; ++i) da[i] = db[i] = 0x5A5A;
            void *ra = mh::crt::utils_w_str_copy((void *)s, da);
            void *rb = ref_utils_w_str_copy((void *)s, db);
            if (std::memcmp(da, db, sizeof(da)) != 0) ++diff;
            if (ra != da || rb != db) ++diff_ret;
        }
        ckf(diff == 0, "C/STRING: utils_w_str_copy matches the assembly, (src,dst) order (%d diff)",
            diff);
        ckf(diff_ret == 0, "C/STRING: ...and both return the DESTINATION (%d wrong)", diff_ret);

        static const wchar_t *PRE[] = {L"", L"x", L"Player 1: "};
        diff = diff_ret = 0;
        for (const wchar_t *p : PRE)
            for (const wchar_t *s : SRC) {
                wchar_t da[64], db[64];
                for (int i = 0; i < 64; ++i) da[i] = db[i] = 0x5A5A;
                wcscpy_s(da, 64, p);
                wcscpy_s(db, 64, p);
                void *ra = mh::crt::utils_concat(da, (void *)s);
                void *rb = ref_utils_concat(db, (void *)s);
                if (std::memcmp(da, db, sizeof(da)) != 0) ++diff;
                if (ra != da || rb != db) ++diff_ret;
            }
        ckf(diff == 0, "C/STRING: utils_concat matches the assembly, (dst,src) order (%d diff)",
            diff);
        ckf(diff_ret == 0, "C/STRING: ...and both return the ORIGINAL dst (%d wrong)", diff_ret);
    }

    // ---- utils_str_cmp: strictly -1/0/+1, unsigned byte order, identical-pointer early-out. The
    //      sweep carries bytes above 0x7f, which is where a signed-char comparison would flip.
    {
        static const char *S[]  = {"", "a", "b", "ab", "abc", "abd", "abcd",
                                   "abcde", "abcdefghijklmno", "abcdefghijklmnp", "A", "Z",
                                   "aBc", "\x80", "\x7f", "\xff", "\xff\x01", "\x01\xff"};
        const int          N    = static_cast<int>(sizeof(S) / sizeof(S[0]));
        int                diff = 0, not_normalised = 0;
        for (int i = 0; i < N; ++i)
            for (int j = 0; j < N; ++j) {
                StrBuf        a(S[i]), b(S[j]);
                const int32_t ours = mh::crt::utils_str_cmp(a.b, b.b);
                const int32_t ref  = ref_utils_str_cmp(a.b, b.b);
                if (ours != ref) ++diff;
                if (ours != -1 && ours != 0 && ours != 1) ++not_normalised;
            }
        // The bytes above 0x7f again, built raw so no source-encoding question arises.
        for (int i = 0; i < 256; i += 7)
            for (int j = 0; j < 256; j += 13) {
                StrBuf        a    = raw2(static_cast<unsigned char>(i), 0x41);
                StrBuf        b    = raw2(static_cast<unsigned char>(j), 0x41);
                const int32_t ours = mh::crt::utils_str_cmp(a.b, b.b);
                if (ours != ref_utils_str_cmp(a.b, b.b)) ++diff;
                if (ours != -1 && ours != 0 && ours != 1) ++not_normalised;
            }
        ckf(diff == 0, "C/STRING: utils_str_cmp matches the assembly over the full byte domain "
                       "(%d diff)",
            diff);
        ckf(not_normalised == 0,
            "C/STRING: ...and returns only -1/0/+1 -- SBB EAX,EAX / OR AL,1 (%d wrong)",
            not_normalised);
        StrBuf same("shared");
        ck(mh::crt::utils_str_cmp(same.b, same.b) == 0 && ref_utils_str_cmp(same.b, same.b) == 0,
           "C/STRING: the identical-pointer early-out returns 0 in both arms (@0x004d16d4)");
    }

    // ---- utils_str_cmp_ci: a DIFFERENCE, not a normalised sign, folding only A-Z. The high bytes
    //      are the whole point -- _stricmp under a Polish or a C locale folds them differently.
    {
        int diff = 0;
        for (int i = 0; i < 256; ++i)
            for (int j = 0; j < 256; j += 3) {
                StrBuf a = raw2(static_cast<unsigned char>(i), 0);
                StrBuf b = raw2(static_cast<unsigned char>(j), 0);
                if (mh::crt::utils_str_cmp_ci(a.b, b.b) != ref_utils_str_cmp_ci(a.b, b.b)) ++diff;
            }
        static const char *S[] = {"", "a", "A", "abc", "ABC", "AbC", "abcd", "ABCE", "\xe0", "\xc0"};
        const int          N   = static_cast<int>(sizeof(S) / sizeof(S[0]));
        for (int i = 0; i < N; ++i)
            for (int j = 0; j < N; ++j) {
                StrBuf a(S[i]), b(S[j]);
                if (mh::crt::utils_str_cmp_ci(a.b, b.b) != ref_utils_str_cmp_ci(a.b, b.b)) ++diff;
            }
        ckf(diff == 0, "C/STRING: utils_str_cmp_ci matches the assembly over 21856 pairs (%d diff)",
            diff);
        StrBuf lo("\xe0"), up("\xc0");
        ck(mh::crt::utils_str_cmp_ci(lo.b, up.b) != 0,
           "C/STRING: 0xe0 vs 0xc0 do NOT fold together -- the fold is a bare 0x41..0x5a test");
        StrBuf a1("aBcD"), a2("AbCd");
        ck(mh::crt::utils_str_cmp_ci(a1.b, a2.b) == 0, "C/STRING: ...but A-Z does fold");
    }

    // ---- atoi. Every clause of the done_when: whitespace runs, both signs, a lone '-', "--5",
    //      empty, no digits, wrapping overflow, and INT_MIN's text.
    {
        static const char *CASES[] = {
            "",
            " ",
            "\t\n\v\f\r 42",
            "  +7",
            "-7",
            "-",
            "+",
            "--5",
            "-+5",
            "+-5",
            "abc",
            "12abc",
            "0",
            "-0",
            "00042",
            "2147483647",
            "2147483648",
            "-2147483648",
            "-2147483649",
            "4294967295",
            "4294967296",
            "9999999999",
            "99999999999999999999",
            " \t 000000000000000000012",
            "007",
            "\v9",
            "\f9",
            "\r9",
            "\n9",
            "\t9",
            " 9",
            "\x0b"
            "3",
        };
        int diff = 0;
        for (const char *c : CASES) {
            StrBuf s(c);
            if (mh::crt::atoi(s.b) != ref_atoi(s.b)) ++diff;
        }
        // Plus a wide numeric sweep, since the wrapping accumulator is the interesting part.
        char num[32];
        for (int64_t v = -3000; v <= 3000; v += 7) {
            std::snprintf(num, sizeof(num), "%lld", static_cast<long long>(v));
            StrBuf s(num);
            if (mh::crt::atoi(s.b) != ref_atoi(s.b)) ++diff;
        }
        for (int e = 0; e < 21; ++e) { // 1, 11, 111, ... past the point where the accumulator wraps
            char big[32];
            for (int i = 0; i <= e; ++i) big[i] = '1';
            big[e + 1] = '\0';
            StrBuf s(big);
            if (mh::crt::atoi(s.b) != ref_atoi(s.b)) ++diff;
        }
        ckf(diff == 0, "C/STRING: atoi matches the assembly on every case incl. overflow (%d diff)",
            diff);
        StrBuf minus("-"), dd("--5"), huge("4294967296");
        ck(ref_atoi(minus.b) == 0, "C/STRING: a lone '-' is 0 in the assembly (premise)");
        ck(ref_atoi(dd.b) == 0, "C/STRING: \"--5\" stops at the second '-' (premise)");
        ck(ref_atoi(huge.b) == 0,
           "C/STRING: 4294967296 WRAPS to 0 -- Watcom's atoi does not clamp (premise)");
    }

    // ---- strtok: the whole sequence protocol, including the documented quirk.
    {
        struct TokCase {
            const char *input;
            const char *delim;
            int         calls;
            const char *what;
        };
        static const TokCase C[] = {
            {"a,b,c", ",", 5, "an ordinary three-token run"},
            {",,,", ",", 3, "a string that is ALL delimiters"},
            {"abc", "", 3, "an EMPTY delimiter set"},
            {"  hello   world  ", " ", 5, "leading, repeated and trailing delimiters"},
            {"", ",", 3, "an empty string"},
            {"a", ",", 3, "one token, no delimiter"},
            {",a,", ",", 4, "leading and trailing delimiter around one token"},
            {"a;;b;c;", ";", 6, "an empty field in the middle"},
        };
        int diff = 0;
        for (const TokCase &tc : C) {
            StrBuf a(tc.input), b(tc.input);
            mh::crt::strtok_saved = nullptr; // reset BOTH state cells between cases
            g_ref_strtok_saved    = nullptr;
            StrBuf da(tc.delim), db(tc.delim);
            for (int i = 0; i < tc.calls; ++i) {
                char      *ours = mh::crt::strtok(i == 0 ? a.b : nullptr, da.b);
                char      *ref  = ref_strtok(i == 0 ? b.b : nullptr, db.b);
                const long oo   = ours ? static_cast<long>(ours - a.b) : -1;
                const long ro   = ref ? static_cast<long>(ref - b.b) : -1;
                if (oo != ro) ++diff;
            }
            if (std::memcmp(a.b, b.b, sizeof(a.b)) != 0) ++diff; // the in-place NULs must match too
            const long os = mh::crt::strtok_saved ? static_cast<long>(mh::crt::strtok_saved - a.b) : -1;
            const long rs = g_ref_strtok_saved ? static_cast<long>(g_ref_strtok_saved - b.b) : -1;
            if (os != rs) ++diff;
        }
        ckf(diff == 0, "C/STRING: strtok matches the assembly across 8 sequence fixtures (%d diff)",
            diff);

        // A NULL first call with no saved state returns NULL without touching the delimiter set
        // (@0x004dab2b) -- and must not crash on a delimiter string it never reads.
        mh::crt::strtok_saved = nullptr;
        g_ref_strtok_saved    = nullptr;
        StrBuf d(",");
        ck(mh::crt::strtok(nullptr, d.b) == nullptr && ref_strtok(nullptr, d.b) == nullptr,
           "C/STRING: strtok(NULL, ...) with no saved state returns NULL in both arms");

        // THE QUIRK: a run that finds only delimiters returns NULL and does NOT clear the saved
        // pointer, so a later continuation resumes where the last SUCCESSFUL call left off.
        {
            StrBuf s("a,,,"), d2(",");
            mh::crt::strtok_saved   = nullptr;
            g_ref_strtok_saved      = nullptr;
            char *t1                = mh::crt::strtok(s.b, d2.b);
            char *saved_after_first = mh::crt::strtok_saved;
            char *t2                = mh::crt::strtok(nullptr, d2.b); // finds only delimiters -> NULL
            ck(t1 != nullptr && t2 == nullptr, "C/STRING: the all-delimiter continuation is NULL");
            ck(mh::crt::strtok_saved == saved_after_first,
               "C/STRING: ...and the saved pointer was NOT cleared (@0x004dab68 skips every write)");

            StrBuf s2("a,,,"), d3(",");
            g_ref_strtok_saved          = nullptr;
            char *r1                    = ref_strtok(s2.b, d3.b);
            char *ref_saved_after_first = g_ref_strtok_saved;
            char *r2                    = ref_strtok(nullptr, d3.b);
            ck(r1 != nullptr && r2 == nullptr && g_ref_strtok_saved == ref_saved_after_first,
               "C/STRING: ...and the ASSEMBLY does exactly the same (this is where the claim comes "
               "from, not from the header)");
        }
        mh::crt::strtok_saved = nullptr;
        g_ref_strtok_saved    = nullptr;
    }
}

// ==================================================================================================
//  D. QSORT -- crt_qsort.h. The clause that matters: the FULL BYTE IMAGE on duplicate-heavy input.
// ==================================================================================================

void run_qsort_cases() {
    // Widths: the four the real sites use (2, 8, 0x14, 0x16) plus 4 for the aligned by-value pivot
    // path and 3/5 for the byte-wise one. Lengths straddle every threshold in the algorithm: the
    // num<=1 return, the num<16 shellsort, 0x1d (median-of-3) and 0x2a (the ninther).
    const uint32_t WIDTHS[]  = {2, 3, 4, 5, 8, 0x14, 0x16};
    const uint32_t LENGTHS[] = {0, 1, 2, 15, 16, 29, 30, 42, 43, 257};

    int diff = 0, cases = 0, unsorted = 0;
    for (uint32_t w : WIDTHS)
        for (uint32_t n : LENGTHS)
            for (int dist = 0; dist < 7; ++dist) {
                const size_t bytes = static_cast<size_t>(n) * w;
                fill_case(g_qs_ref, n, w, dist);
                std::memcpy(g_qs_ours, g_qs_ref, bytes);
                ref_qsort(g_qs_ref, n, w, reinterpret_cast<void *>(&ref_sweep_cmp));
                mh::crt::qsort(g_qs_ours, n, w, reinterpret_cast<void *>(&sweep_cmp));
                ++cases;
                if (std::memcmp(g_qs_ref, g_qs_ours, bytes) != 0) ++diff;
                for (uint32_t i = 1; i < n; ++i)
                    if (g_qs_ours[static_cast<size_t>(i - 1) * w] > g_qs_ours[static_cast<size_t>(i) * w])
                        ++unsorted;
            }
    ckf(diff == 0,
        "D/QSORT: the byte image is identical to the assembly's over %d width/length/distribution "
        "cases (%d diff)",
        cases, diff);
    ckf(unsorted == 0, "D/QSORT: ...and the result really is sorted (%d inversions)", unsorted);

    // The three swap kinds the width test selects (@0x004de8b4) all had to run above -- state it,
    // so a future widths list that accidentally drops one is visible rather than silent.
    ck(((reinterpret_cast<uintptr_t>(g_qs_ref) | 4u) & 3u) == 0u,
       "D/QSORT: the fixture base is 4-aligned, so width 4 really takes the by-value pivot arm");

    // The real widths with the real comparators, driven through both qsorts. This is the shape the
    // two live call sites actually use.
    struct Site {
        uint32_t    width;
        void       *ref_cmp;
        void       *our_cmp;
        const char *name;
    };
    const Site SITES[] = {
        {8u, reinterpret_cast<void *>(&ref_cmp_dist),
         reinterpret_cast<void *>(&mh::sim::detail::group_move_scratch_cmp_dist), "cmp_dist w=8"},
        {0x14u, reinterpret_cast<void *>(&ref_cmp_wave_rank),
         reinterpret_cast<void *>(&mh::sim::detail::group_move_scratch_cmp_wave_rank),
         "cmp_wave_rank w=0x14"},
        {0x16u, reinterpret_cast<void *>(&ref_cmp_scan_target),
         reinterpret_cast<void *>(&mh::ai::detail::scan_target_sort_cmp), "scan_target w=0x16"},
        {2u, reinterpret_cast<void *>(&ref_cmp_spiral),
         reinterpret_cast<void *>(&mh::ai::detail::spiral_offset_sort_cmp), "spiral w=2"},
    };
    for (const Site &s : SITES) {
        int site_diff = 0;
        for (uint32_t n : LENGTHS)
            for (int dist = 0; dist < 7; ++dist) {
                const size_t bytes = static_cast<size_t>(n) * s.width;
                fill_case(g_qs_ref, n, s.width, dist);
                std::memcpy(g_qs_ours, g_qs_ref, bytes);
                ref_qsort(g_qs_ref, n, s.width, s.ref_cmp);
                mh::crt::qsort(g_qs_ours, n, s.width, s.our_cmp);
                if (std::memcmp(g_qs_ref, g_qs_ours, bytes) != 0) ++site_diff;
            }
        ckf(site_diff == 0, "D/QSORT: %s -- byte-identical through the real comparator (%d diff)",
            s.name, site_diff);
    }
}

// ==================================================================================================
//  E. THE FOUR COMPARATORS, each against its own naked reference
// ==================================================================================================

int32_t ref_cmp(void *fn, void *a, void *b) { return call_watcall_cmp(fn, a, b); }

void run_cmp_cases() {
    // The key domains carry negatives, equal keys and the 32-bit extremes -- the last because both
    // group-scratch comparators SUBTRACT rather than compare, so a pair more than INT32_MAX apart
    // returns an inverted sign. That is the original's behaviour and it is reproduced, not fixed.
    const int32_t KEYS[] = {0, 1, -1, 2, -2, 127,
                            -128, 32767, -32768, 65535, -65536, 0x7ffffffe,
                            0x7fffffff, static_cast<int32_t>(0x80000000), static_cast<int32_t>(0x80000001)};
    const int     NK     = static_cast<int>(sizeof(KEYS) / sizeof(KEYS[0]));

    {
        int diff = 0;
        for (int i = 0; i < NK; ++i)
            for (int j = 0; j < NK; ++j) {
                alignas(4) unsigned char a[8] = {0}, b[8] = {0};
                std::memcpy(a, &KEYS[i], 4);
                std::memcpy(b, &KEYS[j], 4);
                if (mh::sim::detail::group_move_scratch_cmp_dist(a, b) !=
                    ref_cmp(reinterpret_cast<void *>(&ref_cmp_dist), a, b))
                    ++diff;
            }
        ckf(diff == 0, "E/CMP: group_move_scratch_cmp_dist matches FUN_004cbe0c over %d pairs "
                       "(%d diff)",
            NK * NK, diff);
    }
    {
        int diff = 0;
        for (int i = 0; i < NK; ++i)
            for (int j = 0; j < NK; ++j) {
                alignas(4) unsigned char a[0x14] = {0}, b[0x14] = {0};
                std::memcpy(a + 8, &KEYS[i], 4);
                std::memcpy(b + 8, &KEYS[j], 4);
                if (mh::sim::detail::group_move_scratch_cmp_wave_rank(a, b) !=
                    ref_cmp(reinterpret_cast<void *>(&ref_cmp_wave_rank), a, b))
                    ++diff;
            }
        ckf(diff == 0, "E/CMP: group_move_scratch_cmp_wave_rank matches FUN_004cbe42 (%d diff)",
            diff);
    }
    {
        int diff = 0;
        for (int i = -32768; i <= 32767; i += 37)
            for (int j = -32768; j <= 32767; j += 4093) {
                alignas(4) unsigned char a[0x16] = {0}, b[0x16] = {0};
                const int16_t            ka = static_cast<int16_t>(i), kb = static_cast<int16_t>(j);
                std::memcpy(a + 4, &ka, 2);
                std::memcpy(b + 4, &kb, 2);
                if (mh::ai::detail::scan_target_sort_cmp(a, b) !=
                    ref_cmp(reinterpret_cast<void *>(&ref_cmp_scan_target), a, b))
                    ++diff;
            }
        ckf(diff == 0,
            "E/CMP: scan_target_sort_cmp matches 0x004ec792, incl. SIGNED 16-bit keys (%d diff)",
            diff);
    }
    {
        int diff = 0;
        for (int dx = -128; dx <= 127; dx += 3)
            for (int dy = -128; dy <= 127; dy += 5) {
                for (int ex = -128; ex <= 127; ex += 61) {
                    unsigned char a[2] = {static_cast<unsigned char>(dx),
                                          static_cast<unsigned char>(dy)};
                    unsigned char b[2] = {static_cast<unsigned char>(ex),
                                          static_cast<unsigned char>(dy)};
                    if (mh::ai::detail::spiral_offset_sort_cmp(a, b) !=
                        ref_cmp(reinterpret_cast<void *>(&ref_cmp_spiral), a, b))
                        ++diff;
                }
            }
        ckf(diff == 0,
            "E/CMP: spiral_offset_sort_cmp matches LAB_004dc546, incl. SIGN-extended int8 fields "
            "(%d diff)",
            diff);
    }
}

// ==================================================================================================
//  F. HEAP -- crt_heap.h. NO REFERENCE ARM, DELIBERATELY.
// ==================================================================================================
//
// The other families reproduce the original's OUTPUT. This one cannot and does not: a standalone
// libmh's allocator hands out different ADDRESSES than the game's Watcom rover (@0x004d018e's
// free-list walk over the DGROUP arena). The user approved our own allocator on 2026-09-08 on the
// strength of the measurement crt_heap.h records -- eight 4-byte MF_HASH regions, all counts or
// cached dimensions, and ZERO pointer-shaped 4-byte MF_SAVE regions -- so no allocated address
// reaches the lockstep hash or a savegame and two peers running different allocators still agree.
// Transcribing 0x004d0155 here would therefore assert a thing that is not claimed. What IS checked
// is the CONTRACT the callers branch on.
void run_heap_cases() {
    ck(mh::crt::MALLOC_MAX == 0xffffffd4u,
       "F/HEAP: the size ceiling is 0xffffffd4 -- `CMP EAX,-0x2c / JBE` @0x004d0169");
    ck(mh::crt::utils_malloc(0u) == nullptr,
       "F/HEAP: size 0 returns NULL, not a unique 1-byte block (@0x004d0165)");
    ck(mh::crt::utils_malloc(0xffffffd5u) == nullptr,
       "F/HEAP: size above the ceiling returns NULL (the block-header overflow guard)");
    ck(mh::crt::utils_malloc(0xffffffffu) == nullptr, "F/HEAP: ...and so does 0xffffffff");
    mh::crt::utils_free(nullptr); // must not fault: @0x004d024b returns without touching the heap
    ck(true, "F/HEAP: free(NULL) is a no-op");

    void *p = mh::crt::utils_malloc(64u);
    ck(p != nullptr, "F/HEAP: an ordinary request succeeds");
    mh::crt::utils_free(p);

    // struct_array_malloc_impl ZEROES the block, and returns the block rather than its end.
    {
        const uint32_t n = 64u, sz = 8u;
        unsigned char *q       = static_cast<unsigned char *>(mh::crt::struct_array_malloc_impl(n, sz));
        int            nonzero = 0;
        if (q == nullptr) ++nonzero;
        else
            for (uint32_t i = 0; i < n * sz; ++i)
                if (q[i] != 0) ++nonzero;
        ckf(nonzero == 0, "F/HEAP: struct_array_malloc_impl zeroes the whole block (%d nonzero)",
            nonzero);
        mh::crt::utils_free(q);
    }

    // THE WRAPPING MULTIPLY (@0x004d013e `IMUL EAX,EDX`, no overflow check). A "safer" checked
    // multiply would return NULL where the original returns a block -- a behaviour change, not a
    // bug fix -- so the wrap is asserted, both to zero and to a small nonzero.
    {
        void *z = mh::crt::struct_array_malloc_impl(0x10000u, 0x10000u); // wraps to 0 -> NULL
        ck(z == nullptr, "F/HEAP: a count*size that wraps to 0 returns NULL through the size-0 guard");
        unsigned char *w = static_cast<unsigned char *>(
            mh::crt::struct_array_malloc_impl(0x40000001u, 4u)); // wraps to 4
        int bad = (w == nullptr) ? 1 : 0;
        if (w)
            for (int i = 0; i < 4; ++i)
                if (w[i] != 0) ++bad;
        ckf(bad == 0, "F/HEAP: a request that WRAPS to 4 allocates and clears the wrapped amount");
        mh::crt::utils_free(w);
    }

    // utils_malloc_struct_array is a pure forwarder over the same body.
    {
        unsigned char *a = static_cast<unsigned char *>(mh::crt::utils_malloc_struct_array(4u, 4u));
        ck(a != nullptr && a[0] == 0 && a[15] == 0,
           "F/HEAP: utils_malloc_struct_array forwards to struct_array_malloc_impl");
        mh::crt::utils_free(a);
        ck(mh::crt::utils_malloc_struct_array(0u, 8u) == nullptr,
           "F/HEAP: ...including the size-0 answer");
    }
}

// ==================================================================================================
//  N. THE NEGATIVE ARM -- one per family, and each must be shown to go RED
// ==================================================================================================

void run_negative_cases() {
    // RAND
    {
        mh::crt::llm_srand(1u);
        g_ref_rand_seed = 1u;
        g_bad_rand_seed = 1u;
        int diff        = 0;
        for (int i = 0; i < 10000; ++i) {
            const int32_t ref = ref_llm_rand();
            if (bad_rand() != ref) ++diff;
        }
        ckf(diff > 0,
            "N/RAND: a one-bit perturbation of the LCG multiplier DIVERGES from the assembly "
            "(%d of 10000 draws) -- the sequence compare can see a difference",
            diff);
        mh::crt::llm_srand(1u);
        g_ref_rand_seed = 1u;
    }

    // MATH -- two perturbations, because the family has two distinct traps.
    {
        int diff_guard = 0;
        for (double v : MATH_EDGE)
            if (bad_sqrt_is_domain_error(v) != (ref_sqrt_is_domain_error(v) != 0)) ++diff_guard;
        ckf(diff_guard > 0,
            "N/MATH: spelling the sqrt guard `x <= 0.0` DIVERGES from the assembly's bit test "
            "(%d of %d edge values) -- -0.0 is the one it gets wrong",
            diff_guard,
            static_cast<int>(sizeof(MATH_EDGE) / sizeof(MATH_EDGE[0])));
        ck(bad_sqrt_is_domain_error(-0.0) && !ref_sqrt_is_domain_error(-0.0),
           "N/MATH: ...and concretely, -0.0 is a domain error to the wrong spelling and not to the "
           "assembly");

        int          diff_floor = 0;
        const double NF[]       = {-0.75, -0.25, 0.25, 0.75};
        for (int k = -50; k <= 50; ++k)
            for (double f : NF) {
                const double v = k + f;
                if (bits(bad_floor(v)) != bits(ref_floor(v))) ++diff_floor;
            }
        ckf(diff_floor > 0,
            "N/MATH: perturbing floor's correction constant to +1.0 DIVERGES from the assembly's "
            "FADD DAT_00506d54 on %d of 404 inputs",
            diff_floor);
    }

    // STRING -- the normalisation and the whitespace set.
    {
        StrBuf a("a"), b("c");
        ck(bad_str_cmp(a.b, b.b) != ref_utils_str_cmp(a.b, b.b),
           "N/STRING: returning the raw byte difference DIVERGES from the normalised -1/0/+1 "
           "(\"a\" vs \"c\" is -2, not -1)");
        StrBuf t("\t42");
        ck(bad_atoi(t.b) != ref_atoi(t.b),
           "N/STRING: an atoi that skips only ' ' DIVERGES on a leading tab -- the ctype table "
           "marks six whitespace bytes, not one");
    }

    // QSORT -- the substitution the whole header exists to refuse.
    {
        const uint32_t WIDTHS[]  = {2, 4, 8, 0x14, 0x16};
        const uint32_t LENGTHS[] = {16, 30, 43, 257};
        int            diff = 0, cases = 0;
        for (uint32_t w : WIDTHS)
            for (uint32_t n : LENGTHS)
                for (int dist = 1; dist < 6; ++dist) { // the duplicate-heavy distributions
                    const size_t bytes = static_cast<size_t>(n) * w;
                    fill_case(g_qs_ref, n, w, dist);
                    std::memcpy(g_qs_bad, g_qs_ref, bytes);
                    ref_qsort(g_qs_ref, n, w, reinterpret_cast<void *>(&ref_sweep_cmp));
                    bad_sort(g_qs_bad, n, w);
                    ++cases;
                    if (std::memcmp(g_qs_ref, g_qs_bad, bytes) != 0) ++diff;
                }
        ckf(diff > 0,
            "N/QSORT: a merely-CORRECT stable sort produces a DIFFERENT byte image on %d of %d "
            "duplicate-heavy cases -- the permutation of equal elements really is observable, and "
            "the byte-image compare really can see it",
            diff, cases);
    }

    // CMP
    {
        int diff = 0;
        for (int i = -32768; i <= 32767; i += 101) {
            alignas(4) unsigned char a[0x16] = {0}, b[0x16] = {0};
            const int16_t            ka = static_cast<int16_t>(i), kb = 0;
            std::memcpy(a + 4, &ka, 2);
            std::memcpy(b + 4, &kb, 2);
            if (bad_scan_target_cmp(a, b) != ref_cmp(reinterpret_cast<void *>(&ref_cmp_scan_target),
                                                     a, b))
                ++diff;
        }
        ckf(diff > 0,
            "N/CMP: reading priority_score UNSIGNED DIVERGES from the assembly's signed CMP/JLE on "
            "%d negative keys",
            diff);
    }

    // HEAP
    {
        void *z = bad_malloc(0u);
        ck(z != nullptr && mh::crt::utils_malloc(0u) == nullptr,
           "N/HEAP: dropping the size-0 guard hands back a block where the original hands back NULL "
           "-- the contract check can tell the two apart");
        std::free(z);
    }
}

} // namespace

// ==================================================================================================
//  THE SUITE
// ==================================================================================================

int run_crt_vendor_test() {
    printf("=== crttest (vendor half): crt_string / crt_math / crt_heap / crt_rand / crt_qsort "
           "against the ORIGINAL assembly ===\n");
    init_ref_tables();
    run_rand_cases();
    run_math_cases();
    run_string_cases();
    run_qsort_cases();
    run_cmp_cases();
    run_heap_cases();
    run_negative_cases();
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
