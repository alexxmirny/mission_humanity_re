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
