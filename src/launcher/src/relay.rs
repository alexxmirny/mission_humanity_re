//! `mh_launcher` dist **LA6**: provisioning the relay the signed manifest names.
//!
//! A player today would reach the relay by hand-editing `mh_net.ini` (`[net] transport=udp` and
//! `[net] relay=host:port`) and pasting the deployment key into `mh_key.txt` -- and the relay's
//! address may never live in this tree (`tools/lint_machine_paths.py`), so no shipped ini can carry
//! it. The manifest can: `tools/gen_update_manifest.py` puts `relay: {addr, key}` INSIDE the signed
//! body from the release workflow's repository settings, and this module writes exactly those three
//! values into the game directory -- on install, on update, and again before every launch.
//!
//! ```text
//!   manifest.relay                 (signed; None when the field is absent)
//!        |
//!        +-- mh_net.ini   [net] transport=udp      \  edited IN PLACE: only these two keys change,
//!        |                [net] relay=<addr>       /  every other byte of the file is preserved
//!        |
//!        +-- mh_key.txt   <key>                       written only when its first line differs
//! ```
//!
//! ---- THREE RULES, each of which is a done_when clause ----
//!
//! 1. **No field, no write.** A manifest without `relay` touches neither file -- an existing ini
//!    stays BYTE-IDENTICAL. This is `provision(dir, None)`, which does nothing at all; a launcher
//!    that "helpfully" normalised the ini on the way past would be editing a player's configuration
//!    on the strength of nothing.
//! 2. **Only the two keys.** The edit is line-wise over the raw bytes: the first `[net]` section is
//!    found, the first `transport=` and `relay=` lines inside it are replaced, and if either is
//!    missing it is inserted right after the section header. Comments, ordering, blank lines, line
//!    endings, the encoding of every other line, a second `[net]` block (dead text to the game --
//!    `GetPrivateProfileString` reads the first one) -- all untouched. `[net]` absent -> the block
//!    is appended; file absent -> a minimal file is created (the DLL arms with no ini at all and a
//!    section it does not find takes its defaults, so a two-key file IS the example ini's semantics).
//! 3. **Nothing the signature did not cover.** `addr` and `key` are written verbatim from the
//!    parsed manifest, and `Relay::validate` refuses -- at PARSE time, so the manifest is refused
//!    as MALFORMED rather than applied half-way -- an address with a `;` (the ini comment char), a
//!    space, or a bad port, and a key that is not 64 hex digits or the word `open`. The game fails
//!    closed on an unparseable key file; this fails closed one step earlier.
//!
//! The values reach here from `update::load_accepted`: the last manifest the launcher ACCEPTED is
//! kept on disk with its signature and re-verified before its relay is read, so a launch a week
//! after the update still writes only what the release key signed for.

use std::path::{Path, PathBuf};

use serde::Deserialize;

use crate::log;

/// The game's configuration file. Read by `mh.dll` from beside the exe (tools/make_lane.py).
pub const INI_NAME: &str = "mh_net.ini";
/// The pre-shared key file, beside the exe (`src/mh_dll/mh_common/include/mh_net_key.h`).
pub const KEY_NAME: &str = "mh_key.txt";
/// `MH_KEY_HEX_LEN` in `mh_net_key.h`.
pub const KEY_HEX_LEN: usize = 64;

const SECTION: &[u8] = b"[net]";
const TRANSPORT_KEY: &[u8] = b"transport";
const RELAY_KEY: &[u8] = b"relay";
const TRANSPORT_VALUE: &[u8] = b"udp";

/// The `relay` object of a schema-1 manifest.
#[derive(Clone, Debug, Deserialize, PartialEq, Eq)]
pub struct Relay {
    /// `host:port`, written verbatim as `[net] relay=`.
    pub addr: String,
    /// 64 hex digits (or `open`), written as the first line of `mh_key.txt`.
    pub key: String,
}

impl Relay {
    /// The parse-time gate. The rules mirror `tools/gen_update_manifest.py`'s `check_relay_addr`
    /// / `check_relay_key`, so a manifest the signer produced always passes and a hand-made one is
    /// held to the same shape.
    pub fn validate(&self) -> Result<(), String> {
        let addr = self.addr.trim();
        if addr.is_empty() {
            return Err("relay.addr is empty".to_string());
        }
        if addr != self.addr {
            return Err(format!(
                "relay.addr {:?} has surrounding whitespace",
                self.addr
            ));
        }
        if addr
            .chars()
            .any(|c| c.is_whitespace() || c == ';' || c == '#' || c == '=')
        {
            return Err(format!(
                "relay.addr {addr:?} contains whitespace, ';', '#' or '=' -- it would not survive \
                 being written into {INI_NAME} as relay={addr}"
            ));
        }
        let (host, port) = addr
            .rsplit_once(':')
            .ok_or_else(|| format!("relay.addr {addr:?} is not host:port"))?;
        if host.is_empty() || host.starts_with('[') != host.ends_with(']') {
            return Err(format!("relay.addr {addr:?} has no usable host"));
        }
        match port.parse::<u32>() {
            Ok(p) if (1..=65535).contains(&p) => {}
            _ => return Err(format!("relay.addr {addr:?} has no port in 1..65535")),
        }
        let key = self.key.as_str();
        if key.eq_ignore_ascii_case("open") {
            return Ok(());
        }
        if key.len() != KEY_HEX_LEN || !key.bytes().all(|b| b.is_ascii_hexdigit()) {
            return Err(format!(
                "relay.key is {} character(s); it must be {KEY_HEX_LEN} hex digits or the word open",
                key.len()
            ));
        }
        Ok(())
    }

    /// The key as the file will hold it: lower-case hex (or `open`). One spelling, so "differs"
    /// below is a comparison and not a guess.
    pub fn key_line(&self) -> String {
        if self.key.eq_ignore_ascii_case("open") {
            "open".to_string()
        } else {
            self.key.to_ascii_lowercase()
        }
    }
}

/// What happened to one of the two files.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Change {
    /// No relay in the manifest: the file was not read, let alone written.
    Untouched,
    /// Already said exactly this; not written.
    Unchanged,
    /// Edited in place (the ini) or overwritten (the key file).
    Written,
    /// Did not exist and was created.
    Created,
}

impl Change {
    fn word(self) -> &'static str {
        match self {
            Change::Untouched => "untouched",
            Change::Unchanged => "already set",
            Change::Written => "updated",
            Change::Created => "created",
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Provisioned {
    pub ini: Change,
    pub key: Change,
}

impl Provisioned {
    pub const NONE: Provisioned = Provisioned {
        ini: Change::Untouched,
        key: Change::Untouched,
    };

    pub fn summary(&self) -> String {
        if *self == Provisioned::NONE {
            return "no relay in the manifest -- the game's configuration was not touched"
                .to_string();
        }
        format!(
            "relay provisioned: {INI_NAME} {} ([net] transport=udp, relay=...), {KEY_NAME} {}",
            self.ini.word(),
            self.key.word()
        )
    }
}

/// Write the relay into a game directory, or -- with `None` -- do nothing.
///
/// THE ADDRESS IS NOT LOGGED. `launcher.log`'s tail goes into every report (`report.rs`), and the
/// relay's host is a value dist LA6 says must never land in one; the ini's own `relay=` line is
/// blanked there for the same reason. The log says WHAT was done, and the ini says what it was done
/// with.
pub fn provision(game_dir: &Path, relay: Option<&Relay>) -> Result<Provisioned, String> {
    let Some(relay) = relay else {
        return Ok(Provisioned::NONE);
    };
    relay.validate()?;
    let ini = provision_ini_file(&game_dir.join(INI_NAME), &relay.addr)?;
    let key = provision_key_file(&game_dir.join(KEY_NAME), &relay.key_line())?;
    let done = Provisioned { ini, key };
    log::line(format!("relay: {}", done.summary()));
    Ok(done)
}

/// The ini half, on one file. Reads the bytes, edits them, and writes back ONLY when the result
/// differs -- so a file that already says the right thing keeps its modification time too.
pub fn provision_ini_file(path: &Path, addr: &str) -> Result<Change, String> {
    let existing = match std::fs::read(path) {
        Ok(b) => Some(b),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => None,
        Err(e) => return Err(format!("cannot read {}: {e}", path.display())),
    };
    let edited = provision_ini_bytes(existing.as_deref(), addr);
    if existing.as_deref() == Some(edited.as_slice()) {
        return Ok(Change::Unchanged);
    }
    write_atomically(path, &edited)?;
    Ok(if existing.is_some() {
        Change::Written
    } else {
        Change::Created
    })
}

/// The key half. "Different" is judged the way the game judges the file (`net_key.cpp`
/// `first_line`): the first non-comment line, BOM and surrounding whitespace ignored, compared
/// case-insensitively -- so a key file the GAME wrote (key + its explanatory comment block) with
/// the same key is left exactly as it is.
pub fn provision_key_file(path: &Path, key_line: &str) -> Result<Change, String> {
    let existing = match std::fs::read(path) {
        Ok(b) => Some(b),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => None,
        Err(e) => return Err(format!("cannot read {}: {e}", path.display())),
    };
    if let Some(bytes) = existing.as_deref() {
        if first_real_line(bytes).is_some_and(|l| l.eq_ignore_ascii_case(key_line.as_bytes())) {
            return Ok(Change::Unchanged);
        }
    }
    let body = format!(
        "{key_line}\r\n\
         ; ^ This line is your multiplayer key. It was written by the launcher from the signed \
         update manifest (dist LA6): it is the deployment key of the relay named in {INI_NAME} \
         under [net] relay=, and every player on that relay holds the same one. The launcher \
         rewrites it only when the manifest's key changes.\r\n"
    );
    write_atomically(path, body.as_bytes())?;
    Ok(if existing.is_some() {
        Change::Written
    } else {
        Change::Created
    })
}

/// A rename over the target, so a launcher killed mid-write leaves the old file whole rather than
/// a truncated one the game would refuse (the key) or half-read (the ini).
fn write_atomically(path: &Path, bytes: &[u8]) -> Result<(), String> {
    let tmp: PathBuf = path.with_extension("mhtmp");
    std::fs::write(&tmp, bytes).map_err(|e| format!("cannot write {}: {e}", tmp.display()))?;
    // Windows' `rename` refuses to replace; `fs::rename` on Windows maps to MoveFileEx with
    // REPLACE_EXISTING, which is what makes this a swap rather than an error.
    std::fs::rename(&tmp, path).map_err(|e| {
        let _ = std::fs::remove_file(&tmp);
        format!("cannot replace {}: {e}", path.display())
    })
}

/// `net_key.cpp`'s `first_line`, over bytes: skip a UTF-8 BOM, then the first line that is not
/// blank and does not start with `;`, trimmed of spaces and tabs.
fn first_real_line(bytes: &[u8]) -> Option<&[u8]> {
    let body = bytes.strip_prefix(b"\xEF\xBB\xBF").unwrap_or(bytes);
    for line in body.split(|b| *b == b'\n') {
        let line = trim_ascii(line);
        if line.is_empty() || line[0] == b';' {
            continue;
        }
        return Some(line);
    }
    None
}

fn trim_ascii(b: &[u8]) -> &[u8] {
    let is_ws = |c: &u8| matches!(c, b' ' | b'\t' | b'\r' | b'\n');
    let start = b.iter().position(|c| !is_ws(c)).unwrap_or(b.len());
    let end = b.iter().rposition(|c| !is_ws(c)).map_or(start, |i| i + 1);
    &b[start..end]
}

/// The pure edit: `existing` (or nothing) -> the ini with `[net] transport=udp` and
/// `[net] relay=<addr>` in it, and NOTHING else different.
///
/// Bytes rather than `&str` on purpose: a player's ini may hold a Latin-1 comment or a stray
/// control byte, and a `from_utf8_lossy` round trip would rewrite those bytes on lines this
/// function has no business touching. The keys and section name are ASCII, so an ASCII compare is
/// the whole of the parsing needed.
///
/// Where the two lines go when they are missing: DIRECTLY AFTER the `[net]` header. Inserting at
/// the section's end would put them after whatever trailing blank lines and comments visually
/// belong to the next section; after the header they are unambiguously inside `[net]`, which is the
/// only thing `GetPrivateProfileString` cares about.
pub fn provision_ini_bytes(existing: Option<&[u8]>, addr: &str) -> Vec<u8> {
    let transport_line = {
        let mut v = TRANSPORT_KEY.to_vec();
        v.push(b'=');
        v.extend_from_slice(TRANSPORT_VALUE);
        v
    };
    let relay_line = {
        let mut v = RELAY_KEY.to_vec();
        v.push(b'=');
        v.extend_from_slice(addr.as_bytes());
        v
    };

    let Some(existing) = existing.filter(|b| !b.is_empty()) else {
        // A fresh file. CRLF because that is what the game's own writers use (net_key.cpp,
        // tools/make_lane.py) and what Notepad expects; the DLL reads either.
        let mut out = Vec::new();
        out.extend_from_slice(
            b"; mh_net.ini -- written by mh_launcher (dist LA6) because no configuration file was \
              here.\r\n\
              ; Every key not set here takes the DLL's default; the full reference is the \
              mh_net.ini that\r\n\
              ; ships in the release zip (src/mh_dll/mh_net.example.ini). The launcher rewrites \
              only the two\r\n\
              ; [net] lines below, from its signed update manifest, and leaves everything else \
              alone.\r\n\
              \r\n",
        );
        out.extend_from_slice(SECTION);
        out.extend_from_slice(b"\r\n");
        out.extend_from_slice(&transport_line);
        out.extend_from_slice(b"\r\n");
        out.extend_from_slice(&relay_line);
        out.extend_from_slice(b"\r\n");
        return out;
    };

    // The line ending to use for lines this function ADDS: whatever the file's first line uses.
    let eol: &[u8] = match existing.iter().position(|b| *b == b'\n') {
        Some(i) if i > 0 && existing[i - 1] == b'\r' => b"\r\n",
        Some(_) => b"\n",
        None => b"\r\n",
    };

    let mut out = Vec::with_capacity(existing.len() + 64);
    let mut in_net = false;
    let mut net_seen = false;
    let mut have_transport = false;
    let mut have_relay = false;
    let mut header_end: Option<usize> = None; // where in `out` the [net] header line ends

    for raw in existing.split_inclusive(|b| *b == b'\n') {
        let (line, term) = split_terminator(raw);
        let t = trim_ascii(line);
        if t.first() == Some(&b'[') {
            // A section header: the first [net] opens the window, any other header closes it.
            if in_net {
                in_net = false;
            }
            if !net_seen && t.eq_ignore_ascii_case(SECTION) {
                in_net = true;
                net_seen = true;
                out.extend_from_slice(raw);
                header_end = Some(out.len());
                continue;
            }
        } else if in_net && !t.is_empty() && t[0] != b';' && t[0] != b'#' {
            if let Some(eq) = t.iter().position(|b| *b == b'=') {
                let key = trim_ascii(&t[..eq]);
                // FIRST occurrence only, like the reader; a later duplicate is dead text and
                // rewriting it would be changing a line the game never reads.
                if !have_transport && key.eq_ignore_ascii_case(TRANSPORT_KEY) {
                    have_transport = true;
                    out.extend_from_slice(&transport_line);
                    out.extend_from_slice(term);
                    continue;
                }
                if !have_relay && key.eq_ignore_ascii_case(RELAY_KEY) {
                    have_relay = true;
                    out.extend_from_slice(&relay_line);
                    out.extend_from_slice(term);
                    continue;
                }
            }
        }
        out.extend_from_slice(raw);
    }

    match header_end {
        Some(at) => {
            let mut insert = Vec::new();
            if !have_transport {
                insert.extend_from_slice(&transport_line);
                insert.extend_from_slice(eol);
            }
            if !have_relay {
                insert.extend_from_slice(&relay_line);
                insert.extend_from_slice(eol);
            }
            if !insert.is_empty() {
                out.splice(at..at, insert);
            }
        }
        None => {
            // No [net] section anywhere: append one. A separating blank line, and a terminator on
            // the last existing line if it had none.
            if !out.ends_with(b"\n") {
                out.extend_from_slice(eol);
            }
            out.extend_from_slice(eol);
            out.extend_from_slice(SECTION);
            out.extend_from_slice(eol);
            out.extend_from_slice(&transport_line);
            out.extend_from_slice(eol);
            out.extend_from_slice(&relay_line);
            out.extend_from_slice(eol);
        }
    }
    out
}

/// `"key=value\r\n"` -> (`"key=value"`, `"\r\n"`); a last line without a newline keeps an empty
/// terminator, so the edit never adds one the file did not have.
fn split_terminator(raw: &[u8]) -> (&[u8], &[u8]) {
    if raw.ends_with(b"\r\n") {
        raw.split_at(raw.len() - 2)
    } else if raw.ends_with(b"\n") {
        raw.split_at(raw.len() - 1)
    } else {
        (raw, &[])
    }
}

/// The `relay=` value an ini currently carries, for `report.rs`'s scrub. `None` when there is no
/// such (uncommented) line in the first `[net]` section.
pub fn relay_addr_in_ini(text: &str) -> Option<String> {
    let mut in_net = false;
    let mut net_seen = false;
    for line in text.lines() {
        let t = line.trim();
        if t.starts_with('[') {
            in_net = !net_seen && t.eq_ignore_ascii_case("[net]");
            if in_net {
                net_seen = true;
            }
            continue;
        }
        if !in_net || t.is_empty() || t.starts_with(';') || t.starts_with('#') {
            continue;
        }
        if let Some((k, v)) = t.split_once('=') {
            if k.trim().eq_ignore_ascii_case("relay") {
                // The R7a reader trims a trailing `; comment`; so does this.
                let v = v.split(';').next().unwrap_or("").trim();
                return if v.is_empty() {
                    None
                } else {
                    Some(v.to_string())
                };
            }
        }
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    const ADDR: &str = "192.0.2.10:7100";
    const KEY: &str = "4d487465737474656b65794d487465737474656b65794d487465737474656b65";

    fn relay() -> Relay {
        Relay {
            addr: ADDR.to_string(),
            key: KEY.to_uppercase(),
        }
    }

    fn scratch(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("mh_launcher_test_relay_{name}"));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    /// The example ini's real shape: comments, a same-line comment on another key, the real
    /// `transport=tcp` line, and the commented `; relay=HOST:PORT` line that must NOT be taken
    /// for the real one.
    const SHIPPED: &str = "; header comment\r\n\
        [config]\r\n\
        mode=brokered\r\n\
        \r\n\
        [net]\r\n\
        ; two axes\r\n\
        module=auto\r\n\
        enable=1                  ; 0 = skip\r\n\
        transport=tcp\r\n\
        ; udp_redundancy=3        ; UDP only\r\n\
        ; relay=HOST:PORT         ; UDP only: the RELAY\r\n\
        ;     A host with this set registers\r\n\
        \r\n\
        [capture]\r\n\
        enable=0\r\n";

    #[test]
    fn a_shipped_ini_gains_exactly_the_two_keys() {
        let out = provision_ini_bytes(Some(SHIPPED.as_bytes()), ADDR);
        let expected = SHIPPED
            .replace("transport=tcp\r\n", "transport=udp\r\n")
            .replace("[net]\r\n", &format!("[net]\r\nrelay={ADDR}\r\n"));
        assert_eq!(String::from_utf8(out).unwrap(), expected);
    }

    #[test]
    fn an_ini_that_already_says_it_is_left_byte_identical() {
        let once = provision_ini_bytes(Some(SHIPPED.as_bytes()), ADDR);
        let twice = provision_ini_bytes(Some(&once), ADDR);
        assert_eq!(once, twice);
    }

    #[test]
    fn a_changed_relay_replaces_only_its_own_line() {
        let once = provision_ini_bytes(Some(SHIPPED.as_bytes()), ADDR);
        let moved = provision_ini_bytes(Some(&once), "198.51.100.7:7200");
        let a = String::from_utf8(once).unwrap();
        let b = String::from_utf8(moved).unwrap();
        assert_eq!(
            a.replace(&format!("relay={ADDR}"), "relay=198.51.100.7:7200"),
            b
        );
    }

    /// Only the FIRST `[net]` block and the FIRST occurrence of each key inside it -- the same
    /// rule `GetPrivateProfileString` reads by. LF endings and a trailing same-line comment on the
    /// key being replaced, so both survive the round trip in the shape the rule predicts.
    #[test]
    fn duplicates_and_a_second_net_block_are_dead_text_and_stay_dead() {
        let src = "[net]\ntransport=tcp ; old\ntransport=tcp\n[net]\ntransport=tcp\n";
        let out = String::from_utf8(provision_ini_bytes(Some(src.as_bytes()), ADDR)).unwrap();
        assert_eq!(
            out,
            format!("[net]\nrelay={ADDR}\ntransport=udp\ntransport=tcp\n[net]\ntransport=tcp\n")
        );
    }

    #[test]
    fn keys_are_matched_case_insensitively_and_with_padding() {
        let src = "[NET]\r\n  Transport = tcp\r\n  Relay = old:1\r\n";
        let out = String::from_utf8(provision_ini_bytes(Some(src.as_bytes()), ADDR)).unwrap();
        assert_eq!(out, format!("[NET]\r\ntransport=udp\r\nrelay={ADDR}\r\n"));
    }

    #[test]
    fn no_net_section_appends_one_in_the_files_own_line_ending() {
        let src = "[config]\nmode=brokered";
        let out = String::from_utf8(provision_ini_bytes(Some(src.as_bytes()), ADDR)).unwrap();
        assert_eq!(
            out,
            format!("[config]\nmode=brokered\n\n[net]\ntransport=udp\nrelay={ADDR}\n")
        );
    }

    #[test]
    fn no_file_creates_a_minimal_one() {
        let out = String::from_utf8(provision_ini_bytes(None, ADDR)).unwrap();
        assert!(out.contains("\r\n[net]\r\ntransport=udp\r\nrelay=192.0.2.10:7100\r\n"));
        assert!(out.starts_with("; mh_net.ini"));
        assert_eq!(relay_addr_in_ini(&out).as_deref(), Some(ADDR));
        // And the empty file is the same case as no file.
        assert_eq!(provision_ini_bytes(Some(b""), ADDR), out.as_bytes());
    }

    /// The whole point of working on bytes: a non-UTF-8 comment passes through untouched.
    #[test]
    fn non_utf8_bytes_on_other_lines_survive() {
        let mut src = b"; caf\xE9 \x01\r\n[net]\r\ntransport=tcp\r\n".to_vec();
        src.extend_from_slice(b"; \xFF\xFE end\r\n");
        let out = provision_ini_bytes(Some(&src), ADDR);
        assert!(out.starts_with(b"; caf\xE9 \x01\r\n[net]\r\nrelay="));
        assert!(out.ends_with(b"transport=udp\r\n; \xFF\xFE end\r\n"));
    }

    #[test]
    fn the_relay_line_is_read_back_the_way_the_game_reads_it() {
        assert_eq!(
            relay_addr_in_ini("[net]\nrelay=192.0.2.10:7100 ; trimmed\n").as_deref(),
            Some("192.0.2.10:7100")
        );
        assert_eq!(relay_addr_in_ini("[net]\n; relay=HOST:PORT\n"), None);
        assert_eq!(relay_addr_in_ini("[other]\nrelay=x:1\n"), None);
        assert_eq!(relay_addr_in_ini("[net]\nrelay=\n"), None);
    }

    #[test]
    fn the_manifest_shape_is_validated_before_anything_is_written() {
        assert!(relay().validate().is_ok());
        let ok = |addr: &str, key: &str| {
            Relay {
                addr: addr.into(),
                key: key.into(),
            }
            .validate()
        };
        assert!(ok("[2001:db8::1]:7100", KEY).is_ok());
        assert!(ok("relay.example:7100", "OPEN").is_ok());
        for bad in [
            "192.0.2.10",
            "192.0.2.10:0",
            "192.0.2.10:70000",
            "192.0.2.10:7100 ; x",
            "a b:1",
            " 192.0.2.10:7100",
            ":7100",
            "",
            "[2001:db8::1:7100",
        ] {
            assert!(ok(bad, KEY).is_err(), "{bad:?}");
        }
        for bad in ["", &KEY[..63], &format!("{KEY}0"), &"zz".repeat(32)] {
            assert!(ok(ADDR, bad).is_err(), "{bad:?}");
        }
        assert_eq!(relay().key_line(), KEY, "hex is normalised to lower case");
    }

    /// done_when (a): a manifest with a relay -> the ini gains exactly the two keys and the key
    /// file is written.
    #[test]
    fn a_manifest_with_a_relay_provisions_both_files() {
        let dir = scratch("with");
        std::fs::write(dir.join(INI_NAME), SHIPPED).unwrap();
        let r = relay();
        let done = provision(&dir, Some(&r)).unwrap();
        assert_eq!(
            done,
            Provisioned {
                ini: Change::Written,
                key: Change::Created
            }
        );
        let ini = std::fs::read_to_string(dir.join(INI_NAME)).unwrap();
        assert_eq!(
            ini,
            SHIPPED
                .replace("transport=tcp\r\n", "transport=udp\r\n")
                .replace("[net]\r\n", &format!("[net]\r\nrelay={ADDR}\r\n"))
        );
        let key = std::fs::read(dir.join(KEY_NAME)).unwrap();
        assert_eq!(first_real_line(&key), Some(KEY.as_bytes()));
        assert!(!dir.join("mh_net.mhtmp").exists(), "the temp file is gone");

        // Provisioning again -- what every launch does -- changes nothing.
        let again = provision(&dir, Some(&r)).unwrap();
        assert_eq!(
            again,
            Provisioned {
                ini: Change::Unchanged,
                key: Change::Unchanged
            }
        );
        assert_eq!(std::fs::read_to_string(dir.join(INI_NAME)).unwrap(), ini);
        assert_eq!(std::fs::read(dir.join(KEY_NAME)).unwrap(), key);
        let _ = std::fs::remove_dir_all(&dir);
    }

    /// done_when (b): a manifest WITHOUT the field leaves an existing ini byte-identical and
    /// writes no key file.
    #[test]
    fn a_manifest_without_a_relay_touches_nothing() {
        let dir = scratch("without");
        let odd = b"[net]\r\ntransport=tcp\r\n; caf\xE9\r\n".to_vec();
        std::fs::write(dir.join(INI_NAME), &odd).unwrap();
        let before = std::fs::metadata(dir.join(INI_NAME))
            .unwrap()
            .modified()
            .unwrap();
        assert_eq!(provision(&dir, None).unwrap(), Provisioned::NONE);
        assert_eq!(std::fs::read(dir.join(INI_NAME)).unwrap(), odd);
        assert_eq!(
            std::fs::metadata(dir.join(INI_NAME))
                .unwrap()
                .modified()
                .unwrap(),
            before,
            "not even rewritten with the same bytes"
        );
        assert!(!dir.join(KEY_NAME).exists());
        assert!(Provisioned::NONE.summary().contains("not touched"));
        let _ = std::fs::remove_dir_all(&dir);
    }

    /// The key file the GAME wrote -- key on line 1, then its comment block -- is recognised as
    /// already holding the key and left alone; a different key (or `open`) is replaced.
    #[test]
    fn a_game_written_key_file_with_the_same_key_is_kept() {
        let dir = scratch("keyfile");
        let path = dir.join(KEY_NAME);
        let game_wrote = format!(
            "\u{feff}{}\r\n; ^ This line is your multiplayer key. Everyone playing together...\r\n",
            KEY.to_uppercase()
        );
        std::fs::write(&path, &game_wrote).unwrap();
        assert_eq!(provision_key_file(&path, KEY).unwrap(), Change::Unchanged);
        assert_eq!(std::fs::read_to_string(&path).unwrap(), game_wrote);

        std::fs::write(&path, "open\r\n").unwrap();
        assert_eq!(provision_key_file(&path, KEY).unwrap(), Change::Written);
        let now = std::fs::read(&path).unwrap();
        assert_eq!(first_real_line(&now), Some(KEY.as_bytes()));
        assert!(std::str::from_utf8(&now).unwrap().contains("dist LA6"));

        std::fs::remove_file(&path).unwrap();
        assert_eq!(provision_key_file(&path, "open").unwrap(), Change::Created);
        assert_eq!(
            first_real_line(&std::fs::read(&path).unwrap()),
            Some(&b"open"[..])
        );
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn a_bad_relay_is_refused_before_any_file_is_touched() {
        let dir = scratch("refused");
        std::fs::write(dir.join(INI_NAME), SHIPPED).unwrap();
        let bad = Relay {
            addr: "192.0.2.10:7100 ; x".into(),
            key: KEY.into(),
        };
        assert!(provision(&dir, Some(&bad)).is_err());
        assert_eq!(
            std::fs::read_to_string(dir.join(INI_NAME)).unwrap(),
            SHIPPED
        );
        assert!(!dir.join(KEY_NAME).exists());
        let _ = std::fs::remove_dir_all(&dir);
    }
}
