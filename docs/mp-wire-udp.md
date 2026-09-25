# The MP UDP wire format

Settled facts only. This is the packet the restored multiplayer will send once `mh_net_udp.dll`
(tracker `mp:T1`) and the Rust relay (`mp:R1`) exist; today the format, its two implementations and
its shared fixtures exist and nothing sends one. The decision that produced it is **D2** of
the MP refinement plan (2026-09-17); the latency vocabulary its channel-B records carry (RTT,
IPDV, loss, SRTT/RTTVAR per RFC 6298) is the project's MP latency vocabulary page.

- Normative C++ declaration: [`net_udp.h`](../src/mh_net_proto/include/mh_net_proto/net_udp.h)
- C++ codec: [`net_udp.cpp`](../src/mh_net_proto/src/net_udp.cpp)
- Rust codec: [`wire.rs`](../src/relay/src/wire.rs)
- Shared fixtures: `src/mh_net_proto/test/fixtures/udp/`
- Oracles: `net_selftest.exe udpwiretest` and `cargo test -p mh_relay`

## Why it is hand-rolled

Every mature library was surveyed and rejected (plan D2): GameNetworkingSockets needs OpenSSL and
protobuf inside a 32-bit injected DLL and documents x64 only; ENet is dependency-free C with no
crypto; every Rust option is Rust-only, and this system needs both ends — a Windows DLL in C++ and a
Linux relay in Rust. So the framing is modelled on the
[netcode.io STANDARD](https://github.com/networkprotocol/netcode/blob/master/STANDARD.md) — a
plaintext header authenticated as associated data, the sequence number used directly as the
encryption nonce, a 64-entry replay window, and a connect token sealed with a key the client never
holds — while the **cryptography is the one already in the tree**
([`net_crypto.h`](../src/mh_net_proto/include/mh_net_proto/net_crypto.h)): the PSK handshake,
ChaCha20 (RFC 8439) and HMAC-SHA256 truncated to 128 bits. No new primitive was introduced.

Everything lives in `mh_net_proto`, which is OS-free by contract: standard library and fixed-width
integers, no `<windows.h>`, no sockets, no allocation. Encoding is explicit little-endian byte work,
as in `net_wire.cpp` — a packed struct would be a different format under a different compiler.

## The packet

Total datagram ≤ **1200 bytes**, the conservative figure RFC 9000 picked so a datagram crosses the
internet without depending on PMTU discovery. A 1201-byte datagram is refused *before anything else
is parsed*, so the refusal cannot depend on the contents.

| off | size | field | plaintext? |
| --- | --- | --- | --- |
| 0 | 1 | magic/version: high nibble `0x4`, low nibble version (`1`) → `0x41` | yes, authenticated |
| 1 | 1 | packet type | yes, authenticated |
| 2 | 8 | `conn_id` — the relay's demux key | yes, authenticated |
| 10 | 8 | `seq`, LE u64 — per direction, strictly increasing | yes, authenticated |
| 18 | *n* | sealed body (ChaCha20, nonce = `seq`) | no |
| 18+*n* | 16 | tag = HMAC-SHA256(mac_key, bytes `[0, 18+n)`) truncated to 128 bits | — |

Derived sizes: header 18, tag 16, smallest datagram 34 (a sealed empty body — a keepalive), largest
body **1166**.

Packet types: `0` DATA, `1` TOKEN, `2` TOKEN_ACK, `3` KEEPALIVE, `4` DISCONNECT. An unknown type is
refused, not ignored.

**The whole header is plaintext and authenticated.** A relay must read `conn_id` without holding any
key, and nothing in the header may be editable in flight — so the header is associated data, even
though the construction is encrypt-then-MAC rather than a packaged AEAD.

**The sequence is the nonce.** `chacha20_xor()` places the 64-bit `seq` in the RFC 8439 nonce, so a
repeated `seq` within one direction repeats a keystream. Hence: sequences are per direction, the two
directions have separate keys, and a sender must never rewind. The replay window protects the
*receiver*; a sender that reuses a sequence breaks the cipher no matter what any receiver does.

### Decode order

Fixed, and part of the format:

1. length > 1200 → `too_long`
2. length < 34 → `too_short`
3. magic nibble → `bad_magic`
4. version nibble → `bad_version`
5. packet type → `bad_type`
6. `conn_id` ≠ this session's → `wrong_conn`
7. replay-window **check** → `replay`
8. MAC → `bad_mac`
9. decrypt, then replay-window **commit**
10. the body's channel framing → `malformed` (a separate question from authenticity)

Steps 7 and 9 are deliberately split. The check runs before the HMAC so a duplicate flood costs no
crypto; the commit runs after it so a forged packet carrying a plausible future sequence cannot push
the window forward and make the *real* packet at that sequence look like a replay.

### Replay window

netcode.io's rule, 64 entries: a 64-bit bitmap whose bit *i* means `newest - i` was accepted.

- `seq > newest` → fresh; on commit the bitmap shifts and `newest` moves.
- `newest - seq < 64` and the bit is set → duplicate, dropped.
- `newest - seq ≥ 64` → below the floor, dropped. Not necessarily a duplicate — a window simply
  cannot prove it is not one.
- A jump of more than a window clears the bitmap: nothing older can be judged any more.

## The three channels

The sealed body is a sequence of frames, each `id(1) | len(2 LE) | payload(len)`, so one datagram
can carry step inputs and a ping together. **A receiver ignores a channel id it does not know** and
keeps walking — the same forward-compatibility rule `net_wire.h`'s flag comment states, for the same
reason: "unknown means data" turns every channel added later into a desync.

### A — step inputs (`id 1`, unreliable)

`count(1) | newest_step(4 LE) | count × { len(1) | bytes }`

Entry *i* is the input for step `newest_step - i`, newest first; the step numbers are **implied**,
which is what makes the redundancy cheap — 5 bytes of header for the whole run instead of 4 per
entry. Each packet carries the last **K** steps, K configurable 1..8, default **3**. There are no
acks and no retransmit: lockstep inputs are tens of bytes, so repeating them repairs loss with zero
added round trip (plan D2; the same trade AoE's *1500 archers* describes).

Refused: K outside 1..8, truncation, trailing bytes. The *encoder* additionally refuses a set of
entries whose steps are not the implied descending run — a caller that gets the order wrong produces
a payload no decoder could distinguish from a correct one, so that check can only live at the source.

### B — latest-wins records (`id 2`)

`repeated { id(1) | len(1) | bytes }`, the newest record per id winning. No queue, nothing to
retransmit: an old ping sample or a stale telemetry line has no value. An unknown record id is
skipped — which is what the length byte is carried for.

| id | record | payload |
| --- | --- | --- |
| 1 | `PING` | `t_origin_ms(4 LE)`, `t_echo_ms(4 LE)`; `t_echo` is 0 in a fresh probe |
| 2 | `PONG` | the same two fields: the probe's origin echoed, plus the responder's own stamp |
| 3 | `STEP_HASH` | `step(4 LE)`, `hash(4 LE)` — the desync sample's UDP carrier |
| 4 | `TELEMETRY` | `srtt_us(4)`, `rttvar_us(4)`, `loss_q16(4)`, `last_step(4)`, all LE |

`loss_q16` is a fraction in Q16 (65536 = 100%). The names follow
SRTT and RTTVAR are RFC 6298's smoothed estimators (alpha 1/8, beta 1/4), not raw samples.

**Ping cadence (`mp:P15`).** Each peer is pinged at `FAST_PING_MS` (250 ms, 4 Hz) for
`FAST_PING_WINDOW_MS` (3 s) after IT is admitted, then drops to the steady `[net] ping_ms` (1000 ms
by default). This is per-conn, not per-endpoint: two peers admitted seconds apart each get their own
warm-up window (`udp_endpoint.h`'s `Conn::admitted_ms` / `Conn::last_ping_ms`). It exists because
`mp:P14`'s adaptive-lookahead start seed needs `AD_START_MIN_RTT_SAMPLES` (3) pings before it can
fire, and at 1 Hz that is a 3 s floor on top of however long the lobby took — measured on the o4 rig
lanes (a hand-clicked lobby, match starting ~1.4 s after connect) accruing only 2 samples and falling
back to the 100 ms guess P14 exists to replace every run. Traffic cost: at most ~12 extra ~40-byte
sealed pings per peer per side, once per connect (see `udp_endpoint.h`'s note on the constants for the
exact accounting). The decision itself is a pure function (`udp_ping_cadence.h`'s `ping_due`), driven
offline by `net_selftest.exe udpstatstest`'s section (o).

### C — bulk reliable (`id 3`)

A chunk is at most **16 KiB** (plan D7) and is fragmented into pieces of at most **1100** bytes, so a
piece plus its 51-byte header plus the packet header stays under 1200.

**The stride is a property of the LINK, not of the format (`mp:R1d`).** 1100 is the direct-path
figure. A relayed link runs at **1024**, because the relay leg wraps 34 bytes around every datagram
and a full-size piece at 1100 would be 1222 on the wire — outside the very guarantee the 1200 is
for. Both ends derive the same stride from the same fact (`[net] relay` is set), and every entry
point that touches it takes it explicitly — `piece_count`, `piece_for`, `piece_decode` — so a site
that forgets one fails to compile rather than reassembling a chunk at the wrong offsets. A receiver
bounds a piece's offset by **its own** stride, so a peer sending at a stride it did not agree to is
refused as a mismatch rather than merged. 1024 divides 16 KiB exactly, so a full chunk is 16 whole
pieces and the last-piece remainder stops being the common case.

```
PIECE : kind(1)=0 | chunk_id(4) | index(2) | total(2) | chunk_len(4)
        | sha256(32) | piece_seq(4) | piece_len(2) | bytes
ACK   : kind(1)=1 | ack_seq(4) | ack_bits(4)
```

`ack_bits` is Gaffer's 32-bit bitfield: bit *i* acknowledges `ack_seq - 1 - i`, so one ack covers 33
pieces and a lost ack is repaired by the next one.

Every piece carries the **whole chunk's SHA-256**. That costs 32 bytes per piece and buys what D7
asks for: a piece is self-describing, so a transfer is resumable by index after a restart with no
side channel saying what was being sent. A piece is refused if its declared length disagrees with
the frame, if `index ≥ total`, or if `total` is not the piece count its own `chunk_len` implies.

#### The transport's two acknowledgements — a third `kind`, and why

The three fields above are T0's and unchanged. What `mp:T2` added to the **module** — not to
`mh_net_proto`, and not to anything the Rust relay reads — is a third `kind` on this channel:

```
BULK_ACK : kind(1)=2 | ack_seq(4) | ack_bits(4)      -- same shape as kind 1
```

It exists because `mp:T1` had already taken `kind = 1` for a **different** frontier: the channel-A
segment stream's acknowledgement, which drives that stream's timeout retransmit. That reuse is
shipped and measured, so T2 did not take the frame back and did not renumber it — it added a kind of
its own, and `udp_endpoint.cpp` routes a CH_BULK frame on its first byte (0 → piece, 1 → the stream
frontier, 2 → the piece frontier). Both ends of a match are the same module build, and a relay never
opens a body at all, so the extension is invisible outside `mh_net_udp.dll`.

Two semantics the module pins down, because the framing does not:

- **`piece_seq` is an address, not a counter:** `chunk_index × 16 + piece_index`. A chunk is at most
  16 pieces (15 at the direct stride, 16 at `mp:R1d`'s relayed one), so 16 numbers every piece
  uniquely and makes the 32-bit bitfield mean exactly two chunks — which is the transfer window. It is derived from the chunk index both ends already have,
  so nothing has to survive a restart for a resume to work.
- **`ack_seq` in a BULK_ACK is the EXCLUSIVE frontier** — one past the top `piece_seq` the bitmap
  describes — and the bitmap alone acknowledges. A receiver holding nothing must still be able to
  send an acknowledgement, and an inclusive frontier would have it claim a piece it does not have.

The **sender advances only on the receiver's frontier**, never on its own "all my pieces are
acknowledged" reasoning. That is not a style choice: a receiver whose delivery lane is full reports
its chunk fully received and deliberately does *not* advance (that is the backpressure), so a sender
with its own advance rule would run a chunk ahead and apply the next bitmap to the wrong chunk.

Implementation: [`udp_channel_c.h`](../src/mh_dll/mh_net_udp/udp_channel_c.h); oracle
`net_selftest.exe udpbulktest`.

#### What rides channel C: the snapshot envelope (`mp:X1`)

Channel C moves *a sequence of chunks* and holds no opinion about what they mean — the length and
the chunk count are the application's. `mp:X1` is the application: one pipeline with the three
consumers plan D7 names (join-in-progress, desync recovery, MP save/load), moving a
`mh::state::world::capture()` blob and, later, a map file.

The image it hands the channel is a **padded manifest followed by the body**:

```
transfer chunk 0 .. manifest_chunks-1    the MANIFEST, zero-padded to a chunk boundary
transfer chunk manifest_chunks + i       body chunk i, SHA-256 == manifest chunk_sha[i]

MANIFEST : magic(8)="MHSNAP\0\1" | format(4)=1 | body_len(4) | chunk_count(4)
           | manifest_chunks(4) | body_sha256(32) | root(32) | chunk_sha256[chunk_count]
root     = SHA-256(the whole manifest with its own root field zeroed)
```

Nothing here is on the packet wire: it is chunk *content*, so `mh_net_proto`, its fixtures and the
Rust relay are untouched, and the manifest crosses as ordinary channel-C chunks with ordinary
per-chunk hashes.

**The padding is load-bearing.** Without it the body's chunk boundaries would be offset by the
manifest's odd length, `chunk_sha[i]` would describe bytes no single transfer chunk contains, and a
receiver could not verify anything until it held everything — which is exactly the property
"resumable by chunk index" exists to buy. It costs at most 16 KiB once. For the current world blob
(7,857,992 bytes, 480 body chunks) the manifest is 15,448 bytes and pads to a single chunk.

**Why a manifest at all, when every piece already carries its chunk's SHA-256.** The transport's
hash answers *did these bytes survive the network*, completely — and it cannot answer the two
questions above it, because the hash it checks against arrived in the same frames as the data. A
sender whose build is wrong, or whose blob moved under it mid-transfer, emits self-consistent
garbage: correct pieces, correct SHA, wrong content. The manifest is the hash vector committed to
**before** the transfer, so every chunk is checked against a promise made before it was sent. And
completeness is not a property any single chunk has: `body_sha256` is the one check that does not
depend on the reassembly arithmetic being right.

**Re-request and resume are the same primitive, and there is no NAK frame.** T2 made the receiver's
frontier the only thing that steers a transfer, in both directions, so X1 drives both through
`Channel::rx_resume_at()`: a chunk that disagrees with the manifest rewinds the frontier to it and
the sender's backward `tx_seek` re-sends it; a transfer truncated by a dead link is resumed by
restoring the frontier to `manifest_chunks + verified_prefix`, which the first acknowledgement
carries.

**The module does not import.** `world::import()` lives in libmh inside mh.dll, and `mh_net_udp.dll`
must not reach it — the module is absent-tolerant by design and libmh's outbound edge is measured
(`tools/check_libmh_outbound.py`). The pipeline's contract stops at `complete()`. That makes the
all-or-nothing rule a check in **two layers on purpose**: the receiver's gate never opens with a
chunk missing, and `world::import()` validates the whole blob before its first write. Neither is the
other's backup — they refuse different things, and the suite watches both.

Measured 2026-09-18 (in-process, two real endpoints on 127.0.0.1, one clock): the 7,857,992-byte
world capture — 480 body chunks plus one manifest chunk — crosses at **5% injected loss in both
directions** in **31.5 / 43.3 / 59.8 s** across three runs (478–549 datagrams destroyed, 357–369
piece retransmits, zero chunks evicted, zero wire SHA failures, zero manifest refusals), and imports
byte-identically every time. The spread is the machine, not the link — seven builds were running
alongside — and the ceiling is `BULK_BURST` × the endpoint tick, the rate limit that keeps a
lockstep datagram out of a bulk burst. **A full world snapshot is therefore a 30–60 s transfer at
this rate**, which is a number join-in-progress has to design around rather than inherit.

Implementation: [`udp_snapshot.h`](../src/mh_dll/mh_net_udp/udp_snapshot.h); oracle
`net_selftest.exe udpsnaptest`.

#### One transfer per endpoint (`mp:T2a`)

**Channel C carries at most one transfer per endpoint at a time, and there is no transfer id on
the wire.** `chunk_id` in the PIECE frame is the chunk INDEX inside the one running transfer, not a
slot selector across several — so a second `start_send` while one is active is refused rather than
multiplexed. This is a **decided contract** (path B, user decision, 2026-09-23; `mp:T2a`, opened as
`mp:T2`'s residue): X1 (world snapshot) and X2 (map download) each want to reach more than one peer,
or receive more than one blob, and they get there by **serialising through the SAME channel slot**,
never by inventing a second one.

- **The refusal.** `Channel::start_send`
  ([`src/mh_dll/mh_net_udp/udp_channel_c.cpp:173`](../src/mh_dll/mh_net_udp/udp_channel_c.cpp)):
  `if (m_tx.active) return false; // one transfer at a time -- see the header`. `start_send_src`
  (the map/snapshot pipeline's entry) calls through the same gate.
- **The caller retries.** `MH_Net_SnapshotSend`
  ([`src/mh_dll/mh_net_udp/udp_transport.cpp:644-647`](../src/mh_dll/mh_net_udp/udp_transport.cpp)):
  a `false` from `bulk_send_src` is "not an error and not a state: the peer is not admitted yet, or
  a transfer is already running. The caller retries." — it tears the sender's copy down and returns
  `0` rather than queuing anything.
- **map_transfer.cpp's own re-arm loop is that caller.** `host_pump_transfer`
  ([`src/mh_dll/mh/seams/map_transfer.cpp:97-101`](../src/mh_dll/mh/seams/map_transfer.cpp)) arms
  one joiner's map transfer and, until that peer reports the map as held (`host_on_join`) or the
  attempt times out (`TX_REARM_MS`, 90 s), does not attempt the next: "channel C carries one
  transfer per link at a time... so serialising three joiners costs seconds rather than the
  multiplexing machinery a simultaneous push would need." Two joiners who both need the map are
  therefore delivered **one after the other**, off the SAME advertised host claim, never
  concurrently.
- **The sender decides only after the peer's report is whole (`mp:T6`, 2026-09-24).** Two threads
  share the per-peer map state. `host_on_join` runs on the transport's RECV thread and records what
  the joiner's JOIN says it holds. `host_pump_transfer` runs on the MAIN thread (the lobby tick) and
  picks the peer to send to. The first cut published a report in two steps: `seated` at the top,
  `holds` at the bottom, with a log write in between. A pump inside that gap armed the 462 KB
  snapshot to a joiner that already held the map. It showed as `snapshot SEND armed` ~4 ms after
  `peer 1 'client' holds the map`, in 4 of 13 wave-4 shim runs and in 20 of the 67 D30 runs where the joiner held the map. Now
  `g_host_lock` (a leaf SRW lock) covers the peer table and `g_tx_peer`. A report is published in one
  step. The pump reserves its chosen peer, reads the file outside the lock, and re-checks before it
  sends. A report or leave that lands in between withdraws the send
  (`; [map] send WITHDRAWN for peer <n>`). **Selftest**: `net_selftest.exe maptest` arm H drives
  both interleavings through test hooks with a counting stub sender. Holds → no snapshot; lacks
  (the forced-snapshot shape) → exactly one. Each of the three mutants turns it red: the early
  `seated` publish, no re-check, and always withdraw.

**A bug this contract's own selftest arm found, and fixed in the same change (2026-09-23).**
`Channel` is one object PER ENDPOINT, so `m_tx` addresses whichever peer is CURRENTLY being sent
to — but `Channel::on_ack` used to apply an incoming ack to `m_tx` unconditionally, without checking
which CONNECTION it arrived on. A peer whose own transfer just finished keeps re-announcing its full
frontier for up to ~2 s after its last piece (`tick()`'s receiver heartbeat), so its FINAL ack could
still be in flight — or repeated — after the host had already re-armed a *different* transfer to a
*different* peer; applying it unfiltered clamps `rx_base` to the new transfer's chunk count and
completes it in one tick, with the second joiner having received almost nothing and no hash ever
disagreeing (the bytes it DID get were correct — it just never got the rest). A two-endpoint arm can
never produce this ack, which is why nothing before `mp:T2a`'s own multi-joiner arm exercised it.
Fixed by giving `on_ack` the connection index (mirroring `on_piece`'s signature) and ignoring an ack
whose connection does not match `m_tx.conn`.

**Selftest**: `net_selftest.exe udpbulktest`'s "two joiners" arm — a host and TWO joiners, both
needing the same payload: the second `start_send` is refused while the first transfer is active,
both eventually receive the identical payload (hash-verified via each chunk's own SHA-256, `0`
`rx_sha_fail` on either receiver), one after the other. Its mutation-red half needs no source
mutation to prove: the arm drains the second joiner's channel for the ENTIRE duration of the first
transfer while calling `start_send` to it exactly zero times, and confirms it received nothing —
demonstrating that a caller who tried once and never retried would leave that joiner with nothing
forever, not merely "not yet".

## The connect token

netcode.io's central idea, kept: the host mints a token at lobby time and seals its private part
with a key only the host — and any relay it authorises — holds. A client presents the token it was
given and cannot read, forge or extend one. The session keys travel *inside* the sealed part, so
possession of a token is what admits a peer; no pre-shared key is typed by a stranger.

The handshake that presents a token is `mp:T1`'s. This is the record and its codec.

```
wire (210 bytes)
  0    magic/version(1) | kind(1) = 1
  2    nonce(8 LE)            -- the ChaCha20 nonce, unique per minted token
  10   conn_id(8)             -- PLAINTEXT: a relay demuxes before it can open anything
  18   expire_unix_ms(8 LE)   -- PLAINTEXT: a relay drops an expired token with no key at all
  26   ciphertext(168)
  194  mac(16)                -- over bytes [0, 194)

private part (168 bytes)
  match_id(16) | conn_id(8) | slot(1) | reserved(7, zero) | expire_unix_ms(8)
  | enc_c2s(32) | enc_s2c(32) | mac_c2s(32) | mac_s2c(32)
```

The two host keys are derived from one 32-byte secret by label
(`HMAC(host_key, "mh-token-enc")` / `"mh-token-mac"`), so a deployment configures one thing and the
two uses can never accidentally be the same bytes.

`conn_id` and the expiry are carried **twice**, outside and inside, and `token_open()` requires them
equal. That is not redundancy for its own sake: the outside copy is what an unauthenticated relay
routes on, the inside copy is what the host trusts, and requiring equality means a token a
key-holder minted inconsistently is refused rather than believed by one party and not the other.
An expired token returns `bad_mac` on the wire — deliberately indistinguishable from a wrong key,
since telling an attacker which of the two applied is free information. The caller learns the
difference from a separate out-parameter.

`match_id` is the UUIDv7 from `SES0` ([`uuid7.h`](../src/mh_net_proto/include/mh_net_proto/uuid7.h));
`conn_id` is `conn_id_from_match()` — the first 8 bytes of `match_id` with the peer slot XORed into
byte 7 — so a packet's routing id and its match's logs cannot disagree. Slots are 0..7.

## The fixtures

`src/mh_net_proto/test/fixtures/udp/` holds `index.json` plus one `.bin` per case (and a
`.plain.bin` for every case expected to decode). Both oracles read these files; **neither generates
them at test time**. They are emitted by the C++ encoder:

```
net_selftest.exe udpwiretest --emit src/mh_net_proto/test/fixtures/udp
```

That direction is deliberate. An encoder agreeing with its own decoder proves one program
self-consistent, which it would be just as happily over a format nothing else can read. The claim
worth making is that two independently written implementations accept and refuse the same committed
bytes — so when the format changes, the C++ encoder moves first and the Rust decoder must still
agree, which is the drift that would otherwise ship.

`index.json` is a JSON array of flat objects **whose values are all strings, numbers included**. The
narrowing exists because the C++ side has no JSON library and the Rust side has `serde_json`: a
grammar this small is ~60 lines of C and still an ordinary JSON document there. The C++ reader
*refuses* any deviation (a bare number, a nested object, an escape) rather than best-effort parsing
it — an index one side misreads is exactly the failure a shared fixture set exists to prevent.

Per-case fields: `kind` (`packet`/`token`), `key`, `wire`, `verdict`, and for packets `enc_key`,
`mac_key`, `conn_id`, `seq`, `type`, `window`, optional `plain`; for tokens `host_key`, `now_ms`,
`expired` and the expected contents. **`window` names a replay window shared by every case carrying
that name**, and cases are processed in index order — so a `replay` case is a genuine second
delivery into a window an earlier case already advanced, not a flag the harness sets.

Verdict names are the shared vocabulary (`ok`, `too_long`, `too_short`, `bad_magic`, `bad_version`,
`bad_type`, `wrong_conn`, `replay`, `bad_mac`, `malformed`, `body_too_long`) and both
implementations spell them identically, because the index records the expected one as that name.

The four acceptance cases `mp:T0` names are each a committed fixture, not only an in-process
assertion: `p01`–`p06` and `p12` round-trip, `p08_tamper_body` and `p10_tamper_tag` fail the MAC,
`p07_replay_of_p01` and `p18_below_floor` are dropped by the window, and `p11_too_long` is 1201
bytes. `p12_max_size` is exactly 1200 and sits next to `p11` on purpose: together they pin the
boundary from both sides, so an off-by-one reds one case or the other.

## Flow control on the segment stream (`mp:T4b`, `mp:T5`)

Channel A's reliable stream (`udp_endpoint.cpp`) sends at most **1024 segments** past the peer's
acknowledgement frontier. Until T4b, reaching that limit **dropped the link** (`outbound stream ran a
full window ahead of the peer's acknowledgements`), on the theory that a full window meant a dead
peer. Two rig runs showed otherwise. In `mp:T4`, a per-frame horizon advert at 1100–3500 fps filled
the window inside one 360 ms round trip. In `mp:T5`, a lobby at a 500 ms round trip dropped after a
3.4 s machine-wide freeze while the peer was **still acknowledging** (451 segments delivered, 12
keepalives answered). A full window means the peer is behind. It does not mean the peer is dead.

**The design: back-pressure, with bundling only while back-pressured.**

- **Open window: no change.** While nothing is queued and the window has room, a frame goes out in
  the call that wrote it, in its own segment, exactly as before. **This path adds no latency.**
- **Full window: the bytes queue.** Whatever does not fit waits in a per-peer backlog, a 256 KiB
  ring. Every later write queues behind it, so the stream cannot reorder. Each frontier advance
  sends the backlog, and so does every 20 ms timer tick as a backstop, at most 64 segments per call
  (≥ 3200 segments/s). The backlog is cut into **full 254-byte segments**, so it is *bundled*: 21-byte
  adverts that took a whole segment each while the window was open take 1/12 of one each once it
  is full.
- **The drop is kept, for a dead peer only, and stated as time.** A peer whose frontier has not moved
  for the link timeout (`rx_timeout_ms`, default 10 s) while data is outstanding is dropped:
  `dropped -- the peer acknowledged nothing for N ms ...`. That is the bound the silence watchdog has
  always promised a dead peer. It also covers the one shape the watchdog cannot see: pings arrive,
  acknowledgements do not. With the watchdog off (`rx_timeout_ms=-1`, the debugger setting), only
  the size bound remains. That is a full window plus 256 KiB queued (`dropped -- the outbound
  backlog overflowed`), which is still a size limit, as the old rule was.
- **The receiver pauses instead of losing frames.** The reorder window holds 1024 segments. The
  application's inbound ring (`mp:U41e`) is the SAME sequence-merged lane pair `mh_net.dll`'s TCP
  transport uses: a 4096-slot lane for bare horizon adverts (evictable by construction, never
  blocks) and a 256-slot lane for everything else, which is the only one that can lose a frame — and
  it REFUSES an arrival rather than destroying one already queued. Before `mp:U41e` this was a
  single 256-slot FIFO whose D24 scan could force-evict a real, non-supersedable input when nothing
  evictable was left; the offline stall arm hit exactly that. So a segment is consumed only while the
  must-keep lane has 32 free slots (the horizon lane's own capacity is irrelevant to this guard,
  since it never blocks anything downstream). Otherwise delivery **pauses**: the published frontier
  stops, the sender's window fills, and the timer resumes delivery each tick. The horizon lane keeps
  evicting its own head as it always has — that is what it is for.

**Retransmit timing follows the measured round trip (T5).** `RTO_MS` was a constant 200 ms. That is
below every round trip the rig and the field run at: field SRTT is 205–230 ms, and the rig shims are
360 and 500 ms. So every segment was re-sent before its acknowledgement could arrive. On every clean
250 ms-one-way rig run the host logged about 2× as many re-sends as new segments. The RTO is now
RFC 6298's `SRTT + max(4·RTTVAR, ACK_MS + 2·TICK_MS)`, with the old 200 ms as the floor and 2 s as
the cap. It doubles each time the oldest segment times out, up to three times (RFC 6298 §5.5), and
resets on the next frontier advance. Channel C's two timers follow the same measurement. The blind
RTO is `max(250, link RTO)`, and the fast-retransmit guard is `max(30, SRTT + 30)`. The old 30 ms
guard let the first acknowledgement after any piece "refute" it: T5's host sent 681 pieces, 591
of them repeats. Offline at a 360 ms round trip, the old timers repeated 401 of 521 pieces and the
new ones 0 of 120.

**Latency cost.**

| Situation | Before | After |
| --- | --- | --- |
| Window open (every normal frame) | sent at once | **unchanged**: sent at once, own segment |
| Window full | link dropped | frame waits for the frontier to move: at least one round trip for the oldest outstanding segment, plus ≤ 20 ms for the pump tick |
| Inbound ring full of non-evictable frames | a frame destroyed (desync) | delivery waits ≤ 20 ms per timer tick for the application to drain |
| Loss the K=3 window did not cover, round trip < ~120 ms | repaired after 200 ms | **unchanged** (the floor) |
| Same, at the field's ~220 ms round trip | 200 ms (and every other segment re-sent spuriously) | ~300 ms; no spurious re-sends |
| Same, at a 500 ms round trip | 200 ms (≈ 2× re-send traffic) | ~580 ms; doubles while the oldest segment keeps timing out |

The K-window already repairs almost every loss (at 5 % loss, K = 3 leaves 1.25·10⁻⁴ per segment),
so the RTO rows are the rare case.

**Wire compatibility: none needed, nothing negotiated.** Nothing new goes on the wire. The segment
header, the acknowledgement and the channel mux are all unchanged. A bundled segment carries the tail
of one frame and the head of the next, and the reassembler has always accepted that: `stream_drain`
walks a segment frame by frame and carries a partial header across a boundary. That parsing code is
unchanged since `v0.2.0-rc2`: before T4b, `git diff v0.2.0-rc2` had no hunk in `on_input_frame` or
`stream_drain`, and T4b's only edit there is the pause check before a segment is consumed. So **an
rc2 peer reads everything a T4b peer sends**. The reverse also holds, with
one caveat. An rc2 *sender* still has its old rule, so it drops a link whose window fills. That is
rc2's behavior with any peer, not something mixing builds causes.

**Alternatives rejected.**

- *Nagle-style bundling on the open-window path* (hold small frames until a segment fills, or until
  a per-present flush). It halves the datagram count of a flood. But it adds up to one flush
  interval of latency to **every** lockstep frame, which is exactly what the transport exists to
  avoid. It also does not remove the class: a burst of large frames still fills the window. Here
  bundling happens only when the window is already full, so it costs nothing.
- *Refusing the write at the API* (`MH_Net_Send` returns 0 on a full window). mh.exe's lobby and
  lockstep callers do not retry a failed send, so a refused order is a lost order: a desync with
  extra steps.
- *A bigger window.* It moves the cliff without removing it. T4's flood would have filled 4096
  segments within about a second at 3500 fps.

The oracle is `net_selftest.exe udploopbacktest`, with four arms through an in-process delay relay
at a 10 Mbit/s link rate:

- **burst**: a steady phase at 360 ms asserting no spurious re-sends, 128 KiB of channel C asserting
  no repeated pieces, then 2548 small frames at once, all of which must survive, in order, through
  the backlog.
- **stall-then-bulk**: T5 rebuilt. A 500 ms round trip, a 462 KB transfer, and a 3.4 s freeze that
  keeps 64 KiB per direction. The link must survive, every frame must arrive in order, and every
  chunk must verify.
- **stops acknowledging**: a mute peer dropped by the kept rule within the timeout and not early, a
  silent peer dropped by the watchdog, and the size bound with the watchdog off.
- **inbound ring full**: 700 non-evictable frames at a peer that is not draining. Nothing is
  destroyed, and every frame arrives once the peer drains.

Each fix has a mutation that turns an arm red.

## What T1 and R1 consume

- **`mp:T1` (`mh_net_udp.dll`)** — `packet_encode`/`packet_decode`, `ReplayWindow`, the frame mux,
  channels A and B. Channel C's piece half is `mp:T2` (`udp_channel_c.{h,cpp}`), reached through the
  module's internal headers only: the module still answers exactly the same 23-export ABI the TCP
  module does, because a bulk row would be a 24th the TCP module cannot answer. `mp:X1`'s snapshot
  pipeline sits on top of it the same way and for the same reason, which is also its one open end:
  nothing in `mh.dll` can drive a bulk transfer today, because driving one from the game would be
  that 24th export. The TCP module stays shipped as the fallback and the determinism-gate
  reference.
- **`mp:R1` (the Rust relay)** — only `Header::peek` and `ReplayWindow` are needed to forward a
  packet: a relay holds no session key, so `conn_id` and `seq` are all it can read, and all it
  needs. It uses `token_open` to decide who is registered. The rest of `wire.rs` exists because the
  fixtures test it and because R1's `--local` mode stands in for a client.
- **`mp:R2` (the relay's session directory)** — consumes **nothing of T0**, and that is the point
  worth recording here rather than only on the relay page. A host publishes its `SESSION_INFO`
  (`src/mh_net_proto/include/mh_net_proto/session_info.h`, ≤ 473 bytes in v4) to the relay and a
  browsing client is handed it back; the relay stores and returns those bytes as an **opaque blob**
  capped at 512 and never decodes one. So the browser's rows are built by the same `SESSION_INFO`
  codec both peers already share, on the peers — the relay gained a payload it carries, not a
  format it understands. Two consequences for this page: the descriptor is not a T0 channel and has
  no fixture here, and `SESSION_INFO`'s size ceiling is now also a **relay** constraint (a v5 field
  that pushed it past 512 would need the relay's `DESC_MAX` raised in the same change).
- **`mp:R3` (hole punching)** — consumes **nothing of T0 either**, and for a sharper reason than R2:
  a promoted pair sends the *identical* leg datagram carrying the *identical* T0 payload, at the
  peer's address instead of the relay's. Nothing on this page changes when a pair goes direct —
  not the header, not the channels, not the MTU (the leg envelope is kept on the direct path
  precisely so the ceiling does not move under a transition), and not the connection: `conn_id`
  already survives a rebinding, which is what lets the source address change without the session
  noticing. The candidate list and the probes are leg ops, not T0 frames, and live on the relay
  page. The one thing worth knowing here is the negative: **a path promotion is not a reconnect**,
  so nothing in `udp_endpoint.cpp` is told about one and nothing needs to be.

## Running the oracles

```
net_selftest.exe udpwiretest                 # fixtures + the range properties
net_selftest.exe udploopbacktest             # mp:T1 stream + mp:T4b/T5 flow control (~40 s)
net_selftest.exe udpsnaptest                 # mp:X1 -- the snapshot pipeline end to end
net_selftest.exe udppunchtest                # mp:R3 -- the hole-punch promotion state machine
net_selftest.exe udpwiretest --fuzz 600      # the seeded decoder fuzz, under the ASan build
net_selftest.exe udpwiretest --emit <dir>    # regenerate the fixtures
cargo test -p mh_relay                       # the Rust half, same fixture files
```

The fuzz arm is **not** a gate row: it is a timed run, reported per item, because a gate step that
costs ten minutes is a gate nobody runs. Its claim is narrow and worth stating precisely — not that
the decoder returns the right answer on garbage, but that it never reads or writes outside its
buffer while deciding. That is what ASan reports and what a hand-written bounds test cannot.

The suite finds the fixtures by walking up from the working directory (the gate runs the exe with
the repo root as cwd) or from `MH_UDP_FIXTURES`. Finding none is a **failure**, not a skip: a run
that quietly tested nothing would report green.
