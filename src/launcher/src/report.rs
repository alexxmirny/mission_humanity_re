//! Building the report zip (dist LA4, plan decision D13; scope widened by dist LA9).
//!
//! A report is one deflate zip and one required sentence. What goes in it:
//!
//! ```text
//! report.json            everything a triager needs before opening anything else
//! description.txt        what the player typed. REQUIRED, and the build refuses without it
//! logs/<name>/...        EVERY process + session directory since the launcher started (dist LA9)
//! config/mh_net.ini      the game's configuration, REDACTED
//! launcher/launcher.log  the tail of this program's own log
//! crash/marker.txt       what THIS run's crash handler wrote, when there was a crash
//! crash/<name>.marker    every `mh_crash_*.marker`(.ctx32) file swept from `logs\` (dist LA9) --
//!                        stale ones from an earlier, undrained crash included, not just this run's
//! minidump.dmp           only when the player ticked the box and a dump was written
//! ```
//!
//! ## dist LA9: a session directory is not the report, the LOGS TREE is
//!
//! LA4 shipped one session directory. Two real 2026-09-20 uploads proved that was ~1 permille of
//! what a freeze/desync analysis needs: the desync, the freezes, the relay/punch evidence and the
//! crash all lived in OTHER directories under `logs\` -- an earlier session, the process directory's
//! own `net:` lines, a `mh_crash_*.marker` nobody's report ever carried -- and the players had to
//! send those by hand. `plan_logs` below now selects EVERY directory under `logs\` written since
//! this launcher process started (a UTC stamp compared against each directory's own name, which is
//! how `mh_common/run_context.cpp` and `mh_common/include/mh_session_dir.h` name them), falls back
//! to the newest `LOGS_FALLBACK_N` when that start time is not known (`--report` from a script that
//! never called `--launch` first, or a test), and drops the OLDEST of those first when the total
//! would not fit -- except the two things a report is never allowed to lose: the match the
//! description form names (`session_dir`, the player's pick or the newest by default) and its own
//! process directory (found via that session's own `session.json` `process_dir` field), and every
//! `mh_crash_*` file, which is swept from `logs\` directly and never subject to the drop.
//!
//! **`report.json` IS the `meta` object RP1 will POST**, byte for byte, not a cousin of it. The
//! collector stores that object as `meta.json` beside the zip (`src/collector/README.md`), and
//! `tools/crash_report.py --report` reads `match_id`, `version`, `exit_code` and the optional
//! `crash` object out of it. Writing one object and sending the same one is what keeps the drained
//! report and the zip's own copy from ever disagreeing -- and it means the uploader in RP1 has no
//! field names of its own to get wrong. dist LA9 adds `included` (every `logs\` directory the zip
//! actually carries) and `dropped` (`{dir, bytes, why}` for every one the size budget refused).
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
/// The margin below 64 MB is deliberate, not the whole story dist LA9 needs: this budget is counted
/// against UNCOMPRESSED bytes while the upload is DEFLATED, so the real body is smaller than this in
/// the overwhelming common case (mp:SES5's log diet made a match's text compress hard) -- the margin
/// exists for what does NOT compress, chiefly the one entry (the minidump) let past `PER_FILE_MAX`.
const TOTAL_MAX: u64 = 48 * 1024 * 1024;

/// dist LA9. How many bytes of `TOTAL_MAX` are set aside for `report.json`, `description.txt`, the
/// redacted ini and the launcher's own log tail BEFORE the `logs\` tree selection below runs its own
/// budget -- sized to the launcher log's own `PER_FILE_MAX` (its only entry that can be large) plus
/// slack for the other three, which are a few KB each. Without this reservation the tree selection
/// would size itself against the WHOLE of `TOTAL_MAX` and could leave nothing for a large launcher
/// log to fit into by the time its turn came.
const RESERVED_FOR_FIXED_ENTRIES: u64 = PER_FILE_MAX + 2 * 1024 * 1024;

/// dist LA9. How many of the newest `logs\` directories to package when the launcher's own start
/// time is unknown (a `--report` invoked from a script that never called `--launch` in this same
/// process, or a test). Large enough to cover "the whole afternoon" of a normal play session without
/// degenerating into "every directory this game folder has ever produced".
const LOGS_FALLBACK_N: usize = 20;

/// What a report is built from. Everything is optional except the description, which is the point.
pub struct Input<'a> {
    pub game_dir: Option<&'a Path>,
    /// The session directory this report is ABOUT -- the match the description names. Newest by
    /// default (`default_session_dir`) or the player's own pick (`app.rs`'s picker, dist LA9); it is
    /// never dropped from the `logs\` selection below and its own process directory rides along with
    /// it, whatever the size budget does to everything else.
    pub session_dir: Option<PathBuf>,
    /// dist LA9. This launcher process's own UTC start stamp, in the same
    /// `YYYYMMDDTHHMMSSZ` shape `mh_common/include/mh_session_dir.h` names directories with (see
    /// `app.rs`'s `stamp_for_file`) -- every directory under `logs\` at or after this stamp is a
    /// candidate for "since the launcher started". `None` (a `--report` from a script with no prior
    /// `--launch` in this process, or a test) falls back to the newest `LOGS_FALLBACK_N`.
    pub launcher_started_utc: Option<String>,
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

    // dist LA9: which `logs\` directories the zip will carry, decided BEFORE report.json is
    // composed -- `included`/`dropped` are part of that object, byte for byte the same object RP1
    // posts, so the decision has to exist first.
    let logs_plan = input
        .game_dir
        .map(|gd| {
            plan_logs(
                gd,
                session.as_deref(),
                input.launcher_started_utc.as_deref(),
                TOTAL_MAX.saturating_sub(RESERVED_FOR_FIXED_ENTRIES),
            )
        })
        .unwrap_or_default();

    let meta = meta_json(
        input,
        &machine,
        installed.as_ref(),
        &build_stamp,
        &match_id,
        &logs_plan,
    );

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

    // dist LA9: the whole planned slice of `logs\`, not just the one chosen session directory --
    // plus (dist LA10) anything that landed loose in `logs\` itself rather than in a subdirectory.
    if let Some(gd) = input.game_dir {
        add_logs_tree(
            &mut zip,
            opts,
            gd,
            &logs_plan,
            &mut entries,
            &mut budget,
            &scrub,
        )?;
        add_loose_log_files(&mut zip, opts, gd, &mut entries, &mut budget, &scrub)?;
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

    // dist LA9: every `mh_crash_*` file sitting under `logs\`, not only the one THIS run's live
    // channel caught -- a marker (or its `.ctx32` sidecar) left by an earlier, undrained crash is
    // exactly the evidence a "something looked wrong later" report exists to carry, and it is never
    // subject to the drop above. Uses a throwaway budget of its own: these files are a few hundred
    // bytes each and must never be the thing a tight `budget` sacrifices.
    if let Some(gd) = input.game_dir {
        add_crash_marker_files(&mut zip, opts, gd, &mut entries, &scrub)?;
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
    logs_plan: &LogsPlan,
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
        // dist LA9: what the `logs\` selection above decided, spelled the way a triager (or a
        // future `crash_report.py`) reads it back -- every directory the zip actually carries, and
        // every one the size budget refused, with its size and why.
        "included": logs_plan.included,
        "dropped": logs_plan
            .dropped
            .iter()
            .map(|d| serde_json::json!({"dir": d.dir, "bytes": d.bytes, "why": d.why}))
            .collect::<Vec<_>>(),
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

// ---- dist LA9: which `logs\` directories the report carries -----------------------------------

/// One directory `report.json`'s `dropped` array names: which one, how big, why it did not fit.
#[derive(Clone, Debug)]
pub struct DroppedDir {
    pub dir: String,
    pub bytes: u64,
    pub why: &'static str,
}

/// What `plan_logs` decided. `included` is every directory that will actually be zipped, in the
/// order they are added (protected ones first, then newest-to-oldest); `dropped` is everything the
/// size budget refused, oldest of the refused ones last (they were refused in that order).
#[derive(Clone, Debug, Default)]
pub struct LogsPlan {
    pub included: Vec<String>,
    pub dropped: Vec<DroppedDir>,
}

/// One directory found directly under `logs\`, named the way `mh_session_dir.h` names them --
/// `<stamp>_menu_<role>` (a process directory) or `<stamp>_<mid8>_<slot>_<role>` (a session one).
/// `stamp` is the leading 16-character UTC prefix, which sorts exactly like the moment it names
/// (see `paths::newest_session_dir`'s note on why the name decides, not the mtime).
struct LogDir {
    name: String,
    path: PathBuf,
    stamp: String,
    bytes: u64,
}

/// Is this a per-PROCESS ("menu") directory rather than a per-session one? The second underscore-
/// separated field is `menu` for a process directory and the match's short hex id for a session one
/// (`mh_session_dir_name` in `mh_common/include/mh_session_dir.h`).
fn is_process_dir_name(name: &str) -> bool {
    name.split('_').nth(1) == Some("menu")
}

/// The total size of every FILE under `dir`, at any depth. A session directory is flat and a process
/// directory close to it, so this rarely recurses more than once, but sizing (unlike `add_dir`'s own
/// one-level cap on what it WRITES) has no reason to under-count a directory shaped differently than
/// expected -- the worst that happens is this directory looks bigger than `add_dir` will actually
/// make it, which only ever makes the selection MORE conservative.
fn dir_size(dir: &Path) -> u64 {
    let mut total = 0u64;
    let Ok(read) = std::fs::read_dir(dir) else {
        return 0;
    };
    for e in read.flatten() {
        let p = e.path();
        match e.file_type() {
            Ok(t) if t.is_dir() => total += dir_size(&p),
            Ok(t) if t.is_file() => {
                total += std::fs::metadata(&p).map(|m| m.len()).unwrap_or(0);
            }
            _ => {}
        }
    }
    total
}

/// `20260917T164346Z` off the front of a directory name, or `None` if it does not start with one.
/// The same shape-check `paths::newest_session_dir` uses (kept as its own small copy here rather
/// than a cross-module dependency, the way `mh_common`'s OS-free headers accept some duplication
/// for the same reason): the only thing that matters is that every real stamp is the same fixed
/// width and character class, so string comparison between two of them IS a comparison of instants.
fn utc_stamp_prefix(name: &str) -> Option<&str> {
    let b = name.as_bytes();
    if b.len() < 16 {
        return None;
    }
    let digits = |r: std::ops::Range<usize>| b[r].iter().all(u8::is_ascii_digit);
    if digits(0..8) && b[8] == b'T' && digits(9..15) && b[15] == b'Z' {
        return Some(&name[..16]);
    }
    None
}

/// Every process/session directory directly under `<game_dir>\logs\`, newest first by name (a
/// directory whose name is not a recognisable stamp -- stray litter, a future format -- is simply
/// not a candidate; this function packages what SES1 writes, not "everything in the folder").
fn list_log_dirs(logs_root: &Path) -> Vec<LogDir> {
    let mut out = Vec::new();
    let Ok(read) = std::fs::read_dir(logs_root) else {
        return out;
    };
    for e in read.flatten() {
        let path = e.path();
        if !e.file_type().map(|t| t.is_dir()).unwrap_or(false) {
            continue;
        }
        let name = e.file_name().to_string_lossy().to_string();
        let Some(stamp) = utc_stamp_prefix(&name) else {
            continue;
        };
        let stamp = stamp.to_string();
        let bytes = dir_size(&path);
        out.push(LogDir {
            name,
            path,
            stamp,
            bytes,
        });
    }
    out.sort_by(|a, b| b.stamp.cmp(&a.stamp).then_with(|| b.name.cmp(&a.name)));
    out
}

/// The process directory a session hangs off, from that session's OWN `session.json` (written by
/// SES1; see `mh_common/include/mh_session_dir.h`'s `MH_SessionRecord::process_dir` and
/// `MH_ProcessDirLeaf`). `None` for a directory with no `session.json` (a process/"menu" directory
/// has none of its own) or one that does not name a process directory.
fn process_dir_of_session(session_dir: &Path) -> Option<String> {
    let text = std::fs::read_to_string(session_dir.join("session.json")).ok()?;
    let v: serde_json::Value = serde_json::from_str(&text).ok()?;
    let pd = v.get("process_dir")?.as_str()?.trim().to_string();
    if pd.is_empty() {
        None
    } else {
        Some(pd)
    }
}

/// Every SESSION-shaped directory under a game's `logs\`, newest first -- what the Report view's
/// picker (dist LA9) offers the player, and what `default_session_dir` would answer if it filtered
/// out the process ("menu") directories the way this does. Exposed so the picker can list every
/// candidate rather than only the newest.
pub fn session_dirs(game_dir: &Path) -> Vec<PathBuf> {
    list_log_dirs(&game_dir.join("logs"))
        .into_iter()
        .filter(|d| !is_process_dir_name(&d.name))
        .map(|d| d.path)
        .collect()
}

/// Decide which `logs\` directories a report carries (dist LA9).
///
/// `protected_session` is the match the report is ABOUT -- the player's pick, or the newest by
/// default -- and it, together with the process directory its own `session.json` names, is never
/// dropped, whatever the budget says. The NEWEST session directory overall is protected the same
/// way even when it differs from `protected_session` (a player reporting an OLDER match should not
/// lose the freshest evidence sitting right next to it). Everything else is a candidate only if its
/// own stamp is at or after `since_utc` (the launcher's own start), or -- when that is unknown --
/// among the newest `LOGS_FALLBACK_N` directories; from that pool, the newest fit first and the
/// OLDEST are dropped once the running total would exceed `budget`.
fn plan_logs(
    game_dir: &Path,
    protected_session: Option<&Path>,
    since_utc: Option<&str>,
    budget: u64,
) -> LogsPlan {
    let dirs = list_log_dirs(&game_dir.join("logs")); // newest first
    if dirs.is_empty() {
        return LogsPlan::default();
    }
    let by_name: std::collections::HashMap<&str, &LogDir> =
        dirs.iter().map(|d| (d.name.as_str(), d)).collect();

    let mut protected: std::collections::BTreeSet<String> = std::collections::BTreeSet::new();
    if let Some(name) = protected_session
        .and_then(|p| p.file_name())
        .map(|n| n.to_string_lossy().to_string())
    {
        if by_name.contains_key(name.as_str()) {
            protected.insert(name);
        }
    }
    if let Some(newest_session) = dirs.iter().find(|d| !is_process_dir_name(&d.name)) {
        protected.insert(newest_session.name.clone());
    }
    // Each protected session's own process directory rides along -- done_when's "contains both
    // session dirs, THE process dir": two matches hosted without a restart share one.
    let mut process_dirs = Vec::new();
    for name in &protected {
        let Some(d) = by_name.get(name.as_str()) else {
            continue;
        };
        if let Some(pd) = process_dir_of_session(&d.path) {
            if by_name.contains_key(pd.as_str()) {
                process_dirs.push(pd);
            }
        }
    }
    protected.extend(process_dirs);

    // The candidate pool. Protected entries ride along regardless of the window: a report is about
    // a SPECIFIC match, and it must never silently lose the thing it is about because that match
    // happens to predate this launcher process (a `--report` built long after `--launch`) or fall
    // outside the fallback's newest-N.
    let mut pool: Vec<&LogDir> = match since_utc {
        Some(since) => dirs
            .iter()
            .filter(|d| d.stamp.as_str() >= since || protected.contains(&d.name))
            .collect(),
        None => {
            let mut v: Vec<&LogDir> = dirs.iter().take(LOGS_FALLBACK_N).collect();
            for d in &dirs {
                if protected.contains(&d.name) && !v.iter().any(|x| x.name == d.name) {
                    v.push(d);
                }
            }
            v
        }
    };
    pool.sort_by(|a, b| b.stamp.cmp(&a.stamp).then_with(|| b.name.cmp(&a.name)));

    let (protected_entries, optional_entries): (Vec<&LogDir>, Vec<&LogDir>) =
        pool.into_iter().partition(|d| protected.contains(&d.name));

    let mut included = Vec::new();
    let mut dropped = Vec::new();
    let mut total = 0u64;
    for d in protected_entries {
        total += d.bytes; // never dropped, even if this alone is over budget
        included.push(d.name.clone());
    }
    // `optional_entries` is still newest-to-oldest (partition preserves relative order), so the
    // first one that does not fit -- and everything after it, all older still -- is the OLDEST
    // material being dropped, exactly the row's "drop the oldest dirs first".
    let mut over_budget = false;
    for d in optional_entries {
        if !over_budget && total + d.bytes <= budget {
            total += d.bytes;
            included.push(d.name.clone());
        } else {
            over_budget = true;
            dropped.push(DroppedDir {
                dir: d.name.clone(),
                bytes: d.bytes,
                why: "over the report size budget",
            });
        }
    }
    LogsPlan { included, dropped }
}

/// Zip every directory `plan_logs` included, each under `logs/<name>/`.
fn add_logs_tree(
    zip: &mut zip::ZipWriter<std::fs::File>,
    opts: zip::write::SimpleFileOptions,
    game_dir: &Path,
    plan: &LogsPlan,
    entries: &mut Vec<String>,
    budget: &mut u64,
    scrub: &Scrub,
) -> Result<(), String> {
    let logs_root = game_dir.join("logs");
    for name in &plan.included {
        add_dir(
            zip,
            opts,
            &logs_root.join(name),
            &format!("logs/{name}"),
            entries,
            budget,
            scrub,
        )?;
    }
    Ok(())
}

/// Every plain FILE sitting directly under `<game_dir>\logs\` (not `mh_crash_*`, handled
/// separately by `add_crash_marker_files`) -- dist LA10's degraded path. When a stamped
/// process/session name does not fit `CreateDirectory`'s real ceiling, `run_context.cpp`'s
/// `make_dir()` degrades to writing every stream loose into the bare `logs\` root instead of a
/// subdirectory, which `list_log_dirs` above (a directory listing) cannot see at all. LA10's own
/// done_when names this: "the report (LA9) finds it" -- so this sweep, under `logs/_root/`, is what
/// finds it. Uses the real shared `budget` (unlike the crash-marker sweep): this can carry a whole
/// run's logs, not a handful of bytes, so it competes for space like everything else rather than
/// riding in free.
fn add_loose_log_files(
    zip: &mut zip::ZipWriter<std::fs::File>,
    opts: zip::write::SimpleFileOptions,
    game_dir: &Path,
    entries: &mut Vec<String>,
    budget: &mut u64,
    scrub: &Scrub,
) -> Result<(), String> {
    let logs_root = game_dir.join("logs");
    let Ok(read) = std::fs::read_dir(&logs_root) else {
        return Ok(());
    };
    let mut files: Vec<PathBuf> = read
        .flatten()
        .map(|e| e.path())
        .filter(|p| p.is_file())
        .filter(|p| {
            !p.file_name()
                .map(|n| n.to_string_lossy().starts_with("mh_crash_"))
                .unwrap_or(false)
        })
        .collect();
    files.sort();
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
            &format!("logs/_root/{leaf}"),
            entries,
            budget,
            scrub,
        )?;
    }
    Ok(())
}

/// Every `mh_crash_*` file sitting directly under `<game_dir>\logs\` (the raw marker `crash.rs`'s
/// `Channel::create` names, and its `.ctx32` sidecar) -- swept in whole and NEVER subject to the
/// `logs\` drop above, on a throwaway budget of its own: these are a handful of hundred bytes each
/// and must never be the entry a tight report budget sacrifices. Distinct from `crash/marker.txt`
/// (this run's OWN crash, synthesised from `input.crash` above): this sweep also picks up a marker
/// an EARLIER, undrained crash left behind, which is exactly the case a "something looked wrong"
/// report -- built well after the fact, with no live crash channel -- exists to carry.
fn add_crash_marker_files(
    zip: &mut zip::ZipWriter<std::fs::File>,
    opts: zip::write::SimpleFileOptions,
    game_dir: &Path,
    entries: &mut Vec<String>,
    scrub: &Scrub,
) -> Result<(), String> {
    let logs_root = game_dir.join("logs");
    let Ok(read) = std::fs::read_dir(&logs_root) else {
        return Ok(());
    };
    let mut files: Vec<PathBuf> = read
        .flatten()
        .map(|e| e.path())
        .filter(|p| p.is_file())
        .filter(|p| {
            p.file_name()
                .map(|n| n.to_string_lossy().starts_with("mh_crash_"))
                .unwrap_or(false)
        })
        .collect();
    files.sort();
    let mut unbounded = u64::MAX;
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
            &format!("crash/{leaf}"),
            entries,
            &mut unbounded,
            scrub,
        )?;
    }
    Ok(())
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
    // dist LA9: `.marker` (the raw DLL-written crash marker text, `mh_crash_marker.h`) goes through
    // the same scrub as everything else on general principle, even though nothing it writes today
    // is secret -- `.ctx32`, its binary sidecar, is deliberately NOT in this list.
    [".log", ".txt", ".json", ".ini", ".csv", ".md", ".marker"]
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
                launcher_started_utc: None,
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
            launcher_started_utc: None,
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
            .find(|(n, _)| n == "logs/20260917T164346Z_dedd707c_1_client/mh_net.log")
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

    /// LA4's done_when, fourth clause -- a report with no crash behind it names the MOST RECENT
    /// session directory as its `session_dir` -- superseded in scope by dist LA9: the OLDER
    /// directory is no longer excluded, it is packaged too (the whole point of LA9's "one session
    /// dir is ~1 permille of what an analysis needs"). `report.json`'s `session_dir` still answers
    /// "which match is this report about" with the newest, and `included` names every directory
    /// the zip actually carries.
    #[test]
    fn a_non_crash_report_carries_the_newest_session_directory_and_the_older_one_too() {
        let dir = fixture("report_session");
        let zip = dir.join("out").join("report.zip");
        let input = Input {
            game_dir: Some(&dir),
            session_dir: default_session_dir(Some(&dir)),
            launcher_started_utc: None,
            description: "nothing crashed, it just looked wrong",
            last_run: None,
            crash: None,
            minidump: None,
            launcher_log: None,
        };
        let built = build(&zip, &input).unwrap();

        let names: Vec<String> = entries_of(&zip).into_iter().map(|(n, _)| n).collect();
        let newest = "logs/20260917T164346Z_dedd707c_1_client";
        let older = "logs/20260916T101010Z_menu_solo";
        assert!(names.contains(&format!("{newest}/mh_net.log")), "{names:?}");
        assert!(
            names.contains(&format!("{newest}/session.json")),
            "{names:?}"
        );
        assert!(
            names.contains(&format!("{newest}/capture_k1.bmp")),
            "{names:?}"
        );
        assert!(names.contains(&"report.json".to_string()));
        assert!(names.contains(&"description.txt".to_string()));

        // dist LA9: the older directory is now IN the report, not excluded from it.
        assert!(
            names.contains(&format!("{older}/mh_net.log")),
            "the older directory did not make it into the report: {names:?}"
        );
        let older_log = entries_of(&zip)
            .into_iter()
            .find(|(n, _)| n == &format!("{older}/mh_net.log"))
            .unwrap()
            .1;
        assert!(String::from_utf8_lossy(&older_log).contains("an older run nobody asked about"));

        // report.json names the session it is ABOUT, and the fields tools/crash_report.py reads.
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

        // dist LA9: both directories are named as INCLUDED, and nothing was dropped -- this
        // fixture is a handful of bytes, nowhere near the report budget.
        let included: Vec<String> = meta["included"]
            .as_array()
            .unwrap()
            .iter()
            .map(|v| v.as_str().unwrap().to_string())
            .collect();
        assert!(
            included.contains(&"20260917T164346Z_dedd707c_1_client".to_string()),
            "{included:?}"
        );
        assert!(
            included.contains(&"20260916T101010Z_menu_solo".to_string()),
            "{included:?}"
        );
        assert!(
            meta["dropped"].as_array().unwrap().is_empty(),
            "{}",
            built.meta
        );
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
            launcher_started_utc: None,
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

    // ---- dist LA9: the whole logs tree, not one session dir -----------------------------------

    /// LA9's done_when, first clause: a report built after TWO matches in one launcher run
    /// contains both session directories, the process directory they hang off (found via each
    /// session's own `session.json` `process_dir` field), and a crash marker sitting loose in
    /// `logs\` -- planted rather than raised through the live crash channel, the shape a marker
    /// from an EARLIER, undrained crash would take.
    #[test]
    fn two_matches_in_one_run_ship_both_sessions_the_process_dir_and_a_marker() {
        let dir = std::env::temp_dir().join("mh_launcher_test_two_matches");
        std::fs::remove_dir_all(&dir).ok();
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(dir.join("mh.exe"), b"not really an exe").unwrap();

        let proc_name = "20260918T090000Z_menu_host";
        let proc_dir = dir.join("logs").join(proc_name);
        std::fs::create_dir_all(&proc_dir).unwrap();
        std::fs::write(proc_dir.join("mh_harness.log"), "; process-level lines\n").unwrap();

        let s1_name = "20260918T090100Z_aaaaaaaa_1_host";
        let s1 = dir.join("logs").join(s1_name);
        std::fs::create_dir_all(&s1).unwrap();
        std::fs::write(
            s1.join("mh_net.log"),
            "; [session] match_id=1111aaaa1111aaaa1111aaaa1111aaaa\n",
        )
        .unwrap();
        std::fs::write(
            s1.join("session.json"),
            format!(
                "{{\n  \"match_id\": \"1111aaaa1111aaaa1111aaaa1111aaaa\",\n  \
                 \"process_dir\": \"{proc_name}\"\n}}\n"
            ),
        )
        .unwrap();

        let s2_name = "20260918T091500Z_bbbbbbbb_1_host";
        let s2 = dir.join("logs").join(s2_name);
        std::fs::create_dir_all(&s2).unwrap();
        std::fs::write(
            s2.join("mh_net.log"),
            "; [session] match_id=2222bbbb2222bbbb2222bbbb2222bbbb\n",
        )
        .unwrap();
        std::fs::write(
            s2.join("session.json"),
            format!(
                "{{\n  \"match_id\": \"2222bbbb2222bbbb2222bbbb2222bbbb\",\n  \
                 \"process_dir\": \"{proc_name}\"\n}}\n"
            ),
        )
        .unwrap();

        // A crash marker sitting directly under logs\, the shape crash.rs's Channel::create
        // writes -- planted rather than raised, so this proves the SWEEP, not the live channel.
        std::fs::write(
            dir.join("logs").join("mh_crash_deadbeef12345678.marker"),
            "mh_crash=1\ncode=0xc0000005\nmodule=mh.dll\noffset=0x000175b0\n\
             match_id=2222bbbb2222bbbb2222bbbb2222bbbb\nbuild=0.1.0-rc1+abc12345\n\
             when=20260918T091600Z\nctx=0\n",
        )
        .unwrap();

        let zip = dir.join("out").join("report.zip");
        let input = Input {
            game_dir: Some(&dir),
            session_dir: Some(s2.clone()), // the description form named the SECOND match
            launcher_started_utc: Some("20260918T085900Z".to_string()),
            description: "second match desynced right after the first one ended",
            last_run: None,
            crash: None,
            minidump: None,
            launcher_log: None,
        };
        let built = build(&zip, &input).unwrap();
        let names: Vec<String> = entries_of(&zip).into_iter().map(|(n, _)| n).collect();

        assert!(
            names
                .iter()
                .any(|n| n.starts_with(&format!("logs/{s1_name}/"))),
            "{names:?}"
        );
        assert!(
            names
                .iter()
                .any(|n| n.starts_with(&format!("logs/{s2_name}/"))),
            "{names:?}"
        );
        assert!(
            names
                .iter()
                .any(|n| n.starts_with(&format!("logs/{proc_name}/"))),
            "the process directory did not make it in: {names:?}"
        );
        assert!(
            names.contains(&"crash/mh_crash_deadbeef12345678.marker".to_string()),
            "{names:?}"
        );

        let meta: serde_json::Value = serde_json::from_str(&built.meta).unwrap();
        let included: Vec<String> = meta["included"]
            .as_array()
            .unwrap()
            .iter()
            .map(|v| v.as_str().unwrap().to_string())
            .collect();
        assert!(included.contains(&s1_name.to_string()), "{included:?}");
        assert!(included.contains(&s2_name.to_string()), "{included:?}");
        assert!(included.contains(&proc_name.to_string()), "{included:?}");
        assert_eq!(meta["session_dir"].as_str().unwrap(), s2_name);

        std::fs::remove_dir_all(&dir).ok();
    }

    /// LA9's done_when, second/third clauses: a `logs\` tree bigger than the budget drops the
    /// OLDEST directories first, lists each in `dropped` with its size and a reason, keeps the
    /// newest directory and a crash marker regardless, and the resulting zip itself lands under
    /// the true 64 MB Caddy edge cap (plan D14) -- "a report over the cap is still accepted"
    /// means the SELECTION keeps the report under it, not that an oversize upload is tolerated.
    #[test]
    fn an_oversize_logs_tree_drops_the_oldest_dirs_first_and_the_zip_stays_under_the_edge_cap() {
        let dir = std::env::temp_dir().join("mh_launcher_test_oversize_logs");
        std::fs::remove_dir_all(&dir).ok();
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(dir.join("mh.exe"), b"not really an exe").unwrap();

        // Ten session directories, 8 MB apiece (two ~4 MB files, so neither alone brushes
        // PER_FILE_MAX) -- 80 MB total, comfortably over the report's logs-tree budget.
        let chunk = "x".repeat(4 * 1024 * 1024);
        let mut names = Vec::new();
        for i in 0..10u32 {
            let name = format!("202609{:02}T090000Z_{:08x}_1_host", 10 + i, i);
            let s = dir.join("logs").join(&name);
            std::fs::create_dir_all(&s).unwrap();
            std::fs::write(s.join("a.log"), &chunk).unwrap();
            std::fs::write(s.join("b.log"), &chunk).unwrap();
            names.push(name);
        }
        // A tiny marker that must survive the cap regardless of everything above.
        std::fs::write(
            dir.join("logs").join("mh_crash_cafefeed00001111.marker"),
            "mh_crash=1\ncode=0xc0000005\n",
        )
        .unwrap();

        let newest = names.last().unwrap().clone();
        let oldest = names.first().unwrap().clone();
        let session_dir = dir.join("logs").join(&newest);

        let zip = dir.join("out").join("report.zip");
        let input = Input {
            game_dir: Some(&dir),
            session_dir: Some(session_dir),
            launcher_started_utc: None, // exercises the fallback window too
            description: "ran out of disk mid-afternoon, way too many matches",
            last_run: None,
            crash: None,
            minidump: None,
            launcher_log: None,
        };
        let built = build(&zip, &input).unwrap();

        let meta: serde_json::Value = serde_json::from_str(&built.meta).unwrap();
        let included: Vec<String> = meta["included"]
            .as_array()
            .unwrap()
            .iter()
            .map(|v| v.as_str().unwrap().to_string())
            .collect();
        let dropped: Vec<serde_json::Value> = meta["dropped"].as_array().unwrap().clone();

        assert!(included.contains(&newest), "{included:?}");
        assert!(!dropped.is_empty(), "{}", built.meta);
        assert!(
            dropped
                .iter()
                .any(|d| d["dir"].as_str() == Some(oldest.as_str())),
            "the oldest directory was not among the dropped: {dropped:?}"
        );
        for d in &dropped {
            assert!(d["bytes"].as_u64().unwrap() > 0, "{d:?}");
            assert!(!d["why"].as_str().unwrap().is_empty(), "{d:?}");
        }

        let names_in_zip: Vec<String> = entries_of(&zip).into_iter().map(|(n, _)| n).collect();
        assert!(
            names_in_zip
                .iter()
                .any(|n| n.starts_with(&format!("logs/{newest}/"))),
            "{names_in_zip:?}"
        );
        assert!(
            !names_in_zip
                .iter()
                .any(|n| n.starts_with(&format!("logs/{oldest}/"))),
            "the dropped directory leaked into the zip anyway: {names_in_zip:?}"
        );
        assert!(
            names_in_zip.contains(&"crash/mh_crash_cafefeed00001111.marker".to_string()),
            "the crash marker did not survive the cap: {names_in_zip:?}"
        );

        // The whole point of the cap: the ACTUAL zip stays under the real Caddy edge limit.
        assert!(
            built.bytes < 64 * 1024 * 1024,
            "the zip itself is {} bytes -- over the 64 MB edge cap",
            built.bytes
        );

        std::fs::remove_dir_all(&dir).ok();
    }

    /// LA10's own done_when, third clause ("the report (LA9) finds it"): when a stamped directory
    /// name did not fit `CreateDirectory`'s ceiling, `run_context.cpp`'s `make_dir()` degrades to
    /// writing every stream loose into the bare `logs\` root instead of a subdirectory -- something
    /// `list_log_dirs` (a directory listing) cannot see at all. The report still has to carry it.
    #[test]
    fn a_degraded_run_with_loose_files_directly_in_logs_is_still_found() {
        let dir = std::env::temp_dir().join("mh_launcher_test_loose_logs");
        std::fs::remove_dir_all(&dir).ok();
        std::fs::create_dir_all(dir.join("logs")).unwrap();
        std::fs::write(dir.join("mh.exe"), b"not really an exe").unwrap();

        // The LA10 degraded path: streams sitting DIRECTLY in `logs\`, no subdirectory at all.
        std::fs::write(
            dir.join("logs").join("mh_net.log"),
            "; [build] mh 0.1.0-rc1+abc12345\n; a run with no room for its own directory\n",
        )
        .unwrap();
        std::fs::write(
            dir.join("logs").join("mh_capture.log"),
            "frame 1\nframe 2\n",
        )
        .unwrap();
        // A crash marker in the same degraded run must still go through the OTHER sweep, not this
        // one -- proving the two do not double-count or clash.
        std::fs::write(
            dir.join("logs").join("mh_crash_baadf00d00000001.marker"),
            "mh_crash=1\ncode=0xc0000005\n",
        )
        .unwrap();

        let zip = dir.join("out").join("report.zip");
        let input = Input {
            game_dir: Some(&dir),
            session_dir: None, // no session ever opened -- the whole point of the degraded case
            launcher_started_utc: None,
            description: "logs folder looked empty but the game clearly ran",
            last_run: None,
            crash: None,
            minidump: None,
            launcher_log: None,
        };
        let built = build(&zip, &input).unwrap();
        let names: Vec<String> = entries_of(&zip).into_iter().map(|(n, _)| n).collect();

        // No session ever opened -> no `session_dir` for report.json to name.
        let meta: serde_json::Value = serde_json::from_str(&built.meta).unwrap();
        assert_eq!(meta["session_dir"].as_str().unwrap(), "");

        assert!(
            names.contains(&"logs/_root/mh_net.log".to_string()),
            "{names:?}"
        );
        assert!(
            names.contains(&"logs/_root/mh_capture.log".to_string()),
            "{names:?}"
        );
        assert!(
            names.contains(&"crash/mh_crash_baadf00d00000001.marker".to_string()),
            "the marker went missing when it should ride the OTHER sweep: {names:?}"
        );
        // The loose marker file must not ALSO be duplicated under logs/_root/.
        assert!(
            !names.contains(&"logs/_root/mh_crash_baadf00d00000001.marker".to_string()),
            "{names:?}"
        );

        let entries = entries_of(&zip);
        let net_log = &entries
            .iter()
            .find(|(n, _)| n == "logs/_root/mh_net.log")
            .unwrap()
            .1;
        assert!(String::from_utf8_lossy(net_log).contains("no room for its own directory"));

        std::fs::remove_dir_all(&dir).ok();
    }
}
