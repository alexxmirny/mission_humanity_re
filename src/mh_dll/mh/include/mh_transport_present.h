#pragma once
//
// mh/include/mh_transport_present.h -- IS THERE A NETWORK TRANSPORT IN THIS PROCESS? (fork F3F)
//
// ---- WHY THIS IS NOT `[net] enable` -------------------------------------------------------------
//
// F3B demoted `[net] enable` from "arm nothing" to "skip the nine net steps". That key is an
// OPERATOR's switch: the transport is here, and this run is asked not to use it. F3F adds the other
// half, which is a question about the BUILD rather than about the run -- is there a transport in
// this process at all? Today the answer is almost always yes, because net_transport.cpp is linked
// into mh.dll; at F4 it becomes mh_net.dll and the answer becomes "did that module load".
//
// The two axes are deliberately separate and neither implies the other:
//
//   module=none, enable=1  -- there is nothing to enable. The net arm says so and skips.
//   module=auto, enable=0  -- the transport is here and this run declines it (F3B's lane).
//
// Conflating them is what F3B spent an item undoing at one level up: a single key that means both
// "off" and "absent" cannot tell a caller which of the two it is looking at, and the caller that
// has to know is the UI -- ruling Q2 says the NETWORK GAME button still arms with no module and the
// browser carries a notice saying why, which is a different sentence from "you switched it off".
//
// ---- WHAT F4B RE-POINTED (and it is done: this is no longer a prediction) -----------------------
//
// `transport_present()` is the WHOLE surface. Callers ask it and branch; none of them knows how it
// is answered. F3F answered it by reading `[net] module`, because the transport was compiled into
// mh.dll and so could not actually be absent. F4B moved the transport into mh_net.dll, and this
// predicate now reports THE LOAD RESULT -- the same bool, the same call sites, no reshaping:
//
//     MH_NetModule_IsBound()   mh/seams/module_bind.cpp, filled by MH_ModuleBind_Early from
//                              DLL_PROCESS_ATTACH (F4A's mechanism; docs/dll-split.md)
//
// `[net] module` survives the move with its meaning intact and has become smaller, not larger: it is
// read ONCE, by the bind, and `none` means "do not even attempt the load" -- which is exactly what
// it always documented ("behave as though the module is not here"). It is no longer this header's
// business, which is why the ini read that used to be here is gone. The DEFAULT is still `auto`, so
// an install with no ini gets ship behaviour, and a TYPO IS STILL REFUSED (F2E's rule) -- at the
// bind, in module_bind.cpp, where the key is now read.
//
// TWO WAYS TO HAVE NO TRANSPORT, and they stay distinguishable ON PURPOSE even though this predicate
// collapses them: `NOT ATTEMPTED` (the run declined) and `NOT BOUND` (the file is not there) are
// different `; [modules] mh_net:` lines at the head of mh_net.log. The predicate answers the
// question its callers actually have -- "is there a transport" -- and the log answers "why not",
// which is the only place that distinction is useful.
//
// ---- THE ANSWER IS NOT CACHED HERE ANY MORE, AND THAT IS THE G104 ARGUMENT SATISFIED, NOT DROPPED
//
// F3F cached the ini read in a function-local static because two consumers (the desync instrument,
// the net arm) run at different points of the boot and one of them predates `g_ini` -- a predicate
// that depended on another module's init would answer differently at different call points and the
// disagreement would be invisible in a green run. The bind result is STRONGER than that: it is
// settled in DLL_PROCESS_ATTACH, before MH_Core_Arm_Early and therefore before every consumer, and
// it never changes afterwards. Reading it directly is the same constant answer at every call point,
// with no second copy of it to get out of step.
//
#ifdef MH_LIBMH_BUILD

namespace mh::net {
// A standalone host has no game binary, no lobby and no mh_net.ini, so there is no file to read and
// nothing under libmh asks this question today. It answers FALSE rather than TRUE because the
// honest statement about a libmh process is "mh.dll's transport is not in this address space"; a
// standalone host that grows one must answer for its own host rather than read a key it has no file
// for.
constexpr bool transport_present() { return false; }
} // namespace mh::net

#else

extern "C" int MH_NetModule_IsBound(void); // mh/seams/module_bind.cpp (mh.dll) --
                                           // mh_nettest/module_bind_compiled_in.cpp (net_selftest.exe)

namespace mh::net {

// Is there a network transport in this process? Since F4B: did mh_net.dll load and resolve its whole
// 23-symbol contract. In net_selftest.exe, where the transport is compiled in, the answer is a
// constant 1 -- see module_bind_compiled_in.cpp for why that build gets its own definition.
inline bool transport_present() { return MH_NetModule_IsBound() != 0; }

} // namespace mh::net

#endif
