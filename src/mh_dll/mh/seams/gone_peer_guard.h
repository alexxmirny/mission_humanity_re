//
// gone_peer_guard.h -- mp:U19i, the BYTE-PATCH CARRIER of `[net] gone_peer_frame_guard`.
//
// The guard itself (U19e) lives in libmh's promoted dispatch_packet (rx_dispatch.cpp): the leader's
// re-broadcast of a drop for an already-GONE sender is emitted into _G_LLM_NET_SEND_BUF, which is
// also the buffer the datagram being dispatched sits in, so the guard snapshots the datagram across
// the emit and puts it back. Configuration (1) -- the -net.zip drop-in players run, no libmh.dll --
// never executes that body, so this is the same fix for the original llm_net_lockstep_dispatch: the
// `call llm_net_send_lockstep_kick` at 0x0049c330 becomes a call into gone_peer_guard::thunk, which
// saves min(len, CAPACITY) bytes of the buffer, makes the displaced call, and restores them -- the
// reimpl body's exact save/restore, around the exact call it wraps.
//
// Declared here rather than kept file-local to net_lockstep.cpp so net_selftest.exe can drive the
// real thunk offline (`gpfgtest`) with a fake buffer, a fake emitter and a fake caller frame. The
// installer and its log line stay in net_lockstep.cpp beside resync_trigger_gate's carriers.
//
#pragma once

#include <stdint.h>

namespace mh::gone_peer_guard {

// 0x0049c330 in llm_net_lockstep_dispatch: `E8 E1 18 00 00` (call 0x0049dc16,
// llm_net_send_lockstep_kick), then `8B 45 C8` (mov eax,[ebp-0x38] -- the datagram length the thunk
// reads from the caller's frame). All eight bytes are the guard; only the first five are rewritten.
constexpr uintptr_t SITE        = 0x0049c330u;
constexpr uintptr_t KICK        = 0x0049dc16u; // llm_net_send_lockstep_kick, __watcall, side_id in EAX
constexpr int       LEN_EBP_OFF = -0x38;       // llm_net_lockstep_dispatch's `len` local
constexpr uint32_t  CAPACITY    = 0x3f8u;      // _G_LLM_NET_SEND_BUF, the recv cap dispatch passes
// The guard. Read off the stock EN image (MD5 b3b389e8...) and cross-checked against the committed
// call-site index (tools/data/en_addr_index.json: 0049c330 -> llm_net_send_lockstep_kick) and the
// label at 0049c335; gpfgtest re-derives the rel32 and the disp8 from SITE/KICK/LEN_EBP_OFF.
inline constexpr uint8_t EXPECT[8] = {0xE8, 0xE1, 0x18, 0x00, 0x00, 0x8B, 0x45, 0xC8};

// Targets the thunk uses. Production sets them once at install (the live buffer base and the real
// emitter); the selftest points them at a heap buffer and a fake emitter.
extern uint8_t  *g_buf;
extern uintptr_t g_kick;
// Diagnostics: how many times the thunk ran, and the last length it snapshot.
extern unsigned g_fires;
extern uint32_t g_last_len;

// The splice target. NOT callable from C++: it expects EAX = side_id and EBP = the dispatch frame,
// exactly as the displaced `call` left them.
void thunk();

} // namespace mh::gone_peer_guard
