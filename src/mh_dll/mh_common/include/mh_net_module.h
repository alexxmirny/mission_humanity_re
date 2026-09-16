#pragma once
//
// mh_net_module.h -- THE mh.dll <-> mh_net.dll CONTRACT (fork F4B).
//
// mh_net_export.h declares WHAT the transport does. This header declares HOW the transport is
// reached once it stops being compiled into mh.dll: the symbol list mh.dll resolves with
// GetProcAddress, the value each of those symbols answers when the module is NOT there, and the two
// module-level entries (init + probe) that are not part of the transport API at all.
//
// Read docs/dll-split.md first -- F4A ruled the mechanism (LoadLibrary on an absolute path beside
// mh.dll + GetProcAddress per symbol, as the FIRST statement of DLL_PROCESS_ATTACH, with an inert
// satellite DllMain and mh.dll as the sole orchestrator) and this is the first real satellite to use
// it. What F4B adds is the CONSEQUENCE of absence, which F4A deliberately left to each satellite.
//
// ---- WHY THE SYMBOL LIST IS A TABLE AND NOT 23 HAND-WRITTEN LINES -------------------------------
//
// The list below is expanded FOUR times from one place -- the function-pointer struct, the
// GetProcAddress loop, the forwarding definition of each MH_Net_* symbol inside mh.dll, and the
// name array the offline gate reads. A hand-maintained binder table is the G106 hand-list shape: it
// looks complete, it is checked by nobody, and the first symbol somebody forgets is a null call
// through a pointer nothing initialised. docs/dll-split.md says the same thing about F4D's 114
// symbols ("should be BOUND BY A GENERATOR, not by hand"); 23 is where that discipline starts.
//
// ---- WHY EVERY ROW CARRIES AN `ABSENT` VALUE ----------------------------------------------------
//
// This is the half F3F could not build and F4B must. Before the split there were 80 MH_Net_* call
// sites in mh.dll and 18 of them were reached with NO transport-present test in front (measured at
// the F4 surface pass: launch.cpp 12, ui_drive.cpp 1, gfx_overlay.cpp 1, harness.cpp 1, and the 3
// desync sites that ARE gated). That was survivable only because the transport was linked in, so an
// ungated call reached a real function that answered "not started". The moment the transport is a
// file that can be missing, an ungated call is a call through a null pointer.
//
// The fix is NOT 18 new `if (transport_present())` guards. Gating call sites makes absence a
// property every caller has to remember; the surface is where it belongs. So mh.dll keeps defining
// all 23 symbols -- as forwarding shims over the bound table -- and each one answers, when unbound,
// the value the real function answers when the transport is not started. Callers are unchanged,
// byte for byte, and the ungated-site count is zero BY CONSTRUCTION rather than by inspection.
// tools/check_module_bind.py --net-surface is the gate that keeps it that way.
//
// The absent values are not defaults; each is the answer the REAL body gives with g_started == 0,
// which is why an absent module and an un-started transport are indistinguishable to every caller:
//
//   MH_Net_IsStarted        0    g_started is 0
//   MH_Net_InitEx           0    "0 on failure" -- nothing to start
//   MH_Net_Send             0    "0 if not started"
//   MH_Net_Recv             0    "0 when the inbound queue is empty"
//   MH_Net_PeerCount        0    no connections
//   MH_Net_LocalPlayerId   -1    "-1 if the transport isn't up" (the header says so)
//   MH_Net_IdAssigned       1    "Always 1 on the host". THE ONE ROW THAT IS NOT ZERO-SHAPED, and
//                                the reason is a hang: launch.cpp:907 WAITS on this before entering
//                                a joined game. With no module nothing can ever assign an id, so 0
//                                would be a wait that never ends -- the degradation turning into a
//                                freeze. 1 means "there is nothing left to settle", which is true.
//   MH_Net_ActivePeerIds    0    no ids written
//   MH_Net_TakeDeadPeer    -1    "-1 if none pending"
//   MH_Net_GetStats         -    zero-fills *out; every field is a counter and 0 is its value
//   MH_Net_Send*  (6)       -    no-op; "no-op if not started" is already their contract
//   MH_Net_Set*Handler (6)  -    no-op; a handler that can never be invoked is not worth storing
//   MH_Key_Load             MH_KEY_OPEN, *out_generated = 0, out_hex empty. No transport means no
//                                session to key, so the first-run mint has nothing to mint FOR. Its
//                                one caller (net_lockstep's ensure_key_once) logs only on SECURE +
//                                generated or on INVALID, so this is silence rather than a claim.
//
#ifndef MH_NET_MODULE_H
#define MH_NET_MODULE_H

#include "mh_net_export.h"
#include "mh_net_key.h"

#ifdef __cplusplus
extern "C" {
#endif

// Bumped whenever anything in this header or mh_net_export.h changes shape. mh.dll REFUSES a
// module whose ABI disagrees rather than calling into it: two DLLs that ship separately can be
// mismatched by a player copying one file, and a silent mismatch is a crash in somebody else's
// stack frame. (Same discipline as libmh_set_host_api, and as the F4A spike it replaces.)
#define MH_NET_MODULE_ABI 0xF4B00001u

// What mh.dll hands the module at bind time. ONE FIELD TODAY, and it is the whole of Q1's residue:
// run_context.cpp stays mh.dll-side (F4 ruling Q1 -- 11 of its 13 consumers are core/harness, and
// config (1) without the transport must still get its logs), so the module cannot call MH_RunDir().
// It is GIVEN the directory instead, which is also the rule the F4A spike established for every
// satellite: the satellite owns no path, no ini and no file handle of its own, because exactly one
// place in the process knows where this run's logs live.
typedef struct MH_NetModuleHost {
    unsigned    size;    /* sizeof(MH_NetModuleHost) as MH.DLL was compiled                  */
    unsigned    abi;     /* MH_NET_MODULE_ABI as MH.DLL was compiled                         */
    const char *run_dir; /* "<exedir>\logs\<runid>_<role>\" -- borrowed, valid for the run    */
} MH_NetModuleHost;

// What the module's own DllMain observed. The R2 measurement, kept for the real satellite and not
// only for the spike that proved it: a statically-imported module's DLL_PROCESS_ATTACH runs BEFORE
// its importer's, and that ordering is the fact "every satellite DllMain must be inert" rests on.
// Reading it every boot is how a build that silently acquired a static import gets caught by its own
// log instead of by a 0xC0000409 nobody can reproduce.
typedef struct MH_NetModuleProbe {
    unsigned  size;         /* sizeof(MH_NetModuleProbe) as the MODULE was compiled          */
    unsigned  abi;          /* MH_NET_MODULE_ABI as the MODULE was compiled                  */
    unsigned  attach_calls; /* DLL_PROCESS_ATTACH count -- must be exactly 1                 */
    unsigned  attach_tid;   /* the thread its DllMain ran on (compare with mh.dll's)         */
    unsigned  init_calls;   /* MH_NetModule_Init count -- must be exactly 1                  */
    long long attach_qpc;   /* QueryPerformanceCounter at DLL_PROCESS_ATTACH                 */
} MH_NetModuleProbe;

/* Hand the module its run context. Returns MH_NET_MODULE_ABI (the module's own), or 0 if `host` is
 * malformed. Called by mh.dll's DllMain immediately after the exports resolve, before any transport
 * call. Safe to call once; a second call is ignored. */
int MH_NetModule_Init(const MH_NetModuleHost *host);

/* Fill *out with the DllMain observations above. Safe to call before MH_NetModule_Init. */
void MH_NetModule_Probe(MH_NetModuleProbe *out);

#ifdef __cplusplus
}
#endif

// ---- THE BOUND SURFACE ---------------------------------------------------------------------------
//
// X(ret, name, PARAMS, ARGS, ABSENT)
//   PARAMS  the declaration's parameter list, parenthesised
//   ARGS    the same names as a call's argument list, parenthesised
//   ABSENT  a statement executed INSTEAD of the forwarded call when the module is not bound. It
//           must end in `return` for a value-returning row, and may be empty-ish (`(void)0;`) for a
//           void one. Writing it here, next to the signature, is what makes "what does this answer
//           with no module" a property of the contract rather than of whoever wrote the shim.
//
// ORDER IS THE COMMITTED ORDER: mh_net.def, the gate's expectations and the bind log's export count
// all read this list, so a row added in the middle is a one-line diff everywhere rather than four.
#define MH_NET_MODULE_SYMBOLS(X)                                                                    \
    X(int, MH_Net_InitEx, (const MH_NetConfig *cfg), (cfg), { return 0; })                          \
    X(int, MH_Net_Send, (int dst_player, const void *buf, int len), (dst_player, buf, len),         \
      { return 0; })                                                                                \
    X(int, MH_Net_Recv, (int *out_sender, void *buf, int *inout_len), (out_sender, buf, inout_len), \
      { return 0; })                                                                                \
    X(int, MH_Net_PeerCount, (void), (), { return 0; })                                             \
    X(int, MH_Net_LocalPlayerId, (void), (), { return -1; })                                        \
    X(int, MH_Net_IdAssigned, (void), (), { return 1; })                                            \
    X(int, MH_Net_ActivePeerIds, (int *out, int cap), (out, cap), {                                 \
        (void)out;                                                                                  \
        (void)cap;                                                                                  \
        return 0;                                                                                   \
    })                                                                                              \
    X(int, MH_Net_TakeDeadPeer, (void), (), { return -1; })                                         \
    X(int, MH_Net_IsStarted, (void), (), { return 0; })                                             \
    X(void, MH_Net_GetStats, (MH_NetStats * out), (out), {                                          \
        if (out) {                                                                                  \
            out->tx_pkts = out->tx_bytes = out->rx_pkts = out->rx_bytes = 0;                        \
            out->last_rx_tick                                           = 0;                        \
            out->dropped                                                = 0;                        \
            out->peers                                                  = 0;                        \
        }                                                                                           \
        return;                                                                                     \
    })                                                                                              \
    X(void, MH_Net_SetSessionInfoHandler, (MH_SessionInfoCb cb), (cb), { return; })                 \
    X(void, MH_Net_SendSessionInfo, (const unsigned char *buf, int len), (buf, len), { return; })   \
    X(void, MH_Net_SetJoinHandler, (MH_JoinCb cb), (cb), { return; })                               \
    X(void, MH_Net_SendJoin, (const unsigned char *buf, int len), (buf, len), { return; })          \
    X(void, MH_Net_SetStartHandler, (MH_StartCb cb), (cb), { return; })                             \
    X(void, MH_Net_SendStart, (const unsigned char *buf, int len), (buf, len), { return; })         \
    X(void, MH_Net_SetLeaveHandler, (MH_LeaveCb cb), (cb), { return; })                             \
    X(void, MH_Net_SendLeave, (void), (), { return; })                                              \
    X(void, MH_Net_SetAnnounceHandler, (MH_AnnounceCb cb), (cb), { return; })                       \
    X(void, MH_Net_SendAnnounce, (const unsigned char *buf, int len), (buf, len), { return; })      \
    X(void, MH_Net_SetHashHandler, (MH_HashCb cb), (cb), { return; })                               \
    X(void, MH_Net_SendHash, (const unsigned char *buf, int len), (buf, len), { return; })          \
    X(int, MH_Key_Load,                                                                             \
      (const char *dir, unsigned char *out_key, char *out_hex, int *out_generated),                 \
      (dir, out_key, out_hex, out_generated), {                                                     \
          (void)dir;                                                                                \
          (void)out_key;                                                                            \
          if (out_hex) out_hex[0] = '\0';                                                           \
          if (out_generated) *out_generated = 0;                                                    \
          return MH_KEY_OPEN;                                                                       \
      })

// The count, derived from the list rather than written next to it (a hand-kept count is the same
// G106 shape one level down). Used by the bind log and by the gate.
#define MH_NET_MODULE_COUNT_ONE(r, n, p, a, ab) +1
#define MH_NET_MODULE_SYMBOL_COUNT              (0 MH_NET_MODULE_SYMBOLS(MH_NET_MODULE_COUNT_ONE))

#endif // MH_NET_MODULE_H
