//
// mh_nettest/module_bind_compiled_in.cpp -- THE OTHER ANSWER TO "IS THERE A TRANSPORT IN THIS IMAGE?"
// (fork F4B).
//
// module_bind.cpp answers it by LOADING mh_net.dll and reporting whether that worked. That file is
// mh.dll's, and only mh.dll's: its 23 forwarding shims would collide, at link time, with the real
// bodies in net_transport.cpp.
//
// net_selftest.exe compiles net_transport.cpp DIRECTLY, and deliberately so (F4B ruling): the
// suites test the TRANSPORT, not the loader. `transporttest`, `netsessiontest`, `watchdogtest` and
// `netqueuetest` exercise sockets, framing, handshakes and queue policy -- none of them is a
// statement about GetProcAddress, and routing them through a DLL boundary would make every one of
// them depend on a deployment step to say anything. The boundary is a SHIPPING-CONFIG concern, and
// it has its own gates: check_module_bind.py (--net-surface offline, --expect on a live run),
// the module_absent suite scenario, and the six manual boot shapes docs/dll-split.md records.
//
// So the test binary needs exactly one symbol from module_bind.cpp and none of the rest: the
// predicate mh::net::transport_present() reads. Here it is, with the only honest answer for an image
// that has the transport linked into it.
//
// WHY THIS IS A FILE AND NOT A `#ifdef` IN module_bind.cpp. A preprocessor arm would have to be
// spelled in two project files anyway, and it would leave the shims' source visible in a build where
// they must not exist -- one `#endif` in the wrong place and the selftest link fails with 23
// duplicate symbols nobody can attribute. Two TUs, each in exactly one project, makes the rule
// "whoever has net_transport.cpp does not get the shims" structural: a build that took both would
// fail to link, loudly, which is the correct outcome.
//
#include "include/mh_module_bind.h"

// 1, unconditionally: the transport is in this image. It cannot fail to bind, there is nothing to
// look for, and MH_ModuleBind_Early is never called here (mh.c is not part of this binary).
extern "C" int MH_NetModule_IsBound(void) { return 1; }

// ---- and the same answer for the SPINE (fork F4D) ------------------------------------------------
//
// net_selftest.exe compiles the whole 627-TU roster directly, for exactly the reason it compiles
// net_transport.cpp directly: the suites test the SPINE, not the loader. So there is no boundary
// here -- every one of the ~100 contract rows is a real body in this image, reached by an ordinary
// call, and seams/libmh_contract.gen.cpp (the forwarding thunks) is in mh.vcxproj alone. A build
// that took both would fail to link with ~92 duplicate symbols, which is the correct outcome and
// the same structural guard the transport half above relies on.
extern "C" int MH_LibmhModule_IsBound(void) { return 1; }

// Nothing to report: the crossing counters exist to answer "did this run go through the DLL
// boundary", and in this image the question has no subject. Reporting a zero here would be a
// number that looks like the failure it is meant to detect.
extern "C" void MH_Libmh_OnPresent(void) {}
