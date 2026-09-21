# The UDP relay

Settled facts only. This is the relay `mp:R1` built, the **session directory** `mp:R2` added on top
of it, and the client half that dials both: a single-port Rust forwarder (`src/relay`) plus a
loopback tunnel inside `mh_net_udp.dll` (`src/mh_dll/mh_net_udp/udp_relay.cpp`). The decision that
produced it is **D4** of the MP refinement plan (2026-09-17) with the user's **relay-first** ruling:
a match starts relayed and is promoted to a direct path later (`mp:R3`), rather than trying direct
first and falling back.

- Relay binary: `src/relay/src/main.rs` (CLI + socket loop), `relay.rs` (rooms/peers/sessions),
  `leg.rs` (the envelope), `wire.rs` (T0, shared with the C++ side)
- Client half: `src/mh_dll/mh_net_udp/udp_relay.{h,cpp}`, hooked from `udp_transport.cpp`
- The packet it carries: the MP UDP wire format page (T0)
- Oracles: `cargo test -p mh_relay`, and the determinism gate over `[net] transport=udp` + `relay=`

## What the relay is, in one paragraph

One bound UDP socket. Peers register into a **room**; the relay pairs a room's host with its clients
and forwards between them. Every datagram on a peer↔relay link carries a small **authenticated leg
envelope** whose payload is the sealed T0 datagram, byte for byte. The relay reads the T0 header's
`conn_id` — the only field it can read without a key — to decide *what it is allowed to do* with a
payload, and it reads the leg header to decide *where the payload goes*. It opens each session's
connect token, keeps the two MAC keys and discards both encryption keys, so it can prove a datagram
is the registered peer's and cannot read a byte of the match.

## Why routing is by a peer handle and not by `conn_id` alone

The item's sketch says "demux on the 8-byte connection id". Two things make that insufficient on its
own, and both are properties of the transport as it already exists:

- **The PSK handshake has no per-peer `conn_id`.** `udp_endpoint.cpp` opens a connection by sending
  its HELLO on `boot_conn = HMAC(psk, "mh-udp-boot-conn")`, a value derived from the shared key
  alone — so it is *the same eight bytes for every peer of every match*. A relay demuxing on that
  field could not tell two joining clients apart.
- **The host cannot tell them apart either, through one address.** `host_on_boot` matches a
  handshake in flight by SOURCE ADDRESS, because at that moment no `conn_id` exists yet. Two clients
  arriving through one relay address would read as one client retransmitting, and the second join
  would never complete — the same defect `alloc_conn_slot`'s comment records from the first
  three-peer loopback run, reached by a different route.

So the leg header carries a **stable 16-bit peer handle**, assigned at registration, and *that* is
the routing key. It keeps the property the `conn_id` was put in T0 for — routing does not depend on a
source address, so a NAT rebinding is survivable (the relay adopts a new address only after the leg
tag verifies) — while working for the pre-`conn_id` phase as well. The `conn_id` is still what binds
a datagram to a *session*: once the relay has opened that session's token, it checks that the two
ends named by the leg header are the two ends of that connection, and authenticates the payload.

## The leg envelope

Peer ↔ relay. `src/relay/src/leg.rs` and `udp_relay.cpp` are two implementations of this table and a
change to one without the other is a link failure, so it is written down here rather than in either.

| off | size | field |
| --- | --- | --- |
| 0 | 1 | magic/version: high nibble `0x5` (the relay-leg family; T0's packets are `0x4`), low nibble `1` |
| 1 | 1 | op |
| 2 | 2 | `src` LE u16 — the sender's handle; `0` before one is assigned, and `0` = the relay |
| 4 | 2 | `dst` LE u16 — the destination handle; `0` = the relay, or for a client's DATA "my room's host" |
| 6 | 4 | `room` LE u32 |
| 10 | 8 | `seq` LE u64 — per leg direction; the relay holds a 64-entry replay window |
| 18 | *n* | payload — for DATA, the T0 datagram verbatim |
| 18+*n* | 16 | tag = HMAC-SHA256(`leg_key`, bytes `[0, 18+n)`) truncated to 128 bits |

Ops: `1` HELLO, `2` WELCOME, `3` DATA, `4` PING, `5` PONG, `6` BYE, `7` ERROR; the four the session
directory added at `mp:R2` — `8` REGISTER, `9` UNREGISTER, `10` LIST, `11` SESSIONS; and the three
hole punching added at `mp:R3` — `12` CAND, `13` PROBE, `14` PROBE_ACK. HELLO's payload is
`role(1) | flags(1) | match_id(16)` — `flags` bit 0 is the re-key capability (`mp:R1c`) and bits
1..7 are the peer's **protocol level** (`mp:R4a`, below; 0 = a build that predates it); WELCOME's is
`self(2) | other(2)`, plus a fifth byte `level(1)` — the relay's own — to a peer whose HELLO carried
one; ERROR's is one code (`1` no host in this room, `2` room busy, `3` not registered, `4` room
full, `5` no such peer, `6` not this room's host, `7` descriptor too long).

**`13` and `14` are peer-to-peer ops and the relay never sends or forwards one.** They live in the
same number space, and are declared in `leg.rs` beside the rest, for the reason a reserved range
always is: an op number means one thing under this leg key or it means nothing. A probe *will*
sometimes arrive at the relay — a peer punches every candidate it is handed, and a peer behind the
same NAT as the relay can be handed the relay's own address — and that has to be a counted no-op
(`probes_misdirected`) on a number nothing may later reuse. Forwarding one would be the single worst
bug this design admits: the relayed path would answer a probe, and the pair would "promote to
direct" onto the route it was already using.

Adding seven ops over two items was additive by construction rather than by care: an older peer
never sends them, and an older relay refuses them as `bad_op` — a counted refusal with a name, not a
mis-parse. Counted **at the relay**, though, and answered with nothing — which is the failure the
protocol level (`mp:R4a`, below) exists to make visible at the peer.

**The leg key starts shared and becomes per-peer (`mp:R1c`).** A leg is tagged with
`HMAC-SHA256(psk, "mh-udp-relay-leg")` — the deployment key, from the same `mh_key.txt` the players
already share, under a label of its own so it can never be the same bytes as `udp_endpoint.cpp`'s
three boot secrets — until that peer's session exists, and with a key of that session's own
afterwards. The section below is the whole of it. **The leg is authenticated, not encrypted**: the
payload is already sealed end to end, and the relay has to read the T0 header it routes on.

**MTU cost: 34 bytes, and since `mp:R1d` T0 pays for them rather than the path.** A relayed datagram
of 1234 is above the 1200 RFC 9000 picked as the figure that needs no PMTU discovery — inside the
1500 an ordinary path offers, but outside the guarantee the number exists for, which is exactly the
kind of margin that fails on one player's network and nobody else's. So the **endpoint's ceiling
drops by the overhead on a relayed link**: `udp_transport.cpp` fills in `Config::leg_overhead` (34
when `[net] relay` is set, 0 otherwise) and `udp_endpoint.cpp` lowers channel C's piece stride from
1100 to 1024, which takes the largest datagram it can build from 1188 to 1112 — 1146 with the
envelope. Channel C is the only path that could reach the ceiling at all (channel A's K-redundant
input frame caps at four 256-byte entries, channel B's records are tens of bytes), so the stride is
the whole of the fix; `send_sealed` holds a backstop that REFUSES anything over the ceiling and
says so, rather than emitting a datagram the path may silently drop.

The endpoint is told the NUMBER and never what adds it, which keeps `mp:R1`'s shape: "something
downstream adds 34 bytes" is a fact about the link. And a promoted (`mp:R3`) pair keeps the leg
envelope precisely so this number never moves mid-session — a stride that changed on promotion
would make a datagram that fitted a second ago stop fitting.

## The room, and where it comes from

A joining client has to say **which host** it wants before either peer has a `conn_id`. That code is
the **room**, and it comes from the lobby: a host's room is a **random 30-bit code minted at every
tunnel start** (`mp:R6`, `udp_room.h`), and a client's is the room of the game it picked out of the
browser (`mp:R2` — the directory names it, `s3_kick_connect` latches it, and it reaches the module
as `MH_NetConfig.port`, which for a relayed client is no longer an address of anything).

**The host's room is minted, never its `[net] port` (`mp:R6`).** R1 and R2 had the host name its own
port, which was one host per relay on the rig and a collision on the shared VPS relay, where every
player ships with `port=6501`: the second player to host was refused `room_busy` until the first
lobby ended, and a host that crashed out of a lobby and relaunched inside the relay's idle window was
refused by its *own* stale slot (measured 2026-09-19 on the VPS: 30 refusals over 16 s while the
killed run's slot aged out). A client never types the room — it learns it from the directory — so
the host's code only has to be unique among the hosts on one relay at one moment, and a random draw
is that. Thirty bits rather than thirty-two because the room reaches mh.dll's browser inside the
SESSION_INFO sender int (`session_sender_for_relay_room`, 30 bits); zero is never drawn (it is the
directory room, and a host may not name it) and a zero mint means the CSPRNG failed, on which the
transport refuses to host, as the endpoint does without secure randomness. The line to read is

```
net: udp relay -- host room <n> (minted for this lobby; mp:R6)
```

once per host start, and a `room_busy` that still arrives — a 2⁻³⁰ collision, or a relay that
answers it to everything — is met by a **bounded re-mint** (`udp_room.h` `BUSY_RETRIES` = 4, each
logged as `host room <n'> (re-minted: room_busy on <n>, retry k/4; mp:R6)`) and only then by R1's
refusal line. "Every tunnel start" is the lobby's first transport init, the R7 browse→Create relink
and the U40 match-end relink; a U13 leave-and-re-create that keeps the link keeps the room, which is
right — the client connected to it is not re-dialling. `tools/check_relay_rooms.py` reads all of
this out of a run's logs (every host minted a room that is not its port, hosts are pairwise distinct,
every client's last leg came up in one of them, the relay's counters say `register_refused=0` and
no `room_busy`).

**`[net] relay_room` is RETIRED (`mp:R1e`).** R1 had the knob default to `[net] port`, for a good
reason at the time — two peers that already agree on a port already agree on a room, so a relayed
game asked the player for nothing new and needed no UI. R2 replaced that with a room the lobby
mints, and what the knob had left was the shape a silent override always has: a stale value in one
player's ini out-voting the lobby they had just clicked, with `no_host` in a log nobody re-reads as
the only symptom. It is now **read and ignored**, with a line in `mh_net.log` saying so:

```
net: udp relay -- [net] relay_room=<n> is IGNORED (mp:R1e); the room comes from the lobby now,
and this peer is using room <m>. Remove the key.
```

Read and ignored rather than deleted, deliberately: dropping the read would leave an operator with
an ini key that does nothing and no line anywhere admitting it.

A client that finds no host at the room it was given does not dead-end on `no_host` either: it
re-registers in the **directory room** (below), lists what IS hosted, and re-dials the room of the
lobby the player picks. So two peers whose `[net] port` disagrees find each other anyway.

**Corrected by mp:R2c (2026-09-21).** Until then a client's FIRST dial guessed its own `[net] port`
as the room — a port is never a plausible room code (mp:R6 mints one), so that guess never landed on
a host and every relayed client's log showed one `no_host` → directory browse → re-dial, on every
process start, whether or not the player had typed anything. That was read as "the ordinary path, not
a fault" here, but it cost a real, measured price: a guaranteed relay refusal plus a guaranteed
4-second peer-handshake timeout (`udp_endpoint.cpp`'s `HS_BUDGET_MS`, armed regardless of the guessed
room) before the directory ever answered — four `no_host` refusals in one session on 2026-09-20, both
in the client's log and the relay's own. mh.dll now hands `relay_default_room()` the directory room
(0) itself whenever there is no typed address and no directory pick, so the common case never dials a
guess at all — `Config::browse_only` (`udp_endpoint.h`) skips the peer handshake entirely, and the
directory LIST still arrives over the same registration. A NON-zero guess that still turns out wrong
(a stale `relay_room=` override, or a picked room that closed in flight) still falls back to
`DIRECTORY_ROOM` exactly as this paragraph always described — R2c only removed the guess that was
never anything but a port number. `tools/check_relay_leg.py` asserts the negative on the
`relay_browse_local` scenario: no client-side `no_host` refusal or handshake-timeout text, and (with
`--relay-log`) no relay-side `why="no_host"` event.

**The Join-inside-the-handshake window (`mp:R6`, found by `relay_browse_local`).** Because the row
now always arrives from the directory and the re-dial into its room starts at that same moment, a
player who clicks Join within the ~0.2–0.5 s the handshake takes clicks with no link up. R2 had
left that as "the send is dropped; click Join again" — but the retail Join click pushes the lobby
screen regardless, so there was nothing to click again, and the first R6 run of `relay_browse_local`
(whose script clicks Join 125 ms after the row appears) sat in a lobby at `players=1/8` until the
timeout. Three things closed it, all measured on that scenario: (1) the client remembers a JOIN it
sent on a relay-listed row with no peer and **re-sends it on the first tick with a connected peer**
(`; S4 join: … re-sending the JOIN …`), for that lobby only; (2) the host **holds a client's retail
`0x17` hello that arrives before that client's JOIN is admitted and replays it at admission**
(`; R6: held player N's 0x17 hello` / `… replayed`), because retail answers a hello with the `0x09`
slot snapshot only for a peer the S4 mirror has seated, and the re-sent JOIN landed 2 ms *after* the
hello; (3) the S3 "record received → re-arm the browser" one-shot now fires **only while a browser is
on screen** — its target slot (`menu_oneshot_cb` = `_G_LLM_UI_MENU_ASYNC_CALLBACK_A`) is the lobby's
own frame callback once the lobby is up, and writing the browser rescan into it from inside the
lobby silently stopped the lobby dispatch (no poll, no keepalive), which was the run's actual
cause of death. None of the three was reachable while a host's room was its port: the first dial
landed, so the store was valid, the JOIN went on a live link, and the record arrived on the browser.

One host holds a room at a time; a second host on the same code is refused (`room_busy`) rather than
silently re-pointing the clients — which is what a host answers with a re-mint (above).

A known handle's HELLO naming a DIFFERENT room re-homes it there (subject to the same admission check
a fresh join uses) rather than leaving it stranded in the old room, which the old room's count drops
for and closes over if that empties it, same as eviction (`mp:R1b`).

## How a session is learned

The relay holds the deployment's PSK, so it can open the bootstrap datagrams (their keys are
PSK-derived) — and when the **host** sends handshake datagram 2, the relay reads the `CH_HS` frame of
kind `GRANT` out of it and calls `token_open()`. That is the moment `docs/mp-wire-udp.md` means by
"it uses `token_open` to decide who is registered". From the token it stores `conn_id → {host handle,
client handle, mac_c2s, mac_s2c}` and **discards `enc_c2s`/`enc_s2c`**.

From then on, for that `conn_id`:

- the two ends named by the leg header must be that connection's two ends, or the datagram is
  dropped and counted (`not_in_peer_set`);
- the T0 tag is verified with `mac_c2s` for a client's datagram and `mac_s2c` for the host's — a
  failure is dropped and counted (`inner_bad_mac`), never forwarded.

**A `conn_id` the relay has NOT learned is forwarded and counted (`inner_unverified`), not dropped.**
The relay normally sees the whole handshake, so that counter should sit at zero or a handful; a relay
restarted mid-match would otherwise black-hole a game it could simply carry. The endpoints' own
crypto is the security boundary; the relay's check is defence in depth. `--strict` drops instead, and
is what a public deployment should run once R2 gives the relay a registration it can require.

## Anti-amplification, and the ping that makes it work

Until an address has sent something **after** the relay replied to it, the relay will not send it
more than **3×** the bytes it has sent (plan D4; the figure RFC 9000 §8 uses, for the same reason).
A HELLO is 52 bytes, so the budget is ~156 and the WELCOME spends 38 of it — which one forwarded game
datagram would exceed.

So **`udp_relay.cpp` PINGs the moment a WELCOME lands.** That single datagram validates the address
and lifts the cap. Without it the first packet of the match is the one the cap eats, which is exactly
what the relay's own loopback test measured before the ping existed. The ledger of unvalidated
addresses is capped at 4096 entries and evicts the least recently seen, because a map keyed on a
value an attacker chooses is the same attack one level up.

## The session directory (`mp:R2`)

R1 could carry a match between two peers who already knew which room they wanted. R2 is how they
find out. The relay keeps, per room, one **session descriptor** its host published, and hands the
set to any registered peer that asks — so a player's in-game browser lists relay-hosted games, with
their real name, map and player counts, **before being connected to any of them**.

**The descriptor is `SESSION_INFO`, verbatim, and the relay cannot read it.** The bytes a host
registers are exactly the bytes its `SESSION_INFO` advert carries (the MP UDP wire format page's
session descriptor, 473 bytes at its v4 worst case); the relay stores and returns them as an opaque
blob capped at 512. That is deliberate: the two game peers already share that codec, and teaching a
third implementation of it to the one process in this system that is **designed** to be unable to
read the match would be an odd thing to do for a feature that needs only bytes. The consequence is
that a browser row is built from the host's own advert, not from anything the relay composed — the
name, the map, the occupancy and the 0x17c map header for the preview are all the host's.

### The three ops, and the one rule behind them

| op | who | what |
| --- | --- | --- |
| `8` REGISTER | a room's host | publish (or refresh) the descriptor; payload = the bytes |
| `9` UNREGISTER | a room's host | withdraw it |
| `10` LIST | any registered peer | ask for the directory |
| `11` SESSIONS | the relay | one page: `total u16 \| offset u16 \| count u8`, then `count` × `room u32 \| len u16 \| bytes` |

**A room's descriptor belongs to its host.** Only the peer the room's host slot names may publish or
withdraw one, so a client cannot advertise a lobby it does not run; and the descriptor dies with the
host — on BYE, on eviction, on a re-home into another room, and on the TTL. Each of those is a
`session_unregistered` event with the reason in it.

**Pages, not a list.** A descriptor is up to 512 bytes and a leg datagram carries 1200, so two
full-sized ones already fill an answer. A LIST is answered with up to **three** pages, and each peer
carries a cursor into the room-ordered directory so successive LISTs walk the whole of it rather
than returning the same first rooms forever. Three pages is also the relay's largest amplification
surface — a 34-byte LIST buying ~3.7 KB — and it is bounded twice: the leg tag must verify, so the
asker holds the deployment PSK, and an address that has not yet validated itself is still held to
the 3× cap by the ledger above.

**An empty directory is an answer.** A LIST against a relay with nothing registered still returns a
page saying `total = 0`, and the client acts on it by dropping every row it was showing. That is the
row-vanish clause: a browser that learns "there are no lobbies here" is in a different state from
one whose LIST went unanswered, and making the player wait out a timer for a lobby the relay has
already forgotten would be merely eventually right.

### The directory room

A peer must be **registered** before it may LIST, and until R2 the only way to register was to name
a room that already had a host — precisely the knowledge a browser does not have yet. So **room 0 is
the directory room**: it admits any client, holds no host, forwards nothing, and is a registration
that buys LIST and PING and nothing else. A host that names it is refused
(`host_in_directory_room`). Zero is free by construction: a host's room is minted non-zero
(`mp:R6`; before that it was `[net] port`, which is never 0 either).

### The two timers, and why they are different numbers

| | value | what it covers |
| --- | --- | --- |
| host refresh | 1 s | the tunnel re-REGISTERs while mh.dll keeps advertising |
| advert staleness | 3 s | mh.dll stopped advertising (the player left the lobby) → UNREGISTER |
| client LIST | 2 s | how often a browsing peer re-asks — **only while the browser is up** (`mp:R2a`) |
| relay session TTL | 20 s (`--session-ttl-secs`) | a host that vanished without a withdrawal |
| browser row TTL | 8 s | a row the directory stopped listing, as a backstop |
| peer eviction | 60 s (`--idle-secs`) | the peer itself |

The session TTL is deliberately far **shorter** than the peer eviction: a lobby that has gone away
has to stop being listed long before its host stops being a peer. But the ordinary path is not a
timer at all — a host leaving its lobby stops advertising, the tunnel notices within 3 s and sends
UNREGISTER, and the client's next LIST (≤ 2 s later) returns an empty directory that clears the row
at once. The timers are what cover a host that crashed or was firewalled off mid-lobby.

A crashed host that **relaunches inside those 60 s** is not held up by either timer (`mp:R6`): its
stale slot still holds the *old* room, and the relaunched process mints a new one, so it is admitted
at once beside the ghost — which the session TTL then takes off the directory and the eviction
timer takes off the peer table on their own schedules. Before R6 the relaunch named the same port-
derived room and was refused `room_busy` by its own ghost until eviction; `cargo test -p mh_relay
a_host_relaunched_inside_the_idle_window` is that scenario at the relay, and `udproomtest` arm D is
the client half (a fresh minter never returns a stale room it was told to avoid).

### How a row reaches the browser without a new module export

The transport module hands mh.dll a received `SESSION_INFO` through one callback,
`MH_SessionInfoCb(int sender, …)`. A directory row has no peer to name as the sender, and what the
browser needs in that field instead is the **room** — because dialling that room is what joining a
listed lobby means. So the room is encoded in `sender` (`session_sender_for_relay_room`, high bit
set; peer ids are 0..7, so the high half of that int was free).

That is not a trick for its own sake. `MH_NET_MODULE_SYMBOLS` is the mh.dll ↔ module ABI and **both**
transports must answer all of it; a session directory is a UDP-relay concept, so a 24th row would
oblige the TCP module — which must stay byte-for-byte the build it is — to answer a question it
cannot have. The field that already means "where did this come from" carries the new answer.

### The poll stops when the player stops browsing (`mp:R2a`)

R2 shipped the client's LIST as an unconditional 2 s poll, and its own rig run measured the price:
**321 LISTs over an 11-minute match**, every one answered, none of them read — because mh.dll's
directory half is already gated on the browser being the active screen (the gate at the bottom of
this page, which stops a host's match-start withdrawal from tearing a client out of its own game).
The answers were being thrown away at the top of the stack while the question kept being asked at
the bottom, so the relay's `list_requests` counter described browsing activity that was not
happening.

The poll now asks the same predicate: **the session browser is the active widget list.** In a match
it is not, so a playing peer's `list_requests` does not move; back in the browser the poll resumes,
and the first poll after the transition is immediate rather than up to 2 s late, because the one
moment a directory is worth asking for is the moment the screen opens. Either transition writes one
line (`net: udp relay directory polling PAUSED|RESUMED`).

**How the predicate crosses into the module, and why it is not a 27th ABI row.**
`MH_NET_MODULE_SYMBOLS` is a contract *both* transports must answer, and a session directory is a
UDP-relay concept — the TCP module has no relay to poll with, which is the same reasoning that kept
R2's directory row off that table. This crossing goes the other way (module → mh.dll) and is
optional by construction: `net_discovery.cpp` exports `MH_Seam_RelayBrowsing`, `udp_relay.cpp`
resolves it once with `GetProcAddress` against the mh.dll handle it already has, and finding nothing
— `net_selftest.exe`, a standalone module, an older mh.dll — keeps R2's unconditional poll and says
so once. It can never be a load-time dependency.

### What a player does

With `[net] relay` set the player has BOTH a relayed path and a direct one, and the SCREEN chooses
which (`mp:R7a`):

- **Browse the relay** — NETWORK GAME → the player-name screen → the **first browser** ("Refresh
  list / Create game / …", `mp:R7`). It lists every game registered on the relay (name, map,
  players); clicking one joins it THROUGH the relay, and the client dials that lobby's room. No IP is
  typed and no *Internet server* window is visited.
- **Dial an address directly** — NETWORK GAME → the player-name screen → *Internet server* → type a
  **Server address** → **Connect**. Since `mp:R7a` this is a GENUINE DIRECT dial to that address; the
  relay is configured but NOT contacted for it. The target host's port must be reachable/forwarded,
  as for any direct connection — a failed direct dial says so, and notes that the browser path would
  go through the relay instead.

Before `mp:R7a` there was no direct mode at all: `[net] relay` made `s3_kick_connect` dial the relay
whatever the player typed, and `mh_net_udp.dll` read `relay=` itself and tunnelled every connection.
Now `mh.dll` decides **per dial** — first-browser refresh/join and a relay-directory row → the relay
+ that room; *Internet server* + a typed IP → that IP, no relay — and hands the decision to the
module in `MH_NetConfig` (an appended `relay_addr`, empty for a direct dial, + `relay_room`) instead
of the module reading the ini. `force_relay=1` still pins the relayed path (below). The module stops
reading `[net] relay` for the dial; the `[net] relay_room` IGNORED notice (`mp:R1e`) and the module's
own `force_relay` read stay.

## Keepalive

The peer pings every **20 s**; the relay pings a peer that has been quiet for **25 s** and evicts one
silent for 60 s (`--idle-secs` / `--ping-secs`). Plan D4's band is 20–25 s and both ends sit in it, so
a NAT binding is refreshed by whichever end is still alive. This is separate from the game's own
`NAT_KEEP_MS` on the T0 connection, which the relay neither sees nor needs to.

## The client half: a loopback tunnel

`[net] relay=<host:port>` turns it on, for both roles. `udp_relay.cpp` runs one thread and:

- **client** — binds a loopback socket, and `udp_transport.cpp` points `MH_NetConfig.host/port` at it
  instead of the real host. The endpoint dials `127.0.0.1:<tunnel>`; everything after that is
  unchanged.
- **host** — binds nothing new for the game; the endpoint binds `[net] port` as always and the tunnel
  dials it on loopback. **One loopback socket per remote peer**, so the endpoint sees a distinct
  source address per client. That is not tidiness: without it `host_on_boot`'s address match would
  read two joining clients as one (see above).

The endpoint does not know the relay exists. That is what makes `mp:R3` cheap — stopping the tunnel
*is* going direct — and it is why the relay work touched `udp_endpoint.cpp` not at all (`mp:R3e`
added one *query* to it, below; it still learns nothing about relays by being asked).

### The per-peer tables, and who frees a slot (`mp:R3e`, 2026-09-19)

A host's tunnel holds **three tables keyed by the relay handle, `MAX_REMOTE` (8) of each**: the
loopback socket (`Remote`), the punch state (`PunchPeer`, `mp:R3`) and the pair keys (`KeyPeer`,
`mp:R1c`). A client holds the punch and the keys for its one host, and no `Remote`. Until R3e a
slot, once used, was held until `stop()`: a lobby that eight distinct peers had joined and left over
its life refused the ninth a socket and a punch (`net: udp relay has no loopback socket left for
peer N`), and that join silently never completed.

**The tunnel has no signal of its own that a peer left.** The relay tells a host nothing when one of
its clients drops — a client's BYE is between the client and the relay — and its one refusal that
means "that peer is gone" (`no_peer`) names no handle and is only provoked by traffic the endpoint
has already stopped sending. What the tunnel does have is a party that already *decides* peer
lifetime: the endpoint, which drops a conn on the peer's LEAVE, on its link timeout and on a stream
fault, and reclaims a handshake that never finished. So the rule is **ownership, not a timer**:

> a `Remote` lives exactly as long as the endpoint holds a conn or a live handshake for the loopback
> address that `Remote` presents to it.

`udp_transport.cpp` installs `Endpoint::knows_addr` as the tunnel's `set_peer_known` predicate. The
pump asks it once a second (`SWEEP_MS`) per `Remote` older than a 5 s grace (`GRACE_MS` — the window
between a socket being made for a peer's first datagram and the endpoint registering the handshake
it carries; a joiner retries that handshake for `HS_BUDGET_MS` = 4 s, so a slot the endpoint has not
learned in five seconds is one nobody is joining through). A `Remote` the endpoint does not know is
released with **everything keyed by its handle in one step** (`free_peer`: socket closed, punch
zeroed, keys zeroed), logged as `net: udp relay -- peer N released: the endpoint no longer holds
it …`. The grace is *not* a liveness timer: a peer that is merely quiet stays in the endpoint's table
until *its* timeout says otherwise, which is R-live's job.

Two orphan rules ride the same sweep. A punch is created by a CAND or a PROBE as well as by DATA, so
a handle that only ever probed — or a PSK holder probing under handles it made up — holds punch
slots with no socket behind them: host side, a `PunchPeer` past its grace whose handle has no
`Remote` is released (`… released: a punch with no peer behind it`); client side the anchor is
`other_handle` and any other handle's punch goes the same way. `stop()` clears all three tables, and
`mp:R4b`'s re-home (`forget_peer`) is the same release under a different reason.

**The ordering rule.** All three tables are pump-thread-only and a datagram is handled to
completion before the next is read, so nothing can observe a handle with a socket but no keys. What
a straggler for a freed handle meets is decided by its path: a *direct* one (a promoted pair's DATA,
a probe) is checked under the pair key we no longer hold, falls to the deployment key, fails the MAC
and is dropped; a *relayed* one verifies under the relay's key and simply makes a **fresh, zeroed**
slot, which the next sweep frees again if the endpoint refuses it. A new peer that lands on the same
handle number starts from that same zero. `my_key` — the relay-directed send key, a copy of the first
pair key installed — is left alone when the pair that minted it goes: the relay keeps `keys[0]` for
the life of our handle.

**With no predicate installed** (`net_selftest` has no transport; a build where the wiring is
missing) the tunnel keeps R1's hold-until-stop and leaks exactly as visibly as before, rather than
freeing on a guess. Oracle: `net_selftest.exe udprelaytest` (20 checks, ~13 s) drives the real
tunnel against a stand-in relay and a stand-in endpoint on loopback — eight join, a ninth is refused
while all eight are held, the eight leave, the ninth and tenth then get a socket and a punch each,
and the orphan rule releases a punch nobody is behind. With the sweep disabled, 10 of its 20 checks
go red in exactly the pre-R3e shape.

**The relay-side half of the same leak, closed with it.** A host's handle holds one leg key per
client token the relay opens for it (`relay.rs install_leg_key`), and `drop_peer` removed the conn
but not the key — so the set grew by one per joiner ever, and at the *tenth* token the
`ROOM_PEER_CAP` (9) FIFO eviction took `keys[0]`, the key the host tags every relay-directed
datagram with, after which its PINGs and relayed DATA failed the relay's MAC silently until idle
eviction. Now `drop_peer` takes a departed peer's pair key out of each counterpart's set (never
`keys[0]`, which is pinned for the life of the handle on both sides), and the cap, should it still
be reached, evicts `keys[1]`. Oracle: `cargo test -p mh_relay` (59; the ten-sequential-clients test,
mutation-checked: restoring either the FIFO or the no-removal reds it).

If `[net] relay` is set and the tunnel cannot start, **the transport refuses to start**. A peer that
believes it is relayed and is not would sit in a lobby nobody can reach while its log said the
transport came up.

## match_id

The relay's structured log is keyed by `match_id` (plan D8), and the transport ABI has no field for
one — `MH_NetConfig` is the mh.dll↔module contract and R1 does not change it. But the id is already
crossing the module: every `SESSION_INFO` a host sends and every one a client receives carries it
(SES0, `session_info.h` v3). `udp_transport.cpp` reads it there, at the one point both roles pass
through, and hands it to the tunnel, which re-sends its HELLO as an **update** naming the same handle.
The peer also writes one line of its own:

```
net: udp relay session match_id=<32 hex>
```

so the relay's log and both peers' `mh_net.log` carry the same 32 hex digits for one match.

## Running it

```
cargo run -p mh_relay -- --bind 0.0.0.0:7100 --key-file <the players' mh_key.txt>
cargo run -p mh_relay -- --local              # loopback, human logs, the all-zero "open" key
```

`--local` binds `127.0.0.1` and prints human-readable lines; the default elsewhere is one JSON object
per line, since a relay's log is read by a collector far more often than by a person. `--key`/
`--key-file` take the deployment's PSK — the same 64 hex digits (or the word `open`) `mh_key.txt`
carries. With no key at all the relay runs on the all-zero key and says so loudly.

**How that key reaches the players (`dist:LA6`).** Nobody types it. The release workflow puts the
relay's address and its deployment key into the launcher's *signed* update manifest
(`relay: {addr, key}` beside the asset table — `tools/gen_update_manifest.py`, fed from the
`MH_RELAY_ADDR` repository variable and the `MH_RELAY_KEY` secret; neither value is ever in this
tree), and the launcher writes `[net] transport=udp` + `[net] relay=<addr>` into the game's
`mh_net.ini` and `<key>` into `mh_key.txt` on every install, update and launch — editing only those
two ini lines in place, and leaving both files alone when the manifest names no relay. The key is
public once shipped, and that is accepted (user, 2026-09-19): it keeps port scanners off the relay,
not players, and the *signature* is what stops anyone else from re-pointing every launcher at a
relay of their own. The launcher's report scrubber blanks the `relay=` line and the address
wherever a log prints it. Details: [`src/launcher/README.md`](../src/launcher/README.md).

The game side, in `mh_net.ini`:

```ini
[net]
transport=udp
relay=<relay host>:7100   ; the DIRECTORY to browse (mp:R7a); a trailing `; comment` is trimmed
; force_relay=1       ; optional; mp:R3 -- pin the relayed path, never go direct
```

`relay=` means **the directory to browse**, not "tunnel everything" (`mp:R7a`): the first browser
lists games registered on it and joins them through it, while *Internet server* + a typed address is a
genuine direct dial that never touches the relay. `mh.dll` makes that choice per dial and hands it to
the module in `MH_NetConfig.relay_addr` (empty = direct) + `relay_room`; the module no longer reads
`[net] relay` for the dial. `relay_room` is gone (`mp:R1e`): the room comes from the lobby (a host's is
minted, `mp:R6`), and a key left in an old ini is read only so the log can say it is being ignored.

The oracles:

```
cargo test -p mh_relay                       # the leg codec, the routing rules, the candidate
                                             # exchange, mp:R1c's per-peer key + its ratchet, and
                                             # mp:R6's two-hosts / re-HELLO / relaunch admission
net_selftest.exe netcfgtest                  # mp:R7a's MH_NetConfig relay fields: default = no relay,
                                             # appended, and the TCP module ignores them
net_selftest.exe udpwiretest                 # mp:R1c's key derivation, incl. the cross-language vector
net_selftest.exe udpbulktest                 # mp:R1d's relayed piece stride and the 1200 arithmetic
net_selftest.exe udppunchtest                # mp:R3's promotion state machine, no sockets
net_selftest.exe udproomtest                 # mp:R6's host room minter, no sockets, injected RNG
python tools/test_ui.py relay_match relay_browse relay_browse_local relay_punch \
                        direct_dial_with_relay_set   # mp:R7a: the relayed twin (relay_browse_local,
                                             #   check_relay_leg) and the DIRECT one
                                             #   (direct_dial_with_relay_set, check_direct_dial)
python tools/test_ui.py --determinism --transport udp --relay
```

A path that actually drops an oversize datagram is `tools/net_shim.py --udp --mtu 1200` in front of
the relay: `ctl status` reports `oversize=` per direction and `max_seen=`, so a run can say both that
nothing was destroyed for its size AND how close the biggest datagram came.

## Hole punching (`mp:R3`)

R1 put the relay in the path and R2 put a lobby directory on it. Both leave every datagram going the
long way round. R3 is the short way, and it is plan D4's second half: **relay-first, promote
silently.** The match is already running relayed before the first probe is sent, so punching can
take as long as it likes and can fail outright — nothing here is on the path of starting a game.

**The punch socket is the leg socket**, and that is the single load-bearing choice in the item. A
NAT's mapping is created by an outbound datagram and keyed on the *source* port, and the address the
relay observes — and reports as the peer's reflexive candidate — is the mapping of that socket.
Probing from any other socket would be probing a mapping the far end was never told about: it works
on a full-cone NAT, which is the population that did not need punching.

**A promoted pair still sends the identical leg envelope**, 34-byte header and MAC and all, straight
at the peer instead of at the relay. Stripping it would make the MTU of a pair 34 bytes larger the
instant it promotes, so a datagram that fitted a second ago could stop fitting — a size-dependent
failure that appears only on transitions. Keeping it makes the path change invisible to every layer
above, including to the sequence space (one `tx_seq` across both paths; the relay's replay window
then sees a strictly increasing sequence with gaps, which is what a sliding window is for).

### The candidate exchange

`OP_CAND` carries one peer's candidate list to its counterpart, by the same room/peer-set rule
`OP_DATA` uses — a client's list goes to its room's host and nowhere else, a host names one of its
own clients. The payload:

| off | size | field |
| --- | --- | --- |
| 0 | 1 | `count`, at most 8 |
| | 1 | `fam` — `4` or `6` |
| | 1 | `flags` — bit 0 = the relay OBSERVED this address |
| | 2 | `port` LE u16 |
| | 4 or 16 | address, network order |

Family-tagged rather than v6-only-with-a-mapping: a v4 candidate is probed from the v4 socket and a
v6 one from the v6 socket, so the two must stay distinguishable end to end, and a `::ffff:` mapping
would have to be undone by exactly the code least able to test it.

The relay does two things a peer cannot do for itself. It **carries** the list, and it **says where
the sender is seen from** — appending the source address it observed, flagged, after stripping that
flag from everything the peer claimed. A peer may not dress a claim up as an observation: the flag
is the receiver's reason to trust one entry over the others, so a peer that could set it could aim
its counterpart's probes anywhere. The relay does **not remember** a list: it would have to be
invalidated on every rebinding, and the peer re-sends on a timer anyway.

The eight-candidate ceiling is a bound on *work* — each candidate costs its receiver a probe every
interval — so a full list drops a *claim*, never the observation.

### The promotion rule

A path is promoted when both of these hold of the same address:

- **ACKED** — we sent a probe carrying a nonce and got that nonce back. This proves the round trip.
- **HEARD** — a probe of *their* own arrived from that address.

ACKED alone is enough to know the path works. It is not enough to know the *peer* knows, and
requiring both means one rule describes both ends, so both switch. It also makes the pairing
backward-compatible for free: a peer from before R3 never probes and never answers, so neither flag
sets and the pair simply stays relayed.

Two details that decide whether this works on real networks. A probe arriving from an address
**nobody advertised** is added as a candidate and marked HEARD — a datagram came *from* it, so it
exists and can reach us, which no advertised candidate proves; this is what recovers a pair when one
end's NAT maps a different port toward the peer than toward the relay. And a probe is **echoed
unconditionally**, including under `force_relay` and including to an address we would never promote:
refusing to answer would make one peer's local policy look to the other like a network that drops
probes.

Cadence: every known candidate is probed every 250 ms for the first ten seconds after the first
candidate arrives, then every two seconds forever — the job stops being "find a path fast" and
becomes "notice if one becomes possible", which is what a lifted firewall rule looks like. Once
direct, only the chosen path is kept warm, at 1 Hz.

### Demotion, and the one-way failure that shaped it

Three seconds without **an echo of our own keepalive** and the pair goes back to the relay and
re-opens the search — including on the path that just died, because the commonest cause is a
rebinding. Losing a direct path is not a fault, it is Tuesday (a NAT rebinding, a Wi-Fi roam, a
sleeping laptop), and the cost of being wrong is one relayed second. The relay leg was warm
underneath the whole time, so there is no reconnect.

**Only the echo counts, and the obvious alternative is wrong.** Counting *any* authenticated
datagram from the peer — its game traffic, or a probe of its own — looks strictly better: more
evidence, less keepalive traffic. It is evidence of a different thing. An inbound datagram proves
**peer → us** and says nothing whatever about **us → peer**, and a path that fails in one direction
only is completely ordinary (an asymmetric firewall rule; a NAT that expired one direction's
mapping). Under that rule the peer whose *outbound* half died is told it is alive by its inbound
half, never demotes, and sends the whole match into a hole — while the other end, correctly seeing
nothing, has already fallen back to the relay and is *still probing the direct path it wants back*,
which is precisely what keeps the first peer's timer fresh.

That is not a hypothetical: it is what the first implementation did, and the rig measured it on
2026-09-18. A mid-match firewall cut demoted the client in 3.0 s, never demoted the host at all, and
the run stalled at step 555 with the relay having resumed carrying 52 datagrams and no more. The
fix is one line and the regression is `udppunchtest` arm **F2** — a promoted path with a busy
inbound half and nothing answering outbound, which must still demote at `DEAD_MS`. Restoring the old
line turns that arm red and nothing else in the suite.

### `[net] force_relay=1`

Pins every pair to the relay: no candidates published, no probes sent, and an inbound probe answered
but never promoted. Setting it on **one** peer keeps the pair relayed — a peer that publishes no
addresses gives the other nothing to probe. Two uses, both live-system: proving a relay deployment
really carries a whole match (a test that silently went direct proves nothing about the relay), and
giving a player whose direct path is genuinely worse than two good paths into a datacentre a way to
say so.

### What the logs say

`mh_net.log`, per peer: exactly one of `udp punch ARMED` / `udp path RELAY (forced)` at start, so
"which mode was this run in" is never an inference from silence; `udp punch -- peer N offered …`
when a candidate list arrives with something new in it; and `udp path DIRECT` / `udp path RELAY` per
transition, each naming the peer, the address and a duration. On the relay: a `candidates` event per
forwarded list, and `cands_forwarded` / `cands_refused` / `probes_misdirected` in the counters line.

**The relay's view of a promotion is an absence**, and deliberately so — nobody tells it. What it
sees is `forwarded` going flat while the match continues, and the `candidates` events stopping
(a promoted pair has no rendezvous left to do). That absence is also the acceptance evidence: a pair
that claims to be direct while the relay's `forwarded` keeps climbing is not direct.

### `relay_punch`'s claim, made assertable (mp:R3a, 2026-09-18)

`relay_punch` (`tools/test_ui.py`) was registered and green from the day `mp:R3` landed, but its
actual claim — the mid-handshake path switch above does not break the link — was provable only by
re-reading this page's rig-determinism table, not by the scenario's own run. Nothing in the
`[uitest]` capture grammar can read a log line, so a pair that silently stopped promoting (a
regression in the candidate exchange, or one that simply never reached the punch and stayed relayed
for the whole run) would leave the scenario exactly as green as a real promotion: the frame the
scripts capture is the same lobby either way.

`tools/check_relay_path.py` closes that gap as `relay_punch`'s `post_check`: it reads the host lane's
`mh_net.log` for `net: udp path DIRECT -- peer N via ADDR after MS ms of punching` (the promotion
line above) and fails if it is absent. **The negative proof is the pinned scenarios themselves** —
`relay_match` and `relay_browse` both carry `force_relay=1`, which writes `udp path RELAY (forced)`
once at start and never arms the punch at all (see "What the logs say" above), so running this same
checker against either of them fails on purpose: a forced-relay lane never promotes, by design, and
a checker that could not tell that apart from `relay_punch`'s real promotion would not be asserting
anything. The checker's own `--selftest` (wired into `tools/lint_repo.py`) plants both corpora —
a real `DIRECT` line, and a `force_relay`-forced one with none — so the negative case is proven
offline rather than only by re-reading a rig run nobody re-reads.

### Measured on the rig (2026-09-18)

**First, what the rig is not.** `mp:R3`'s acceptance clause was written as "rig peers behind the two
Hyper-V NATs go direct", and **there are no two NATs**: host `…0.199`, peer A `…0.37` and peer B
`…0.38` are one flat bridged `/24` with no translation between them, which `route print` and a ping
both say plainly. On that rig "relayed" and "direct" cannot differ by *reachability* — they differ
by **destination**, so that is what the runs measure, and the instrument is the relay's own
`forwarded` counter: a pair that claims to be direct while the relay keeps forwarding is not direct.
Where the clause needs a path that is genuinely unavailable, a Windows firewall rule applied to peer
A over ssh (block UDP in and out to peer B's address, and nothing else) supplies one; the relay
lives on a third address and is untouched by it. **This does not test NAT traversal** — the
inbound-probe arm that recovers a symmetric mapping is covered offline by `udppunchtest` §E and not
by any run here.

Four UI-path determinism runs, 1500 steps each, two rig peers, `transport=udp` + a relay on this box:

| config | promotion | relay `forwarded` | `cands_forwarded` | verdict |
| --- | --- | --- | --- | --- |
| punching on | **both peers DIRECT at +297 ms**, before the T0 handshake completed | **0** for the whole run | 2 | ALL PAIRS IDENTICAL (1500) |
| `force_relay=1` | none; both logged the forced line | 12855 | **0** | ALL PAIRS IDENTICAL (1500) |
| direct path firewalled off | none; both kept probing | 12852 | 128 (kept trying) | ALL PAIRS IDENTICAL (1500) |
| direct path cut mid-match | DIRECT at +297/313 ms; **both peers demoted 3.0 s after the cut, 70 ms apart** | 0 → 330 in the 5 s window containing the cut, then ~155/s | 2 → 4 → 14 → … (punching restarted) | ALL PAIRS IDENTICAL (1500) |

The first two rows are the A/B that makes the rest readable: the same scenario, the same scripts, one
knob apart, and the relay either carries every game datagram or none of them.

**No stall and no icon storm** on the firewalled run, against the pure-relay run as the reference
(`mp_pacing_report`, per peer): `icon_per_1k` 6/6 and 9/9 — identical; `hitch_100ms` and
`hitch_250ms` identical; `la_moves` 0 in both, so the adaptive-lookahead controller never moved;
`deficit_%` 17.0→18.9 and 12.2→13.7, `stall_%` 11.2→11.5 and 5.3→6.9. The frame-time tails move
inside single-run noise (host `ft_p95` 26.9→36.2, client 30.6→30.9).

Three things these runs establish that are easy to miss:

- **The promotion beat the handshake.** The candidate exchange happens as soon as the client's leg
  registers, so on a fast path the pair is direct *before* `udp_endpoint.cpp` finishes its T0
  handshake — which is why the relay forwarded zero game datagrams in the first row, and why the
  `relay_match` scenario had to be pinned with `force_relay=1` or it would have silently stopped
  testing the relay.
- **A demotion is not a reconnect, and it costs a repair window rather than three seconds.** The
  mid-cut run lost the direct path, spent 3.0 s noticing, and carried on over the relay with
  `state-mismatch steps=0` across 1500 compared steps. What the transport recorded for the whole
  episode is **one gap stall, 297 ms on the client and 328 ms on the host**, plus ~140 RTO repairs:
  T0's own retransmit covers the hole the moment the destination changes, so the blackhole is three
  seconds long and the *stall* is a third of a second. Nothing above the tunnel was told.
- **The mid-cut run is also where the one-way-failure bug was found**, and it was found only because
  the cut was moved ten seconds earlier. With the cut at +55 s the host reached its step budget
  before the consequences arrived and the run went green over a broken rule; at +45 s the same
  configuration stalled at step 555. A single passing rig run over a timing bug is a coin toss, and
  this one landed heads first.
- **A pair that cannot punch keeps trying and it costs nothing visible.** `cands_forwarded` reaching
  128 on the firewalled run is the two-second slow cadence running for the whole match, next to
  pacing numbers indistinguishable from the pure-relay run.

### What R3 deliberately did not build

- **A third-party rendezvous.** No ICE, no STUN, no TURN. We control both ends and already have a
  mutually-reachable server holding a session; the whole of the rendezvous is one op.
- **A relay-side view of the outcome.** The relay is not told that a pair went direct and has no
  state for it. Every candidate it forwards is stateless.
- **Probing on behalf of a pair that never registered.** The candidate exchange rides the same peer
  set as DATA, so an unregistered handle gets nothing.
- **A PROVEN IPv6 path.** The candidate format, the relay's routing and the client's second (v6-only)
  punch socket are all there, and a machine with a global or ULA v6 address will advertise and probe
  it. Nothing has ever done so: both rig VMs bound the v6 socket (its port is in the `punch ARMED`
  line) and gathered **no** v6 candidate, because link-local `fe80::/10` is deliberately skipped —
  it needs a scope id that means nothing on the machine you hand it to. So the v6 half is code that
  has been compiled and never executed end to end. Treat it as unproven until a peer with real IPv6
  is on the rig.
- ~~**A narrowing of `mp:R1c`'s trust boundary.**~~ Closed at `mp:R1c` — the per-peer leg key,
  below. Until it landed, a holder of the deployment PSK could send a probe under another peer's
  handle and have that address adopted as a direct path; a probe is now checked under that peer's
  own session key, and only a pair that has never had a session is still on the shared one.
  R3's own statement of the hole, kept because it is what the fix had to close: *a probe is accepted
  on the strength of its leg MAC, so a holder of the deployment PSK can send one under another
  peer's handle and have that address adopted as a direct path — the same boundary R1 documents,
  except that R1's version needed the relay to be fooled and this one does not.* What is left after
  R1c is the pre-session window: a pair that has not yet completed a handshake is still on the
  shared key, and a peer admitted to the match could always read it.

## The per-peer leg key (`mp:R1c`)

R1's leg key is one key for a whole deployment: `HMAC(psk, "mh-udp-relay-leg")`. Every peer holds it,
so **any peer can forge another peer's handle** — put their handle in `src`, send from your own
address, and the relay's rebinding path (which exists so a NAT can move a real peer) hands their
traffic to you. `mp:R3` widened it: a probe is accepted on the same key, so a forged *direct* path
needs no relay to be fooled at all.

**The fix is to key each leg on something only that peer's session has**, and the connect token is
exactly that — minted by the host for one client, sealed inside a bootstrap datagram that only the
two endpoints and the relay carrying it ever see. A PSK holder off that path never has it.

| end | key |
| --- | --- |
| a client | `HMAC(token.mac_c2s, "mh-udp-relay-leg-c")` |
| a host, per client | `HMAC(token.mac_s2c, "mh-udp-relay-leg-h")` |

Two keys and not one, for the reason T0 splits its own MACs: a single shared pair key would let each
end replay the other's datagrams back at it. The material is the token's **MAC** keys rather than its
enc keys because the relay deliberately discards the enc keys — it must not be able to read the match
— so it could not derive from them, and deriving from a mac key gives it nothing it did not hold.

**A host holds one key per client, and the relay accepts any key a handle owns.** A host's
relay-directed traffic (PING, REGISTER, LIST, BYE) names no pair and has to be taggable under
something; "any key this handle owns" is the rule that makes one sender and N pairs agree with no
negotiation. It costs at most one HMAC per key on a miss and one on a hit, since the relay moves the
winning key to the front.

### The handover is a ratchet, and the ordering is the whole of it

The token does not exist until the T0 handshake has run, and the handshake rides the leg — so the leg
starts on the deployment key and changes under itself:

1. the host's endpoint emits handshake datagram 2, whose `CH_HS`/`GRANT` frame holds the token;
2. the host's tunnel **sends it, then** sniffs it and installs the pair's keys;
3. the relay **forwards it, then** opens the same frame and installs both ends' keys;
4. the client's tunnel **delivers it to its endpoint, then** sniffs it and installs its own.

**The datagram carrying the grant is the last one on the old key in every direction.** Installing
before sending would tag the grant with a key only the grant can produce — a deadlock, not a race,
and one no retry could resolve. All three parties learn the same keys from one event with nothing
added to any wire; the tunnel opens the token itself (it holds the PSK and is already on the path)
rather than being handed it by `udp_endpoint.cpp`, which is what keeps that file free of relay
knowledge.

Once a handle has a key, **the deployment key is refused from it** — that is the security property,
and without the refusal the fix would buy nothing, since a forger would simply keep using the shared
key. A forged handle under a different token is dropped and counted as `leg_bad_mac`.

### The two exceptions, both stated rather than hidden

- **A handle that has never had a session is still on the shared key.** A peer can be forged before
  its first handshake completes, which is a window of milliseconds and the price of not needing a
  second key distribution. What cannot be forged is a peer that is *in a match*.
- **`ERR_NOT_REGISTERED` is still accepted under the deployment key.** It is the one thing a
  **restarted** relay has to be able to say — it no longer holds anyone's keys — and a peer that
  refused it could never recover from a relay restart. Forging it is a nuisance a PSK holder can
  cause by other means (it makes the peer re-HELLO); black-holing every match across a restart is
  not.

### Surviving a relay restart (`mp:R4b`, 2026-09-19)

The second exception above was **reasoned, not run**, and the first restart of the deployed relay
with a match in flight showed why that is a distinction (dead-ends G245): a restarted relay could
not *send* `ERR_NOT_REGISTERED`, because the MAC check runs before the handle lookup and every
datagram from a re-keyed peer fails it — the peers' session keys are the one thing the new process
does not have. Both peers sat in `bad_mac` silence and dropped the link at the endpoint's 10 s
timeout. Four changes, two per side, make the exception real:

- **Relay: a MAC failure under a handle it does not hold is answered** with `ERR_NOT_REGISTERED`
  under the deployment key, at most once per address per 500 ms (`stale_handle_replies` counter,
  `refused why=stale_handle` event), through the amplification ledger like everything else. A MAC
  failure under a handle it *does* hold stays a silent refusal — that one is a forgery, and a reply
  would be an oracle for it.
- **Relay: a HELLO naming a handle it does not hold gets that handle back** if it is free
  (`handles_restored`). Handles are not secrets and carry no authority until a token is learned;
  what granting the requested number buys is that everything the *counterpart* keys by it — pair
  keys, punch state, a host's per-client loopback socket — is still right afterwards.
- **Peer: on `NOT_REGISTERED` the handle is kept, the ratchet is dropped** (`my_key_proved`
  cleared, so the re-HELLO goes out under the key the relay actually holds — R1's "zero the handle"
  left it standing, and the re-HELLO itself then failed the MAC), the relayed DATA path is paused
  until the WELCOME (a promoted pair is untouched), and the HELLO retries at R1's 500 ms naming the
  old handle. `net: udp relay leg LOST` … `net: udp relay leg RESTORED after N ms -- handle N kept`.
  The leg then runs on the deployment key for the rest of the match: a relay re-learns a token only
  from handshake datagram 2, which a live match does not send again.
- **Relay: it exits on SIGTERM.** `docker stop` sends SIGTERM, waits 10 s, then SIGKILLs; the relay
  handled only SIGINT, so every restart or redeploy took 15 s — longer than the link timeout, so no
  re-HELLO could ever have helped. The compose stack also pins `stop_grace_period: 3s`.

A mid-match client whose room is refused `no_host` (the host has not re-registered yet) does not
fall back to the directory room: that fallback is gated on *having no handle*, and a lost leg keeps
its handle. It simply retries; the host's own re-HELLO lands within one retry.

Measured on the rig (`relay_restart`, the registered scenario: match_launch pinned to the relay,
the relay killed and respawned on the client's `signal ingame`, both peers then required to
advance the game clock a further 18 s): relay back in 0.1 s, host `RESTORED after 0 ms`, client
`RESTORED after 500 ms` (its first re-HELLO beat the host's by 10 ms and drew `no_host`), both
handles kept, no `dropped`, the clock advanced. `tools/check_relay_restart.py` is the post-check
and refuses a run where either peer lacks LOST + RESTORED, either logs a drop, or the relay's log
shows one `listening` line.

### Compatibility

The HELLO's reserved `flags` byte carries bit 0, *"I can re-key from the connect token"*. The relay
ratchets a handle only when it has seen that bit, so a peer of an older build keeps the shared key
and keeps working; a newer peer against an older relay is never re-keyed and also keeps working.
Neither is weakened by the other, because the ratchet is per handle. Since `mp:R4a` the newer peer
against the older relay also **says so** — the section below.

**What to read to tell "R1c is live" from "R1c compiled":** the relay's `peers_rekeyed` counter (2
per learned session — the host end and the client end) and its `leg_rekeyed` events; on a peer,
`net: udp relay leg re-keyed` in `mh_net.log`. `peers_rekeyed = 0` on a run that claims to be
re-keyed means every leg is still on the shared key.

## The protocol level (`mp:R4a`, 2026-09-19)

The leg header's low nibble is the **wire** version, checked on both ends (`bad_version`). The
**ops** a relay speaks are a second axis the nibble does not describe: seven ops were added over
three items without touching it, each one "additive" because an older relay refuses an op it lacks
as a counted `bad_op` — counted at the relay, answered with nothing. So a peer built after an op
was added, against a relay built before it, sees a leg that comes up, a match that is carried, and
the new thing silently never happening: a pre-wave-9 relay refused the re-key op and the pair simply
never promoted, with both peers' logs clean (the failure ledger's G241, the same shape as the G245
restart silence above).
`dist:RP5` keeps the VPS relay current; a self-hosted one (`src/relay/README.md`) can still be old.

**The level is one number, owned by `leg.rs` (`PROTOCOL_LEVEL`) and mirrored in `udp_relay.cpp`,
bumped in the same commit whenever an op is added or its payload/semantics change**, with what
each level means listed beside the constant. Level 0 is "never advertised" — every build before
R4a. Level 1 is the R4a state: ops 1..14, the R1c re-key, R4b's NOT_REGISTERED on a stale handle,
R6's room re-mint, R3e.

**Where it rides, and why not a new field.** A pre-R4a relay's `hello_parse` refuses any HELLO that
is not exactly 18 bytes (`malformed`, unanswered), so a trailing byte would have made a current
peer unable to register with the deployed relay at all — the opposite of additive. The reserved
`flags` byte is the one place an old relay reads and carries unknown bits through untouched (it
tests bit 0 only), so the level is `flags >> 1`: seven bits, 0..127. In the other direction a
pre-R4a **peer** refuses any WELCOME that is not exactly 4 bytes, so the relay appends its level
**only** to a peer whose HELLO carried one; an older peer keeps getting the 4-byte form. Both
directions were measured, not assumed: an R4a HELLO at the deployed pre-R4a VPS image got a 4-byte
WELCOME back (registered, no level), and a pre-R4a HELLO at the R4a relay got the same 4 bytes
(`cargo test`: `a_hello_without_a_level_gets_the_four_byte_welcome_it_can_read_and_is_counted`).

**The peer decides, on every WELCOME** (`udp_relay.cpp on_relay_level`; logged on change, so a
relay redeployed mid-session at another level is reported again after the R4b re-HELLO):

| relay's level | peer writes to `mh_net.log` | player sees |
| --- | --- | --- |
| absent (4-byte WELCOME) = 0, or lower | `net: udp relay -- relay protocol <theirs> < <ours> (…what may silently not happen…) (mp:R4a)` | `Relay outdated (protocol <theirs> < <ours>)` on the browser's status line |
| equal | nothing | nothing |
| higher | `net: udp relay -- relay protocol <theirs> > <ours> (the relay is newer …)` — information only | nothing (every op this build sends is in the relay's table) |
| a leg version this build does not speak (`bad_version` on a datagram from the relay's address) | `net: udp relay -- relay protocol leg v<x> != v<y> …`, once per tunnel start | `Incompatible relay (leg v<x>/v<y>)` |
| an op above this build's table, tag verified (`bad_op` at the peer — a newer relay) | `… the relay sent op <n>, above this build's table …`, once | nothing |

The relay-side `bad_op` (the G241 shape) is not something a peer can observe — the old relay is
mute — which is exactly why the level is carried at registration time: the WELCOME that comes up
is the one message an old relay does send, and its shape (4 bytes) is itself the answer.

**The notice is the F3c carrier, not a new widget.** The UDP module resolves
`MH_Seam_RelayNotice` from `mh.dll` by name (the same optional module→mh.dll shape as
`MH_Seam_RelayBrowsing`; absent, the log line is the whole report and the module writes
`browser notice not deliverable in this build`), hands it the ASCII line on the pump thread;
`net_discovery.cpp` queues it and the present hook (`MH_MP_DrainRelayNotice`) arms
`browser_notice_arm_relay` on the main thread — the status-line widget the host-left / link-lost /
JOIN-refused notices already paint through, with the same 10 s dwell. It paints on the **first
browser**, which is where a relay is dialled from (`mp:R7`), so the player reads it before Create
or Join; a host that has already moved on to its lobby gets the log line only (the lobby's status
line is retail's map text).

**The relay's side.** WELCOME carries `Limits::level`; `peer_registered` events carry the peer's
`level`; the counters line has `hello_level_mismatch` — a HELLO whose level (absent = 0) is not the
relay's, counted, never refused. `mh_relay --advertise-level N` (hidden; a test knob) makes the
relay *claim* level N — `0` is the 4-byte WELCOME a pre-R4a build sends, byte for byte — without
changing anything it does with a datagram, so a scenario can stage an older relay without an old
binary. Whether a **running** relay is behind its peers is the peers' logs and this counter; the
`listening` line names the level it claims.

**Proven by:** `cargo test -p mh_relay` (the HELLO/WELCOME field both ways, absent = old, the
staged-older relay, a newer peer); `net_selftest.exe udprelaytest` arm D (the tunnel against a
stand-in answering no level / lower / equal / newer / a foreign leg version / an op above the
table: the R4a line and the notice attempt for the two "behind" shapes, silence for the match,
info only for the newer relay); the `relay_stale_notice` UI scenario (`--advertise-level 0`, the
notice captured on the client's first browser, `check_relay_stale.py` reading the line + the
queued notice on both peers and `protocol_level=0` / `hello_level_mismatch` on the relay), which
borrows `relay_browse_local`'s lanes (TL-LANEPOOL).

## What R1 deliberately did not build

- ~~**Promotion to a direct path.**~~ Built at `mp:R3` — hole punching, above.
- ~~**A relay-hosted session browser.**~~ Built at `mp:R2` — the session directory above.
- **A container image.** `mp:R4` (and Docker is not installed on the dev box).
- ~~**A per-peer leg key.**~~ Built at `mp:R1c` — the per-peer leg key, below.
- **Per-session keys at the relay.** One relay holds one PSK and serves every match under it. Fine
  for a group's own relay; a public one wants `--strict` plus a per-peer leg key.

## The directory seam runs during the match, too (measured 2026-09-18)

`MH_Seam_ClientDiscoveryTick` is driven by the game's menu state tick, and that tick **does not stop
when the match starts**: the U29 diagnostic keeps sampling `gm=3` with an empty widget list for the
whole in-game half. So every per-frame thing the seam does runs while the player is playing.

That matters here because a host **withdraws** its lobby from the relay the moment it launches the
match, which is exactly the "row vanished" event the directory half is built to react to. Ungated,
the reaction fired mid-match: the vanish handler re-armed mh.exe's browser one-shot and dropped the
connected-store snapshot while the client was in a running game, and the client left the match about
4.5 s after entering it. Both peers then stalled at ~3.2 s of game clock, the host ran on alone, and
the comparison had no comparable steps at all. Measured twice on the rig before the gate went in
(RED runs `…T051054Z` and `…T053818Z`), and reproduced at 1200 steps in ~2 min once the shape was
known. The 300-step determinism gate had been passing only because it stops about twenty steps short
of the stall.

The fix is one predicate: the directory half runs **only while the session browser is the active
widget list** — the same gate the U40 relink already uses, and the only screen that can do anything
with a directory row anyway. Answers that arrive elsewhere wait in the inbox; each is stamped when
the relay said it, not when it is read, so a lobby the relay listed before a ten-minute match cannot
be re-listed by its own backlog when the browser comes back up.

Numbers either side of the gate, relayed, UDP, two rig peers:

| run | steps asked | steps compared | verdict |
| --- | --- | --- | --- |
| before the gate | 1200 | 0 | NO COMPARABLE STEPS (client left at step 273) |
| after the gate | 1200 | 1200 | ALL PAIRS IDENTICAL |

## What `mp:R2` deliberately did not build

- **More than one row in the browser at a time.** The directory holds up to eight lobbies and the
  store keeps all of them, but the retail browser renders **one**: `browser_ui_rows`
  (`0x00e622a0`) is an array whose bound has not been recovered, and writing a second entry past it
  would be a memory bug in the game's own `.bss`. The pick is the lowest live row, deliberately
  stable rather than clever. Recovering that bound is what a multi-row browser needs first.
- **A lobby code the player types.** A relayed game is found by browsing, not by typing a code. The
  room is still a number the client learns from the directory.
