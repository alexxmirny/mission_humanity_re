#include "hook/watcall.h"

namespace mh::hook {

int call_watcall1(uintptr_t fn, void *a) {
    int r;
    __asm {
        mov  eax, a
        call fn
        mov  r, eax
    }
    return r;
}

int call_watcall2(uintptr_t fn, void *a, void *b) {
    int r;
    __asm {
        mov  eax, a
        mov  edx, b
        call fn
        mov  r, eax
    }
    return r;
}

int call_watcall3(uintptr_t fn, void *a, void *b, void *c) {
    int r;
    __asm {
        push ebx // preserve (EBX is a __watcall arg reg here, and cdecl callee-saved)
        mov  eax, a
        mov  edx, b
        mov  ebx, c
        call fn
        mov  r, eax
        pop  ebx
    }
    return r;
}

} // namespace mh::hook
