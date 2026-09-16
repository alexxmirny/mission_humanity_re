//
// fp/st0_call.h -- the ONE naked thunk that calls an original with its argument already on ST0.
//
// SEPARATE FROM x87.h AND x87_shapes.h ON PURPOSE. Those hold NUMERIC idioms -- sequences that
// compute something, and that a C++ expression could in principle replace. This is not one of those:
// it is a CALLING CONVENTION, the ST0-argument sibling of `mh::call::detail::s_f64_S4/S8` in
// addr/mh_calls.gen.cpp, and no amount of measurement will ever turn it into C++. Filing it with the
// truncation helpers would put a permanent resident in a module whose whole point is a queue of
// replacement candidates. It lives in mh/fp/ because the argument's home is the FPU stack.
//
// WHY IT EXISTS AT ALL. llm_math_fsin_reduce_loop @0x004dabc6 and llm_math_cos_impl @0x004dabbc take
// their argument IMPLICITLY on ST0. Ghidra's watcomcpp cspec models a float/double RETURN via ST0
// but has no concept of an ST0 PARAMETER, so no committed prototype and no mh_calls.gen.h shape can
// express "call this with the argument already on the FPU stack". These are the ONLY two such call
// sites in the whole committed call surface (checked tools/data/dll_call_protos.json: zero other
// functions use ST0 as a parameter storage), so a hand-written thunk is the right-sized fix --
// teaching gen_dll_calls.py's shape-derivation engine a population of two would be the wrong
// direction per the repo's script policy.
//
// Shape modelled directly on mh::call::detail::s_f64_S4/S8: same prologue/epilogue, same
// ESP-discipline-agnostic restore via `lea esp, [ebp-12]`, except the argument is FLD'd onto the FPU
// stack instead of written to an outgoing frame slot. The callee's own return is already ST0, which
// the naked thunk's __cdecl double return leaves untouched.
//
// ---- A CENSUS BLIND SPOT, RECORDED WHERE IT IS VISIBLE -------------------------------------------
//
// The two callers below pass a LITERAL VA. tools/gen_va_census.py routes a site by finding the text
// `mh::call::` in a libmh-destined module, so these two are outward VA calls that the census does not
// count and no owning item covers. That is the same shape recorded elsewhere -- an address
// passed as a VALUE is invisible to the tools that look for a named dependency -- except here it also
// hides from the source-level census, not just from the linker. They are real: a standalone libmh
// reaching either of them jumps into unmapped memory. Tracked on LIB-VA0; do not read the census's
// total as "every VA call in libmh" while these two exist.
//
#pragma once
#include <cstdint>

namespace mh::fp {

// FP-ASM-KEEP: NEVER -- a CALLING CONVENTION (an ST0-argument thunk), not a numeric idiom, so no
//   measurement can turn it into C++. It also leaves the standalone artifact entirely: both call
//   sites are MH_LIBMH_BUILD-guarded onto the vendored crt_math.h bodies.
__declspec(naked) inline double __cdecl call_f64_st0_arg(uintptr_t fn, double angle) {
    // clang-format off  (one instruction per line; clang-format would fold the __asm block)
    __asm {
        push ebp
        mov  ebp, esp
        push ebx
        push esi
        push edi
        fld  qword ptr [ebp + 12]
        call dword ptr [ebp + 8]
        lea  esp, [ebp - 12]
        pop  edi
        pop  esi
        pop  ebx
        pop  ebp
        ret
    }
    // clang-format on
}

} // namespace mh::fp
