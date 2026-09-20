# Playing over the internet (the launcher + the relay; direct dial; the legacy SSH tunnel)

How two people on two home connections get into one match. Rewritten 2026-09-20 (tracker `mp:R5`)
around the relay: the first section is the whole answer for a player, and it contains **no port
forwarding, no ini edit and no key to send**. The rest of the page is the direct dial (the one path
that does need a reachable port), the SSH tunnel that used to be the answer (kept as the legacy
fallback for the TCP transport), and how to read the logs when something fails.

Related: [INSTALL.md](../INSTALL.md) "Using the launcher" (getting the launcher and a build),
[src/launcher/README.md](../src/launcher/README.md) (what the launcher writes and why),
[src/relay/README.md](../src/relay/README.md) (the relay: punch-then-relay, the key, self-hosting),
[mp-relay.md](mp-relay.md) (the relay's settled design), [mp-wire-udp.md](mp-wire-udp.md) (the UDP
packet format), [src/mh_dll/README.md](../src/mh_dll/README.md) (build + gate).

- [1. The way to play: the launcher and the relay](#1-the-way-to-play-the-launcher-and-the-relay)
- [2. Direct dial: *Internet server* + a typed address](#2-direct-dial-internet-server--a-typed-address)
- [3. The SSH reverse tunnel (legacy fallback, TCP only)](#3-the-ssh-reverse-tunnel-legacy-fallback-tcp-only)
- [4. The key (`mh_key.txt`)](#4-the-key-mh_keytxt)
- [5. What an exposed port faces](#5-what-an-exposed-port-faces)
- [6. Latency](#6-latency)
- [7. Testing the path without the game](#7-testing-the-path-without-the-game)
- [8. Reproducing internet conditions on the LAN rig](#8-reproducing-internet-conditions-on-the-lan-rig)
- [9. When a join fails](#9-when-a-join-fails)
- [10. When a session dies mid-game](#10-when-a-session-dies-mid-game)

## 1. The way to play: the launcher and the relay

**Run the launcher, press Host or Join, and use the game's own first list.** That is the whole
procedure; the rest of this section says what happened underneath so a log makes sense.

1. **Get the launcher** — `mh_launcher-<version>.exe` from the public repository's Releases page,
   dropped next to `mh.exe` ([INSTALL.md](../INSTALL.md) "Using the launcher"). It finds the game
   beside itself, and the first *Play* installs the build for the configuration you pick. Under the
   two buttons it says which relay it will use: `Relay: <relay-host>:7100 (from the signed
   manifest, …)`.
2. **The host** presses **Host**. In the game: NETWORK GAME → type your name → the first list screen
   (*Refresh list* / *Create game* / *Internet server*) → **Create game** → the lobby. The lobby is
   now registered on the relay's directory; `mh_net.log` says
   `net: udp relay -- lobby published to the relay's directory`. Tell your friends the name you are
   hosting under — there is no address and no code to send.
3. **A joiner** presses **Join**. In the game: NETWORK GAME → type your name → the same first list
   screen. **Refresh list** shows the games on the relay (name, map, players); pick the host's and
   press **Join** → you are in the lobby. The *Internet server* button and its IP window are never
   visited (`mp:R7`).
4. The host starts the match when the lobby is full.

**What the launcher did for you.** The relay's address and its key travel inside the launcher's
**signed update manifest** (`dist:LA6`), so nobody types them: on every install, update and launch
the launcher writes `[net] transport=udp` and `[net] relay=<relay-host>:7100` into the game's
`mh_net.ini` (editing only those two lines, in place) and the relay's deployment key into
`mh_key.txt`. Every player who came through the launcher therefore holds the same key, which is
also why two launcher installs can dial each other directly (section 2) with no key exchange.

**Neither side forwards a port.** Both peers dial *out* to the relay; the relay pairs them and
carries the match. That is what a relay is for, and it is the difference from every other section
on this page.

**Punch-then-relay: most pairs go DIRECT, the rest stay relayed, and you never wait for it.** The
match starts relayed. In the background (`mp:R3`) the two peers swap the addresses they might be
reachable at *through* the relay, probe them all at once, and switch to a direct peer-to-peer path
the moment a probe *and its echo* both land — the promotion rule needs both directions, so a
one-way hole never gets promoted. Measured on the rig the switch happens ~300 ms into the match,
before the handshake has finished; through two real home NATs both peers went direct with translated
addresses (`mp:R3b`, 2026-09-19). A pair whose probes never succeed (a network that drops all UDP
between them, a symmetric NAT on both ends) simply stays on the relay for the whole match, and a
direct path that stops answering is demoted back to the relay within ~3 s. Nothing about the
match changes at a promotion or a demotion: the same authenticated datagram is sent to a different
address, and the transport above it is never told.

`mh_net.log`, per peer, names the path and every transition — these are the lines to look for:

| Line | Meaning |
| --- | --- |
| `net: udp RELAY mode -- …` | this run dials a relay (both roles) |
| `net: udp relay leg UP -- <relay-host>:7100 room=<n> …` | the relay accepted this peer |
| `net: udp punch ARMED -- …` | written once at start: this match may go direct |
| `net: udp path RELAY (forced) -- …` | written once at start *instead* of the line above: `force_relay=1`, never direct |
| `net: udp punch -- peer <n> offered <k> candidate(s) …` | the other peer's address list arrived through the relay |
| `net: udp path DIRECT -- peer <n> via <addr> after <ms> ms of punching` | **the promotion** |
| `net: udp path RELAY -- peer <n> demoted after <ms> ms direct` | **the demotion** |

Exactly one of `udp punch ARMED` / `udp path RELAY (forced)` appears at start, so "which mode was
this run in" is never an inference from silence.

**`[net] force_relay=1`** pins the relayed path: no candidates published, no probes sent, never a
promotion. Set it on one peer and the *pair* stays relayed (a peer that publishes no addresses gives
the other nothing to probe). Two reasons to want it: proving a relay carries a whole match (a run
that silently went direct proves nothing about the relay), and a direct path that is genuinely
*worse* than two good paths into a datacentre — if the game got choppy a few seconds in, this is the
knob, and the log lines above tell you which case you are in first. It is documented in
[src/mh_dll/mh_net.example.ini](../src/mh_dll/mh_net.example.ini) with the other `[net]` keys.

**`Relay outdated (protocol <theirs> < <ours>)`** on the first list's status line (`mp:R4a`) means
the relay you dialled was built before your game build — its op table is one or more steps behind.
The match still runs on what both sides speak; what may *silently not happen* is the newer part: the
per-session re-key, going direct, a host room re-mint, surviving a relay restart. `mh_net.log`
carries the same fact as `net: udp relay -- relay protocol <theirs> < <ours> …`. The published relay
is redeployed from the same tree as every release (`dist:RP5`), so on the launcher's relay the notice
means "redeploy in progress — retry later, or check the launcher for an update"; on a self-hosted
relay it means rebuild it from the tree the players' build came from
([src/relay/README.md](../src/relay/README.md)). `Incompatible relay (leg v<x>/v<y>)` is the harder
case — a wire version this build does not speak — and needs the relay or the game updated before a
match can run at all.

**Two more lines that are not failures.** A relayed joiner's first dial always logs
`net: udp relay refused us -- no host has claimed this room -- the host must be on the relay
first` followed by the directory fallback and a re-dial of the room it picked (`; R2: relayed connect to room …`) — the ordinary path since a host's
room is minted per lobby (`mp:R6`). And after the relay itself restarts mid-match both peers log
`net: udp relay leg LOST -- the relay does not know handle …` then
`net: udp relay leg RESTORED after <ms> ms` (`mp:R4b`); the match continues.

## 2. Direct dial: *Internet server* + a typed address

With a relay configured you still have a direct path, chosen **per connection** (`mp:R7a`): on the
first list screen, **Internet server** → type the host's address → **Connect** dials *that address*
and the relay is not contacted for it (`mh_net.log`: `; R7a: client dial is DIRECT`). This is the
right path for a friend whose address you know, and for a LAN — and it is **the one path on this
page that needs the host's port reachable**:

| Situation | What the host does |
| --- | --- |
| All peers on one LAN | Nothing. Joiners type the host's LAN address. |
| Host can port-forward | Forward **`[net] port` (6501)** to the host machine — **UDP** 6501 for `transport=udp` (the default, and what the launcher writes), **TCP** 6501 for `transport=tcp`. They are different ports to a router; a rule for one does not cover the other. Joiners type the public address. |
| Host cannot forward (CGNAT, no router access) | Do not dial direct — host through the relay (section 1). For the TCP transport only, the SSH tunnel of section 3 is the legacy way to publish the port. |

A direct dial that fails with a relay configured says so in the joiner's `mh_net.log`:
`; S8: (relay configured) that was a DIRECT dial to a typed address -- the host's port must be
reachable (forwarded/open); to reach it through the relay instead, use the first browser (Refresh
list), whose dials are relayed (mp:R7a)`. The relay does not rescue a typed dial; the first list does.

Both peers must hold the same `mh_key.txt` (section 4) — automatic when both came through the
launcher, otherwise the host sends its file.

**The TCP transport (`transport=tcp`) has no relay**: it is direct dial only, over a forwarded TCP
port or the tunnel below. UDP is the default since 2026-09-20 (user ruling) — an untouched
`mh_net.ini` and no ini at all both mean `transport=udp`, and the launcher additionally writes the
relay address — so TCP is only ever an explicit `transport=tcp` in the ini, chosen for a path where
UDP is blocked outright.

## 3. The SSH reverse tunnel (legacy fallback, TCP only)

> **Legacy.** This was the internet answer from the 2026-07-25 ship build until the relay
> (`mp:R1`–`R7`, 2026-09). It only makes sense for the **TCP** transport with a host that cannot
> forward a port; a UDP install should host through the relay instead. Kept because it works and
> because its two non-properties (below) are the reason the transport authenticates on the wire.

The host opens an outbound SSH connection to a VPS and asks it to publish a port on the host's
behalf. Players then connect to *the VPS*, and their traffic rides the tunnel down to the host.

On the **VPS**, once, in `/etc/ssh/sshd_config`:

```
GatewayPorts clientspecified      # let a -R binding ask for a public interface
```

(`sudo systemctl reload sshd`. Without this, `ssh -R` can only bind the VPS's loopback, which no
player can reach — that is the usual "the tunnel is up but nobody can connect".)

On the **host**, for the duration of the session:

```bash
ssh -N -R 0.0.0.0:6501:127.0.0.1:6501 user@vps.example.com
#      │  │                └── the host's own game port
#      │  └── publish on every VPS interface, port 6501
#      └── no shell, just the tunnel
```

Players type the **VPS address**. Open TCP 6501 in the VPS firewall (`ufw allow 6501/tcp`).

**`-o ExitOnForwardFailure=yes` is not optional** (learned the hard way, 2026-07-26). Without it, if
the VPS port is still held by an earlier session, `ssh` prints a warning and **keeps running with no
forward at all**. Everything then looks healthy — the tunnel process is alive, and the VPS port still
*accepts* connections, because the stale session owns the listener — but nothing reaches the host, so
every joiner fails at the handshake. A `:retry` loop around `ssh` does not help either, because ssh
never exits. Add `ServerAliveInterval=30 ServerAliveCountMax=3` too, so a dead link is noticed rather
than left half-open (which is what creates the stale listener in the first place).

Name the forward target `127.0.0.1`, not `localhost`: on Windows `localhost` resolves to `::1` first,
and the game listens on IPv4.

If a join fails and you suspect the tunnel, check the VPS for who owns the port —
`ss -tlnp | grep 6501` — and compare that process's start time against your current tunnel. An older
one means a zombie; `kill <pid>` frees it.

**Use `tools/mh_tunnel.bat`** rather than typing the `ssh` line: it carries the options above, retries,
and writes a **timestamped** log (`tunnel/logs/tunnel_<stamp>.log`). The stamps are not cosmetic: a
2026-08-02 post-mortem had four tunnel logs with no times in them, so no `forwarded-tcpip` channel
could be lined up against a `conn 0 dropped`, and `mh_net.log` gained matching `[HH:MM:SS.mmm]`
stamps in the same change so the two read as one timeline. The loop runs in PowerShell specifically
so `$LASTEXITCODE` is **ssh's** and not the log-stamper's.

### The port is not yours alone

The tunnel forwards to `127.0.0.1:6501` — **the same port `mp_run` and the determinism rig use**, and
every install ships its **own** `mh_key.txt`. A rig instance left running on the host machine will
answer your joiners with the wrong key, and the joiner reports `handshake REJECTED`. Two things make
that impossible to misread:

- the host's listen banner names the key it is serving with —
  `net: HOST listening on :6501 as player 0 (expect 1 peers) [key 5e69e5db]`;
- a second instance can no longer take the port: the listener uses `SO_EXCLUSIVEADDRUSE` and refuses
  with a line that names the real cause instead of a bare winsock number.

### What the tunnel does NOT do

Two things are easy to assume and both are false:

- **It is not an access control.** `ssh -R` terminates on the host box, so a remote player's
  connection arrives at the game as `127.0.0.1`. The game cannot tell a tunnelled stranger from
  someone sitting at the machine, and no address-based rule can help. This is precisely why the
  authentication in section 4 lives on the wire.
- **It does not encrypt the players.** SSH protects the *host↔VPS* leg only. Each player's
  connection to the VPS is a plain TCP connection across the internet. The DLL's own encryption is
  what covers that leg.

### Telling the game apart from the tunnel

`mh_tunnel.bat` writes one log per run to `tunnel\logs\tunnel_<stamp>.log` (`ssh -v -E`), which records
every forwarded connection:

```
debug1: client_request_forwarded_tcpip: listen 0.0.0.0 port 6501, originator <player ip> port 51234
debug1: connect_next: host 127.0.0.1 ([127.0.0.1]:6501) in progress, fd=5
debug1: channel 2: free: ...                <- that player's link ended
```

Read it alongside the host's `mh_net.log`:

- a join with **no `forwarded_tcpip` line** never reached the host box — a VPS/tunnel problem, and the
  game's logs will have nothing to say about it;
- a **`channel … free`** while the player was still in the lobby means the *tunnel* dropped them;
- a game-side drop reason with no matching channel close means the connection died above the tunnel.

## 4. The key (`mh_key.txt`)

The transport authenticates every connection against a 32-byte pre-shared key and encrypts the
session with keys derived from it
([net_crypto.h](../src/mh_net_proto/include/mh_net_proto/net_crypto.h),
[mh_net_key.h](../src/mh_dll/mh_common/include/mh_net_key.h)). The file sits next to the game exe:

- **Through the launcher you never touch it.** The relay's deployment key comes from the signed
  manifest and the launcher writes it into `mh_key.txt` (rewriting only when it changed). The
  relay authenticates its own leg with a key derived from the same value, so a player whose file
  differs from the relay's is refused *by the relay*, before any host sees them. The key is public
  once shipped, by decision: it keeps port scanners off the relay, not players — the manifest's
  *signature* is what stops anyone re-pointing launchers at a relay of their own.
- **Without the launcher, first run generates one** and writes it with an explanation. The first
  line is the key in hex. **The host's key is then the session's password**: send that file (or
  just its first line) to the people you are playing with; they replace their own.
- **A wrong key is refused before the connection gets a player slot**, and both sides log a specific
  reason — the joiner sees `handshake REJECTED -- host key mismatch (ask the host for its
  mh_key.txt)`, not a timeout.
- **A corrupt key file refuses to start the transport** rather than quietly falling back to no
  protection. A UTF-8 BOM (Notepad's "Save as UTF-8") and trailing `;` comment lines are fine.
- `MH_KEY_FILE=<path>` overrides the location, e.g. to share one key across several installs.
- Replacing the whole file with the single word `open` turns authentication **and** encryption off.
  That is for a LAN test or talking to a pre-2026-07-25 build; on a published port it means anyone
  who finds it can join and feed data straight into the game's parser, and the published relay
  refuses it. It is logged as a warning on every launch.

Rotating a hand-managed key is "delete the file, relaunch, resend it". Rotating the relay's is the
maintainer's job ([docs/release.md](release.md) "The relay"): every launcher rewrites the file on
its next update.

## 5. What an exposed port faces

This applies to a **directly dialled** host (section 2) and to the tunnel (section 3); a relayed
host exposes no port at all. A public port gets scanned within hours. The relevant protections, and
their limits:

| Against | What stops it |
| --- | --- |
| Scanners, random probes | No valid HELLO → connection closed at the handshake, never reaches game code. |
| Someone guessing the key | 32 random bytes; not guessable. |
| Replaying a captured handshake | Both sides contribute a fresh nonce, so no proof replays. |
| Reading or tampering with traffic | ChaCha20 + a MAC over an implicit per-direction sequence number: forged, reordered, duplicated or truncated records all fail. |
| Slot exhaustion (connect and go silent) | The handshake happens *before* a peer slot is allocated, with a 5 s reap. |
| Connection floods (TCP) | Max 8 handshakes in flight, 20 accepts/second. |
| **A player you invited misbehaving** | **Nothing.** An authenticated peer speaks the retail protocol, and the retail parser is from 2001. The key is the trust boundary — treat it as "people I would hand a controller to". |

The last row is the honest limit of this design, and it holds on the relay path too: the relay
authenticates what it carries and cannot read it, so it cannot filter it either. Hardening the
game's own lobby/lockstep parsing against a hostile *authenticated* peer is a separate, much larger
job.

## 6. Latency

The lookahead is the input-latency budget, and it tunes itself: the DLL starts at 100 ms (chosen
for internet play), measures how often the sim is horizon-starved, and walks the value down toward
the floor the link and sim step allow — `max(lockstep_min_ms, 3 × sim_step_ms)`, i.e. 60 ms at the
shipping 20 ms sim step. It grows quickly again if the link degrades. Each peer tunes its own value
independently and this cannot desync, because the effective horizon is a `min` over peers.

Measured, 3000-step determinism runs, same build both times (the 2026-07 TCP build, through the
SSH tunnel; a relayed UDP match adds one hop to a datacentre until it goes direct):

| Path | RTT | Adaptive settled at | Sim rate | Determinism |
| --- | --- | --- | --- | --- |
| 2-machine LAN | <1 ms | 60 ms (the floor), both peers | 0.987× / 0.999× | IDENTICAL |
| Through a VPS (SSH tunnel) | ~80 ms | host ~130 ms, client 60 ms | 0.957× / 0.968× | IDENTICAL |

So an internet game runs essentially real-time, and the controller does respond to real
latency — on the LAN it walks down to the floor, over the VPS it climbs to ~130 ms and tracks.

Two things that run also exposed, worth knowing before reading a log:

- **The two peers need not agree, and didn't.** `committed` is a `min` over peers, so the peer with
  the *smaller* lookahead binds the pair; the other one can see starvation that growing its own value
  cannot fix. That is why the host oscillated 115–160 ms while the client sat at 60 ms. It is
  determinism-safe and cost ~3 % of rate here, but it means a log showing one peer hunting is not
  necessarily a bug on that peer.
- **The starvation signal counts frames, not time**, so a high-fps peer samples the starved condition
  far more often than a slow one and grows where the slow peer shrinks — on the same link.

`[net] lockstep_step_ms=<n>` pins the lookahead manually and disables the controller.

## 7. Testing the path without the game

`net_selftest` can act as either peer, and its client takes an address, so the whole TCP transport —
connect, PSK handshake, encryption, a round-trip datagram — can be exercised over a direct or
tunnelled path before involving the game's UI:

```
host machine :  net_selftest.exe host   6501
remote peer  :  net_selftest.exe client 6501 <host or vps address>
```

Both need the same `mh_key.txt` beside the exe. A pass looks like this (verified 2026-07-26,
VM → public internet → VPS → tunnel → host):

```
[VM]   [client] connected to host as player 1 ... round-trip OK
[host] net: handshake from 127.0.0.1 OK (authenticated + encrypted)
[host] net: accepted 127.0.0.1 -> conn 0
```

Note the host logs the peer as **127.0.0.1**: the tunnel terminates locally, so a remote joiner is
indistinguishable from a local one at the socket level. That is the concrete reason the
authentication has to be on the wire — see section 3.

For the **relay** path the equivalent probe is the relay's own healthcheck —
`mh_relay --health <relay-host>:7100 --key-file mh_key.txt` exits 0 on a PONG, and answers nothing
under a wrong key ([src/relay/README.md](../src/relay/README.md)).

*(If the remote peer exits with `0xC000007B` it never ran: `net_selftest` links the CRT dynamically,
so it needs `vcruntime140.dll`/`msvcp140.dll` beside it on a machine without the VC redistributable.
`mh.dll` itself is statically linked and has no such dependency.)*

## 8. Reproducing internet conditions on the LAN rig

The rig is sub-millisecond, so none of the above can be exercised on it directly — which is why the
dead link, the icon storm and the adaptive controller's latency response were all found (or left
untested) on the real path, at the cost of needing a second person online.

`tools/net_shim.py` closes that gap: a middlebox the peers connect through (TCP, or `--udp` for the
UDP transport), whose delay, jitter and rate can be changed *while a game is running*, and which can
kill a link in each of the four ways a real one dies. Run it on the dev box with both game peers on
VMs, so the game port stays 6501 everywhere and no peer ini changes:

```
python tools/net_shim.py --listen 0.0.0.0:6501 --target 192.168.0.37:6501 --delay 100
python tools/ui_test.py --host 192.168.0.37:mp_host_start.txt \
                        --client 192.168.0.38:mp_client_start.txt \
                        --connect-ip <dev box LAN ip>
```

`--delay` is **one way**, so `--delay 100` is a ~200 ms round trip. Then either drive it by hand
(`net_shim.py ctl "set delay 250"`, `ctl "blackhole on"`) or replay a timeline from
`tools/uiscripts/shim/` for a repeatable run. A relayed run is exercised the same way with the shim
in front of the relay (`--udp --mtu 1200` also reports oversize datagrams per direction).

The four link deaths map onto the drop reasons in section 10, which makes a single run a complete
check of the detection:

| Shim command | Far peer should log |
| --- | --- |
| `cut` | `peer closed the connection` |
| `cut rst` | `socket error (winsock 10054)` |
| `blackhole on` | `no data from peer within the link timeout` |
| `stall on` (shorter than `rx_timeout_ms`) | **nothing** — a quiet link that is alive must survive |

Two honest limits. In TCP mode it **cannot** model packet loss or reordering — deleting bytes from a
TCP stream is corruption, not loss; real loss reaches the application as retransmit delay, which the
delay/jitter model already covers. And an injected-delay LAN is not the internet: it reproduces RTT,
not a real jitter distribution or a NAT's behaviour. **Confirm any shipped default on the real path
before trusting it.**

Verify the tool before trusting a number it produced: `python tools/net_shim.py selftest`.

## 9. When a join fails

Read the joiner's `logs\<timestamp>_client\mh_net.log` — the failure is named there:

| Log line | Meaning |
| --- | --- |
| `net: udp relay refused us -- no host has claimed this room …` (once, then a re-dial) | Normal for a relayed joiner's first dial (section 1). A problem only if no `udp relay leg UP` for the picked room follows. |
| `net: udp relay refused us -- …` (any other reason) | The relay refused an action and says why: `another host already holds this room code`, `the room is full`, `this peer is not registered (the relay restarted?)`. Logged once per distinct reason. |
| no `udp relay leg UP` at all, transport refused to start | The relay could not be reached: no internet, the address in `mh_net.ini` is wrong, or the relay is down. The launcher's front page names the relay it configured. |
| `net: udp relay -- relay protocol <theirs> < <ours> …` | The relay is older than your build (section 1, `Relay outdated`). The match runs; the newer parts may not. |
| `; S8: (relay configured) that was a DIRECT dial to a typed address …` | You used *Internet server* + an address: that host's port must be forwarded (section 2), or join from *Refresh list* instead. |
| `connect(...) failed` | (TCP) Never reached the host: wrong address, port not forwarded, tunnel down, firewall. |
| `handshake REJECTED -- host key mismatch` | Reached the host; wrong `mh_key.txt`. |
| `handshake failed -- connected, but the peer never answered` | We reached *something* that never spoke the protocol. Most often a tunnel whose far end is down (the host has no game open, or the forward is stale) — **not** usually a key problem. Also an older host build, or our own key being wrong. |
| `bad HELLO (magic/version N, expected M)` | Build skew — the two sides speak different handshake versions. |
| `mh_key.txt is unreadable/corrupt` | Fix or delete the file (delete = a fresh key is minted; a launcher install rewrites it on the next launch). |
| the lobby bounces you back with `Refused: codepage <host>/<yours>` | The host's text codepage is not installed on your machine (`mp:F3c`); the joiner normally adopts the host's automatically. |

On a directly dialled host, `handshake from <ip> -- no HELLO within 5000 ms (scan/probe?)` is
background noise from the internet, not a problem.

## 10. When a session dies mid-game

A join failing is loud. A join succeeding and then quietly dying is not, and until 2026-07-26 it was
close to undiagnosable: the transport dropped a connection with a single `frame error` that covered
four unrelated causes, and — worse — a link that stopped delivering *without* erroring was detected by
nobody at all.

**What that looked like.** A real internet game (~200 ms, via the tunnel): the client's connection died
about 30 s into the lobby, and the host logged **nothing, for the whole 15-minute run**. It went on
advertising `players=2/4 peers=1` every 50 ms into a dead socket, then started the match against a peer
that had been gone for 90 s, and only found out through the in-game presence timeout. To the player
this read as four separate bugs — the client could not change its race, the host's changes did not
propagate, the host started and the client did not, the host then dropped the client — all of which
were the one dead link.

Two things changed.

**Every drop now names its cause** in `mh_net.log`, with the WinSock code where there is one:

| Log line | Meaning |
| --- | --- |
| `conn N dropped -- peer closed the connection` | An orderly FIN. The peer quit, or something in the path (a relay, the tunnel) closed the stream on its behalf. |
| `conn N dropped -- socket error (winsock 10054)` | Reset. 10054 = the peer/relay sent RST; 10060 = a send timed out. |
| `conn N dropped -- no data from peer within the link timeout` | The peer never closed and never errored — it simply stopped delivering. A blackholed path (dead tunnel, NAT eviction) or a frozen peer process. **Read the `conn N silent … (keepalives: sent X, received Y)` line above it**: `sent N, received 0` means we asked and nothing came back (the path); a low `sent` means we barely got to ask at all. |
| `link watchdog was blocked N ms` | The watchdog thread did not run for N ms — a send holding the conn lock, a suspended machine, or a debugger. That interval is credited back to every peer instead of being read as silence. Seeing this repeatedly means the link is stalling long enough to block sends. |
| `conn N dropped -- authentication failed (...)` | A record failed its MAC: a corrupted stream, or two peers whose record sequences diverged. Not a wrong-key symptom (that fails at the handshake, above). |
| `bind(:N) REFUSED -- something else on THIS machine is already listening` | Another `mh.focus.exe` (a rig lane, a determinism run, an older host) owns the port. **Not** a key problem — but if you ignore it and host anyway, joiners reach *that* instance and report `handshake REJECTED`. |
| `conn N dropped -- bad record length` / `bad frame header (magic)` | Something that is not this protocol is on the port, or the stream lost sync. |
| `net: udp path RELAY -- peer N demoted after <ms> ms direct` | Not a drop: the direct path died and the pair is back on the relay (section 1). The match continues. |

**The link is actively probed.** Each connection sends a `FLAG_PING` every second and is dropped after
10 s with nothing inbound (`[net] ping_ms` / `rx_timeout_ms`; a negative value turns either half off).
Sockets also carry `SO_KEEPALIVE` and a bounded send timeout. Because the traffic is ours, silence is a
fact rather than a guess, and it works the same in the lobby, during a map load, and mid-lockstep. The
arm line is `net: link watchdog armed (ping every 1000 ms, drop after 10000 ms of silence)`.

> **The watchdog only counts time it was watching.** It needs the connection lock both to ping and
> to judge silence, and a send holds that lock until it completes (bounded by a 5 s send timeout). A
> stalled link therefore used to starve it into sending no keepalives at all, so each peer read the
> other's *involuntary* silence as death and both dropped — on a link that had been carrying data
> moments earlier, and only over a relay, never on a LAN. A pass that starts a full ping interval
> late now credits that blind interval back to every peer and logs that it did. The decision is
> pure and asserted by `net_selftest.exe watchdogtest`.

> **Debugging caveat.** A peer frozen under a debugger (the TTD/cdb harness) looks dead after 10 s.
> Set `[net] rx_timeout_ms=-1` on both peers for those sessions.
