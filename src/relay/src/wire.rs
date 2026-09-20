//! The UDP packet format, decoded and encoded on the Rust side (mp:T0, plan decision D2).
//!
//! # Why this exists as a second implementation
//!
//! The format's normative description is `src/mh_net_proto/include/mh_net_proto/net_udp.h` and its
//! C++ codec is `src/mh_net_proto/src/net_udp.cpp`. This module is a SEPARATE implementation, and
//! that is the point of tracker item T0: an encoder agreeing with its own decoder proves one
//! program self-consistent, which it would be just as happily over a format nothing else can read.
//! The claim worth making is that two independently written decoders accept and refuse the same
//! committed bytes -- so the tests at the bottom read the very fixture files that
//! `net_selftest.exe udpwiretest` reads, and neither side generates them at test time.
//!
//! The fixtures are emitted by the C++ side (`net_selftest.exe udpwiretest --emit <dir>`) and this
//! side only ever READS them. That direction is deliberate: if the format changes, the C++ encoder
//! moves first and this decoder must still agree, which is the drift that would otherwise ship.
//!
//! # What the relay (mp:R1) uses
//!
//! [`Header::peek`] and [`ReplayWindow`] are all that is needed to ROUTE a packet: the plaintext
//! header is what a key-less forwarder can read, and it is enough.
//!
//! R1 does one thing more, and it is a deliberate narrowing rather than a widening. It opens each
//! session's [`token_open`] grant out of the bootstrap exchange (which is sealed under keys derived
//! from the PSK the relay also holds), keeps that session's two MAC keys and **discards both
//! encryption keys** -- so [`mac_verify`] lets it refuse a forged datagram instead of carrying it,
//! while leaving it unable to read one byte of the match. `docs/mp-relay.md` states the trust
//! boundary that follows from that.
//!
//! The rest of this module -- the three channels, the bulk pieces -- is here because the fixtures
//! test it and because the relay's loopback test stands in for a client.
//!
//! Crypto is the same as the C++ side and nothing new: ChaCha20 (RFC 8439, 12-byte nonce, counter
//! starting at zero) with the 64-bit sequence in nonce bytes 4..12, and HMAC-SHA256 truncated to
//! 128 bits over `header || ciphertext`.

// R1 is the first consumer; until it lands, the encode half and several accessors have no caller in
// this crate. Allowing dead code beats deleting the half the fixtures prove, or marking each item.
#![allow(dead_code)]

use chacha20::cipher::{KeyIvInit, StreamCipher};
use chacha20::ChaCha20;
use hmac::{Hmac, Mac};
use sha2::{Digest, Sha256};

type HmacSha256 = Hmac<Sha256>;

pub const MAX_DATAGRAM: usize = 1200;
pub const HDR_SIZE: usize = 18;
pub const TAG_SIZE: usize = 16;
pub const MAX_BODY: usize = MAX_DATAGRAM - HDR_SIZE - TAG_SIZE;
pub const MIN_DATAGRAM: usize = HDR_SIZE + TAG_SIZE;

pub const MAGIC_NIBBLE: u8 = 0x4;
pub const VERSION: u8 = 1;
pub const MAGIC_VER: u8 = (MAGIC_NIBBLE << 4) | VERSION;

pub const PKT_DATA: u8 = 0;
pub const PKT_TOKEN: u8 = 1;
pub const PKT_TOKEN_ACK: u8 = 2;
pub const PKT_KEEPALIVE: u8 = 3;
pub const PKT_DISCONNECT: u8 = 4;
pub const PKT_TYPE_MAX: u8 = 4;

pub const CONN_ID_BYTES: usize = 8;
pub const KEY_LEN: usize = 32;
pub const REPLAY_WINDOW: u64 = 64;

pub const FRAME_HDR: usize = 3;
pub const CH_INPUT: u8 = 1;
pub const CH_STATE: u8 = 2;
pub const CH_BULK: u8 = 3;

pub const INPUT_K_MIN: u8 = 1;
pub const INPUT_K_MAX: u8 = 8;

pub const REC_PING: u8 = 1;
pub const REC_PONG: u8 = 2;
pub const REC_STEP_HASH: u8 = 3;
pub const REC_TELEMETRY: u8 = 4;
pub const REC_ID_MAX: u8 = 4;
pub const REC_LEN_MAX: usize = 64;

pub const CHUNK_MAX: u32 = 16 * 1024;
pub const PIECE_MAX: usize = 1100;
pub const PIECE_HDR: usize = 51;
pub const ACK_LEN: usize = 9;
pub const BULK_PIECE: u8 = 0;
pub const BULK_ACK: u8 = 1;
/// 16, not the 15 that `CHUNK_MAX / PIECE_MAX` implies, because since **mp:R1d the piece stride is
/// a property of the LINK**: a relayed endpoint slices at [`PIECE_MAX_RELAYED`] so the 34-byte leg
/// envelope still leaves the datagram inside T0's 1200-byte ceiling, and 16 KiB at that stride is
/// 16 pieces. Mirrored here only to keep this file and `mh_net_proto/net_udp.h` one contract -- the
/// relay never opens a game body and therefore never decodes a piece outside its own fixtures,
/// which all run at the default stride.
pub const PIECE_TOTAL_MAX: u16 = 16;
/// mp:R1d -- the relayed stride. See `docs/mp-wire-udp.md` for why 1024 and not "1100 minus 34".
pub const PIECE_MAX_RELAYED: usize = 1024;

pub const TOKEN_KIND: u8 = 1;
pub const TOKEN_PRIVATE: usize = 168;
pub const TOKEN_WIRE: usize = 26 + TOKEN_PRIVATE + TAG_SIZE;
pub const TOKEN_SLOT_MAX: u8 = 7;

/// Every way a datagram can be refused, one value each -- the same set and the same NAMES as the
/// C++ `Verdict`, because the fixture index records the expected verdict as that name. A refusal
/// that arrived here as a bare `false` would make "the peer is on an old build" and "we are being
/// attacked" the same line in a relay's log.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Verdict {
    Ok,
    TooLong,
    TooShort,
    BadMagic,
    BadVersion,
    BadType,
    WrongConn,
    Replay,
    BadMac,
    Malformed,
    BodyTooLong,
}

impl Verdict {
    pub fn name(self) -> &'static str {
        match self {
            Verdict::Ok => "ok",
            Verdict::TooLong => "too_long",
            Verdict::TooShort => "too_short",
            Verdict::BadMagic => "bad_magic",
            Verdict::BadVersion => "bad_version",
            Verdict::BadType => "bad_type",
            Verdict::WrongConn => "wrong_conn",
            Verdict::Replay => "replay",
            Verdict::BadMac => "bad_mac",
            Verdict::Malformed => "malformed",
            Verdict::BodyTooLong => "body_too_long",
        }
    }
}

fn u32le(b: &[u8]) -> u32 {
    u32::from_le_bytes([b[0], b[1], b[2], b[3]])
}
fn u16le(b: &[u8]) -> u16 {
    u16::from_le_bytes([b[0], b[1]])
}
fn u64le(b: &[u8]) -> u64 {
    let mut a = [0u8; 8];
    a.copy_from_slice(&b[..8]);
    u64::from_le_bytes(a)
}

/// Fixed-time-ish equality. A byte-at-a-time `==` on a tag leaks its matching prefix through
/// timing; folding every byte into one accumulator does not, and needs no extra dependency.
pub(crate) fn ct_eq(a: &[u8], b: &[u8]) -> bool {
    a.len() == b.len() && a.iter().zip(b).fold(0u8, |acc, (x, y)| acc | (x ^ y)) == 0
}

fn chacha20_xor(key: &[u8; KEY_LEN], seq: u64, buf: &mut [u8]) {
    let mut nonce = [0u8; 12];
    nonce[4..12].copy_from_slice(&seq.to_le_bytes());
    let mut c = ChaCha20::new(key.into(), (&nonce).into());
    c.apply_keystream(buf);
}

pub(crate) fn hmac16(key: &[u8; KEY_LEN], msg: &[u8]) -> [u8; TAG_SIZE] {
    let mut m = <HmacSha256 as Mac>::new_from_slice(key).expect("hmac accepts any key length");
    m.update(msg);
    let full = m.finalize().into_bytes();
    let mut out = [0u8; TAG_SIZE];
    out.copy_from_slice(&full[..TAG_SIZE]);
    out
}

pub(crate) fn hmac32(key: &[u8], msg: &[u8]) -> [u8; 32] {
    let mut m = <HmacSha256 as Mac>::new_from_slice(key).expect("hmac accepts any key length");
    m.update(msg);
    let full = m.finalize().into_bytes();
    let mut out = [0u8; 32];
    out.copy_from_slice(&full);
    out
}

// ---- header -------------------------------------------------------------------------------------

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Header {
    pub ty: u8,
    pub conn_id: [u8; CONN_ID_BYTES],
    pub seq: u64,
}

impl Header {
    /// What a relay sees: the routing fields, with no key and no authentication. `None` means the
    /// datagram is not one of ours at all -- it never means "drop it silently and count nothing".
    pub fn peek(pkt: &[u8]) -> Option<Header> {
        if pkt.len() < MIN_DATAGRAM || pkt.len() > MAX_DATAGRAM {
            return None;
        }
        if pkt[0] >> 4 != MAGIC_NIBBLE || pkt[0] & 0x0f != VERSION || pkt[1] > PKT_TYPE_MAX {
            return None;
        }
        let mut conn_id = [0u8; CONN_ID_BYTES];
        conn_id.copy_from_slice(&pkt[2..10]);
        Some(Header {
            ty: pkt[1],
            conn_id,
            seq: u64le(&pkt[10..18]),
        })
    }
}

// ---- replay window ------------------------------------------------------------------------------

/// netcode.io's 64-entry sliding window. `check` is separate from `commit` for the reason the C++
/// header spells out: the check runs before the HMAC so a duplicate flood costs no crypto, and the
/// commit runs after it so a forged packet carrying a plausible future sequence cannot push the
/// window forward and make the real packet at that sequence look like a replay.
#[derive(Debug, Default, Clone)]
pub struct ReplayWindow {
    newest: u64,
    bits: u64,
    seeded: bool,
}

impl ReplayWindow {
    pub fn new() -> Self {
        Self::default()
    }
    pub fn reset(&mut self) {
        *self = Self::default();
    }
    pub fn check(&self, seq: u64) -> bool {
        if !self.seeded || seq > self.newest {
            return true;
        }
        let age = self.newest - seq;
        if age >= REPLAY_WINDOW {
            return false;
        }
        self.bits & (1u64 << age) == 0
    }
    pub fn commit(&mut self, seq: u64) {
        if !self.check(seq) {
            return;
        }
        if !self.seeded {
            self.seeded = true;
            self.newest = seq;
            self.bits = 1;
        } else if seq > self.newest {
            let shift = seq - self.newest;
            self.bits = if shift >= REPLAY_WINDOW {
                0
            } else {
                self.bits << shift
            };
            self.bits |= 1;
            self.newest = seq;
        } else {
            self.bits |= 1u64 << (self.newest - seq);
        }
    }
}

// ---- packet -------------------------------------------------------------------------------------

pub fn packet_encode(
    ty: u8,
    conn_id: &[u8; CONN_ID_BYTES],
    seq: u64,
    enc_key: &[u8; KEY_LEN],
    mac_key: &[u8; KEY_LEN],
    body: &[u8],
) -> Result<Vec<u8>, Verdict> {
    if ty > PKT_TYPE_MAX {
        return Err(Verdict::BadType);
    }
    if body.len() > MAX_BODY {
        return Err(Verdict::BodyTooLong);
    }
    let mut out = Vec::with_capacity(HDR_SIZE + body.len() + TAG_SIZE);
    out.push(MAGIC_VER);
    out.push(ty);
    out.extend_from_slice(conn_id);
    out.extend_from_slice(&seq.to_le_bytes());
    out.extend_from_slice(body);
    chacha20_xor(enc_key, seq, &mut out[HDR_SIZE..]);
    let tag = hmac16(mac_key, &out);
    out.extend_from_slice(&tag);
    Ok(out)
}

/// Verify and decrypt. Returns the header and the plaintext body.
///
/// The ORDER of the refusals is part of the format, not an implementation choice: the length
/// ceiling is tested first so an oversize datagram is refused for its size whatever it contains.
pub fn packet_decode(
    pkt: &[u8],
    expect_conn: Option<&[u8; CONN_ID_BYTES]>,
    enc_key: &[u8; KEY_LEN],
    mac_key: &[u8; KEY_LEN],
    win: Option<&mut ReplayWindow>,
) -> Result<(Header, Vec<u8>), Verdict> {
    if pkt.len() > MAX_DATAGRAM {
        return Err(Verdict::TooLong);
    }
    if pkt.len() < MIN_DATAGRAM {
        return Err(Verdict::TooShort);
    }
    if pkt[0] >> 4 != MAGIC_NIBBLE {
        return Err(Verdict::BadMagic);
    }
    if pkt[0] & 0x0f != VERSION {
        return Err(Verdict::BadVersion);
    }
    if pkt[1] > PKT_TYPE_MAX {
        return Err(Verdict::BadType);
    }
    let mut conn_id = [0u8; CONN_ID_BYTES];
    conn_id.copy_from_slice(&pkt[2..10]);
    let hdr = Header {
        ty: pkt[1],
        conn_id,
        seq: u64le(&pkt[10..18]),
    };
    if let Some(want) = expect_conn {
        if &hdr.conn_id != want {
            return Err(Verdict::WrongConn);
        }
    }
    let win = match win {
        Some(w) => {
            if !w.check(hdr.seq) {
                return Err(Verdict::Replay);
            }
            Some(w)
        }
        None => None,
    };
    let ct_end = pkt.len() - TAG_SIZE;
    let tag = hmac16(mac_key, &pkt[..ct_end]);
    if !ct_eq(&tag, &pkt[ct_end..]) {
        return Err(Verdict::BadMac);
    }
    let mut body = pkt[HDR_SIZE..ct_end].to_vec();
    chacha20_xor(enc_key, hdr.seq, &mut body);
    if let Some(w) = win {
        w.commit(hdr.seq);
    }
    Ok((hdr, body))
}

/// Authenticate a datagram WITHOUT decrypting it -- the relay's operation (mp:R1).
///
/// A relay that has opened a connect token holds that session's two MAC keys and deliberately
/// keeps NEITHER of its encryption keys (`relay.rs`), so it can prove a datagram is the
/// registered peer's and still be unable to read a byte of the match. That is a stronger property
/// than "the relay is trusted not to look", and it costs one HMAC per packet.
///
/// Length and magic are checked first for the same reason [`packet_decode`] checks them first: the
/// answer for an oversize or foreign datagram must not depend on a key.
pub fn mac_verify(pkt: &[u8], mac_key: &[u8; KEY_LEN]) -> bool {
    if pkt.len() > MAX_DATAGRAM || pkt.len() < MIN_DATAGRAM {
        return false;
    }
    let ct_end = pkt.len() - TAG_SIZE;
    ct_eq(&hmac16(mac_key, &pkt[..ct_end]), &pkt[ct_end..])
}

// ---- channel mux ---------------------------------------------------------------------------------

#[derive(Debug, Clone, Copy)]
pub struct Frame<'a> {
    pub id: u8,
    pub data: &'a [u8],
}

/// Walk a body's frames. `Err(Verdict::Malformed)` on a truncated frame -- NOT an early `Ok`,
/// because a caller treating truncation as the end of the stream is how a short read becomes a
/// silently-dropped channel.
pub fn frames(body: &[u8]) -> Result<Vec<Frame<'_>>, Verdict> {
    let mut out = Vec::new();
    let mut off = 0usize;
    while off != body.len() {
        if off + FRAME_HDR > body.len() {
            return Err(Verdict::Malformed);
        }
        let len = u16le(&body[off + 1..off + 3]) as usize;
        if off + FRAME_HDR + len > body.len() {
            return Err(Verdict::Malformed);
        }
        out.push(Frame {
            id: body[off],
            data: &body[off + FRAME_HDR..off + FRAME_HDR + len],
        });
        off += FRAME_HDR + len;
    }
    Ok(out)
}

pub fn frame_append(body: &mut Vec<u8>, id: u8, payload: &[u8]) -> bool {
    if payload.len() > 0xffff || body.len() + FRAME_HDR + payload.len() > MAX_BODY {
        return false;
    }
    body.push(id);
    body.extend_from_slice(&(payload.len() as u16).to_le_bytes());
    body.extend_from_slice(payload);
    true
}

// ---- channel A: step inputs ------------------------------------------------------------------------

#[derive(Debug, Clone)]
pub struct InputEntry {
    pub step: u32,
    pub bytes: Vec<u8>,
}

/// `count(1) | newest_step(4 LE) | count x { len(1) | bytes }`, entry *i* being step
/// `newest_step - i`. The step numbers are implied, which is what makes the redundancy cheap.
pub fn input_decode(payload: &[u8]) -> Result<Vec<InputEntry>, Verdict> {
    if payload.len() < 5 {
        return Err(Verdict::Malformed);
    }
    let count = payload[0];
    if !(INPUT_K_MIN..=INPUT_K_MAX).contains(&count) {
        return Err(Verdict::Malformed);
    }
    let newest = u32le(&payload[1..5]);
    let mut off = 5usize;
    let mut out = Vec::with_capacity(count as usize);
    for i in 0..count {
        if off >= payload.len() {
            return Err(Verdict::Malformed);
        }
        let len = payload[off] as usize;
        off += 1;
        if off + len > payload.len() {
            return Err(Verdict::Malformed);
        }
        out.push(InputEntry {
            step: newest.wrapping_sub(i as u32),
            bytes: payload[off..off + len].to_vec(),
        });
        off += len;
    }
    if off != payload.len() {
        return Err(Verdict::Malformed); // trailing bytes: the frame disagrees with its contents
    }
    Ok(out)
}

pub fn input_encode(entries: &[InputEntry]) -> Result<Vec<u8>, Verdict> {
    let count = entries.len();
    if count < INPUT_K_MIN as usize || count > INPUT_K_MAX as usize {
        return Err(Verdict::Malformed);
    }
    for (i, e) in entries.iter().enumerate() {
        if e.bytes.len() > 255 || e.step != entries[0].step.wrapping_sub(i as u32) {
            return Err(Verdict::Malformed);
        }
    }
    let mut out = Vec::new();
    out.push(count as u8);
    out.extend_from_slice(&entries[0].step.to_le_bytes());
    for e in entries {
        out.push(e.bytes.len() as u8);
        out.extend_from_slice(&e.bytes);
    }
    Ok(out)
}

// ---- channel B: latest-wins records ------------------------------------------------------------------

/// `repeated { id(1) | len(1) | bytes }`, the newest record per id winning. An id this build does
/// not know is SKIPPED rather than ending the walk -- that is what the length byte is carried for,
/// and it is what lets a later build add a record without a flag day.
#[derive(Debug, Default, Clone)]
pub struct LatestSlots {
    pub slots: [Option<Vec<u8>>; (REC_ID_MAX + 1) as usize],
}

pub fn records_absorb(payload: &[u8], slots: &mut LatestSlots) -> Result<(), Verdict> {
    let mut off = 0usize;
    while off != payload.len() {
        if off + 2 > payload.len() {
            return Err(Verdict::Malformed);
        }
        let id = payload[off];
        let len = payload[off + 1] as usize;
        if off + 2 + len > payload.len() {
            return Err(Verdict::Malformed);
        }
        if (1..=REC_ID_MAX).contains(&id) && len <= REC_LEN_MAX {
            slots.slots[id as usize] = Some(payload[off + 2..off + 2 + len].to_vec());
        }
        off += 2 + len;
    }
    Ok(())
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PingRecord {
    pub t_origin_ms: u32,
    pub t_echo_ms: u32,
}
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct StepHashRecord {
    pub step: u32,
    pub hash: u32,
}
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct TelemetryRecord {
    pub srtt_us: u32,
    pub rttvar_us: u32,
    pub loss_q16: u32,
    pub last_step: u32,
}

impl LatestSlots {
    fn get(&self, id: u8, want_len: usize) -> Option<&[u8]> {
        self.slots
            .get(id as usize)?
            .as_deref()
            .filter(|b| b.len() == want_len)
    }
    pub fn ping(&self, id: u8) -> Option<PingRecord> {
        if id != REC_PING && id != REC_PONG {
            return None;
        }
        let b = self.get(id, 8)?;
        Some(PingRecord {
            t_origin_ms: u32le(&b[0..4]),
            t_echo_ms: u32le(&b[4..8]),
        })
    }
    pub fn step_hash(&self) -> Option<StepHashRecord> {
        let b = self.get(REC_STEP_HASH, 8)?;
        Some(StepHashRecord {
            step: u32le(&b[0..4]),
            hash: u32le(&b[4..8]),
        })
    }
    pub fn telemetry(&self) -> Option<TelemetryRecord> {
        let b = self.get(REC_TELEMETRY, 16)?;
        Some(TelemetryRecord {
            srtt_us: u32le(&b[0..4]),
            rttvar_us: u32le(&b[4..8]),
            loss_q16: u32le(&b[8..12]),
            last_step: u32le(&b[12..16]),
        })
    }
}

// ---- channel C: bulk reliable -----------------------------------------------------------------------

#[derive(Debug, Clone)]
pub struct Piece {
    pub chunk_id: u32,
    pub index: u16,
    pub total: u16,
    pub chunk_len: u32,
    pub chunk_sha: [u8; 32],
    pub piece_seq: u32,
    pub bytes: Vec<u8>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Ack {
    pub ack_seq: u32,
    pub ack_bits: u32,
}

pub fn piece_count(chunk_len: u32) -> u16 {
    if chunk_len == 0 || chunk_len > CHUNK_MAX {
        return 0;
    }
    chunk_len.div_ceil(PIECE_MAX as u32) as u16
}

pub fn piece_decode(payload: &[u8]) -> Result<Piece, Verdict> {
    if payload.len() < PIECE_HDR || payload[0] != BULK_PIECE {
        return Err(Verdict::Malformed);
    }
    let mut chunk_sha = [0u8; 32];
    chunk_sha.copy_from_slice(&payload[13..45]);
    let plen = u16le(&payload[49..51]) as usize;
    if plen > PIECE_MAX || PIECE_HDR + plen != payload.len() {
        return Err(Verdict::Malformed);
    }
    let p = Piece {
        chunk_id: u32le(&payload[1..5]),
        index: u16le(&payload[5..7]),
        total: u16le(&payload[7..9]),
        chunk_len: u32le(&payload[9..13]),
        chunk_sha,
        piece_seq: u32le(&payload[45..49]),
        bytes: payload[PIECE_HDR..].to_vec(),
    };
    if p.chunk_len == 0 || p.chunk_len > CHUNK_MAX {
        return Err(Verdict::Malformed);
    }
    if p.total == 0 || p.total > PIECE_TOTAL_MAX || p.index >= p.total {
        return Err(Verdict::Malformed);
    }
    if p.total != piece_count(p.chunk_len) {
        return Err(Verdict::Malformed);
    }
    Ok(p)
}

pub fn piece_encode(p: &Piece) -> Result<Vec<u8>, Verdict> {
    if p.bytes.len() > PIECE_MAX || p.chunk_len == 0 || p.chunk_len > CHUNK_MAX {
        return Err(Verdict::Malformed);
    }
    if p.total == 0 || p.total > PIECE_TOTAL_MAX || p.index >= p.total {
        return Err(Verdict::Malformed);
    }
    let mut out = Vec::with_capacity(PIECE_HDR + p.bytes.len());
    out.push(BULK_PIECE);
    out.extend_from_slice(&p.chunk_id.to_le_bytes());
    out.extend_from_slice(&p.index.to_le_bytes());
    out.extend_from_slice(&p.total.to_le_bytes());
    out.extend_from_slice(&p.chunk_len.to_le_bytes());
    out.extend_from_slice(&p.chunk_sha);
    out.extend_from_slice(&p.piece_seq.to_le_bytes());
    out.extend_from_slice(&(p.bytes.len() as u16).to_le_bytes());
    out.extend_from_slice(&p.bytes);
    Ok(out)
}

pub fn ack_decode(payload: &[u8]) -> Option<Ack> {
    if payload.len() != ACK_LEN || payload[0] != BULK_ACK {
        return None;
    }
    Some(Ack {
        ack_seq: u32le(&payload[1..5]),
        ack_bits: u32le(&payload[5..9]),
    })
}

pub fn ack_encode(a: &Ack) -> Vec<u8> {
    let mut out = Vec::with_capacity(ACK_LEN);
    out.push(BULK_ACK);
    out.extend_from_slice(&a.ack_seq.to_le_bytes());
    out.extend_from_slice(&a.ack_bits.to_le_bytes());
    out
}

pub fn sha256(data: &[u8]) -> [u8; 32] {
    let mut h = Sha256::new();
    h.update(data);
    let d = h.finalize();
    let mut out = [0u8; 32];
    out.copy_from_slice(&d);
    out
}

// ---- connect token ------------------------------------------------------------------------------

#[derive(Debug, Clone)]
pub struct ConnectToken {
    pub match_id: [u8; 16],
    pub conn_id: [u8; CONN_ID_BYTES],
    pub slot: u8,
    pub expire_unix_ms: u64,
    pub enc_c2s: [u8; KEY_LEN],
    pub enc_s2c: [u8; KEY_LEN],
    pub mac_c2s: [u8; KEY_LEN],
    pub mac_s2c: [u8; KEY_LEN],
}

pub fn token_keys(host_key: &[u8; KEY_LEN]) -> ([u8; KEY_LEN], [u8; KEY_LEN]) {
    (
        hmac32(host_key, b"mh-token-enc"),
        hmac32(host_key, b"mh-token-mac"),
    )
}

/// Open a token. `now_unix_ms == 0` skips the expiry test. An expired token comes back as
/// [`Verdict::BadMac`] with `expired == true`: the wire answer is deliberately the same as a wrong
/// key, because telling an attacker which of the two applied is free information.
pub fn token_open(
    host_key: &[u8; KEY_LEN],
    wire: &[u8],
    now_unix_ms: u64,
) -> (Result<ConnectToken, Verdict>, bool) {
    if wire.len() != TOKEN_WIRE {
        return (Err(Verdict::TooShort), false);
    }
    if wire[0] >> 4 != MAGIC_NIBBLE {
        return (Err(Verdict::BadMagic), false);
    }
    if wire[0] & 0x0f != VERSION {
        return (Err(Verdict::BadVersion), false);
    }
    if wire[1] != TOKEN_KIND {
        return (Err(Verdict::BadType), false);
    }
    let (enc, mac) = token_keys(host_key);
    let signed = 26 + TOKEN_PRIVATE;
    let tag = hmac16(&mac, &wire[..signed]);
    if !ct_eq(&tag, &wire[signed..]) {
        return (Err(Verdict::BadMac), false);
    }
    let nonce = u64le(&wire[2..10]);
    let mut priv_part = wire[26..signed].to_vec();
    chacha20_xor(&enc, nonce, &mut priv_part);

    let mut tok = ConnectToken {
        match_id: [0u8; 16],
        conn_id: [0u8; CONN_ID_BYTES],
        slot: priv_part[24],
        expire_unix_ms: u64le(&priv_part[32..40]),
        enc_c2s: [0u8; KEY_LEN],
        enc_s2c: [0u8; KEY_LEN],
        mac_c2s: [0u8; KEY_LEN],
        mac_s2c: [0u8; KEY_LEN],
    };
    tok.match_id.copy_from_slice(&priv_part[0..16]);
    tok.conn_id.copy_from_slice(&priv_part[16..24]);
    tok.enc_c2s.copy_from_slice(&priv_part[40..72]);
    tok.enc_s2c.copy_from_slice(&priv_part[72..104]);
    tok.mac_c2s.copy_from_slice(&priv_part[104..136]);
    tok.mac_s2c.copy_from_slice(&priv_part[136..168]);

    if priv_part[25..32].iter().any(|&b| b != 0) {
        return (Err(Verdict::Malformed), false);
    }
    // The routing copies of conn_id and expiry must equal the sealed ones: a party holding the key
    // could otherwise mint a token a relay routes one way and a host reads another.
    if tok.conn_id != wire[10..18] {
        return (Err(Verdict::Malformed), false);
    }
    if tok.expire_unix_ms != u64le(&wire[18..26]) {
        return (Err(Verdict::Malformed), false);
    }
    if tok.slot > TOKEN_SLOT_MAX {
        return (Err(Verdict::Malformed), false);
    }
    if now_unix_ms != 0 && now_unix_ms > tok.expire_unix_ms {
        return (Err(Verdict::BadMac), true);
    }
    (Ok(tok), false)
}

pub fn token_seal(
    host_key: &[u8; KEY_LEN],
    nonce: u64,
    tok: &ConnectToken,
) -> Result<Vec<u8>, Verdict> {
    if tok.slot > TOKEN_SLOT_MAX {
        return Err(Verdict::Malformed);
    }
    let (enc, mac) = token_keys(host_key);
    let mut priv_part = vec![0u8; TOKEN_PRIVATE];
    priv_part[0..16].copy_from_slice(&tok.match_id);
    priv_part[16..24].copy_from_slice(&tok.conn_id);
    priv_part[24] = tok.slot;
    priv_part[32..40].copy_from_slice(&tok.expire_unix_ms.to_le_bytes());
    priv_part[40..72].copy_from_slice(&tok.enc_c2s);
    priv_part[72..104].copy_from_slice(&tok.enc_s2c);
    priv_part[104..136].copy_from_slice(&tok.mac_c2s);
    priv_part[136..168].copy_from_slice(&tok.mac_s2c);

    let mut out = Vec::with_capacity(TOKEN_WIRE);
    out.push(MAGIC_VER);
    out.push(TOKEN_KIND);
    out.extend_from_slice(&nonce.to_le_bytes());
    out.extend_from_slice(&tok.conn_id);
    out.extend_from_slice(&tok.expire_unix_ms.to_le_bytes());
    chacha20_xor(&enc, nonce, &mut priv_part);
    out.extend_from_slice(&priv_part);
    let tag = hmac16(&mac, &out);
    out.extend_from_slice(&tag);
    Ok(out)
}

// ---- the shared fixtures -----------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::Value;
    use std::collections::BTreeMap;
    use std::path::{Path, PathBuf};

    /// The fixture directory, resolved from this crate's manifest rather than from the working
    /// directory: `cargo test` runs with cwd = the crate root, but a workspace-level invocation
    /// does not, and a fixture set found only sometimes is worse than one found never.
    fn fixture_dir() -> PathBuf {
        Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("..")
            .join("mh_net_proto")
            .join("test")
            .join("fixtures")
            .join("udp")
    }

    fn unhex(s: &str) -> Vec<u8> {
        assert!(s.len() % 2 == 0, "hex of odd length: {s}");
        (0..s.len() / 2)
            .map(|i| u8::from_str_radix(&s[i * 2..i * 2 + 2], 16).expect("hex digit"))
            .collect()
    }

    fn key32(s: &str) -> [u8; KEY_LEN] {
        let v = unhex(s);
        let mut k = [0u8; KEY_LEN];
        k.copy_from_slice(&v);
        k
    }

    fn conn8(s: &str) -> [u8; CONN_ID_BYTES] {
        let v = unhex(s);
        let mut c = [0u8; CONN_ID_BYTES];
        c.copy_from_slice(&v);
        c
    }

    /// The index is a JSON array of flat objects whose values are ALL strings, numbers included.
    /// The C++ side has no JSON library, so the grammar was narrowed to what ~60 lines of C can
    /// read; from here it is ordinary JSON and serde reads it as written.
    fn load_index() -> Vec<BTreeMap<String, String>> {
        let path = fixture_dir().join("index.json");
        let text = std::fs::read_to_string(&path).unwrap_or_else(|e| {
            panic!(
                "cannot read {} ({e}) -- regenerate with `net_selftest.exe udpwiretest --emit`",
                path.display()
            )
        });
        let v: Value = serde_json::from_str(&text).expect("the index is JSON");
        v.as_array()
            .expect("the index is an array")
            .iter()
            .map(|obj| {
                obj.as_object()
                    .expect("each case is an object")
                    .iter()
                    .map(|(k, val)| {
                        (
                            k.clone(),
                            val.as_str()
                                .expect("every index value is a string, numbers included")
                                .to_string(),
                        )
                    })
                    .collect()
            })
            .collect()
    }

    fn blob(name: &str) -> Vec<u8> {
        let p = fixture_dir().join(name);
        std::fs::read(&p).unwrap_or_else(|e| panic!("cannot read {} ({e})", p.display()))
    }

    /// THE ACCEPTANCE TEST. Every committed case, decoded by THIS implementation, must produce the
    /// verdict the C++ implementation recorded -- including the four clauses tracker mp:T0 names:
    /// the round-trips, the tampered byte failing the MAC, the replayed sequence being dropped, and
    /// the 1201-byte datagram being refused.
    #[test]
    fn fixtures_agree_with_the_cpp_side() {
        let cases = load_index();
        assert!(cases.len() >= 20, "the fixture set is suspiciously small");
        let mut windows: BTreeMap<String, ReplayWindow> = BTreeMap::new();
        let mut seen_ok = 0;
        let mut seen_refusals: BTreeMap<&str, usize> = BTreeMap::new();

        for c in &cases {
            let key = c["key"].as_str();
            let want = c["verdict"].as_str();
            let wire = blob(&c["wire"]);

            if c["kind"] == "token" {
                let host = key32(&c["host_key"]);
                let now: u64 = c["now_ms"].parse().unwrap();
                let (res, expired) = token_open(&host, &wire, now);
                let got = match &res {
                    Ok(_) => "ok",
                    Err(v) => v.name(),
                };
                assert_eq!(got, want, "{key}: verdict");
                assert_eq!(expired, c["expired"] == "1", "{key}: expired flag");
                if let Ok(tok) = res {
                    assert_eq!(
                        tok.match_id.to_vec(),
                        unhex(&c["match_id"]),
                        "{key}: match_id"
                    );
                    assert_eq!(tok.conn_id.to_vec(), unhex(&c["conn_id"]), "{key}: conn_id");
                    assert_eq!(tok.slot.to_string(), c["slot"], "{key}: slot");
                    assert_eq!(
                        tok.expire_unix_ms.to_string(),
                        c["expire_ms"],
                        "{key}: expiry"
                    );
                    assert_eq!(tok.enc_c2s.to_vec(), unhex(&c["enc_c2s"]), "{key}: enc_c2s");
                    assert_eq!(tok.mac_s2c.to_vec(), unhex(&c["mac_s2c"]), "{key}: mac_s2c");
                    // Re-sealing the opened token must reproduce the committed bytes exactly: the
                    // encode half is then proven against the same fixture as the decode half.
                    let nonce = u64le(&wire[2..10]);
                    assert_eq!(
                        token_seal(&host, nonce, &tok).expect("re-seal"),
                        wire,
                        "{key}: re-seal is byte-identical"
                    );
                }
                *seen_refusals.entry(want).or_default() += 1;
                continue;
            }

            let enc = key32(&c["enc_key"]);
            let mac = key32(&c["mac_key"]);
            let conn = conn8(&c["conn_id"]);
            // Windows are shared by NAME and the cases are processed in index order, so a replay
            // case is a genuine second delivery into a window an earlier case already advanced.
            let wname = c["window"].clone();
            let res = if wname.is_empty() {
                packet_decode(&wire, Some(&conn), &enc, &mac, None)
            } else {
                let w = windows.entry(wname).or_default();
                packet_decode(&wire, Some(&conn), &enc, &mac, Some(w))
            };
            let got = match &res {
                Ok(_) => "ok",
                Err(v) => v.name(),
            };
            assert_eq!(got, want, "{key}: verdict");
            *seen_refusals.entry(want).or_default() += 1;

            let (hdr, body) = match res {
                Ok(v) => v,
                Err(_) => continue,
            };
            seen_ok += 1;
            assert_eq!(hdr.seq.to_string(), c["seq"], "{key}: sequence");
            if let Some(t) = c.get("type") {
                assert_eq!(hdr.ty.to_string(), *t, "{key}: packet type");
            }
            if let Some(pf) = c.get("plain") {
                assert_eq!(body, blob(pf), "{key}: plaintext body");
            }
            // Re-encoding must reproduce the committed datagram byte for byte -- which is the only
            // way this side's ENCODER is under test at all.
            assert_eq!(
                packet_encode(hdr.ty, &hdr.conn_id, hdr.seq, &enc, &mac, &body).expect("re-encode"),
                wire,
                "{key}: re-encode is byte-identical"
            );

            let walked = frames(&body);
            if key.contains("malformed") {
                assert!(
                    walked.is_err(),
                    "{key}: the frame walk must refuse this body"
                );
                continue;
            }
            let walked = walked.unwrap_or_else(|v| panic!("{key}: frame walk failed: {v:?}"));
            for f in &walked {
                match f.id {
                    CH_INPUT => {
                        let e = input_decode(f.data)
                            .unwrap_or_else(|v| panic!("{key}: channel A: {v:?}"));
                        for (i, entry) in e.iter().enumerate() {
                            assert_eq!(
                                entry.step,
                                e[0].step - i as u32,
                                "{key}: channel A implied step run"
                            );
                        }
                        assert_eq!(
                            input_encode(&e).expect("channel A re-encode"),
                            f.data,
                            "{key}: channel A re-encode"
                        );
                    }
                    CH_STATE => {
                        let mut slots = LatestSlots::default();
                        records_absorb(f.data, &mut slots)
                            .unwrap_or_else(|v| panic!("{key}: channel B: {v:?}"));
                        assert!(slots.ping(REC_PING).is_some(), "{key}: channel B ping");
                        assert!(slots.step_hash().is_some(), "{key}: channel B step hash");
                        assert!(slots.telemetry().is_some(), "{key}: channel B telemetry");
                    }
                    CH_BULK => {
                        if let Some(a) = ack_decode(f.data) {
                            assert_eq!(ack_encode(&a), f.data, "{key}: channel C ack re-encode");
                        } else {
                            let p = piece_decode(f.data)
                                .unwrap_or_else(|v| panic!("{key}: channel C: {v:?}"));
                            assert_eq!(
                                piece_encode(&p).expect("piece re-encode"),
                                f.data,
                                "{key}: channel C piece re-encode"
                            );
                        }
                    }
                    // An unknown channel id is skipped, by design (p12_max_size carries one).
                    _ => {}
                }
            }
        }

        // A run that asserted nothing would still pass every line above. These four say the fixture
        // set actually carried the acceptance clauses rather than having quietly lost them.
        assert!(seen_ok >= 6, "too few accepted packets: {seen_ok}");
        assert!(
            seen_refusals.get("bad_mac").copied().unwrap_or(0) >= 2,
            "no tampered-byte case"
        );
        assert_eq!(
            seen_refusals.get("replay").copied().unwrap_or(0),
            2,
            "expected a duplicate AND a below-the-floor replay case"
        );
        assert_eq!(
            seen_refusals.get("too_long").copied().unwrap_or(0),
            1,
            "no 1201-byte refusal case"
        );
    }

    /// The window's own arithmetic, at every position -- the ranges a fixture per value could not
    /// express without a directory nobody reads.
    #[test]
    fn replay_window_positions() {
        let mut w = ReplayWindow::new();
        assert!(w.check(12345), "a fresh window accepts anything");
        w.commit(1000);
        assert!(!w.check(1000), "the same sequence twice is a replay");
        for s in 1001..=1000 + REPLAY_WINDOW {
            assert!(w.check(s), "sequence {s} should be fresh");
            w.commit(s);
            assert!(!w.check(s), "sequence {s} should now be a duplicate");
        }
        assert!(
            !w.check(1001),
            "the oldest in-window sequence is still known"
        );
        assert!(!w.check(1000), "a sequence at the floor is refused");
        assert!(!w.check(500), "a sequence below the floor is refused");

        let mut j = ReplayWindow::new();
        j.commit(10);
        j.commit(10 + REPLAY_WINDOW + 5);
        assert!(
            !j.check(10),
            "a jump past the window forgets the old bitmap"
        );
        assert!(
            j.check(10 + REPLAY_WINDOW),
            "a fresh mid sequence is accepted"
        );
    }

    /// An unauthenticated packet must not move the window -- otherwise one forged datagram at a
    /// chosen sequence denies the real one.
    #[test]
    fn a_forged_packet_does_not_move_the_window() {
        let enc = [7u8; KEY_LEN];
        let mac = [9u8; KEY_LEN];
        let conn = [1u8, 2, 3, 4, 5, 6, 7, 8];
        let body = vec![CH_STATE, 0, 0];
        let pkt = packet_encode(PKT_DATA, &conn, 500, &enc, &mac, &body).expect("encode");
        let mut forged = pkt.clone();
        forged[HDR_SIZE + 1] ^= 0x01;
        let mut w = ReplayWindow::new();
        assert_eq!(
            packet_decode(&forged, Some(&conn), &enc, &mac, Some(&mut w)).unwrap_err(),
            Verdict::BadMac
        );
        assert!(w.check(500), "the forged packet moved the window");
        assert!(packet_decode(&pkt, Some(&conn), &enc, &mac, Some(&mut w)).is_ok());
    }

    /// The size ceiling from both sides, since a relay enforces it before it forwards.
    #[test]
    fn the_datagram_ceiling_is_1200() {
        let enc = [3u8; KEY_LEN];
        let mac = [4u8; KEY_LEN];
        let conn = [0u8; CONN_ID_BYTES];
        let big = vec![0x5au8; MAX_BODY];
        let pkt = packet_encode(PKT_DATA, &conn, 1, &enc, &mac, &big).expect("encode");
        assert_eq!(pkt.len(), MAX_DATAGRAM);
        assert!(packet_decode(&pkt, Some(&conn), &enc, &mac, None).is_ok());
        assert_eq!(
            packet_encode(PKT_DATA, &conn, 1, &enc, &mac, &vec![0u8; MAX_BODY + 1]).unwrap_err(),
            Verdict::BodyTooLong
        );
        let mut over = pkt.clone();
        over.push(0);
        assert_eq!(
            packet_decode(&over, Some(&conn), &enc, &mac, None).unwrap_err(),
            Verdict::TooLong
        );
    }

    /// A relay's whole view: routing fields without a key.
    #[test]
    fn a_relay_reads_the_header_without_a_key() {
        let enc = [5u8; KEY_LEN];
        let mac = [6u8; KEY_LEN];
        let conn = [9u8, 8, 7, 6, 5, 4, 3, 2];
        let pkt = packet_encode(PKT_KEEPALIVE, &conn, 4242, &enc, &mac, &[]).expect("encode");
        let h = Header::peek(&pkt).expect("peek");
        assert_eq!(h.seq, 4242);
        assert_eq!(h.ty, PKT_KEEPALIVE);
        assert_eq!(h.conn_id, conn);
        let mut foreign = pkt;
        foreign[0] ^= 0x10;
        assert!(
            Header::peek(&foreign).is_none(),
            "a foreign magic is refused"
        );
    }

    /// Channel C over a whole chunk: the per-chunk SHA-256 must be the hash of what reassembles.
    #[test]
    fn bulk_pieces_reassemble_under_their_own_hash() {
        let chunk: Vec<u8> = (0..2600u32)
            .map(|i| ((i * 31).wrapping_add(i >> 5) & 0xff) as u8)
            .collect();
        let sha = sha256(&chunk);
        let total = piece_count(chunk.len() as u32);
        assert_eq!(total, 3);
        let mut rebuilt = Vec::new();
        for i in 0..total {
            let start = i as usize * PIECE_MAX;
            let end = (start + PIECE_MAX).min(chunk.len());
            let p = Piece {
                chunk_id: 0x0b,
                index: i,
                total,
                chunk_len: chunk.len() as u32,
                chunk_sha: sha,
                piece_seq: 100 + i as u32,
                bytes: chunk[start..end].to_vec(),
            };
            let wire = piece_encode(&p).expect("encode");
            let q = piece_decode(&wire).expect("decode");
            assert_eq!(q.chunk_sha, sha);
            rebuilt.extend_from_slice(&q.bytes);
        }
        assert_eq!(rebuilt, chunk);
        assert_eq!(sha256(&rebuilt), sha);
        assert_eq!(piece_count(CHUNK_MAX + 1), 0);
        assert_eq!(piece_count(0), 0);
    }

    /// Latest-wins is a property of the walk, not of a comment.
    #[test]
    fn channel_b_keeps_only_the_newest_per_id() {
        let mut payload = vec![REC_PING, 8];
        payload.extend_from_slice(&1u32.to_le_bytes());
        payload.extend_from_slice(&2u32.to_le_bytes());
        payload.extend_from_slice(&[REC_PING, 8]);
        payload.extend_from_slice(&9u32.to_le_bytes());
        payload.extend_from_slice(&8u32.to_le_bytes());
        let mut slots = LatestSlots::default();
        records_absorb(&payload, &mut slots).expect("absorb");
        assert_eq!(
            slots.ping(REC_PING).unwrap(),
            PingRecord {
                t_origin_ms: 9,
                t_echo_ms: 8
            }
        );
        // An unknown id is skipped and the record after it still arrives.
        let mut p2 = vec![200u8, 3, 7, 7, 7, REC_PONG, 8];
        p2.extend_from_slice(&5u32.to_le_bytes());
        p2.extend_from_slice(&6u32.to_le_bytes());
        let mut s2 = LatestSlots::default();
        records_absorb(&p2, &mut s2).expect("absorb past an unknown id");
        assert!(s2.ping(REC_PONG).is_some());
        assert_eq!(
            records_absorb(&p2[..p2.len() - 1], &mut s2).unwrap_err(),
            Verdict::Malformed,
            "truncation is refused, not treated as the end"
        );
    }

    /// Every redundancy K, since K is configurable 1..8 and a format that round-trips only its
    /// default has seven untested branches.
    #[test]
    fn channel_a_round_trips_every_k() {
        for k in INPUT_K_MIN..=INPUT_K_MAX {
            let entries: Vec<InputEntry> = (0..k)
                .map(|i| InputEntry {
                    step: 5000 - i as u32,
                    bytes: vec![0x40 + i, 1, 2, 3],
                })
                .collect();
            let wire = input_encode(&entries).expect("encode");
            let back = input_decode(&wire).expect("decode");
            assert_eq!(back.len(), k as usize);
            for (i, e) in back.iter().enumerate() {
                assert_eq!(e.step, 5000 - i as u32);
                assert_eq!(e.bytes, entries[i].bytes);
            }
            let mut trailing = wire.clone();
            trailing.push(0);
            assert!(
                input_decode(&trailing).is_err(),
                "a trailing byte is refused"
            );
            assert!(
                input_decode(&wire[..wire.len() - 1]).is_err(),
                "truncation is refused"
            );
        }
        let mut zero = vec![0u8];
        zero.extend_from_slice(&0u32.to_le_bytes());
        assert!(input_decode(&zero).is_err(), "K=0 is refused");
        let mut nine = vec![INPUT_K_MAX + 1];
        nine.extend_from_slice(&0u32.to_le_bytes());
        assert!(input_decode(&nine).is_err(), "K=9 is refused");
    }
}
