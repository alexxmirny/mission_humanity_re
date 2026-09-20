# mh_relay

The single-port UDP relay for Mission Humanity multiplayer, and **the relay's published page**
(tracker `mp:R1`, the session directory of `mp:R2`, hole punching of `mp:R3`, the protocol level of
`mp:R4a`; the page itself is `mp:R3d`). One bound socket serves many matches: peers register into a
**room**, the relay pairs a room's host with its clients and forwards between them, and the sealed
game datagram crosses byte for byte. A host also publishes one opaque **session descriptor** for the
room it holds, and any registered peer can ask for the set — the **directory** — which is what makes
a player's in-game first list show the games running on a relay before it has connected to any of
them (`mp:R7`).

```
mh_relay --bind 0.0.0.0:7100 --key-file <the players' mh_key.txt>
mh_relay --local        # loopback, human-readable logs, the all-zero "open" key
mh_relay --health 127.0.0.1:7100 --key-file <same file>   # probe: exit 0 on a PONG (dist:RP5)
```

`--health` is the container healthcheck (`deploy/docker-compose.yml`): one PING from no handle,
tagged under the deployment key, answered with a PONG of the same size and counted as
`health_pings` — it registers nothing and logs nothing. It proves the socket is served *and* the
key the prober holds is the relay's; under a wrong key the relay answers nothing (`leg_bad_mac`).

The full settled design — the leg envelope's bytes, why routing is by a peer handle rather than by
the T0 connection id alone, how a session's MAC keys are learned from the host's connect token, the
3× anti-amplification cap and the ping that lifts it, the candidate exchange and the promotion rule,
and the client half in `mh_net_udp.dll` — is [`docs/mp-relay.md`](../../docs/mp-relay.md). The
packet it carries is the shared UDP wire format ([`docs/mp-wire-udp.md`](../../docs/mp-wire-udp.md)),
implemented here in `wire.rs` and in `src/mh_net_proto` (C++) against the same committed fixtures.
What a *player* does is [`docs/mp-internet.md`](../../docs/mp-internet.md).

- [The files](#the-files)
- [What it can and cannot see](#what-it-can-and-cannot-see)
- [Punch-then-relay: a match starts relayed and goes direct](#punch-then-relay-a-match-starts-relayed-and-goes-direct)
- [The deployment key](#the-deployment-key)
- [Keep it as new as the players' build](#keep-it-as-new-as-the-players-build)
- [Self-hosting](#self-hosting)
- [The counters line](#the-counters-line)
- [Tests](#tests)

## The files

All four live under this crate's `src/`.

| file | what it is |
| --- | --- |
| `main.rs` | the CLI, the tokio socket loop, the structured logs, and the loopback acceptance test |
| `relay.rs` | the decisions: rooms, peers, sessions, the session directory, counters. Owns no socket and no clock, so all of it is testable without binding a port |
| `leg.rs` | the peer↔relay envelope: 18-byte header, HMAC tag, fourteen ops, and the **protocol level** (`PROTOCOL_LEVEL`) that says which of them this build speaks |
| `wire.rs` | the Rust half of the T0 packet format (`mp:T0`), read against the C++ side's fixtures |

## What it can and cannot see

The relay opens each session's **connect token**, keeps the two MAC keys and **discards both
encryption keys**. So it authenticates every datagram it forwards — a registered peer sending a bad
MAC is dropped and counted, never passed on — and cannot read a byte of the match.

The session descriptor is the same story one level up: the relay stores and returns the host's bytes
as an **opaque blob** (≤ 512 B) and never parses one. What a descriptor means is the game's
`SESSION_INFO` format, which both peers already share; a third implementation of it inside the one
process designed not to read the match would buy nothing.

## Punch-then-relay: a match starts relayed and goes direct

The relay is **relay-first, promote silently** (`mp:R3`). Every match starts through it — that is
why nobody forwards a port — and then each peer pair tries, in the background, to replace the relay
hop with a direct one:

1. **Candidates via the relay.** Each peer sends the addresses it might be reachable at (its local
   interfaces, IPv4 and IPv6) as a CAND op; the relay forwards the list to the peer's counterpart
   and **appends the address it observed the datagram from** — the NAT-translated public one the
   peer itself cannot know — while stripping that flag from anything a peer claimed. Counted as
   `cands_forwarded` / `cands_refused`.
2. **Simultaneous probes, peer to peer.** Both ends probe every candidate at once from the same
   socket the relay leg uses (a NAT mapping is keyed on the source port, so probing from another
   socket only works on the NATs that did not need punching). PROBE / PROBE_ACK are **never
   forwarded by the relay** — one that arrives here counts as `probes_misdirected`, because
   forwarding it would validate the relayed path as if it were direct.
3. **Promote to DIRECT** only when a probe *and its echo* both land for the same address — both
   directions validated. From then on the identical leg datagram is sent to the peer instead of the
   relay; the relay learns of it only as an absence (its `forwarded` counter goes flat while the
   match goes on, and the `candidates` events stop).
4. **Demote** back to the relay when the direct path stops echoing this peer's own probes for
   ~3 s. Liveness is an echo of *our* nonce, not "any datagram from the peer": a one-way failure
   used to pin the losing side to a dead path while the other end's probes kept its timer fresh
   (`udppunchtest` arm F2 is the regression).

Measured on the rig, a pair promotes ~300 ms into the match; through two real home NATs both peers
went direct with translated addresses (`mp:R3b`). A pair that never validates stays relayed for the
whole match, with no stall and no visible difference (`mp:R3`'s pacing comparison).

**What the peers log** (`mh_net.log`), exactly one of the first two at start:

```
net: udp punch ARMED -- …                                      this match may go direct
net: udp path RELAY (forced) -- …                              [net] force_relay=1: never direct
net: udp punch -- peer <n> offered <k> candidate(s) …          the counterpart's list arrived
net: udp path DIRECT -- peer <n> via <addr> after <ms> ms of punching     the promotion
net: udp path RELAY -- peer <n> demoted after <ms> ms direct   the demotion
```

**`[net] force_relay=1`** (in the game's `mh_net.ini`) pins the relayed path: no candidates
published, no probes sent, an inbound probe answered but never promoted. On **one** peer it keeps
the *pair* relayed — a peer that publishes no addresses gives the other nothing to probe. Two uses:
proving a relay deployment carries a whole match (a test that silently went direct proves nothing
about the relay — the `relay_match` UI scenario is pinned this way), and a player whose direct path
is genuinely worse than two good paths into a datacentre.

## The deployment key

`--key` / `--key-file` take the deployment's pre-shared key — the same 64 hex digits (or the word
`open`) the players' `mh_key.txt` carries. The peer↔relay leg is authenticated with a key derived
from it (`HMAC-SHA256(psk, "mh-udp-relay-leg")`), so a peer holding a different key is refused by
the relay before any host sees it, and a scanner's datagram is `leg_bad_mac`, answered with nothing.
With no key at all the relay runs on the all-zero key and says so loudly; `open` is for a LAN or a
test, never for a published port.

**How players get it.** Nobody types it. The release workflow puts the relay's address and its key
into the launcher's *signed* update manifest (`relay: {addr, key}` — `dist:LA6`,
[`docs/release.md`](../../docs/release.md) "The relay"), and the launcher writes
`[net] transport=udp` + `[net] relay=<relay-host>:7100` into the game's `mh_net.ini` and the key
into `mh_key.txt` on every install, update and launch. The key is public once shipped, and that is
accepted: it keeps port scanners off the relay, not players; the manifest's *signature* is what
stops anyone re-pointing every launcher at a relay of their own. A self-hosted relay's players put
the value in `mh_key.txt` by hand (first line), or ship their own manifest.

## Keep it as new as the players' build

The leg's op table has grown over the project's life and an older relay refuses an op it lacks as a
counted `bad_op` — counted here, answered with nothing. Since `mp:R4a` that is **visible at the
player**: each peer's HELLO carries the protocol level its build was made against, the WELCOME
carries this relay's (`PROTOCOL_LEVEL` in `leg.rs`), and a peer that finds the relay behind writes
`net: udp relay -- relay protocol <theirs> < <ours>` to its `mh_net.log` and shows *Relay outdated
(protocol <theirs> < <ours>)* on its first list. A relay older than R4a sends no level at all, which a
current peer reads as level 0 — the same notice. The match still runs on whatever both sides do
speak; what the notice means is that something newer (a re-key, a punch, a room re-mint, the
restart recovery) may silently not happen until the relay is rebuilt from the same tree as the
players' build. On this side: the `listening` line names the level this relay claims, every
`peer_registered` event carries the peer's, and the counters line's `hello_level_mismatch` counts
HELLOs from a build on a different level (a relay newer than its players counts them too; nothing
is refused either way). The published relay is redeployed from the tree every release comes from
(`dist:RP5`, [`docs/deploy.md`](../../docs/deploy.md) 4b), so on that relay the notice is a
redeploy in progress; on a self-hosted one it is your rebuild.

## Self-hosting

One container, one UDP port, host networking. The mechanics — the image
(`src/relay/Dockerfile`, distroless, built by `.github/workflows/images.yml`), the compose stack
(`deploy/docker-compose.yml`: `--network host` because Docker's userland proxy rewrites source
addresses and would break the demux; the healthcheck above; the key as a compose `secrets:` file),
minting the key, the redeploy rule and the restart cost — are [`docs/deploy.md`](../../docs/deploy.md);
this page does not repeat them. The short form, on a box with Docker, in a copy of `deploy/`
whose `.env` names `GHCR_OWNER` and `IMAGE_TAG` (docs/deploy.md section 4):

```
mkdir -p secrets && python3 -c "import secrets; print(secrets.token_hex(32))" > secrets/relay_key.txt
docker compose up -d relay
docker logs mh-relay          # a `listening` line naming the address and protocol_level, then counters
```

Open **UDP** 7100 (or whatever `--bind` names) on the box's firewall; nothing else. Give the
players the box's address and the key file's line. A relay restart ends nothing since `mp:R4b`
(the peers keep their handles and re-HELLO; `docker compose restart relay` is under a second), but
redeploy when the counters line says `peers=0` if you can. Without Docker, `cargo run -p mh_relay
-- --bind 0.0.0.0:7100 --key-file <file>` is the same binary; its log is one JSON object per line
(`--local` prints human-readable lines and binds loopback).

## The counters line

Every `--stats-secs` (and at exit) the relay logs one `counters` event; the deploy verdict and the
rig checks read it. The fields, grouped:

| group | fields |
| --- | --- |
| load | `peers` `rooms` `leg_rx` `leg_tx` `bytes_rx` `bytes_tx` `max_seen` `forwarded` |
| refused at the leg | `unregistered` `replay` `amplification_capped` `leg_bad_mac` `leg_bad_magic` `leg_bad_version` `leg_bad_op` `leg_too_short` `leg_too_long` |
| refused inside a session | `inner_bad_mac` `inner_unverified` `inner_malformed` `not_in_peer_set` `boot_refused` |
| rooms and sessions | `peers_registered` `peers_evicted` `rooms_opened` `tokens_learned` `sessions_registered` `sessions_unregistered` `sessions_expired` `register_refused` `list_requests` `list_entries_sent` |
| punching (`mp:R3`) | `cands_forwarded` `cands_refused` `probes_misdirected` |
| keys and restart (`mp:R1c`, `mp:R4b`) | `peers_rekeyed` `stale_handle_replies` `handles_restored` |
| health and level (`dist:RP5`, `mp:R4a`) | `health_pings` `hello_level_mismatch` |

Three readings worth knowing. A pair that claims to be direct while `forwarded` keeps climbing is
not direct. A deployed relay whose `health_pings` stays 0 has no healthcheck wired. And
`leg_bad_mac` climbing with `peers=0` after a restart is the pre-`R4b` black hole — a current relay
answers those with NOT_REGISTERED (`stale_handle_replies`) and the peers come back
(`handles_restored`).

## Tests

```
cargo test -p mh_relay
```

Covers the shared T0 fixtures, the leg codec's refusals, the room and session state machine, the
amplification cap, the session directory (publish/refresh/withdraw, the TTL, who may publish, the
directory room, and paging a directory larger than one answer), the candidate exchange, the per-peer
leg key and its ratchet, two same-port hosts under distinct rooms, a host relaunched inside the idle
window, the restart recovery, the protocol level in both directions (a HELLO with and without one,
a WELCOME with and without one), and one end-to-end run of a relay plus two synthetic peers over real
loopback sockets. The end-to-end run with the real game is the determinism gate
(`tools/test_ui.py --determinism --transport udp --relay`) and the `relay_match` / `relay_browse` /
`relay_browse_local` / `relay_punch` / `relay_restart` / `relay_stale_notice` UI scenarios, each of
which starts a real relay process beside the rig; `net_selftest.exe udppunchtest` is the promotion
state machine with no sockets, and `udprelaytest` the client tunnel against a stand-in relay.
