//! Building the report zip (dist LA4, plan decision D13).
//!
//! A report is one deflate zip and one required sentence. What goes in it:
//!
//! ```text
//! report.json            everything a triager needs before opening anything else
//! description.txt        what the player typed. REQUIRED, and the build refuses without it
//! session/...            the per-session log directory (mp SES1) -- mh_net.log, session.json, ...
//! config/mh_net.ini      the game's configuration, REDACTED
//! launcher/launcher.log  the tail of this program's own log
//! crash/<name>.marker    what mh.dll's handler wrote, when there was a crash
//! minidump.dmp           only when the player ticked the box and a dump was written
//! ```
//!
//! **`report.json` IS the `meta` object RP1 will POST**, byte for byte, not a cousin of it. The
//! collector stores that object as `meta.json` beside the zip (`src/collector/README.md`), and
//! `tools/crash_report.py --report` reads `match_id`, `version`, `exit_code` and the optional
//! `crash` object out of it. Writing one object and sending the same one is what keeps the drained
//! report and the zip's own copy from ever disagreeing -- and it means the uploader in RP1 has no
//! field names of its own to get wrong.
//!
//! ## The description is required, and it is required HERE
//!
//! The Report view disables its button on an empty box, but the refusal lives in `build()` because
//! the view is not the only caller: `--report` builds one from a script. A rule enforced only by a
//! greyed-out button is a rule that holds until the first automated path, and "a report without a
//! description is not a report" is D13's wording, not a UI nicety -- a zip of logs with no account
//! of what the player was doing is a thing nobody can act on.
//!
//! ## The key must not be in the zip, and three separate things could have put it there
//!
//! `mh_key.txt` is the pre-shared secret that authenticates and encrypts the MP transport
//! (`src/mh_dll/mh_common/include/mh_net_key.h`): whoever has it can join and can read the traffic.
//! It reaches a report by three routes, and the done_when names only the first two:
//!
//!   1. **the file itself**, which sits next to `mh.exe`. Excluded by construction -- this builder
//!      names the files it adds -- and then again by `DENY`, because "by construction" is a claim
//!      about code that will be edited later.
//!   2. **a `key=` line in `mh_net.ini`**. There is no such key today (the secret is deliberately a
//!      FILE and not an ini setting, for exactly this reason -- see `mh_net_key.h`), so the
//!      redaction here is prospective: any setting whose NAME reads secret loses its value.
//!   3. **`mh_net.log`**, and this one is real today. On first run the transport mints a key and
//!      logs it in full so the host can send it to their players:
//!      `"; mh_key.txt generated -- send this file (or the line below) to the players joining you:"`
//!      followed by the 64 hex digits (`src/mh_dll/mh/seams/net_lockstep.cpp`). That line is inside
//!      the very directory a report is a zip of. Nothing in LA4's acceptance clause would have
//!      caught it.
//!
//! So the redaction is not a line filter, it is a **scrub over every text file added**: any run of
//! 64 or more hex digits becomes `<redacted-64hex>`. The threshold is 64 because the key is 64 hex
//! digits and the `match_id` is 32 -- and the match_id must SURVIVE, since `crash_report.py`
//! cross-checks `report.json`'s against the `; [session] match_id=` lines in these same logs. A
//! scrub at 32 would have destroyed the evidence that proves the report is the match it claims.
//!
//! ## The relay's address is scrubbed the same way (dist LA6)
//!
//! Since LA6 the launcher itself writes `[net] relay=host:port` into `mh_net.ini`, and the game
//! prints that host into `mh_net.log` (`udp relay leg UP -- host:port`, `relay=host:port does not
//! resolve`). Two scrubs, both keyed off the ini in the game directory at build time: the
//! `relay=` line's value is blanked in `config/mh_net.ini` like a secret's would be, and the
//! address it held (and its bare host, when that is dotted) is a LITERAL that every text file goes
//! through -- `Scrub::line` -- so the log routes are covered without a list of log formats. A
//! report built from a game directory with no `relay=` line has nothing to blank and blanks
//! nothing; a report built with no game directory at all likewise.

use std::io::Write;
use std::path::{Path, PathBuf};

use crate::crash::Marker;
use crate::install;
use crate::launch::Finished;
use crate::log;
use crate::machine::Machine;
use crate::paths;
use crate::relay;

/// Files that never enter a report, whatever directory they turn up in.
const DENY: [&str; 1] = ["mh_key.txt"];

/// Per-file cap. A single log bigger than this is tailed, not dropped: the end of a log is the part
/// next to the crash.
const PER_FILE_MAX: u64 = 8 * 1024 * 1024;

/// Whole-zip cap on UNCOMPRESSED input. Caddy rejects a body over 64 MB at the edge (plan D14) and
/// a report that is refused on arrival is worse than a report missing its least interesting file.
const TOTAL_MAX: u64 = 48 * 1024 * 1024;

/// What a report is built from. Everything is optional except the description, which is the point.
pub struct Input<'a> {
    pub game_dir: Option<&'a Path>,
    /// The session log directory to ship. `Input::describe` fills this from `newest_session_dir`
    /// when the caller has not chosen one.
    pub session_dir: Option<PathBuf>,
    pub description: &'a str,
    pub last_run: Option<&'a Finished>,
    pub crash: Option<&'a Marker>,
    /// A dump already written by `crash::write_dump`, if the player asked for one.
    pub minidump: Option<&'a Path>,
    pub launcher_log: Option<PathBuf>,
}

/// What a finished report is.
#[derive(Clone, Debug)]
pub struct Built {
    pub zip: PathBuf,
    pub entries: Vec<String>,
    pub bytes: u64,
    /// The `report.json` text, which is also the `meta` field RP1 posts.
    pub meta: String,
}

impl Built {
    pub fn summary(&self) -> String {
        format!(
            "report written to {} -- {} file(s), {} bytes",
            self.zip.display(),
            self.entries.len(),
            self.bytes
        )
    }
}

/// The one refusal, stated once so the UI and the command line give the same reason.
pub const NO_DESCRIPTION: &str =
    "a report without a description is not a report -- say what you were doing and what went wrong";

pub fn description_ok(text: &str) -> bool {
    !text.trim().is_empty()
}

/// Build the zip at `dest`.
pub fn build(dest: &Path, input: &Input) -> Result<Built, String> {
    if !description_ok(input.description) {
        return Err(NO_DESCRIPTION.to_string());
    }
    if let Some(parent) = dest.parent() {
        std::fs::create_dir_all(parent)
            .map_err(|e| format!("cannot create {}: {e}", parent.display()))?;
    }

    let machine = Machine::detect();
    let installed = input.game_dir.and_then(install::read_manifest);
    let session = input.session_dir.clone();
    let build_stamp = session
        .as_deref()
        .and_then(build_stamp_from_logs)
        .or_else(|| input.crash.map(|c| c.build.clone()))
        .unwrap_or_default();
    let match_id = session
        .as_deref()
        .and_then(match_id_from_session)
        .or_else(|| {
            input
                .crash
                .map(|c| c.match_id.clone())
                .filter(|s| !s.is_empty())
        })
        .unwrap_or_default();

    let meta = meta_json(input, &machine, installed.as_ref(), &build_stamp, &match_id);

    // dist LA6: the ini is read ONCE, here, both to be added (redacted) and to seed the literal
    // scrub every other text file goes through.
    let ini_text: Option<String> = input.game_dir.and_then(|game_dir| {
        let ini = game_dir.join(relay::INI_NAME);
        if !ini.is_file() {
            return None;
        }
        match std::fs::read_to_string(&ini) {
            Ok(t) => Some(t),
            Err(e) => {
                log::line(format!("report: cannot read {}: {e}", ini.display()));
                None
            }
        }
    });
    let scrub = Scrub::for_ini(ini_text.as_deref());

    let file = std::fs::File::create(dest)
        .map_err(|e| format!("cannot create {}: {e}", dest.display()))?;
    let mut zip = zip::ZipWriter::new(file);
    let opts: zip::write::SimpleFileOptions = zip::write::SimpleFileOptions::default()
        .compression_method(zip::CompressionMethod::Deflated);

    let mut entries: Vec<String> = Vec::new();
    let mut budget = TOTAL_MAX;

    add_text(
        &mut zip,
        opts,
        "report.json",
        &meta,
        &mut entries,
        &mut budget,
    )?;
    add_text(
        &mut zip,
        opts,
        "description.txt",
        input.description.trim(),
        &mut entries,
        &mut budget,
    )?;

    if let Some(dir) = session.as_deref() {
        add_dir(
            &mut zip,
            opts,
            dir,
            "session",
            &mut entries,
            &mut budget,
            &scrub,
        )?;
    }

    // The configuration, redacted three times over: by setting name, by the relay's own line,
    // and by the scrub.
    if let Some(text) = ini_text.as_deref() {
        add_text(
            &mut zip,
            opts,
            "config/mh_net.ini",
            &redact_ini_with(text, &scrub),
            &mut entries,
            &mut budget,
        )?;
    }

    if let Some(p) = input.launcher_log.as_deref() {
        if p.is_file() {
            add_file(
                &mut zip,
                opts,
                p,
                "launcher/launcher.log",
                &mut entries,
                &mut budget,
                &scrub,
            )?;
        }
    }

    if let Some(m) = input.crash {
        add_text(
            &mut zip,
            opts,
            "crash/marker.txt",
            &format!(
                "module={}\noffset=0x{:08x}\ncode=0x{:08x}\npid={}\ntid={}\nbuild={}\nwhen={}\n",
                m.module, m.offset, m.code, m.pid, m.tid, m.build, m.when
            ),
            &mut entries,
            &mut budget,
        )?;
    }

    if let Some(dmp) = input.minidump {
        if dmp.is_file() {
            // The dump is the one entry allowed past the PER-FILE cap, and only that one: a
            // truncated minidump is not a smaller minidump, it is a file no debugger will open. It
            // still respects the total budget, so it can be left out whole.
            add_capped(
                &mut zip,
                opts,
                dmp,
                "minidump.dmp",
                &mut entries,
                &mut budget,
                TOTAL_MAX,
                &scrub,
            )?;
        }
    }

    zip.finish()
        .map_err(|e| format!("cannot finish {}: {e}", dest.display()))?;
    let bytes = std::fs::metadata(dest).map(|m| m.len()).unwrap_or(0);
    log::line(format!(
        "report: {} -- {} entries, {bytes} bytes",
        dest.display(),
        entries.len()
    ));
    Ok(Built {
        zip: dest.to_path_buf(),
        entries,
        bytes,
        meta,
    })
}

// ---- report.json ---------------------------------------------------------------------------------

/// The object. Hand-built with `serde_json::json!` rather than a `#[derive(Serialize)]` struct
/// because the field names are a CONTRACT with two readers outside this crate
/// (`tools/crash_report.py`, `src/collector`), and a literal object is the spelling in which a
/// rename is visible in a diff instead of hidden behind a `#[serde(rename)]`.
fn meta_json(
    input: &Input,
    machine: &Machine,
    installed: Option<&install::Manifest>,
    build_stamp: &str,
    match_id: &str,
) -> String {
    // `exit_code` is the SIGNED i32 Windows hands a parent, which is what the collector's own
    // example shows (`-1073741819`). The hex spelling is the one a human searches for, so both are
    // present -- and D13's "exit code hex" is the second of them, not a replacement for the first.
    let (exit_code, exit_hex) = match input.last_run {
        Some(f) => match &f.outcome {
            crate::launch::Outcome::Normal => (Some(0i64), "0x00000000".to_string()),
            crate::launch::Outcome::Nonzero(c) | crate::launch::Outcome::Crash(c) => {
                (Some(*c as i32 as i64), format!("0x{c:08X}"))
            }
        },
        None => (None, String::new()),
    };

    let mut obj = serde_json::json!({
        "match_id": match_id,
        "version": installed.map(|m| m.version.clone()).unwrap_or_default(),
        "build": build_stamp,
        "exit_code": exit_code,
        "exit_code_hex": exit_hex,
        // The collector contract's `os`. Deliberately the DETAILED string rather than the literal
        // "windows" of its example: nothing reads this field programmatically, and "Windows 11 Pro
        // 24H2 (build 28000)" answers a question the constant does not.
        "os": machine.os,
        "cpu": machine.cpu,
        "gpu": machine.gpu,
        "ram_mb": machine.ram_mb,
        "launcher_version": crate::version::VERSION,
        "configuration": installed.map(|m| m.tag.clone()).unwrap_or_default(),
        "package": installed.map(|m| m.package.clone()).unwrap_or_default(),
        "session_dir": input
            .session_dir
            .as_deref()
            .and_then(|p| p.file_name())
            .map(|n| n.to_string_lossy().to_string())
            .unwrap_or_default(),
        "created_at": crate::log::stamp(),
    });

    // OPTIONAL, and its absence is meaningful: `print_drained_report` treats a report with no
    // `crash` key as an ordinary bug report rather than an error. The two fields are exactly the
    // ones `resolve_module_fault()` consumes, spelled the way it spells them -- `offset` as a
    // "0x..." STRING (it calls `int(off_hex, 16)`), and module-relative, which for the exe is
    // image-relative and for mh.dll is an RVA. One subtraction produces both; see
    // `mh_common/include/mh_crash_marker.h`.
    // `thread_id` and `code` are ADDITIONS to that pair, not a change to it: `crash_report.py`
    // reads the object with `.get()`, so an extra key costs it nothing. They earn their place
    // because of a measured limitation -- a 64-bit `MiniDumpWriteDump` refuses the 32-bit game's
    // `EXCEPTION_POINTERS` (ERROR_NOACCESS), so the dump this report carries has NO exception
    // stream and a debugger opening it does not know which of its ~25 threads faulted. These two
    // fields are how a triager finds out without unzipping `crash/marker.txt`.
    if let Some(m) = input.crash {
        if !m.module.is_empty() {
            obj["crash"] = serde_json::json!({
                "module": m.module,
                "offset": format!("0x{:08x}", m.offset),
                "thread_id": m.tid,
                "code": format!("0x{:08x}", m.code),
            });
        }
    }
    serde_json::to_string_pretty(&obj).unwrap_or_else(|_| "{}".to_string())
}

/// The build stamp mh.dll writes as the first line of `mh_net.log`: `; [build] mh 0.1.0-rc1+abc1234`.
fn build_stamp_from_logs(session: &Path) -> Option<String> {
    let text = std::fs::read_to_string(session.join("mh_net.log")).ok()?;
    for line in text.lines().take(200) {
        if let Some(rest) = line.split("; [build] mh ").nth(1) {
            let v = rest.trim();
            if !v.is_empty() {
                return Some(v.to_string());
            }
        }
    }
    None
}

/// The match this report is about, from the session directory's own `session.json` (SES1 writes it
/// at SESSION_BEGIN and rewrites it at SESSION_END, so it exists even for a session a crash never
/// closed -- which is the case that matters most here).
fn match_id_from_session(session: &Path) -> Option<String> {
    let text = std::fs::read_to_string(session.join("session.json")).ok()?;
    let v: serde_json::Value = serde_json::from_str(&text).ok()?;
    let id = v.get("match_id")?.as_str()?.trim().to_string();
    if id.is_empty() {
        None
    } else {
        Some(id)
    }
}

// ---- redaction -------------------------------------------------------------------------------

/// Setting names whose VALUE is removed from `mh_net.ini`, whatever section they are in. Matched
/// on the whole name and on three suffixes, so a future `relay_token` or `upload_secret` is
/// covered without this list being edited (which is the failure mode a list like this always has).
const SECRET_NAMES: [&str; 6] = ["key", "psk", "secret", "token", "password", "passphrase"];
const SECRET_SUFFIXES: [&str; 4] = ["_key", "_psk", "_secret", "_token"];
/// Setting names whose value is an ADDRESS the report must not carry (dist LA6): the relay the
/// launcher wrote. Blanked exactly like a secret; listed apart because it is not one.
const ADDRESS_NAMES: [&str; 1] = ["relay"];

fn is_secret_name(name: &str) -> bool {
    let n = name.trim().to_ascii_lowercase();
    SECRET_NAMES.contains(&n.as_str()) || SECRET_SUFFIXES.iter().any(|s| n.ends_with(s))
}

fn is_blanked_name(name: &str) -> bool {
    let n = name.trim().to_ascii_lowercase();
    is_secret_name(&n) || ADDRESS_NAMES.contains(&n.as_str())
}

/// What every text line in a report goes through: the 64-hex rule, plus the literal tokens dist
/// LA6 adds (the relay address the ini names, if any).
#[derive(Clone, Debug, Default)]
pub struct Scrub {
    /// Longest first, so `host:port` is replaced before `host` alone could split it.
    literals: Vec<String>,
}

impl Scrub {
    /// The hex rule plus the relay address an ini carries. The bare host is added only when it is
    /// dotted (an IP or a domain): a relay called `relay:7100` would otherwise blank the word
    /// "relay" out of every log line about the relay, which is the opposite of useful.
    pub fn for_ini(ini_text: Option<&str>) -> Scrub {
        let mut literals = Vec::new();
        if let Some(addr) = ini_text.and_then(relay::relay_addr_in_ini) {
            if let Some((host, _port)) = addr.rsplit_once(':') {
                let host = host.trim_matches(|c| c == '[' || c == ']');
                if host.contains('.') && host.len() >= 4 {
                    literals.push(host.to_string());
                }
            }
            literals.push(addr);
        }
        literals.sort_by_key(|l| std::cmp::Reverse(l.len()));
        literals.dedup();
        Scrub { literals }
    }

    pub fn line(&self, line: &str) -> String {
        let mut out = scrub_hex(line);
        for lit in &self.literals {
            if out.contains(lit.as_str()) {
                out = out.replace(lit.as_str(), "<redacted-relay>");
            }
        }
        out
    }
}

/// `mh_net.ini`, with secret-looking settings blanked, the `relay=` line blanked by name (dist
/// LA6), and the scrub -- hex rule plus the relay address wherever else in the file it appears (a
/// `; relay=...` comment, a `force_relay` note) -- applied over the rest. `Scrub::default()` is
/// the hex rule alone.
pub fn redact_ini_with(text: &str, scrub: &Scrub) -> String {
    let mut out = String::with_capacity(text.len());
    for line in text.lines() {
        let trimmed = line.trim_start();
        // A `;` comment can carry a key too (see the module header) -- it goes through the scrub
        // below like everything else, but it is never treated as a setting.
        if !trimmed.starts_with(';') && !trimmed.starts_with('#') {
            if let Some((name, _)) = line.split_once('=') {
                if is_blanked_name(name) {
                    out.push_str(name);
                    out.push_str("=<redacted>\n");
                    continue;
                }
            }
        }
        out.push_str(&scrub.line(line));
        out.push('\n');
    }
    out
}

/// Replace every run of 64 or more hex digits with a marker.
///
/// SIXTY-FOUR, NOT THIRTY-TWO, and the difference is the whole design. The wire key is 32 bytes =
/// 64 hex digits (`MH_KEY_HEX_LEN`); the `match_id` is a UUIDv7 = 32 hex digits, and it must
/// survive, because `tools/crash_report.py` proves a report's `match_id` by finding the same value
/// in the `; [session] match_id=` lines inside this zip. A 32-digit threshold would have redacted
/// the evidence and left the secret's 64-digit form untouched only by luck of ordering.
///
/// The run is measured on a maximal hex span, so a 64-digit key embedded in a longer word is still
/// caught and a 40-character hex sha is not.
pub fn scrub_hex(line: &str) -> String {
    let bytes = line.as_bytes();
    let mut out = String::with_capacity(line.len());
    let mut i = 0;
    while i < bytes.len() {
        if bytes[i].is_ascii_hexdigit() {
            let start = i;
            while i < bytes.len() && bytes[i].is_ascii_hexdigit() {
                i += 1;
            }
            if i - start >= 64 {
                out.push_str("<redacted-64hex>");
            } else {
                out.push_str(&line[start..i]);
            }
        } else {
            // UTF-8 safe: a non-hex byte can be a continuation byte, so step by character.
            let ch = line[i..].chars().next().unwrap_or('\u{fffd}');
            out.push(ch);
            i += ch.len_utf8();
        }
    }
    out
}

/// Is this entry's content text we should scrub? The scrub is a string operation, so a binary file
/// (a screen capture, a save) is added byte for byte.
fn is_text(name: &str) -> bool {
    let lower = name.to_ascii_lowercase();
    [".log", ".txt", ".json", ".ini", ".csv", ".md"]
        .iter()
        .any(|ext| lower.ends_with(ext))
}

// ---- zip plumbing ------------------------------------------------------------------------------

fn add_text(
    zip: &mut zip::ZipWriter<std::fs::File>,
    opts: zip::write::SimpleFileOptions,
    name: &str,
    text: &str,
    entries: &mut Vec<String>,
    budget: &mut u64,
) -> Result<(), String> {
    let bytes = text.as_bytes();
    if bytes.len() as u64 > *budget {
        log::line(format!(
            "report: {name} would not fit the report budget; left out"
        ));
        return Ok(());
    }
    zip.start_file(name, opts)
        .map_err(|e| format!("cannot start {name} in the report: {e}"))?;
    zip.write_all(bytes)
        .map_err(|e| format!("cannot write {name} into the report: {e}"))?;
    *budget = budget.saturating_sub(bytes.len() as u64);
    entries.push(name.to_string());
    Ok(())
}

fn add_file(
    zip: &mut zip::ZipWriter<std::fs::File>,
    opts: zip::write::SimpleFileOptions,
    src: &Path,
    name: &str,
    entries: &mut Vec<String>,
    budget: &mut u64,
    scrub: &Scrub,
) -> Result<(), String> {
    add_capped(zip, opts, src, name, entries, budget, PER_FILE_MAX, scrub)
}

/// `add_file` with the per-file cap spelled out, because exactly one entry -- the minidump -- must
/// be either whole or absent, never tailed.
#[allow(clippy::too_many_arguments)]
fn add_capped(
    zip: &mut zip::ZipWriter<std::fs::File>,
    opts: zip::write::SimpleFileOptions,
    src: &Path,
    name: &str,
    entries: &mut Vec<String>,
    budget: &mut u64,
    per_file_max: u64,
    scrub: &Scrub,
) -> Result<(), String> {
    let leaf = src
        .file_name()
        .map(|n| n.to_string_lossy().to_ascii_lowercase())
        .unwrap_or_default();
    if DENY.iter().any(|d| *d == leaf) {
        log::line(format!("report: {leaf} is never included in a report"));
        return Ok(());
    }
    let Ok(md) = std::fs::metadata(src) else {
        return Ok(());
    };
    if !md.is_file() {
        return Ok(());
    }

    if is_text(name) {
        let raw = std::fs::read(src).unwrap_or_default();
        let text = String::from_utf8_lossy(&raw);
        // TAIL, NOT HEAD, when a log is too big: the interesting end of a log is the end.
        let text: &str = if text.len() as u64 > per_file_max {
            let cut = text.len() - per_file_max as usize;
            let cut = text
                .char_indices()
                .map(|(i, _)| i)
                .find(|i| *i >= cut)
                .unwrap_or(0);
            &text[cut..]
        } else {
            &text
        };
        let scrubbed: String = text.lines().map(|l| scrub.line(l) + "\n").collect();
        return add_text(zip, opts, name, &scrubbed, entries, budget);
    }

    if md.len() > *budget || md.len() > per_file_max {
        log::line(format!(
            "report: {name} is {} bytes and does not fit the report budget; left out",
            md.len()
        ));
        return Ok(());
    }
    let data = std::fs::read(src).map_err(|e| format!("cannot read {}: {e}", src.display()))?;
    zip.start_file(name, opts)
        .map_err(|e| format!("cannot start {name} in the report: {e}"))?;
    zip.write_all(&data)
        .map_err(|e| format!("cannot write {name} into the report: {e}"))?;
    *budget = budget.saturating_sub(data.len() as u64);
    entries.push(name.to_string());
    Ok(())
}

/// Every file under `dir`, one level of subdirectories included (a session directory is flat today,
/// and a recursion that went arbitrarily deep would be a way to ship a whole game folder by
/// accident).
fn add_dir(
    zip: &mut zip::ZipWriter<std::fs::File>,
    opts: zip::write::SimpleFileOptions,
    dir: &Path,
    prefix: &str,
    entries: &mut Vec<String>,
    budget: &mut u64,
    scrub: &Scrub,
) -> Result<(), String> {
    let Ok(read) = std::fs::read_dir(dir) else {
        log::line(format!("report: cannot list {}", dir.display()));
        return Ok(());
    };
    let mut files: Vec<PathBuf> = Vec::new();
    let mut subdirs: Vec<PathBuf> = Vec::new();
    for e in read.flatten() {
        let p = e.path();
        match e.file_type() {
            Ok(t) if t.is_dir() => subdirs.push(p),
            Ok(t) if t.is_file() => files.push(p),
            _ => {}
        }
    }
    files.sort();
    subdirs.sort();
    for p in files {
        let leaf = p
            .file_name()
            .unwrap_or_default()
            .to_string_lossy()
            .to_string();
        add_file(
            zip,
            opts,
            &p,
            &format!("{prefix}/{leaf}"),
            entries,
            budget,
            scrub,
        )?;
    }
    for d in subdirs {
        let leaf = d
            .file_name()
            .unwrap_or_default()
            .to_string_lossy()
            .to_string();
        let Ok(read) = std::fs::read_dir(&d) else {
            continue;
        };
        let mut inner: Vec<PathBuf> = read.flatten().map(|e| e.path()).collect();
        inner.sort();
        for p in inner {
            if !p.is_file() {
                continue;
            }
            let name = p
                .file_name()
                .unwrap_or_default()
                .to_string_lossy()
                .to_string();
            add_file(
                zip,
                opts,
                &p,
                &format!("{prefix}/{leaf}/{name}"),
                entries,
                budget,
                scrub,
            )?;
        }
    }
    Ok(())
}

/// The session directory a report would ship, given a game directory: the newest one under
/// `logs\`. Exposed so the Report view can SAY which it would take before anything is built.
pub fn default_session_dir(game_dir: Option<&Path>) -> Option<PathBuf> {
    game_dir.and_then(paths::newest_session_dir)
}

#[cfg(test)]
mod tests {
    use super::*;

    const KEY: &str = "4d487465737474656b65794d487465737474656b65794d487465737474656b65";
    /// dist LA6: the relay the fixture ini names. RFC 5737 documentation space, so it is a literal
    /// this tree may carry (tools/lint_machine_paths.py reads this file too).
    const RELAY: &str = "192.0.2.10:7100";

    #[test]
    fn a_report_without_a_description_is_refused() {
        for desc in ["", "   ", "\n\t "] {
            let input = Input {
                game_dir: None,
                session_dir: None,
                description: desc,
                last_run: None,
                crash: None,
                minidump: None,
                launcher_log: None,
            };
            let err = build(Path::new("nowhere/report.zip"), &input).unwrap_err();
            assert_eq!(err, NO_DESCRIPTION, "{desc:?}");
            assert!(!description_ok(desc));
        }
        assert!(description_ok("it crashed on turn 40"));
    }

    /// The three routes the key could take into a report, each checked on the text that really
    /// carries it. Route 1 (the file) is checked by the integration test, which builds a real zip.
    #[test]
    fn the_wire_key_is_scrubbed_wherever_it_appears() {
        assert_eq!(KEY.len(), 64);

        // Route 3: the line net_lockstep.cpp writes into mh_net.log on first run.
        let logged = format!(
            "[12:00:01.000] ; mh_key.txt generated -- send this file (or the line below):\n;   {KEY}\n"
        );
        let scrubbed: String = logged.lines().map(|l| scrub_hex(l) + "\n").collect();
        assert!(!scrubbed.contains(KEY), "{scrubbed}");
        assert!(scrubbed.contains("<redacted-64hex>"));

        // Route 2: a prospective ini setting, by name and by value.
        let ini = format!("[net]\nrole=host\nkey={KEY}\nport=6501\n");
        let red = redact_ini_with(&ini, &Scrub::default());
        assert!(!red.contains(KEY), "{red}");
        assert!(red.contains("key=<redacted>"));
        assert!(
            red.contains("role=host"),
            "unrelated settings survive: {red}"
        );
        assert!(red.contains("port=6501"));
    }

    /// The threshold, which is the part of the scrub that is easy to get wrong in the safe-looking
    /// direction. A `match_id` MUST survive -- crash_report.py proves the report's identity by
    /// finding it in these logs.
    #[test]
    fn a_match_id_survives_the_scrub_and_a_key_does_not() {
        let mid = "0199a3c1b2d04f1e8a7c6b5d4e3f2a10";
        assert_eq!(mid.len(), 32);
        let line = format!("; [session] match_id={mid}");
        assert_eq!(scrub_hex(&line), line);
        assert!(!scrub_hex(&format!("x{KEY}")).contains(KEY));
        // A git sha, a checksum, a UUID -- all shorter than the key, all preserved.
        for keep in ["abc12345", "deadbeefcafebabe", mid] {
            assert!(scrub_hex(keep).contains(keep), "{keep}");
        }
    }

    #[test]
    fn secret_setting_names_are_matched_whole_and_by_suffix() {
        for yes in [
            "key",
            "KEY",
            " psk ",
            "secret",
            "token",
            "relay_token",
            "wire_key",
        ] {
            assert!(is_secret_name(yes), "{yes}");
        }
        for no in ["keyboard", "monkey", "role", "toggle_key_delay", "port"] {
            assert!(!is_secret_name(no), "{no}");
        }
    }

    /// `is_secret_name` matches `toggle_key`, and the debug overlay really has one
    /// (`[debug] toggle_key`). Redacting it costs nothing -- it is a virtual-key code -- and the
    /// alternative, an exception list, is how a redactor acquires the hole it was built to close.
    #[test]
    fn a_false_positive_is_preferred_to_a_hole() {
        assert!(is_secret_name("toggle_key"));
        let red = redact_ini_with("[debug]\ntoggle_key=0x77\n", &Scrub::default());
        assert!(red.contains("toggle_key=<redacted>"), "{red}");
    }

    #[test]
    fn utf8_survives_the_scrub() {
        let line = "; player joined: Ярослав — slot 2";
        assert_eq!(scrub_hex(line), line);
    }

    // ---- the whole thing, against a real game directory on disk -------------------------------

    /// A game folder as a player's would be: the exe, the key file, a configuration, and two
    /// session directories so "the newest one" is a claim with a wrong answer available.
    fn fixture(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("mh_launcher_test_{name}"));
        std::fs::remove_dir_all(&dir).ok();
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(dir.join("mh.exe"), b"not really an exe").unwrap();
        // Route 1: the key file itself, beside the exe, exactly where the game puts it.
        std::fs::write(dir.join("mh_key.txt"), KEY).unwrap();
        // Route 2: a prospective ini setting -- and, since dist LA6, the relay line the launcher
        // itself writes (a documented example address, RFC 5737 TEST-NET-1).
        std::fs::write(
            dir.join("mh_net.ini"),
            format!(
                "[net]\nrole=host\nkey={KEY}\nport=6501\ntransport=udp\nrelay={RELAY} ; the \
                 directory\n; force_relay=1 pins {RELAY}\n"
            ),
        )
        .unwrap();

        let old = dir.join("logs").join("20260916T101010Z_menu_solo");
        std::fs::create_dir_all(&old).unwrap();
        std::fs::write(
            old.join("mh_net.log"),
            "; an older run nobody asked about\n",
        )
        .unwrap();

        let new = dir.join("logs").join("20260917T164346Z_dedd707c_1_client");
        std::fs::create_dir_all(&new).unwrap();
        // Route 3: the key, logged in full, inside the directory a report is a zip of -- and the
        // relay's host, which udp_relay.cpp prints when the leg comes up.
        std::fs::write(
            new.join("mh_net.log"),
            format!(
                "; [build] mh 0.1.0-rc1+abc12345\n\
                 ; mh_key.txt generated -- send this file (or the line below) to the players \
                 joining you:\n;   {KEY}\n\
                 ; [session] match_id=0199a3c1b2d04f1e8a7c6b5d4e3f2a10\n\
                 net: udp relay leg UP -- {RELAY} room=6501, handle 3\n\
                 net: udp relay={RELAY} does not resolve -- REFUSING to start\n"
            ),
        )
        .unwrap();
        std::fs::write(
            new.join("session.json"),
            "{\n  \"match_id\": \"0199a3c1b2d04f1e8a7c6b5d4e3f2a10\",\n  \"slot\": 1\n}\n",
        )
        .unwrap();
        // Something binary, so the text/binary split is exercised rather than assumed.
        std::fs::write(new.join("capture_k1.bmp"), [0x42u8, 0x4d, 0, 0, 0, 0]).unwrap();
        dir
    }

    fn entries_of(zip: &Path) -> Vec<(String, Vec<u8>)> {
        let f = std::fs::File::open(zip).unwrap();
        let mut a = zip::ZipArchive::new(f).unwrap();
        let mut out = Vec::new();
        for i in 0..a.len() {
            let mut e = a.by_index(i).unwrap();
            let name = e.name().to_string();
            let mut buf = Vec::new();
            std::io::Read::read_to_end(&mut e, &mut buf).unwrap();
            out.push((name, buf));
        }
        out
    }

    /// LA4's done_when, third clause: unzip the report and grep. `mh_key.txt` must not be an entry
    /// and the key's 64 hex digits must not appear in ANY entry -- which is the stronger statement,
    /// and the one that catches the `mh_net.log` route the clause does not mention.
    #[test]
    fn the_key_is_in_no_entry_of_a_real_report() {
        let dir = fixture("report_key");
        let zip = dir.join("out").join("report.zip");
        let input = Input {
            game_dir: Some(&dir),
            session_dir: default_session_dir(Some(&dir)),
            description: "the lobby froze when the second player joined",
            last_run: None,
            crash: None,
            minidump: None,
            launcher_log: None,
        };
        let built = build(&zip, &input).unwrap();

        let entries = entries_of(&zip);
        assert!(!entries.is_empty());
        for (name, _) in &entries {
            assert!(
                !name.to_ascii_lowercase().contains("mh_key.txt"),
                "the key FILE is in the report as {name}"
            );
        }
        for (name, body) in &entries {
            let text = String::from_utf8_lossy(body);
            assert!(!text.contains(KEY), "the key's value is inside {name}");
        }
        // And the redaction actually happened rather than the files being absent.
        let log = entries
            .iter()
            .find(|(n, _)| n == "session/mh_net.log")
            .expect("the session log is in the report");
        assert!(String::from_utf8_lossy(&log.1).contains("<redacted-64hex>"));
        let ini = entries
            .iter()
            .find(|(n, _)| n == "config/mh_net.ini")
            .expect("the configuration is in the report");
        assert!(String::from_utf8_lossy(&ini.1).contains("key=<redacted>"));

        // dist LA6 done_when (c): the relay line's value is blanked, and the address is in NO
        // entry -- not the ini's comment, not the game log's "leg UP" line.
        let ini_text = String::from_utf8_lossy(&ini.1);
        assert!(ini_text.contains("relay=<redacted>"), "{ini_text}");
        assert!(
            ini_text.contains("transport=udp"),
            "unrelated keys survive: {ini_text}"
        );
        for (name, body) in &entries {
            let text = String::from_utf8_lossy(body);
            assert!(
                !text.contains(RELAY),
                "the relay address is inside {name}: {text}"
            );
            assert!(
                !text.contains("192.0.2.10"),
                "the relay host is inside {name}: {text}"
            );
        }
        let log_text = String::from_utf8_lossy(&log.1);
        assert!(
            log_text.contains("udp relay leg UP -- <redacted-relay> room=6501"),
            "{log_text}"
        );

        assert!(built.summary().contains("report written to"));
        std::fs::remove_dir_all(&dir).ok();
    }

    /// dist LA6: the scrub is keyed off the ini, so a game directory WITHOUT a relay line blanks
    /// nothing that looks like an address -- a report must not lose a log line about some other
    /// host because it resembled a relay.
    #[test]
    fn the_relay_scrub_only_knows_the_address_the_ini_names() {
        let none = Scrub::for_ini(None);
        assert_eq!(
            none.line("net: peer 192.0.2.99:6501 joined"),
            "net: peer 192.0.2.99:6501 joined"
        );
        let no_relay = Scrub::for_ini(Some("[net]\ntransport=udp\n; relay=HOST:PORT\n"));
        assert_eq!(
            no_relay.line("udp relay leg UP -- 192.0.2.10:7100"),
            "udp relay leg UP -- 192.0.2.10:7100"
        );

        let with = Scrub::for_ini(Some(&format!("[net]\nrelay={RELAY}\n")));
        assert_eq!(
            with.line(&format!("leg UP -- {RELAY} room=1")),
            "leg UP -- <redacted-relay> room=1"
        );
        assert_eq!(
            with.line("host 192.0.2.10 alone"),
            "host <redacted-relay> alone"
        );
        assert_eq!(
            with.line("other 192.0.2.11:7100 stays"),
            "other 192.0.2.11:7100 stays"
        );
        // The hex rule still runs underneath.
        assert!(!with.line(&format!("{KEY} {RELAY}")).contains(KEY));

        // A relay whose host is a bare word is blanked only as host:port, never as the word.
        let word = Scrub::for_ini(Some("[net]\nrelay=relay:7100\n"));
        assert_eq!(
            word.line("udp relay leg UP -- relay:7100"),
            "udp relay leg UP -- <redacted-relay>"
        );
        assert_eq!(word.line("the relay refused"), "the relay refused");

        // And the ini's own relay line is blanked by NAME, comment or not aside.
        let red = redact_ini_with(&format!("[net]\nRelay = {RELAY}\n; was {RELAY}\n"), &with);
        assert_eq!(red, "[net]\nRelay =<redacted>\n; was <redacted-relay>\n");
    }

    /// LA4's done_when, fourth clause: a report with no crash behind it carries the MOST RECENT
    /// session directory -- not the first one found, and not both.
    #[test]
    fn a_non_crash_report_carries_the_newest_session_directory() {
        let dir = fixture("report_session");
        let zip = dir.join("out").join("report.zip");
        let input = Input {
            game_dir: Some(&dir),
            session_dir: default_session_dir(Some(&dir)),
            description: "nothing crashed, it just looked wrong",
            last_run: None,
            crash: None,
            minidump: None,
            launcher_log: None,
        };
        let built = build(&zip, &input).unwrap();

        let names: Vec<String> = entries_of(&zip).into_iter().map(|(n, _)| n).collect();
        assert!(
            names.contains(&"session/mh_net.log".to_string()),
            "{names:?}"
        );
        assert!(
            names.contains(&"session/session.json".to_string()),
            "{names:?}"
        );
        assert!(
            names.contains(&"session/capture_k1.bmp".to_string()),
            "{names:?}"
        );
        assert!(names.contains(&"report.json".to_string()));
        assert!(names.contains(&"description.txt".to_string()));

        // The older directory's contents must NOT be here. Its log line is the marker.
        for (name, body) in entries_of(&zip) {
            assert!(
                !String::from_utf8_lossy(&body).contains("an older run nobody asked about"),
                "the older session leaked in through {name}"
            );
        }

        // report.json names the session it shipped, and the fields tools/crash_report.py reads.
        let meta: serde_json::Value = serde_json::from_str(&built.meta).unwrap();
        assert_eq!(
            meta["session_dir"].as_str().unwrap(),
            "20260917T164346Z_dedd707c_1_client"
        );
        assert_eq!(
            meta["match_id"].as_str().unwrap(),
            "0199a3c1b2d04f1e8a7c6b5d4e3f2a10"
        );
        assert_eq!(meta["build"].as_str().unwrap(), "0.1.0-rc1+abc12345");
        // No crash -> no `crash` key. print_drained_report treats that as an ordinary bug report.
        assert!(meta.get("crash").is_none(), "{}", built.meta);
        std::fs::remove_dir_all(&dir).ok();
    }

    /// The `crash` object, spelled the way `tools/crash_report.py::resolve_module_fault` reads it:
    /// `module` a name it knows, `offset` a "0x..." STRING it passes to `int(x, 16)`.
    #[test]
    fn a_crash_report_carries_the_module_and_offset_crash_report_py_expects() {
        let dir = fixture("report_crash");
        let zip = dir.join("out").join("report.zip");
        let marker = Marker {
            version: 1,
            code: 0xC000_0005,
            pid: 4242,
            tid: 1337,
            address: 0x1001_75b0,
            pointers: 0,
            module: "mh.dll".to_string(),
            module_base: 0x1000_0000,
            offset: 0x0001_75b0,
            match_id: "0199a3c1b2d04f1e8a7c6b5d4e3f2a10".to_string(),
            build: "0.1.0-rc1+abc12345".to_string(),
            when: "20260917T164346Z".to_string(),
            has_context: false,
        };
        let finished = Finished {
            outcome: crate::launch::classify(0xC000_0005),
            seconds: 91.5,
        };
        let input = Input {
            game_dir: Some(&dir),
            session_dir: default_session_dir(Some(&dir)),
            description: "crashed right after I ordered the third harvester",
            last_run: Some(&finished),
            crash: Some(&marker),
            minidump: None,
            launcher_log: None,
        };
        let built = build(&zip, &input).unwrap();
        let meta: serde_json::Value = serde_json::from_str(&built.meta).unwrap();
        assert_eq!(meta["crash"]["module"].as_str().unwrap(), "mh.dll");
        assert_eq!(meta["crash"]["offset"].as_str().unwrap(), "0x000175b0");
        // The two additions -- see meta_json. The dump has no exception stream to name the thread.
        assert_eq!(meta["crash"]["thread_id"].as_u64().unwrap(), 1337);
        assert_eq!(meta["crash"]["code"].as_str().unwrap(), "0xc0000005");
        // The signed form is what src/collector/README.md's own example shows.
        assert_eq!(meta["exit_code"].as_i64().unwrap(), -1_073_741_819);
        assert_eq!(meta["exit_code_hex"].as_str().unwrap(), "0xC0000005");
        assert!(!meta["os"].as_str().unwrap().is_empty());
        assert!(!meta["launcher_version"].as_str().unwrap().is_empty());

        let names: Vec<String> = entries_of(&zip).into_iter().map(|(n, _)| n).collect();
        assert!(names.contains(&"crash/marker.txt".to_string()), "{names:?}");
        std::fs::remove_dir_all(&dir).ok();
    }
}
