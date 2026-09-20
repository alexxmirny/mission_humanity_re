#pragma once
//
// MP transport -- C interface to the TCP client-server star (mh_net/net_transport.cpp).
// Part of the multiplayer-restoration vehicle A (Step 2; the lobby RE).
//
// SINCE FORK F4B THIS IS A CROSS-MODULE INTERFACE. The bodies live in mh_net.dll, which mh.dll loads
// with LoadLibrary + GetProcAddress from DLL_PROCESS_ATTACH and which can simply be absent; inside
// mh.dll every name below is a forwarding shim (mh/seams/module_bind.cpp) that answers a DEFINED
// value when the module is not bound. Callers are unchanged and do not need to know either fact --
// but if you ADD a function here, add it to mh_common/include/mh_net_module.h's symbol table and to
// mh_net/mh_net.def in the same commit, or tools/check_module_bind.py --net-surface will say so.
// The contract, the absent values and the reasoning are in mh_net_module.h.
//
// The intact-but-dead-at-the-wire mh.exe lobby + lockstep engine carries its own CRC/framing/
// dispatch, so this transport is a DUMB DATAGRAM PIPE: send a buffer to a player id (or broadcast),
// and dequeue received buffers tagged with the sender's player id. Topology is a client-server
// STAR: the host listens and RELAYS; every client connects to the host. TCP gives reliable ordered
// delivery for free -- exactly what a lockstep order stream needs (no ARQ to build).
//
// Two connection modes, same seam code (the MP design invariants):
//   direct  -- host binds/listens, client connects to the host IP (LAN / port-forward; v1).
//   relayed -- (future, Milestone R) every peer connects OUT to a dedicated relay server.
// A0 implements the direct LAN path only.
//
// LOADER-LOCK NOTE: MH_Net_InitEx starts background threads, so it must NOT be called from DllMain
// (DLL_PROCESS_ATTACH holds the loader lock). Call it from a lobby-open hook instead. The A0
// self-test calls it from a normal main(), which is fine. F4B does not weaken this: mh.dll BINDS
// mh_net.dll in DllMain, which maps a file and resolves symbols; it starts nothing.
//
#ifdef __cplusplus
extern "C" {
#endif

#define MH_NET_BROADCAST   (-1) /* dst id meaning "every other player"                  */
#define MH_NET_MAX_PAYLOAD 2048 /* max bytes per datagram (mh.exe uses <=~1024)          */
#define MH_NET_MAX_PEERS   8    /* host: max simultaneous clients (matches ~8 lobby slots)*/

/* Transport configuration. Filled by the caller and passed to MH_Net_InitEx -- net_seams.cpp's
 * lazy_start (from mh_net.ini plus the lobby's own role/ip), or the self-test. */
typedef struct MH_NetConfig {
    int  role;        /* 0 = host, 1 = client                                              */
    char host[64];    /* client: connect target (IPv4/hostname); host: bind addr ("" = all)*/
    int  port;        /* TCP port                                                          */
    int  player_id;   /* this peer's player id (host is conventionally 0)                  */
    int  peers;       /* host: expected client count (informational / logging only)        */
    int  log;         /* 1 = append diagnostics to mh_net.log next to the exe             */
    int  host_assign; /* N1: host auto-assigns each joiner the first-free id + WELCOMEs it */
                      /*     (client learns its OWN id -> hand-clicked N-player). 0 = the   */
                      /*     declared-id behaviour (each peer's player_id is authoritative).*/
    /* Link liveness (R-live). A silent TCP connection is indistinguishable from a healthy idle one,
     * which is how a 2026-07-26 internet game ran for 90 s -- and STARTED a match -- against a peer
     * whose socket was already gone. So the transport generates its own traffic and times out on the
     * absence of the peer's. Both are milliseconds; 0 disables that half. */
    int ping_ms;       /* send a FLAG_PING on every idle conn this often (0 = never)         */
    int rx_timeout_ms; /* drop a conn with NO inbound bytes for this long (0 = never)        */
    /* mp:R7a -- THE RELAY DIAL, DECIDED PER DIAL BY mh.dll, not by the module reading [net] relay.
     * Before R7a a peer with `[net] relay` set had no direct mode at all: mh_net_udp.dll read the key
     * itself and tunnelled EVERY connection, so a typed *Internet server* address was dead weight. Now
     * mh.dll reads the ini once and decides, per dial, whether THIS connection is relayed -- and hands
     * the answer here. `relay_addr` empty is a DIRECT dial (the module dials `host:port`, no tunnel),
     * even when the ini still carries `relay=`; a non-empty `relay_addr` is a relayed dial through that
     * relay in `relay_room`. A zero-initialised config is therefore a direct dial ("absent = no relay"),
     * which is what every self-test and the force-entry path get for free.
     *
     * ABI-APPEND ONLY. These two fields are the last members on purpose: the TCP module (mh_net.dll,
     * net_transport.cpp) has no relay and never reads them, and any reader compiled before R7a addresses
     * the fields ABOVE at unchanged offsets -- so it ignores the trailing bytes rather than misreading a
     * moved one. (MH_NET_MODULE_ABI is bumped in the same change so a mismatched pair is refused at bind,
     * per this contract's shape-change rule, rather than left to depend on that append-safety.) */
    char     relay_addr[80]; /* "host:port" of the relay to tunnel through, or "" for a DIRECT dial     */
    unsigned relay_room;     /* the room to dial. host: 0 (the module mints one per tunnel, mp:R6);     */
                             /* relayed client: the directory's pick (mp:R2). Unused on a direct dial.  */
} MH_NetConfig;

/* Start the transport with an explicit config (host: bind+listen+accept thread; client: connect +
 * recv thread). Returns 1 on success, 0 on failure. Idempotent-ish: a second call is ignored.
 * With no module bound it returns 0, which is what every caller already handles. */
int MH_Net_InitEx(const MH_NetConfig *cfg);

/* MH_Net_Init WAS HERE (parse mh_net.ini, then MH_Net_InitEx from it) and fork F4B DELETED IT: zero
 * call sites in the whole tree, and net_seams.cpp's lazy_start has read the same [net] keys itself
 * since the ship build -- two drifting readers of one config block. See the note at its old home in
 * mh_net/net_transport.cpp. */

/* Send a datagram to dst_player (or MH_NET_BROADCAST for all other players). On the host this routes
 * directly to the client socket(s); on a client it goes to the host, which relays. Returns 1 if the
 * transport is running (best-effort send), 0 if not started / bad length. */
int MH_Net_Send(int dst_player, const void *buf, int len);

/* Dequeue one received datagram. On entry *inout_len = buffer capacity; on a successful pop, sets
 * *out_sender to the origin player id, *inout_len to the payload length, and returns 1. Returns 0
 * when the inbound queue is empty. Non-blocking; poll it. */
int MH_Net_Recv(int *out_sender, void *buf, int *inout_len);

/* Currently-connected peer count (host: active clients; client: 1 if connected, else 0). */
int MH_Net_PeerCount(void);

/* This peer's own player id (g_my_id) -- from [net] player_id (host default 0, client default 1), OR the
 * host-assigned id once a WELCOME has arrived (host_assign mode). N1: each peer derives its PlayerSide /
 * local slot index from THIS, not a hardcoded is_host?0:1, so a >2-player game seats each client at its
 * own distinct slot. Returns -1 if the transport isn't up. */
int MH_Net_LocalPlayerId(void);

/* 1 once this client's id is settled -- immediately for a host or a declared-id client, or (in host_assign
 * mode) only after the host's WELCOME has assigned it. The lobby glue waits for this before entering, so a
 * client never seats itself at the wrong (default) slot. Always 1 on the host. */
int MH_Net_IdAssigned(void);

/* N2: enumerate the ACTIVE connections' transport player ids (host side). Fills out[] with each active
 * conn's id (>=1; a conn still awaiting its id in declared-id mode is skipped), returns the count. The
 * lobby peer-table mirror uses THIS instead of a positional 1..N -- the id is what the client uses as its
 * own slot index and stamps as the sender of every lobby push, so the host slot it allocates carries
 * player_id == that id == the client's local index. Positional numbering breaks the moment the live id set
 * isn't {1..N} (a MIDDLE peer leaves, or host-assign hands out a non-contiguous id). */
int MH_Net_ActivePeerIds(int *out, int cap);

/* U17: return + CLEAR the most-recently-disconnected peer's transport player_id (== game side_id), or -1
 * if none pending. Set by the recv thread when a conn drops (active->0). The host learns real client ids
 * (>=1) so this yields a droppable side_id; a client's only conn (to the host) carries player_id -1, so a
 * host death returns -1 here -> the caller (main-thread on_time_tick) fast-drops CLIENT departures only,
 * never a host death (which is a relay-topology problem, out of U17 scope). One-shot (Interlocked). */
int MH_Net_TakeDeadPeer(void);

/* ---- per-peer link latency (mp:T3) ---------------------------------------------------------------
 * What a transport that MEASURES its link can say about each peer. Carried inside MH_NetStats rather
 * than behind a 24th export because it is the same snapshot, read by the same caller, on the same
 * frame -- and because `check_module_bind.py --net-surface` gates the SYMBOL list: a number that
 * rides an existing symbol cannot drift away from the binder table.
 *
 * NOT EVERY TRANSPORT CAN FILL THIS IN, and the struct says which rather than answering zeros. The
 * TCP module (mh_net.dll) has no channel B -- its FLAG_PING is an empty keepalive and TCP's own
 * retransmits make RFC 7680 loss meaningless anyway (a drop shows up as added delay, never as a
 * missing sequence number) -- so it reports
 * `lat_supported = 0` and the lockstep log prints `n/a` in these columns. Zeros would read as "a
 * perfect 0 ms link", which is the one answer worse than no answer.
 *
 * Units are integers on purpose: this crosses a DLL boundary and is printed by wsprintfA, which has
 * no float conversion. Microseconds keep the RFC 6298 smoothing's sub-millisecond resolution intact
 * across the ABI; the log rounds to ms on the way out. */
typedef struct MH_NetPeerLatency {
    int player_id; /* the peer's transport player id, or -1 when the transport has not learnt it  */
    int samples;   /* RTT samples folded in; 0 = nothing measured on this peer yet                */
    int srtt_us;   /* RFC 6298 smoothed RTT, microseconds                                          */
    int rttvar_us; /* RFC 6298 smoothed deviation, microseconds                                    */
    int ipdv_us;   /* smoothed consecutive-sample delay variation, microseconds (RFC 3393 shape)   */
    int loss_pm;   /* RFC 7680 loss over a 256-packet sequence window, per mille; -1 = not yet     */
} MH_NetPeerLatency;

/* Transport diagnostics snapshot (for the lockstep timing log). tx = the local game's outbound game
 * frames (not host relays); rx = inbound DATA frames off the wire; last_rx_tick = GetTickCount() at
 * the last rx (0 if none). Counters are racy (no lock) -- fine for a timing trace. */
typedef struct MH_NetStats {
    long     tx_pkts, tx_bytes;
    long     rx_pkts, rx_bytes;
    unsigned last_rx_tick;
    long     dropped;
    int      peers;
    /* mp:T3. `lat_supported` 0 means every `lat[]` entry is meaningless, not that the link is
     * perfect. `lat_count` is how many entries were filled, in the transport's own peer-slot order
     * (NOT the lockstep horizon-slot order that `peer0_ms`/`peer1_ms` use; on a 2-player game there
     * is exactly one peer, so the distinction does not arise). */
    int               lat_supported;
    int               lat_count;
    MH_NetPeerLatency lat[MH_NET_MAX_PEERS];
} MH_NetStats;
void MH_Net_GetStats(MH_NetStats *out);

/* 1 if the transport has been started (Init/InitEx succeeded), else 0. */
int MH_Net_IsStarted(void);

/* ---- discovery: SESSION_INFO control channel (S2) ------------------------------------------------
 * A host advertises its session (game name + lobby-id tag + map + player counts) as an out-of-band
 * FLAG_SESSION_INFO control frame. It is NOT delivered to the game queue (MH_Net_Recv) -- it is routed
 * to a registered handler instead, so discovery traffic never perturbs the lockstep input stream.
 * Payload = a serialized mh_net_proto::SessionInfo (see src/mh_net_proto/include/mh_net_proto/session_info.h). */
typedef void (*MH_SessionInfoCb)(int sender, const unsigned char *buf, int len);

/* Register the handler invoked (on the recv thread) for each received SESSION_INFO frame. NULL clears. */
void MH_Net_SetSessionInfoHandler(MH_SessionInfoCb cb);

/* Broadcast a SESSION_INFO control frame (host -> all connected clients). Best-effort; no-op if not
 * started. `buf`/`len` = the serialized descriptor. */
void MH_Net_SendSessionInfo(const unsigned char *buf, int len);

/* ---- discovery: JOIN control channel (S4) -------------------------------------------------------
 * A client asks to join a lobby it saw in the browser by sending a FLAG_JOIN control frame carrying
 * the lobby-id (game name + tag) -- identity, not address. Like SESSION_INFO it is routed to a handler,
 * NEVER the game queue, so it cannot perturb the lockstep. The host validates the lobby-id against its
 * own advertised session and, on a match, admits the sender (slots it + starts the map/slot flood).
 * Payload = a serialized mh_net_proto::JoinRequest (see session_info.h). */
typedef void (*MH_JoinCb)(int sender, const unsigned char *buf, int len);

/* Register the handler invoked (on the recv thread) for each received JOIN frame. NULL clears. */
void MH_Net_SetJoinHandler(MH_JoinCb cb);

/* Send a JOIN control frame (client -> host). Best-effort; no-op if not started. `buf`/`len` = the
 * serialized JoinRequest. */
void MH_Net_SendJoin(const unsigned char *buf, int len);

/* ---- start: host-clicked-Start control channel (U2) ---------------------------------------------
 * When the host clicks Start in the lobby, it broadcasts a FLAG_START control frame so each client
 * enters the game at the host's command instead of auto-entering on slot-sync. Routed to a handler,
 * never the game queue (determinism-safe like SESSION_INFO/JOIN).
 * U28 (2026-08-29): the payload is NO LONGER EMPTY -- it carries the host's AUTHORITATIVE lobby slot
 * array (8 x 0x39 bytes), snapshotted at the instant of the click. It used to be a bare signal, and
 * each peer then built Players[] from its OWN _G_LLM_LOBBY_SLOTS: two independently-maintained copies,
 * so any client slot edit still in flight at Start survived on the client and was dropped on the host
 * -- a step-1 desync with no warning on either side (measured in the 2026-08-28 internet game, where
 * the client's race change was lost host-side in the same millisecond as the snapshot). Shipping the
 * array makes the host's copy the only one that decides. Length is validated by the receiver; a
 * zero-length frame is still accepted as the bare legacy signal. */
typedef void (*MH_StartCb)(int sender, const unsigned char *buf, int len);

/* Register the handler invoked (on the recv thread) when a FLAG_START frame arrives. NULL clears. */
void MH_Net_SetStartHandler(MH_StartCb cb);

/* Broadcast a FLAG_START control frame (host -> all clients), carrying `len` bytes of `buf` (the
 * authoritative lobby slot array). Pass NULL/0 for the legacy bare signal. Best-effort; no-op if not
 * started. */
void MH_Net_SendStart(const unsigned char *buf, int len);

/* ---- leave: client-left-the-lobby control channel (U12) -----------------------------------------
 * When a client navigates out of the lobby back to the discovery browser (Cancel) while staying
 * connected, retail sends the host nothing (net_udp's dead job), so the host would keep a stale slot.
 * The client sends a FLAG_LEAVE control frame; the host frees that player's lobby slot. Routed to a
 * handler, never the game queue. Empty payload -- the sender player id is the signal. */
typedef void (*MH_LeaveCb)(int sender);

/* Register the handler invoked (on the recv thread) when a FLAG_LEAVE frame arrives. NULL clears. */
void MH_Net_SetLeaveHandler(MH_LeaveCb cb);

/* Send a FLAG_LEAVE control frame (client -> host). Best-effort; no-op if not started. */
void MH_Net_SendLeave(void);

/* ---- announce: host->all "player joined/left the lobby" text (U16) -------------------------------
 * The retail admin loop announces a join/leave only LOCALLY (host) + unicasts a type it can't act on
 * to the affected peer, so other clients never show the "<name> joined/left" line. The host broadcasts
 * a FLAG_ANNOUNCE control frame ({is_join:1, name[..]}) to every peer; each client's handler enqueues
 * it and the main thread renders it via llm_lobby_announce_line. Routed to a handler, NEVER the game
 * queue -- pure lobby text, determinism-neutral. */
typedef void (*MH_AnnounceCb)(int sender, const unsigned char *buf, int len);

/* Register the handler invoked (on the recv thread) when a FLAG_ANNOUNCE frame arrives. NULL clears. */
void MH_Net_SetAnnounceHandler(MH_AnnounceCb cb);

/* Broadcast a FLAG_ANNOUNCE control frame to every connected peer (host -> all). Best-effort. */
void MH_Net_SendAnnounce(const unsigned char *buf, int len);

/* ---- desync samples: runtime state-hash exchange (D21) -------------------------------------------
 * Each peer periodically broadcasts a (step, state-hash, per-region hashes) sample so the OTHER peers
 * can compare it against their own value for that same sim step and say so when a match has diverged.
 * Routed to a handler, NEVER the game queue -- the sample must not perturb the lockstep input stream,
 * and the game's own wire cannot carry it: the retail dispatcher treats an unknown outer tag as a
 * garbled stream and ends the session. Unlike the lobby control frames the HOST RELAYS this one to the
 * other clients, so every pair in an N-peer run is compared, not just each client against the host.
 * Payload = mh::desync::sample_wire (src/mh_dll/mh/desync/desync_watch.h).
 * The handler runs on the RECV THREAD: queue, do not touch game state. */
typedef void (*MH_HashCb)(int sender, const unsigned char *buf, int len);

/* Register the handler invoked (on the recv thread) for each received HASH frame. NULL clears. */
void MH_Net_SetHashHandler(MH_HashCb cb);

/* Broadcast a desync sample to every connected peer. Best-effort; no-op if not started. */
void MH_Net_SendHash(const unsigned char *buf, int len);

/* MH_Net_Shutdown WAS HERE (close the sockets, join the watchdog thread) and fork F4B DELETED IT
 * for the same reason: zero call sites, ever. The game exits the process; a teardown path that has
 * never once run is untested code with a plausible name, not a feature. If an orderly stop is wanted
 * it gets one something calls, and a test. */

#ifdef __cplusplus
}
#endif
