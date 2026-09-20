//! `mh_relay` -- the single-port UDP relay for Mission Humanity multiplayer (tracker `mp:R1`).
//!
//! One bound socket, many rooms, many sessions. A peer wraps each sealed T0 datagram
//! (`docs/mp-wire-udp.md`) in the small authenticated leg envelope of [`leg`], the relay reads the
//! envelope to know who is talking and the T0 header's `conn_id` to know what it may do with the
//! payload, and forwards it verbatim to the other end. The relay opens each session's CONNECT
//! TOKEN so it can authenticate what it carries, and keeps only the MAC keys -- it can prove a
//! datagram is the registered peer's and cannot read a byte of the match.
//!
//! The decisions live in [`relay::Relay`], which owns no socket and no clock; this file is the
//! socket, the clock, and the logs. `docs/mp-relay.md` is the settled design; the client half is
//! `src/mh_dll/mh_net_udp/udp_relay.cpp`.
//!
//! ```text
//! mh_relay --bind 0.0.0.0:7100 --key-file mh_key.txt        # a deployment
//! mh_relay --local                                          # loopback, human logs, open key
//! ```

pub mod leg;
pub mod relay;
pub mod wire;

use std::net::SocketAddr;
use std::time::{Duration, Instant};

use clap::Parser;
use relay::{Event, Limits, Relay};
use tokio::net::UdpSocket;
use tracing::{info, warn};

/// The default port. Nothing in the game hard-codes it -- `[net] relay=<host:port>` carries it --
/// but a default that is written down in exactly one place is one fewer thing to get wrong between
/// a compose file and an ini.
const DEFAULT_PORT: u16 = 7100;

#[derive(Parser, Debug)]
#[command(
    name = "mh_relay",
    about = "UDP relay for Mission Humanity multiplayer (mp:R1)"
)]
struct Args {
    /// Address to bind. Defaults to 0.0.0.0:7100, or 127.0.0.1:7100 under --local.
    #[arg(long)]
    bind: Option<SocketAddr>,

    /// The 32-byte pre-shared key as 64 hex digits -- the same value the players' mh_key.txt holds.
    #[arg(long, conflicts_with = "key_file")]
    key: Option<String>,

    /// Read the key from an mh_key.txt (the first meaningful line: 64 hex digits, or `open`).
    #[arg(long)]
    key_file: Option<String>,

    /// Loopback only, human-readable logs, and the all-zero "open" key unless one is given. This is
    /// the mode the single-machine acceptance run uses: relay and both game peers on one box.
    #[arg(long)]
    local: bool,

    /// Drop a datagram on a connection the relay has not learned, instead of forwarding it. Off by
    /// default so a relay restarted mid-match carries the game rather than black-holing it; on is
    /// what a public deployment wants.
    #[arg(long)]
    strict: bool,

    /// Evict a peer after this many seconds of silence.
    #[arg(long, default_value_t = 60)]
    idle_secs: u64,

    /// Ping a peer that has been quiet this long (plan D4's 20-25 s keepalive band; the peers ping
    /// at 20, so this is the other half of the same floor).
    #[arg(long, default_value_t = 25)]
    ping_secs: u64,

    /// mp:R2 -- forget a host's lobby descriptor after this many seconds without a refresh. The
    /// host refreshes at ~1 Hz while it sits in its lobby, and leaves with an explicit withdrawal,
    /// so this only covers a host that vanished.
    #[arg(long, default_value_t = 20)]
    session_ttl_secs: u64,

    /// Emit a counters line this often. 0 turns the periodic line off.
    #[arg(long, default_value_t = 60)]
    stats_secs: u64,

    /// `json` (the default; one object per line, for a collector) or `human`.
    #[arg(long, value_parser = ["json", "human"])]
    log: Option<String>,

    /// dist:RP5 -- PROBE MODE, for the container healthcheck: instead of serving, send one PING
    /// from no handle to this relay address under the same key (--key/--key-file) and exit 0 on a
    /// PONG within --health-timeout-ms, 1 otherwise. Proves the socket is served AND the key
    /// matches; registers nothing. Compose runs `relay --health 127.0.0.1:7100 --key-file ...`.
    #[arg(long, value_name = "ADDR")]
    health: Option<SocketAddr>,

    #[arg(long, default_value_t = 2000)]
    health_timeout_ms: u64,

    /// mp:R4a -- TEST KNOB: the protocol level this relay CLAIMS in its WELCOME instead of the one
    /// it was built with (leg::PROTOCOL_LEVEL). `0` claims none, which is the 4-byte WELCOME a
    /// pre-R4a build sends. It changes nothing the relay does with a datagram -- only what a peer
    /// is told -- and exists so a scenario can stage "an older relay" against a current peer and
    /// prove the peer's notice. Hidden from --help: a deployment has no reason to set it.
    #[arg(long, hide = true)]
    advertise_level: Option<u8>,
}

/// One PING from `HANDLE_NONE`, one PONG back with the same seq, or an error naming which step
/// failed -- the exit status is the healthcheck's whole verdict, the message is for `docker
/// inspect`'s health log.
async fn health_probe(target: SocketAddr, psk: [u8; 32], timeout: Duration) -> Result<(), String> {
    let s = leg::Secrets::derive(&psk);
    let bind = if target.is_ipv4() {
        "0.0.0.0:0"
    } else {
        "[::]:0"
    };
    let sock = UdpSocket::bind(bind)
        .await
        .map_err(|e| format!("bind: {e}"))?;
    let seq = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_millis() as u64)
        .unwrap_or(1);
    let ping =
        leg::encode(leg::OP_PING, leg::HANDLE_NONE, 0, 0, seq, &[], &s.leg).ok_or("encode")?;
    sock.send_to(&ping, target)
        .await
        .map_err(|e| format!("send to {target}: {e}"))?;
    let mut buf = [0u8; leg::LEG_MAX];
    let deadline = Instant::now() + timeout;
    loop {
        let left = deadline.saturating_duration_since(Instant::now());
        if left.is_zero() {
            return Err(format!("no PONG from {target} within {timeout:?}"));
        }
        let (n, from) = tokio::time::timeout(left, sock.recv_from(&mut buf))
            .await
            .map_err(|_| format!("no PONG from {target} within {timeout:?}"))?
            .map_err(|e| format!("recv: {e}"))?;
        if from != target {
            continue;
        }
        match leg::decode(&buf[..n], &s.leg) {
            Ok(l) if l.op == leg::OP_PONG && l.seq == seq => return Ok(()),
            Ok(l) => return Err(format!("unexpected op {} seq {} from {from}", l.op, l.seq)),
            Err(e) => return Err(format!("undecodable reply from {from}: {}", e.name())),
        }
    }
}

/// mh_key.txt's rule, mirrored: the first line that is neither blank nor a `;` comment, which is
/// either 64 hex digits or the single word `open`. Strict about trailing junk for the same reason
/// `key_from_hex` is -- a truncated key must be an error, not a silently different key.
fn parse_key_text(text: &str) -> Result<[u8; 32], String> {
    let line = text
        .lines()
        .map(str::trim)
        .find(|l| !l.is_empty() && !l.starts_with(';'))
        .unwrap_or("");
    if line.eq_ignore_ascii_case("open") {
        return Ok([0u8; 32]);
    }
    parse_key_hex(line)
}

fn parse_key_hex(s: &str) -> Result<[u8; 32], String> {
    let s = s.trim();
    if s.len() != 64 || !s.bytes().all(|b| b.is_ascii_hexdigit()) {
        return Err(format!(
            "expected 64 hex digits (or the word `open`), got {} character(s)",
            s.len()
        ));
    }
    let mut k = [0u8; 32];
    for (i, b) in k.iter_mut().enumerate() {
        *b = u8::from_str_radix(&s[i * 2..i * 2 + 2], 16).map_err(|e| e.to_string())?;
    }
    Ok(k)
}

fn log_event(e: &Event) {
    match e {
        Event::PeerRegistered {
            handle,
            room,
            role,
            addr,
            match_id,
            level,
        } => info!(
            event = "peer_registered",
            handle,
            room,
            role = if *role == leg::ROLE_HOST { "host" } else { "client" },
            addr = %addr,
            match_id = %match_id,
            level,
            "peer registered"
        ),
        Event::PeerRebound {
            handle,
            room,
            from,
            to,
        } => info!(event = "peer_rebound", handle, room, from = %from, to = %to, "peer rebound"),
        Event::Rehomed {
            handle,
            from_room,
            to_room,
        } => info!(
            event = "rehomed",
            handle, from_room, to_room, "peer re-registered into a new room"
        ),
        Event::MatchId {
            handle,
            room,
            match_id,
        } => info!(event = "match_id", handle, room, match_id = %match_id, "peer named its match"),
        Event::SessionLearned {
            room,
            conn,
            slot,
            host,
            client,
            scope,
        } => info!(
            event = "session_learned",
            room, conn = %conn, slot, host, client, scope = %scope,
            "connect token opened -- session authenticated from here on"
        ),
        Event::PeerGone { handle, room, why } => {
            info!(event = "peer_gone", handle, room, why, "peer gone")
        }
        Event::RoomClosed { room } => info!(event = "room_closed", room, "room closed"),
        Event::Refused { addr, why } => {
            warn!(event = "refused", addr = %addr, why, "datagram refused")
        }
        Event::SessionRegistered {
            room,
            handle,
            bytes,
            fresh,
        } => {
            // A refresh arrives at ~1 Hz for as long as a lobby is open, so only the FIRST one is
            // an event worth a line; the rest are visible as `sessions_registered` holding steady
            // while the counters line keeps printing.
            if *fresh {
                info!(
                    event = "session_registered",
                    room, handle, bytes, "host published its lobby descriptor"
                )
            }
        }
        Event::SessionUnregistered { room, why } => info!(
            event = "session_unregistered",
            room, why, "lobby descriptor withdrawn"
        ),
        // mp:R3. Logged EVERY time and not only the first, unlike a session refresh: a peer re-sends
        // its list only while a pair is still trying to go direct, so the cadence of these lines is
        // itself the answer to "how long did punching go on" -- and once a pair is promoted they
        // stop, which is the only trace of the promotion the relay can have (it is not told).
        Event::Candidates {
            from,
            to,
            room,
            n,
            observed,
        } => info!(
            event = "candidates",
            from, to, room, n, observed = %observed,
            "candidate list carried to the counterpart"
        ),
        // mp:R1c. Once per handle, at its FIRST key: the later ones are a host adding another
        // pair, which is routine registration rather than a change of trust boundary.
        Event::LegRekeyed { handle, room } => info!(
            event = "leg_rekeyed",
            handle, room, "leg key derived from this peer's connect token"
        ),
    }
}

fn log_counters(r: &Relay) {
    let c = r.counters;
    info!(
        event = "counters",
        peers = r.peer_count(),
        rooms = r.room_count(),
        leg_rx = c.leg_rx,
        leg_tx = c.leg_tx,
        bytes_rx = c.bytes_rx,
        bytes_tx = c.bytes_tx,
        max_seen = c.max_seen,
        forwarded = c.forwarded,
        unregistered = c.unregistered,
        replay = c.replay,
        amplification_capped = c.amplification_capped,
        inner_bad_mac = c.inner_bad_mac,
        inner_unverified = c.inner_unverified,
        inner_malformed = c.inner_malformed,
        not_in_peer_set = c.not_in_peer_set,
        boot_refused = c.boot_refused,
        tokens_learned = c.tokens_learned,
        leg_bad_mac = c.leg_bad_mac,
        leg_bad_magic = c.leg_bad_magic,
        leg_bad_version = c.leg_bad_version,
        leg_bad_op = c.leg_bad_op,
        leg_too_short = c.leg_too_short,
        leg_too_long = c.leg_too_long,
        peers_registered = c.peers_registered,
        peers_evicted = c.peers_evicted,
        rooms_opened = c.rooms_opened,
        sessions_registered = c.sessions_registered,
        sessions_unregistered = c.sessions_unregistered,
        sessions_expired = c.sessions_expired,
        register_refused = c.register_refused,
        list_requests = c.list_requests,
        list_entries_sent = c.list_entries_sent,
        cands_forwarded = c.cands_forwarded,
        cands_refused = c.cands_refused,
        probes_misdirected = c.probes_misdirected,
        peers_rekeyed = c.peers_rekeyed,
        stale_handle_replies = c.stale_handle_replies,
        handles_restored = c.handles_restored,
        health_pings = c.health_pings,
        hello_level_mismatch = c.hello_level_mismatch,
        "counters"
    );
}

#[tokio::main(flavor = "current_thread")]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args = Args::parse();

    // The log SHAPE is a deployment choice and the default is machine-readable: a relay's log is
    // read by a collector far more often than by a person, and plan D8 asks for correlation by
    // match_id, which is a field rather than a phrase. --local flips it, because the one reader of
    // a loopback run is the person who started it.
    let json = args
        .log
        .as_deref()
        .unwrap_or(if args.local { "human" } else { "json" })
        == "json";
    if json {
        tracing_subscriber::fmt().json().flatten_event(true).init();
    } else {
        tracing_subscriber::fmt().with_target(false).init();
    }

    let psk = match (&args.key, &args.key_file) {
        (Some(h), _) => parse_key_hex(h).map_err(|e| format!("--key: {e}"))?,
        (_, Some(p)) => {
            let text = std::fs::read_to_string(p).map_err(|e| format!("--key-file {p}: {e}"))?;
            parse_key_text(&text).map_err(|e| format!("--key-file {p}: {e}"))?
        }
        // No key at all is the "open" link `mh_key.txt` spells with one word: the handshake still
        // runs (a UDP host needs SOMETHING to bind a source address to a slot) but authenticates
        // nobody. Refusing to start instead would make the single-machine gate need a key file for
        // a run that is not defending anything, so it is allowed and it is SHOUTED about.
        _ => {
            warn!(
                event = "open_key",
                "no --key/--key-file: running with the all-zero OPEN key -- the leg is not \
                 authenticated and any reachable peer can register. LAN/testing only."
            );
            [0u8; 32]
        }
    };

    if let Some(target) = args.health {
        return match health_probe(target, psk, Duration::from_millis(args.health_timeout_ms)).await
        {
            Ok(()) => {
                println!("ok: PONG from {target}");
                Ok(())
            }
            Err(e) => {
                eprintln!("unhealthy: {e}");
                std::process::exit(1)
            }
        };
    }

    let bind = args.bind.unwrap_or_else(|| {
        let ip = if args.local { "127.0.0.1" } else { "0.0.0.0" };
        format!("{ip}:{DEFAULT_PORT}")
            .parse()
            .expect("literal address")
    });
    let sock = UdpSocket::bind(bind).await?;
    let local = sock.local_addr()?;

    let mut r = Relay::new(
        psk,
        Limits {
            idle: Duration::from_secs(args.idle_secs),
            ping_after: Duration::from_secs(args.ping_secs),
            strict: args.strict,
            session_ttl: Duration::from_secs(args.session_ttl_secs),
            level: args.advertise_level.unwrap_or(leg::PROTOCOL_LEVEL),
        },
    );
    if let Some(claimed) = args.advertise_level {
        if claimed != leg::PROTOCOL_LEVEL {
            warn!(
                event = "advertise_level",
                claimed,
                built = leg::PROTOCOL_LEVEL,
                "--advertise-level: claiming a protocol level this relay was not built with (a test                  knob -- peers will report the mismatch)"
            );
        }
    }

    info!(
        event = "listening",
        addr = %local,
        strict = args.strict,
        idle_secs = args.idle_secs,
        ping_secs = args.ping_secs,
        session_ttl_secs = args.session_ttl_secs,
        version = env!("CARGO_PKG_VERSION"),
        protocol_level = r.level(),
        "mh_relay listening"
    );

    tokio::select! {
        _ = serve(&sock, &mut r, args.stats_secs) => {}
        _ = tokio::signal::ctrl_c() => {}
        _ = sigterm() => {}
    }
    log_counters(&r);
    info!(event = "shutdown", "mh_relay stopping");
    Ok(())
}

/// mp:R4b -- `docker stop` (and therefore `docker compose restart` and every redeploy) sends
/// SIGTERM, waits 10 s, then SIGKILLs. With only ctrl_c (SIGINT) handled, the relay sat through
/// the whole grace period on every restart: measured 15 s from `restart` to the new `listening`
/// line on the VPS, which is longer than the peers' 10 s link timeout, so a redeploy ended every
/// match it was carrying before the peers could even try to re-HELLO. Exiting on SIGTERM makes the
/// gap the process start time. Windows has no SIGTERM; there the arm never resolves.
async fn sigterm() {
    #[cfg(unix)]
    {
        match tokio::signal::unix::signal(tokio::signal::unix::SignalKind::terminate()) {
            Ok(mut s) => {
                s.recv().await;
            }
            Err(_) => std::future::pending::<()>().await,
        }
    }
    #[cfg(not(unix))]
    {
        std::future::pending::<()>().await
    }
}

/// The socket loop. Borrows rather than owns so the caller still has the relay afterwards (to
/// print the final counters) and so the loopback test can cancel it by simply finishing -- which
/// is the only shutdown a select! arm needs, and cheaper than a channel nothing else would use.
async fn serve(sock: &UdpSocket, r: &mut Relay, stats_secs: u64) {
    let mut buf = vec![0u8; leg::LEG_MAX + 64];
    let mut housekeeping = tokio::time::interval(Duration::from_secs(1));
    let mut stats = tokio::time::interval(Duration::from_secs(if stats_secs == 0 {
        3600
    } else {
        stats_secs
    }));
    stats.tick().await; // the first tick of an interval is immediate; skip the startup line

    let mut events: Vec<Event> = Vec::new();
    loop {
        tokio::select! {
            got = sock.recv_from(&mut buf) => {
                let (n, from) = match got {
                    Ok(v) => v,
                    // On Windows a UDP socket that gets an ICMP port-unreachable back fails the
                    // NEXT recv with ConnectionReset. That is a fault of a datagram we SENT, not of
                    // this receive, and treating it as fatal would mean one departed peer ends the
                    // relay -- the same trap udp_endpoint.cpp handles with SIO_UDP_CONNRESET.
                    Err(e) if e.kind() == std::io::ErrorKind::ConnectionReset => continue,
                    Err(e) => { warn!(event = "recv_error", error = %e, "recv failed"); continue; }
                };
                events.clear();
                let outs = r.on_datagram(from, &buf[..n], Instant::now(), &mut events);
                for e in &events { log_event(e); }
                for o in outs {
                    if let Err(e) = sock.send_to(&o.bytes, o.to).await {
                        warn!(event = "send_error", to = %o.to, error = %e, "send failed");
                    }
                }
            }
            _ = housekeeping.tick() => {
                events.clear();
                let outs = r.tick(Instant::now(), &mut events);
                for e in &events { log_event(e); }
                for o in outs {
                    let _ = sock.send_to(&o.bytes, o.to).await;
                }
            }
            _ = stats.tick(), if stats_secs != 0 => log_counters(r),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_key_file_grammar_is_mh_key_txts() {
        assert_eq!(parse_key_text("open\n").unwrap(), [0u8; 32]);
        assert_eq!(parse_key_text("; a comment\nOPEN\n").unwrap(), [0u8; 32]);
        let hex = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
        let k = parse_key_text(&format!("; header\n{hex}\nexplanation\n")).unwrap();
        assert_eq!(k[0], 0x00);
        assert_eq!(k[31], 0xff);
        // A truncated key is an error, never a different key.
        assert!(parse_key_text(&hex[..62]).is_err());
        assert!(parse_key_text("not hex at all").is_err());
    }

    // ---- the loopback stand-in ------------------------------------------------------------------
    //
    // R-transport's proof idea, kept as R1's acceptance test (plan D5): a relay and two peers on
    // one machine, over REAL sockets. The unit tests in `relay.rs` prove the decisions; this proves
    // the wiring around them -- the bind, the select loop, the send path -- which is the half a
    // pure core cannot check and the half that is wrong when a relay "runs" and forwards nothing.
    //
    // The two peers here are synthetic: they speak the leg and T0 by hand rather than being
    // `mh_net_udp.dll`. The end-to-end run with the real DLL is the determinism gate, recorded in
    // docs/mp-relay.md; this is what keeps `cargo test` able to catch a regression without a rig.

    use std::time::Duration as Dur;
    use tokio::time::timeout;

    const T: Dur = Dur::from_secs(5);

    async fn recv_leg(s: &UdpSocket, key: &[u8; 32]) -> leg::Leg {
        let mut buf = [0u8; leg::LEG_MAX + 64];
        let (n, _) = timeout(T, s.recv_from(&mut buf))
            .await
            .expect("the relay answered within 5 s")
            .expect("recv");
        leg::decode(&buf[..n], key).expect("a leg datagram")
    }

    #[tokio::test(flavor = "current_thread")]
    async fn two_peers_and_a_relay_on_loopback() {
        let psk = [0x5au8; 32];
        let s = leg::Secrets::derive(&psk);
        let sock = UdpSocket::bind("127.0.0.1:0").await.unwrap();
        let relay_addr = sock.local_addr().unwrap();
        let mut r = Relay::new(psk, Limits::default());

        let drive = async {
            let host = UdpSocket::bind("127.0.0.1:0").await.unwrap();
            let client = UdpSocket::bind("127.0.0.1:0").await.unwrap();
            let room = 6501u32;

            // --- registration -----------------------------------------------------------------
            let p = leg::hello_payload(leg::ROLE_HOST, &[0u8; 16]);
            let pkt = leg::encode(leg::OP_HELLO, 0, 0, room, 0, &p, &s.leg).unwrap();
            host.send_to(&pkt, relay_addr).await.unwrap();
            let w = recv_leg(&host, &s.leg).await;
            assert_eq!(w.op, leg::OP_WELCOME);
            let (h, _) = leg::welcome_parse(&w.payload).unwrap();
            // ANSWERING THE WELCOME IS NOT OPTIONAL, and this line is the test's memory of why:
            // until an address has sent something AFTER we replied to it, the relay holds it to 3x
            // what it sent (plan D4), which a 52-byte HELLO does not stretch far. `udp_relay.cpp`
            // pings the moment the WELCOME lands for exactly this reason -- without it the first
            // forwarded datagram of the match is the one the cap eats.
            let ping = leg::encode(leg::OP_PING, h, 0, room, 1, &[], &s.leg).unwrap();
            host.send_to(&ping, relay_addr).await.unwrap();
            let pong = recv_leg(&host, &s.leg).await;
            assert_eq!(pong.op, leg::OP_PONG);

            let mid = [0x77u8; 16];
            let p = leg::hello_payload(leg::ROLE_CLIENT, &mid);
            let pkt = leg::encode(leg::OP_HELLO, 0, 0, room, 0, &p, &s.leg).unwrap();
            client.send_to(&pkt, relay_addr).await.unwrap();
            let w = recv_leg(&client, &s.leg).await;
            let (c, other) = leg::welcome_parse(&w.payload).unwrap();
            assert_eq!(other, h, "the client is told which handle its host is");
            let ping = leg::encode(leg::OP_PING, c, 0, room, 1, &[], &s.leg).unwrap();
            client.send_to(&ping, relay_addr).await.unwrap();
            assert_eq!(recv_leg(&client, &s.leg).await.op, leg::OP_PONG);

            // --- the host grants a connect token, and the relay learns the session ------------
            let conn_id = [1u8, 2, 3, 4, 5, 6, 7, 1];
            let tok = wire::ConnectToken {
                match_id: [0x33u8; 16],
                conn_id,
                slot: 1,
                expire_unix_ms: 1_900_000_000_000,
                enc_c2s: [21u8; 32],
                enc_s2c: [22u8; 32],
                mac_c2s: [23u8; 32],
                mac_s2c: [24u8; 32],
            };
            let sealed = wire::token_seal(&psk, 99, &tok).unwrap();
            let mut fr = vec![3u8]; // HSK_GRANT
            fr.extend_from_slice(&sealed);
            let mut body = Vec::new();
            wire::frame_append(&mut body, 0x40, &fr); // CH_HS
            let boot = wire::packet_encode(
                wire::PKT_DATA,
                &s.boot_conn,
                1,
                &s.boot_enc,
                &s.boot_mac,
                &body,
            )
            .unwrap();
            let pkt = leg::encode(leg::OP_DATA, h, c, room, 2, &boot, &s.leg).unwrap();
            host.send_to(&pkt, relay_addr).await.unwrap();
            let got = recv_leg(&client, &s.leg).await;
            assert_eq!(
                got.payload, boot,
                "the grant reaches the client byte for byte"
            );

            // --- an authenticated datagram crosses --------------------------------------------
            let good = wire::packet_encode(
                wire::PKT_DATA,
                &conn_id,
                1,
                &tok.enc_c2s,
                &tok.mac_c2s,
                b"step 300",
            )
            .unwrap();
            let pkt = leg::encode(leg::OP_DATA, c, h, room, 2, &good, &s.leg).unwrap();
            client.send_to(&pkt, relay_addr).await.unwrap();
            let got = recv_leg(&host, &s.leg).await;
            assert_eq!(got.src, c);
            assert_eq!(got.payload, good);

            // --- a forged one does not, and an unregistered address does not -------------------
            let mut forged = good.clone();
            forged[wire::HDR_SIZE] ^= 0x80;
            let pkt = leg::encode(leg::OP_DATA, c, h, room, 3, &forged, &s.leg).unwrap();
            client.send_to(&pkt, relay_addr).await.unwrap();

            let stranger = UdpSocket::bind("127.0.0.1:0").await.unwrap();
            let pkt = leg::encode(leg::OP_DATA, 31337, h, room, 1, &good, &s.leg).unwrap();
            stranger.send_to(&pkt, relay_addr).await.unwrap();

            // A third, legitimate datagram behind the two bad ones: if either had been forwarded
            // it would arrive FIRST, so this is an ordering assertion, not a sleep.
            let tail = wire::packet_encode(
                wire::PKT_DATA,
                &conn_id,
                2,
                &tok.enc_c2s,
                &tok.mac_c2s,
                b"the next real one",
            )
            .unwrap();
            let pkt = leg::encode(leg::OP_DATA, c, h, room, 4, &tail, &s.leg).unwrap();
            client.send_to(&pkt, relay_addr).await.unwrap();
            let got = recv_leg(&host, &s.leg).await;
            assert_eq!(
                got.payload, tail,
                "the forged datagram and the stranger's were dropped, not queued"
            );
        };

        tokio::select! {
            _ = serve(&sock, &mut r, 0) => unreachable!("the socket loop does not return"),
            _ = drive => {}
        }

        let cn = r.counters;
        assert_eq!(cn.forwarded, 3, "grant + two good datagrams");
        assert_eq!(cn.inner_bad_mac, 1);
        assert_eq!(cn.unregistered, 1);
        assert_eq!(cn.tokens_learned, 1);
        assert_eq!(
            cn.inner_unverified, 0,
            "every forwarded packet was authenticated"
        );
    }
}
