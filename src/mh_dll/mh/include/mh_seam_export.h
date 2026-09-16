#pragma once
//
// MP seam wiring (Phase A1) -- redirect mh.exe's two dead LOBBY transport stubs onto the A0 TCP
// transport (mh_lib/net_transport.cpp). mh.exe-specific: it patches fixed VAs in the RU retail
// build. See the lobby RE (MP restoration Step 2 Phase A1).
//
// The two seams (both open with the Watcom prologue 55 89 e5 68; EAX-return):
//   llm_net_send_packet(mode,dest,buf,len) @0x004bc4f3  -- full-replaced: transmit buf[0..len-1]
//       via MH_Net_Send (mode -1 = broadcast, else unicast to dest).
//   llm_net_poll_recv() @0x004bc532                     -- full-replaced: dequeue one datagram into
//       the lobby RX buffer, tag the sender, force the dispatch CRC self-check to pass. Returns
//       0x3f8 (packet present) or 0 (nothing) in EAX.
//
#ifdef __cplusplus
extern "C" {
#endif

// Arm the seams: if mh_net.ini is present next to the exe AND the two stubs carry the expected
// Watcom prologue, install the two E9 detours and return 1. Returns 0 (inert, nothing patched) if
// the ini is absent or the prologue guard fails -- ship-safe. Call once from DllMain (byte patches
// only; no threads -> loader-lock safe). Transport threads start lazily on the first poll.
int MH_Seam_Init(void);

// The two seam bodies -- also the E9 targets. Exposed so the loopback self-test can drive the
// packet plumbing over the real transport without patching the game.
void MH_Seam_Send(int mode, int dest, unsigned char *buf, int len); // llm_net_send_packet body
int  MH_Seam_PollRecv(void);                                        // llm_net_poll_recv body

// Phase A2 -- the two IN-GAME lockstep transport seams (llm_net_transport_send @0x0049b635 /
// llm_net_transport_recv @0x0049b65b, both prologue 55 89 e5 68). Distinct from the lobby seams
// above: the mode-3 sim (llm_net_lockstep_pump -> _dispatch) carries orders/horizons through THESE,
// and the force-entry path bypasses the lobby entirely -- so these are what make a running game work.
// The game passes args in Watcom registers, so the E9 targets are register-marshalling detours, not
// these bodies directly. Broadcast on send (lockstep goes to every peer); dequeue one datagram on
// recv. Exposed for the unit test (mh_nettest).
void MH_Seam_GameSend(unsigned char *buf, int len);                         // llm_net_transport_send body
int  MH_Seam_GameRecv(int *out_sender, unsigned char *buf, int *inout_len); // llm_net_transport_recv body

// Bring the transport up on demand (role from the game's IS_HOST/LOCAL_PLAYER_INDEX -- set these
// first -- + host/port from mh_net.ini). Used by the MP force-entry to start it at the menu and gate
// the sim start on peer presence (F-gate). Idempotent.
void MH_Seam_StartTransport(void);

// S8: clear the one-shot transport-init latch (g_tried_init) so lazy_start re-runs -- used after a FAILED
// client connect (dead/typo'd IP) so a corrected IP re-attempts. MH_Net_InitEx is re-callable while stopped.
void MH_Seam_ResetTransportInit(void);

// S8(b): non-zero once a FAILED client connect has cleared the connect latches (retry-ready). The ui_drive
// `retryready` predicate gates the corrected-IP re-Connect on this, so the round-trip UI test needs no time
// wait for the ~4s connect-fail. 0 until the failure is detected; re-armed on each fresh connect kick.
int MH_Seam_S8RetryArmed(void);

// S3/S5-core: per-menu-frame driver for a manual client's real discovery -- kick the connect to the typed
// host, then re-arm the browser to list the host's received SESSION_INFO. Call every menu frame for a
// manual (non-force-entry) session; no-ops unless the local peer is a client. See net_seams.cpp.
void MH_Seam_ClientDiscoveryTick(void);

// S3 dev gate ([net] hold_start=1): non-zero => the manual host must not auto-enter the game at 2 slots
// (stays in the lobby advertising while a client browses). Superseded by S4 join-gating + U2 Start.
int MH_Seam_HoldStart(void);

// U2 (manual Start): the manual host enters only when the player clicks the lobby Start button (which
// drives llm_lobby_begin_map_load -- hooked in launch.cpp to do the host prep + broadcast FLAG_START).
// The manual client waits for that FLAG_START before entering (mp_lobby_entry_tick gates on this). 0
// until the host's Start.
int MH_Seam_ClientStartReceived(void);
/* U28: copy the host's authoritative lobby slot array (shipped with FLAG_START) into `out`.
 * Returns the byte count written, or 0 if the host sent the legacy bare signal (caller then
 * keeps its own slots). Read once, on the main thread, immediately before begin_map_load. */
int MH_Seam_TakeStartSlots(unsigned char *out, int cap);

// The mh.exe globals the seams touch. Defaults are the RU-retail VAs; override for tests via
// MH_Seam_SetAddrs so MH_Seam_PollRecv writes into a local mock RX region instead of 0x0065d66e.
typedef struct MH_SeamAddrs {
    unsigned char *rx_type;            // packet lands here: [type][crc4][payload]   (0x0065d66e)
    int           *rx_sender_id;       // sender player id (RX_TYPE-4)               (0x0065d66a)
    unsigned int  *rx_crc_computed;    // _G_LLM_LOBBY_RX_CRC32_COMPUTED             (0x0065d662)
    unsigned int  *rx_crc_embedded;    // embedded CRC field (RX_TYPE+1)             (0x0065d66f)
    unsigned int  *rx_len;             // received length latch (DAT_0065d65e)       (0x0065d65e)
    int           *local_player_index; // _G_LLM_NET_LOCAL_PLAYER_INDEX              (0x005d55ac)
    int           *is_host;            // _G_LLM_NET_IS_HOST                         (0x005d55b0)
} MH_SeamAddrs;
void MH_Seam_SetAddrs(const MH_SeamAddrs *a);

#ifdef __cplusplus
}
#endif
