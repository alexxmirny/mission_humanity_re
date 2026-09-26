//! Building the report zip (dist LA4, plan decision D13; scope widened by dist LA9).
//!
//! A report is one deflate zip and one required sentence. What goes in it:
//!
//! ```text
//! report.json            everything a triager needs before opening anything else
//! description.txt        what the player typed. REQUIRED, and the build refuses without it
//! logs/<name>/...        EVERY process + session directory since the launcher started (dist LA9)
//! config/mh_net.ini      the game's configuration, REDACTED
//! launcher/launcher.log  this program's own log (its newest part, if the budget must cut it)
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
//! ## dist LA14: the budget is the COMPRESSED upload body, not the input
//!
//! Until LA14 every file was capped at 8 MB (text tailed, a larger binary left out) and the whole
//! report at 48 MB of UNCOMPRESSED input. A long match's logs deflate ~20:1, so that threw away
//! most of what would have fit -- and the game had started rotating its per-match files to fit the
//! per-file cap. Now no single file is capped. Every entry is deflated once into a staging zip,
//! and the real upload body (the zip plus `description` and `meta` sent again as multipart fields)
//! must fit `BODY_BUDGET` = the 64,000,000-byte Caddy edge cap minus a 2 MiB margin. When it does
//! not, the packer gives up the least important material first (`collect` has the order): optional
//! folders oldest first (whole), then the other protected folders, the loose root files and the
//! launcher log (cut file by file), then the minidump (whole), and last the reported match itself.
//! Inside a cut folder: plain binaries go first (whole), text logs keep their NEWEST part down to a
//! floor, replay inputs go next -- whole, never cut -- and only then the text below the floor.
//! Every folder, file and cut log given up is named in `report.json`'s `dropped`.
//!
//! **`report.json` IS the `meta` object RP1 will POST**, byte for byte, not a cousin of it. The
//! collector stores that object as `meta.json` beside the zip (`src/collector/README.md`), and
//! `tools/crash_report.py --report` reads `match_id`, `version`, `exit_code` and the optional
//! `crash` object out of it. Writing one object and sending the same one is what keeps the drained
//! report and the zip's own copy from ever disagreeing -- and it means the uploader in RP1 has no
//! field names of its own to get wrong. dist LA9 adds `included` (every `logs\` directory the zip
//! actually carries) and `dropped` (`{dir, bytes, why}` for every one the size budget refused;
//! dist LA14 adds `{dir, file, bytes, why}` rows for single files, with `kept_bytes` on a cut log).
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

/// dist LA14. The edge cap on the upload BODY. Caddy's `request_body max_size 64MB` (plan D14,
/// `src/collector/Caddyfile`) is parsed by go-humanize, where `MB` is 10^6 -- 64,000,000 bytes.
/// The collector's own `MAX_BODY_MB=64` is 64 MiB, the larger reading, so the edge is the limit
/// that bites. A report refused on arrival (413) is worse than one missing its oldest folder.
const EDGE_CAP_BYTES: u64 = 64_000_000;

/// dist LA14. Headroom kept under `EDGE_CAP_BYTES`, for what the estimate below cannot see: a
/// proxy's own framing, a future field in the POST.
const SAFETY_MARGIN: u64 = 2 * 1024 * 1024;

/// dist LA14. What the whole upload body may weigh: the zip, plus `description` and `meta` sent a
/// second time as their own multipart fields (`upload.rs`'s `multipart`), plus that framing. The
/// budget is checked against the ACTUAL zip bytes after it is written, not against uncompressed
/// input -- an hour of text logs deflates ~20:1, so a cap on input threw away most of what fits.
pub const BODY_BUDGET: u64 = EDGE_CAP_BYTES - SAFETY_MARGIN;

/// Upper bound for `upload.rs`'s multipart framing: three boundaries, three part headers, the
/// closing boundary. About 400 bytes today.
const MULTIPART_FRAMING: u64 = 2048;

/// Per-entry zip structure (local header + central-directory record + data descriptor + extra
/// fields), excluding the entry name, which is counted twice on top.
const ENTRY_OVERHEAD: u64 = 160;

/// Slack for `report.json` growing between the estimate and the write (more `dropped` rows).
const META_SLACK: u64 = 64 * 1024;

/// dist LA14. When a text log has to be cut, it first keeps at least this many COMPRESSED bytes of
/// its tail (~5 MB of log text) while replay inputs and other binaries in the same folder are still
/// there to give up. Only below that do the replay inputs go, and then the text itself.
const TEXT_TAIL_FLOOR: u64 = 256 * 1024;

/// A cut tail shorter than this is not evidence, it is noise: drop the file instead.
const TEXT_TAIL_MIN: u64 = 4 * 1024;

/// dist LA14. Optional (not protected) folders are compressed into the staging zip newest first
/// only while their UNCOMPRESSED total stays under this. It bounds the build time on a `logs\`
/// tree full of old runs: past it, what would be dropped anyway is not compressed first.
const STAGE_UNCOMPRESSED_MAX: u64 = 512 * 1024 * 1024;

/// How many times the zip is rewritten to converge on `BODY_BUDGET`. Each rewrite copies the
/// already-compressed entries raw; only cut tails are compressed again.
const FIT_ATTEMPTS: usize = 6;

/// The replay inputs (`mp:SES7` match segment + the process recording). A cut replay input is
/// not replayable, so these are kept whole or left out whole -- never tailed.
const REPLAY_INPUTS: [&str; 6] = [
    "mh_match_orders.bin",
    "mh_match_clock.bin",
    "mh_match_seed.bin",
    "mh_orders.bin",
    "mh_clock.bin",
    "mh_harness_seed.bin",
];

const WHY_OVER_BUDGET: &str = "over the report upload budget (oldest folders go first)";
const WHY_NOT_STAGED: &str = "past the report's staging limit (older than what could fit)";
const WHY_FILE_DROPPED: &str = "left out whole to fit the report upload budget";
const WHY_FILE_TAILED: &str = "only the newest part kept to fit the report upload budget";

/// dist LA9. How many of the newest `logs\` directories to package when the launcher's own start
/// time is unknown (a `--report` invoked from a script that never called `--launch` in this same
/// process, or a test). Large enough to cover "the whole afternoon" of a normal play session without
/// degenerating into "every directory this game folder has ever produced".
const LOGS_FALLBACK_N: usize = 20;

/// What a report is built from. Everything is optional except the description, which is the point.
pub struct Input<'a> {
    pub game_dir: Option<&'a Path>,
    /// dist LA13: the directory the session directories live in -- the launcher-owned root
    /// (`Layout::game_log_root`) for a game the launcher started. `None` falls back to the game's
    /// own `<game_dir>\logs\`, which is where a hand-launched game writes.
    pub logs_root: Option<PathBuf>,
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

impl Input<'_> {
    /// Where the `logs\` tree this report packages is (dist LA13).
    fn logs_root(&self) -> Option<PathBuf> {
        self.logs_root
            .clone()
            .or_else(|| self.game_dir.map(|g| g.join("logs")))
    }
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

/// Build the zip at `dest`, fitted to `BODY_BUDGET`.
pub fn build(dest: &Path, input: &Input) -> Result<Built, String> {
    build_with_budget(dest, input, BODY_BUDGET)
}

/// `build` with the body budget spelled out, so a test can exercise the trimming order on a few
/// megabytes instead of generating 70 MB of incompressible data for every case.
fn build_with_budget(dest: &Path, input: &Input, body_budget: u64) -> Result<Built, String> {
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

    // dist LA9: which `logs\` directories are candidates. Since dist LA14 this decides SCOPE only
    // (the launcher-start window, what is protected); what the size budget costs is decided below,
    // on compressed bytes, after everything is staged.
    let logs_root = input.logs_root();
    let pool = logs_root
        .as_deref()
        .map(|lr| {
            plan_logs(
                lr,
                session.as_deref(),
                input.launcher_started_utc.as_deref(),
            )
        })
        .unwrap_or_default();

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
    let description = input.description.trim().to_string();

    let mut set = collect(
        input,
        logs_root.as_deref(),
        &pool,
        ini_text.as_deref(),
        &scrub,
    );

    let opts: zip::write::SimpleFileOptions = zip::write::SimpleFileOptions::default()
        .compression_method(zip::CompressionMethod::Deflated);
    let staging = dest.with_extension("staging");
    let result = (|| -> Result<Built, String> {
        stage(&mut set, &staging, opts, &scrub)?;
        let meta_for = |set: &Set| {
            meta_json(
                input,
                &machine,
                installed.as_ref(),
                &build_stamp,
                &match_id,
                &set.included(),
                &set.dropped_records(),
            )
        };
        let desc_len = description.len() as u64;
        let mut correction = 0u64;
        let mut meta = String::new();
        let mut entries = Vec::new();
        let mut bytes = 0u64;
        for attempt in 0..FIT_ATTEMPTS {
            let meta_now = meta_for(&set);
            let estimate = set.estimate_zip(meta_now.len() as u64 + META_SLACK)
                + meta_now.len() as u64
                + desc_len
                + MULTIPART_FRAMING
                + correction;
            if estimate > body_budget {
                let left = set.shed(estimate - body_budget);
                if left > 0 {
                    log::line(format!(
                        "report: {left} bytes over the upload budget with nothing left to trim"
                    ));
                }
            }
            set.materialize_tails(opts, &scrub)?;
            meta = meta_for(&set);
            entries = write_final(dest, &staging, &set, &meta, opts)?;
            bytes = std::fs::metadata(dest).map(|m| m.len()).unwrap_or(0);
            let body = bytes + meta.len() as u64 + desc_len + MULTIPART_FRAMING;
            if body <= body_budget {
                break;
            }
            log::line(format!(
                "report: attempt {} came to {body} bytes of upload body, over {body_budget}; \
                 trimming again",
                attempt + 1
            ));
            correction += body - body_budget + 64 * 1024;
        }
        Ok(Built {
            zip: dest.to_path_buf(),
            entries,
            bytes,
            meta: std::mem::take(&mut meta),
        })
    })();
    std::fs::remove_file(&staging).ok();
    let built = result?;
    log::line(format!(
        "report: {} -- {} entries, {} bytes",
        dest.display(),
        built.entries.len(),
        built.bytes
    ));
    Ok(built)
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
    included: &[String],
    dropped: &[serde_json::Value],
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
        // every one the size budget refused, with its size and why. dist LA14: `dropped` also
        // names single FILES -- `{dir, file, bytes, why}` for one left out whole, plus
        // `kept_bytes` for a log whose newest part only was kept.
        "included": included,
        "dropped": dropped,
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

/// How much a `logs\` directory matters to this report -- the order the budget gives them up in.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum DirRole {
    /// The match the description form names. Given up last, and never whole.
    Reported,
    /// The newest session and each protected session's own process directory (LA9). Trimmed file
    /// by file after every optional directory is gone, never dropped whole.
    Protected,
    /// Everything else in the window. Dropped whole, OLDEST first.
    Optional,
}

/// One candidate directory: its name, its on-disk size, and its role.
#[derive(Clone, Debug)]
struct PoolDir {
    name: String,
    bytes: u64,
    role: DirRole,
}

/// Decide which `logs\` directories are CANDIDATES for a report (dist LA9), newest first.
///
/// `protected_session` is the match the report is ABOUT -- the player's pick, or the newest by
/// default. It, the process directory its own `session.json` names, and the NEWEST session overall
/// (a player reporting an OLDER match should not lose the freshest evidence next to it) are never
/// dropped whole. Everything else is a candidate only if its own stamp is at or after `since_utc`
/// (the launcher's own start), or -- when that is unknown -- among the newest `LOGS_FALLBACK_N`
/// directories. What the size budget costs is NOT decided here any more (dist LA14): it is decided
/// on compressed bytes once everything is staged -- see `Set::shed`.
fn plan_logs(
    logs_root: &Path,
    protected_session: Option<&Path>,
    since_utc: Option<&str>,
) -> Vec<PoolDir> {
    let dirs = list_log_dirs(logs_root); // newest first
    if dirs.is_empty() {
        return Vec::new();
    }
    let by_name: std::collections::HashMap<&str, &LogDir> =
        dirs.iter().map(|d| (d.name.as_str(), d)).collect();

    let reported: Option<String> = protected_session
        .and_then(|p| p.file_name())
        .map(|n| n.to_string_lossy().to_string())
        .filter(|n| by_name.contains_key(n.as_str()));

    let mut protected: std::collections::BTreeSet<String> = std::collections::BTreeSet::new();
    if let Some(r) = &reported {
        protected.insert(r.clone());
    }
    if let Some(newest_session) = dirs.iter().find(|d| !is_process_dir_name(&d.name)) {
        protected.insert(newest_session.name.clone());
    }
    // Each protected session's own process directory rides along -- LA9's done_when "contains both
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
    let in_window: Vec<&LogDir> = match since_utc {
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
    let mut pool: Vec<PoolDir> = in_window
        .into_iter()
        .map(|d| PoolDir {
            name: d.name.clone(),
            bytes: d.bytes,
            role: if reported.as_deref() == Some(d.name.as_str()) {
                DirRole::Reported
            } else if protected.contains(&d.name) {
                DirRole::Protected
            } else {
                DirRole::Optional
            },
        })
        .collect();
    pool.sort_by(|a, b| b.name.cmp(&a.name));
    pool
}

/// Every file `dir` holds, one level of subdirectories included (a session directory is flat
/// today, and a recursion that went arbitrarily deep would be a way to ship a whole game folder by
/// accident), as `(path, name relative to dir with '/' separators)` in a stable order.
fn dir_files(dir: &Path) -> Vec<(PathBuf, String)> {
    let mut out = Vec::new();
    let Ok(read) = std::fs::read_dir(dir) else {
        log::line(format!("report: cannot list {}", dir.display()));
        return out;
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
    let leaf = |p: &Path| {
        p.file_name()
            .unwrap_or_default()
            .to_string_lossy()
            .to_string()
    };
    for p in files {
        let n = leaf(&p);
        out.push((p, n));
    }
    for d in subdirs {
        let Ok(read) = std::fs::read_dir(&d) else {
            continue;
        };
        let mut inner: Vec<PathBuf> = read
            .flatten()
            .map(|e| e.path())
            .filter(|p| p.is_file())
            .collect();
        inner.sort();
        let dn = leaf(&d);
        for p in inner {
            let n = format!("{dn}/{}", leaf(&p));
            out.push((p, n));
        }
    }
    out
}

/// Plain files directly under `logs\`, split into `(loose, crash)`: `mh_crash_*` markers (and
/// their `.ctx32` sidecars) on one side, everything else on the other. The loose ones are dist
/// LA10's degraded path -- when a stamped directory name does not fit `CreateDirectory`'s real
/// ceiling, `run_context.cpp`'s `make_dir()` writes every stream loose into the bare `logs\` root,
/// which `list_log_dirs` (a directory listing) cannot see at all.
fn root_files(logs_root: &Path) -> (Vec<PathBuf>, Vec<PathBuf>) {
    let Ok(read) = std::fs::read_dir(logs_root) else {
        return (Vec::new(), Vec::new());
    };
    let mut files: Vec<PathBuf> = read
        .flatten()
        .map(|e| e.path())
        .filter(|p| p.is_file())
        .collect();
    files.sort();
    files.into_iter().partition(|p| {
        !p.file_name()
            .map(|n| n.to_string_lossy().starts_with("mh_crash_"))
            .unwrap_or(false)
    })
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
/// directory close to it, so this rarely recurses more than once, but sizing (unlike `dir_files`'s own
/// one-level cap on what it WRITES) has no reason to under-count a directory shaped differently than
/// expected -- the worst that happens is this directory looks bigger than `dir_files` will actually
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
pub fn session_dirs(logs_root: &Path) -> Vec<PathBuf> {
    list_log_dirs(logs_root)
        .into_iter()
        .filter(|d| !is_process_dir_name(&d.name))
        .map(|d| d.path)
        .collect()
}

// ---- dist LA14: packing against the COMPRESSED upload body -------------------------------------
//
// The pipeline, once per report:
//
//   1. `collect` lists every entry the report could carry, each in a GROUP (a `logs\` folder, the
//      loose root files, the launcher log, the minidump, the fixed entries). No size limit on any
//      single file; `DENY` still applies.
//   2. `stage` deflates every entry ONCE into a staging zip beside `dest` and reads back each
//      entry's compressed size.
//   3. `Set::shed` takes the bytes the estimate is over budget by and gives them up in the groups'
//      shed order, least important first (see `collect`). A folder that is Optional goes whole;
//      a protected one is trimmed file by file (`shrink_group`).
//   4. `write_final` writes `report.json` and then copies every kept entry RAW out of the staging
//      zip -- no second compression -- plus the few cut tails, compressed once each.
//   5. The real zip size decides. Over budget: shed the overshoot and rewrite (`FIT_ATTEMPTS`).

/// What an entry is, for the purpose of cutting it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Kind {
    /// A log or other text file: scrubbed, and cut from the FRONT when it must shrink.
    Text,
    /// Any other binary: whole or absent.
    Binary,
    /// A replay input (`REPLAY_INPUTS`): whole or absent, and given up after the text is cut.
    Replay,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Fate {
    Whole,
    /// Keep the newest part, at most this many compressed bytes.
    Tail(u64),
    Dropped,
}

enum Source {
    File(PathBuf),
    Text(String),
}

/// A cut tail, compressed: a one-entry zip whose entry is copied raw into the report.
struct TailBuf {
    target: u64,
    zip: Vec<u8>,
    kept: u64,
}

struct Item {
    name: String,
    source: Source,
    kind: Kind,
    group: usize,
    /// Uncompressed bytes as staged (after the scrub).
    raw: u64,
    /// Compressed bytes in the staging zip.
    comp: u64,
    /// Index in the staging zip; `None` if it could not be read and is not in the report at all.
    staged: Option<usize>,
    fate: Fate,
    tail: Option<TailBuf>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Mode {
    /// Never given up: report.json's companions, the ini, the crash markers.
    Fixed,
    /// Given up whole (an Optional folder, the minidump).
    DropWhole,
    /// Given up file by file (`shrink_group`).
    Shrink,
}

struct Group {
    /// The `logs\` folder name, for a folder group.
    dir: Option<String>,
    mode: Mode,
    /// On-disk bytes of the folder (for `dropped`).
    bytes: u64,
    /// Set when the whole group went; the reason `dropped` gives.
    dropped_why: Option<&'static str>,
}

#[derive(Default)]
struct Set {
    items: Vec<Item>,
    groups: Vec<Group>,
    /// Group indices, least important first: the order `shed` gives them up in.
    shed_order: Vec<usize>,
    /// The `logs\` folders in the order they are written (for `included`).
    folders: Vec<usize>,
}

fn entry_cost(it: &Item) -> u64 {
    let size = match it.fate {
        Fate::Whole => it.comp,
        Fate::Tail(t) => t,
        Fate::Dropped => return 0,
    };
    size + ENTRY_OVERHEAD + 2 * it.name.len() as u64
}

fn live_size(it: &Item) -> u64 {
    match it.fate {
        Fate::Whole => it.comp,
        Fate::Tail(t) => t,
        Fate::Dropped => 0,
    }
}

impl Set {
    fn group(&mut self, dir: Option<String>, mode: Mode, bytes: u64) -> usize {
        self.groups.push(Group {
            dir,
            mode,
            bytes,
            dropped_why: None,
        });
        self.groups.len() - 1
    }

    fn push(&mut self, group: usize, name: String, source: Source, kind: Kind) {
        self.items.push(Item {
            name,
            source,
            kind,
            group,
            raw: 0,
            comp: 0,
            staged: None,
            fate: Fate::Whole,
            tail: None,
        });
    }

    fn text(&mut self, group: usize, name: &str, text: String) {
        self.push(group, name.to_string(), Source::Text(text), Kind::Text);
    }

    /// Add a file, unless `DENY` names it or it is not a file.
    fn file(&mut self, group: usize, src: &Path, name: String) {
        let leaf = src
            .file_name()
            .map(|n| n.to_string_lossy().to_ascii_lowercase())
            .unwrap_or_default();
        if DENY.iter().any(|d| *d == leaf) {
            log::line(format!("report: {leaf} is never included in a report"));
            return;
        }
        if !src.is_file() {
            return;
        }
        let kind = if is_text(&name) {
            Kind::Text
        } else if REPLAY_INPUTS.contains(&leaf.as_str()) {
            Kind::Replay
        } else {
            Kind::Binary
        };
        self.push(group, name, Source::File(src.to_path_buf()), kind);
    }

    /// The zip's size if written now: every live entry plus `report.json` at `meta_upper` bytes
    /// (stored uncompressed as an upper bound) plus the end-of-central-directory records.
    fn estimate_zip(&self, meta_upper: u64) -> u64 {
        let entries: u64 = self.items.iter().map(entry_cost).sum();
        entries + meta_upper + ENTRY_OVERHEAD + 2 * "report.json".len() as u64 + 128
    }

    /// Give up `excess` bytes, least important group first. Returns what could NOT be freed.
    fn shed(&mut self, mut excess: u64) -> u64 {
        for gi in self.shed_order.clone() {
            if excess == 0 {
                break;
            }
            match self.groups[gi].mode {
                Mode::Fixed => {}
                Mode::DropWhole => {
                    let freed: u64 = self
                        .items
                        .iter()
                        .filter(|i| i.group == gi)
                        .map(entry_cost)
                        .sum();
                    if freed == 0 {
                        continue;
                    }
                    for it in self.items.iter_mut().filter(|i| i.group == gi) {
                        it.fate = Fate::Dropped;
                    }
                    self.groups[gi].dropped_why.get_or_insert(WHY_OVER_BUDGET);
                    excess = excess.saturating_sub(freed);
                }
                Mode::Shrink => excess = shrink_group(&mut self.items, gi, excess),
            }
        }
        excess
    }

    /// Compress every cut tail whose target changed since it was last compressed.
    fn materialize_tails(
        &mut self,
        opts: zip::write::SimpleFileOptions,
        scrub: &Scrub,
    ) -> Result<(), String> {
        for it in self.items.iter_mut() {
            let Fate::Tail(target) = it.fate else {
                it.tail = None;
                continue;
            };
            if it.tail.as_ref().map(|t| t.target) == Some(target) {
                continue;
            }
            it.tail = None;
            let Source::File(p) = &it.source else {
                continue; // only file text is ever in a Shrink group; nothing to cut
            };
            let text = read_scrubbed(p, scrub);
            let total = text.len() as u64;
            let ratio = target as f64 / it.comp.max(1) as f64;
            let mut keep = (total as f64 * ratio * 0.97) as u64;
            for _ in 0..6 {
                keep = keep.min(total);
                let (body, kept) = tail_of(&text, keep);
                let (zip, comp) = compress_one(&it.name, body.as_bytes(), opts)?;
                if comp <= target {
                    it.tail = Some(TailBuf { target, zip, kept });
                    break;
                }
                keep = (keep as f64 * (target as f64 / comp as f64) * 0.95) as u64;
            }
            if it.tail.is_none() {
                it.fate = Fate::Dropped; // could not get under the target: whole, never garbage
            }
        }
        Ok(())
    }

    /// The `logs\` folders the zip carries (not given up whole), in write order.
    fn included(&self) -> Vec<String> {
        self.folders
            .iter()
            .filter(|&&g| self.groups[g].dropped_why.is_none())
            .filter_map(|&g| self.groups[g].dir.clone())
            .collect()
    }

    /// `report.json`'s `dropped`: every folder given up whole, every single file left out, and
    /// every log cut to its newest part -- so the recipient knows what the report does NOT hold.
    fn dropped_records(&self) -> Vec<serde_json::Value> {
        let mut out = Vec::new();
        for &gi in &self.shed_order {
            let g = &self.groups[gi];
            if let (Some(why), Some(dir)) = (g.dropped_why, g.dir.as_ref()) {
                out.push(serde_json::json!({"dir": dir, "bytes": g.bytes, "why": why}));
                continue;
            }
            for it in self.items.iter().filter(|i| i.group == gi) {
                if it.staged.is_none() {
                    continue;
                }
                let dir = g.dir.clone().unwrap_or_default();
                match it.fate {
                    Fate::Whole => {}
                    Fate::Dropped => out.push(serde_json::json!({
                        "dir": dir, "file": it.name, "bytes": it.raw, "why": WHY_FILE_DROPPED,
                    })),
                    Fate::Tail(_) => out.push(serde_json::json!({
                        "dir": dir,
                        "file": it.name,
                        "bytes": it.raw,
                        "kept_bytes": it.tail.as_ref().map(|t| t.kept).unwrap_or(0),
                        "why": WHY_FILE_TAILED,
                    })),
                }
            }
        }
        out
    }
}

/// Shrink one protected group by `excess` bytes, in this order: (a) plain binaries, largest
/// first, whole; (b) text logs cut from the front, largest first, down to `TEXT_TAIL_FLOOR` each
/// (a water level -- small logs stay whole); (c) replay inputs, whole; (d) text below the floor,
/// down to nothing. Returns what is still over.
fn shrink_group(items: &mut [Item], gi: usize, excess: u64) -> u64 {
    let excess = drop_largest(items, gi, Kind::Binary, excess);
    let excess = water_fill(items, gi, TEXT_TAIL_FLOOR, excess);
    let excess = drop_largest(items, gi, Kind::Replay, excess);
    water_fill(items, gi, 0, excess)
}

fn drop_largest(items: &mut [Item], gi: usize, kind: Kind, mut excess: u64) -> u64 {
    let mut idx: Vec<usize> = (0..items.len())
        .filter(|&i| {
            items[i].group == gi && items[i].kind == kind && items[i].fate != Fate::Dropped
        })
        .collect();
    idx.sort_by_key(|&i| std::cmp::Reverse(items[i].comp));
    for i in idx {
        if excess == 0 {
            break;
        }
        let c = entry_cost(&items[i]);
        items[i].fate = Fate::Dropped;
        excess = excess.saturating_sub(c);
    }
    excess
}

/// Lower every text entry of group `gi` above a common level L (L >= `floor`) to L, choosing the
/// highest L that frees `excess` -- or as much as the floor allows. An entry cut to under
/// `TEXT_TAIL_MIN` is dropped instead.
fn water_fill(items: &mut [Item], gi: usize, floor: u64, excess: u64) -> u64 {
    if excess == 0 {
        return 0;
    }
    let idx: Vec<usize> = (0..items.len())
        .filter(|&i| {
            items[i].group == gi && items[i].kind == Kind::Text && items[i].fate != Fate::Dropped
        })
        .collect();
    let sizes: Vec<u64> = idx.iter().map(|&i| live_size(&items[i])).collect();
    let over = |level: u64| -> u64 { sizes.iter().map(|s| s.saturating_sub(level)).sum() };
    let reducible = over(floor);
    if reducible == 0 {
        return excess;
    }
    let want = excess.min(reducible);
    // The highest level whose cut still frees `want`: over(lo) >= want always, over(hi) < want.
    let (mut lo, mut hi) = (floor, sizes.iter().copied().max().unwrap_or(0));
    while hi - lo > 1 {
        let mid = lo + (hi - lo) / 2;
        if over(mid) >= want {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    let level = lo;
    let mut freed = 0u64;
    for (k, &i) in idx.iter().enumerate() {
        if sizes[k] <= level {
            continue;
        }
        if level < TEXT_TAIL_MIN {
            freed += entry_cost(&items[i]);
            items[i].fate = Fate::Dropped;
        } else {
            freed += sizes[k] - level;
            items[i].fate = Fate::Tail(level);
        }
    }
    excess.saturating_sub(freed)
}

/// A text file's content as it enters the report: lossily decoded and scrubbed line by line.
fn read_scrubbed(p: &Path, scrub: &Scrub) -> String {
    let raw = std::fs::read(p).unwrap_or_default();
    let text = String::from_utf8_lossy(&raw);
    let mut out = String::with_capacity(text.len() + 64);
    for l in text.lines() {
        out.push_str(&scrub.line(l));
        out.push('\n');
    }
    out
}

/// The newest `keep` bytes of `text`, started at a line boundary, behind a one-line notice saying
/// how much was left out. Returns the body and how many bytes of the original it kept.
fn tail_of(text: &str, keep: u64) -> (String, u64) {
    let len = text.len();
    let mut start = len - (keep as usize).min(len);
    while !text.is_char_boundary(start) {
        start += 1;
    }
    if start > 0 {
        if let Some(nl) = text[start..].find('\n') {
            if start + nl + 1 < len {
                start += nl + 1;
            }
        }
    }
    let kept = &text[start..];
    let body = format!(
        "; [report] the first {start} of {len} bytes of this log were left out to fit the report \
         upload budget -- the newest part is kept\n{kept}"
    );
    (body, kept.len() as u64)
}

/// Deflate one entry into a one-entry in-memory zip; returns the zip and the entry's compressed size.
fn compress_one(
    name: &str,
    data: &[u8],
    opts: zip::write::SimpleFileOptions,
) -> Result<(Vec<u8>, u64), String> {
    let mut w = zip::ZipWriter::new(std::io::Cursor::new(Vec::new()));
    w.start_file(name, opts)
        .map_err(|e| format!("cannot start {name}: {e}"))?;
    w.write_all(data)
        .map_err(|e| format!("cannot write {name}: {e}"))?;
    let bytes = w
        .finish()
        .map_err(|e| format!("cannot finish {name}: {e}"))?
        .into_inner();
    let mut a = zip::ZipArchive::new(std::io::Cursor::new(&bytes[..]))
        .map_err(|e| format!("cannot reread {name}: {e}"))?;
    let comp = a
        .by_index_raw(0)
        .map_err(|e| format!("cannot reread {name}: {e}"))?
        .compressed_size();
    Ok((bytes, comp))
}

/// Everything a report could carry, grouped, with the shed order decided (dist LA14):
///
///   1. Optional `logs\` folders, OLDEST first, each dropped whole.
///   2. Protected folders other than the reported match (the newest session when the player picked
///      an older one, the process folders), OLDEST first, trimmed file by file.
///   3. The loose files of LA10's degraded path (`logs/_root/`), trimmed.
///   4. The launcher's own log, trimmed.
///   5. The minidump, dropped whole (a cut dump opens in no debugger).
///   6. The reported match's own folder, trimmed -- the last thing given up.
///
/// Never given up: `report.json`, `description.txt`, the redacted ini, `crash/marker.txt` and every
/// swept `mh_crash_*` file (a few hundred bytes each).
fn collect(
    input: &Input,
    logs_root: Option<&Path>,
    pool: &[PoolDir],
    ini_text: Option<&str>,
    scrub: &Scrub,
) -> Set {
    let mut set = Set::default();
    let fixed = set.group(None, Mode::Fixed, 0);
    set.text(
        fixed,
        "description.txt",
        input.description.trim().to_string(),
    );

    // The folders, written reported first, then protected, then optional -- each newest first.
    let mut ordered: Vec<&PoolDir> = pool.iter().collect();
    ordered.sort_by_key(|d| match d.role {
        DirRole::Reported => 0,
        DirRole::Protected => 1,
        DirRole::Optional => 2,
    });
    let mut optional_staged = 0u64;
    let mut optional_groups = Vec::new(); // newest first
    let mut protected_groups = Vec::new(); // newest first
    let mut reported_group = None;
    if let Some(lr) = logs_root {
        for d in ordered {
            let mode = if d.role == DirRole::Optional {
                Mode::DropWhole
            } else {
                Mode::Shrink
            };
            let g = set.group(Some(d.name.clone()), mode, d.bytes);
            set.folders.push(g);
            match d.role {
                DirRole::Reported => reported_group = Some(g),
                DirRole::Protected => protected_groups.push(g),
                DirRole::Optional => {
                    optional_groups.push(g);
                    optional_staged += d.bytes;
                    if optional_staged > STAGE_UNCOMPRESSED_MAX {
                        set.groups[g].dropped_why = Some(WHY_NOT_STAGED);
                        continue;
                    }
                }
            }
            for (p, rel) in dir_files(&lr.join(&d.name)) {
                set.file(g, &p, format!("logs/{}/{rel}", d.name));
            }
        }
    }

    let loose = set.group(None, Mode::Shrink, 0);
    let mut crash_files = Vec::new();
    if let Some(lr) = logs_root {
        let (loose_files, crash) = root_files(lr);
        for p in loose_files {
            let leaf = p
                .file_name()
                .unwrap_or_default()
                .to_string_lossy()
                .to_string();
            set.file(loose, &p, format!("logs/_root/{leaf}"));
        }
        crash_files = crash;
    }

    // The configuration, redacted three times over: by setting name, by the relay's own line,
    // and by the scrub.
    if let Some(text) = ini_text {
        set.text(fixed, "config/mh_net.ini", redact_ini_with(text, scrub));
    }

    let launcher = set.group(None, Mode::Shrink, 0);
    if let Some(p) = input.launcher_log.as_deref() {
        set.file(launcher, p, "launcher/launcher.log".to_string());
    }

    if let Some(m) = input.crash {
        set.text(
            fixed,
            "crash/marker.txt",
            format!(
                "module={}\noffset=0x{:08x}\ncode=0x{:08x}\npid={}\ntid={}\nbuild={}\nwhen={}\n",
                m.module, m.offset, m.code, m.pid, m.tid, m.build, m.when
            ),
        );
    }

    // dist LA9: every `mh_crash_*` file sitting under `logs\`, not only the one THIS run's live
    // channel caught -- a marker (or its `.ctx32` sidecar) left by an earlier, undrained crash is
    // exactly the evidence a "something looked wrong later" report exists to carry. Fixed: never
    // the thing the budget sacrifices.
    for p in crash_files {
        let leaf = p
            .file_name()
            .unwrap_or_default()
            .to_string_lossy()
            .to_string();
        set.file(fixed, &p, format!("crash/{leaf}"));
    }

    let dump = set.group(None, Mode::DropWhole, 0);
    if let Some(dmp) = input.minidump {
        set.file(dump, dmp, "minidump.dmp".to_string());
    }

    set.shed_order.extend(optional_groups.iter().rev());
    set.shed_order.extend(protected_groups.iter().rev());
    set.shed_order.extend([loose, launcher, dump]);
    set.shed_order.extend(reported_group);
    set
}

/// Deflate every collected entry once into `staging`, then read back each one's compressed size.
fn stage(
    set: &mut Set,
    staging: &Path,
    opts: zip::write::SimpleFileOptions,
    scrub: &Scrub,
) -> Result<(), String> {
    let file = std::fs::File::create(staging)
        .map_err(|e| format!("cannot create {}: {e}", staging.display()))?;
    let mut zip = zip::ZipWriter::new(file);
    let mut n = 0usize;
    for it in set.items.iter_mut() {
        if set.groups[it.group].dropped_why.is_some() {
            it.fate = Fate::Dropped;
            continue;
        }
        let raw = match (&it.source, it.kind) {
            (Source::Text(t), _) => {
                zip.start_file(it.name.as_str(), opts)
                    .map_err(|e| format!("cannot start {} in the report: {e}", it.name))?;
                zip.write_all(t.as_bytes())
                    .map_err(|e| format!("cannot write {} into the report: {e}", it.name))?;
                t.len() as u64
            }
            (Source::File(p), Kind::Text) => {
                let text = read_scrubbed(p, scrub);
                zip.start_file(it.name.as_str(), opts)
                    .map_err(|e| format!("cannot start {} in the report: {e}", it.name))?;
                zip.write_all(text.as_bytes())
                    .map_err(|e| format!("cannot write {} into the report: {e}", it.name))?;
                text.len() as u64
            }
            (Source::File(p), _) => {
                let mut f = match std::fs::File::open(p) {
                    Ok(f) => f,
                    Err(e) => {
                        log::line(format!("report: cannot read {}: {e}", p.display()));
                        it.fate = Fate::Dropped;
                        continue;
                    }
                };
                zip.start_file(it.name.as_str(), opts)
                    .map_err(|e| format!("cannot start {} in the report: {e}", it.name))?;
                std::io::copy(&mut f, &mut zip)
                    .map_err(|e| format!("cannot write {} into the report: {e}", it.name))?
            }
        };
        it.raw = raw;
        it.staged = Some(n);
        n += 1;
    }
    zip.finish()
        .map_err(|e| format!("cannot finish {}: {e}", staging.display()))?;

    let f = std::fs::File::open(staging)
        .map_err(|e| format!("cannot reopen {}: {e}", staging.display()))?;
    let mut a = zip::ZipArchive::new(f)
        .map_err(|e| format!("cannot read back {}: {e}", staging.display()))?;
    for it in set.items.iter_mut() {
        if let Some(i) = it.staged {
            it.comp = a
                .by_index_raw(i)
                .map_err(|e| format!("cannot read back {}: {e}", it.name))?
                .compressed_size();
        }
    }
    Ok(())
}

/// Write the report: `report.json` first, then every kept entry copied raw out of the staging zip
/// (or out of its cut tail's own one-entry zip). Returns the entry names in order.
fn write_final(
    dest: &Path,
    staging: &Path,
    set: &Set,
    meta: &str,
    opts: zip::write::SimpleFileOptions,
) -> Result<Vec<String>, String> {
    let sf = std::fs::File::open(staging)
        .map_err(|e| format!("cannot reopen {}: {e}", staging.display()))?;
    let mut stage = zip::ZipArchive::new(sf)
        .map_err(|e| format!("cannot read back {}: {e}", staging.display()))?;
    let file = std::fs::File::create(dest)
        .map_err(|e| format!("cannot create {}: {e}", dest.display()))?;
    let mut zip = zip::ZipWriter::new(file);
    zip.start_file("report.json", opts)
        .map_err(|e| format!("cannot start report.json in the report: {e}"))?;
    zip.write_all(meta.as_bytes())
        .map_err(|e| format!("cannot write report.json into the report: {e}"))?;
    let mut entries = vec!["report.json".to_string()];
    for it in &set.items {
        let Some(si) = it.staged else {
            continue;
        };
        match it.fate {
            Fate::Dropped => continue,
            Fate::Whole => {
                let f = stage
                    .by_index_raw(si)
                    .map_err(|e| format!("cannot read back {}: {e}", it.name))?;
                zip.raw_copy_file(f)
                    .map_err(|e| format!("cannot copy {} into the report: {e}", it.name))?;
            }
            Fate::Tail(_) => {
                let Some(t) = it.tail.as_ref() else {
                    continue;
                };
                let mut a = zip::ZipArchive::new(std::io::Cursor::new(&t.zip[..]))
                    .map_err(|e| format!("cannot reread the tail of {}: {e}", it.name))?;
                let f = a
                    .by_index_raw(0)
                    .map_err(|e| format!("cannot reread the tail of {}: {e}", it.name))?;
                zip.raw_copy_file(f)
                    .map_err(|e| format!("cannot copy {} into the report: {e}", it.name))?;
            }
        }
        entries.push(it.name.clone());
    }
    zip.finish()
        .map_err(|e| format!("cannot finish {}: {e}", dest.display()))?;
    Ok(entries)
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

/// The session directory a report would ship, given a game directory: the newest one under
/// `logs\`. Exposed so the Report view can SAY which it would take before anything is built.
pub fn default_session_dir(logs_root: Option<&Path>) -> Option<PathBuf> {
    logs_root.and_then(paths::newest_session_dir_in)
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
                logs_root: None,
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
            logs_root: None,
            session_dir: default_session_dir(Some(&dir.join("logs"))),
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
            logs_root: None,
            session_dir: default_session_dir(Some(&dir.join("logs"))),
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
            logs_root: None,
            session_dir: default_session_dir(Some(&dir.join("logs"))),
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
            logs_root: None,
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

    // ---- dist LA14: the budget is the COMPRESSED upload body ------------------------------------

    /// Incompressible bytes, deterministic (xorshift64): what a deflate cannot shrink.
    fn noise(len: usize, seed: u64) -> Vec<u8> {
        let mut x = seed | 1;
        let mut out = Vec::with_capacity(len + 8);
        while out.len() < len {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            out.extend_from_slice(&x.to_le_bytes());
        }
        out.truncate(len);
        out
    }

    /// Log text that deflates poorly (random hex), `len` bytes of whole lines.
    fn noisy_log(len: usize, seed: u64) -> String {
        let n = noise(len / 2, seed);
        let mut s = String::with_capacity(len + 64);
        for (i, chunk) in n.chunks(30).enumerate() {
            s.push_str(&format!("[{i:08}] "));
            for b in chunk {
                s.push_str(&format!("{b:02x}"));
            }
            s.push('\n');
            if s.len() >= len {
                break;
            }
        }
        s
    }

    fn input_for<'a>(dir: &'a Path, session: PathBuf, description: &'a str) -> Input<'a> {
        Input {
            game_dir: Some(dir),
            logs_root: None,
            session_dir: Some(session),
            launcher_started_utc: None,
            description,
            last_run: None,
            crash: None,
            minidump: None,
            launcher_log: None,
        }
    }

    fn fresh(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("mh_launcher_test_{name}"));
        std::fs::remove_dir_all(&dir).ok();
        std::fs::create_dir_all(dir.join("logs")).unwrap();
        std::fs::write(dir.join("mh.exe"), b"not really an exe").unwrap();
        dir
    }

    fn body_of(built: &Built) -> u64 {
        // What upload.rs's multipart() sends: the zip, plus description and meta again, plus framing.
        let desc = entries_of(&built.zip)
            .into_iter()
            .find(|(n, _)| n == "description.txt")
            .map(|(_, b)| b.len() as u64)
            .unwrap_or(0);
        built.bytes + built.meta.len() as u64 + desc + MULTIPART_FRAMING
    }

    /// LA14 (1): a 30 MB log that compresses well is carried WHOLE -- the old 8 MB per-file cap
    /// would have kept its last 8 MB only -- and nothing is reported dropped.
    #[test]
    fn a_30_mb_compressible_log_goes_in_whole() {
        let dir = fresh("big_compressible");
        let name = "20260926T120000Z_abcdef01_1_host";
        let s = dir.join("logs").join(name);
        std::fs::create_dir_all(&s).unwrap();
        let line = "; [mtrace] f=1 produced=0 depth=0 max=0 di=on last=0,0 cur=512,384 vis=1 \
                    edge=---- held=---- cam=40,40 camd=0 gest=00\n";
        let mut log = String::with_capacity(31 * 1024 * 1024);
        let mut i = 0u64;
        while log.len() < 30 * 1024 * 1024 {
            log.push_str(&format!("[{i:010}] {line}"));
            i += 1;
        }
        std::fs::write(s.join("mh_mtrace.log"), &log).unwrap();

        let zip = dir.join("out").join("report.zip");
        let built = build(
            &zip,
            &input_for(&dir, s.clone(), "the map scroll got stuck"),
        )
        .unwrap();
        let entry = entries_of(&zip)
            .into_iter()
            .find(|(n, _)| n == &format!("logs/{name}/mh_mtrace.log"))
            .expect("the 30 MB log is in the report");
        assert_eq!(entry.1.len(), log.len(), "the log was cut");
        assert!(entry.1 == log.as_bytes(), "the log changed on the way in");

        let meta: serde_json::Value = serde_json::from_str(&built.meta).unwrap();
        assert!(
            meta["dropped"].as_array().unwrap().is_empty(),
            "{}",
            built.meta
        );
        assert!(body_of(&built) < BODY_BUDGET, "{}", body_of(&built));
        std::fs::remove_dir_all(&dir).ok();
    }

    /// LA9's done_when (2)/(3), re-proved for LA14's compressed budget: an INCOMPRESSIBLE `logs\`
    /// tree over the cap drops the OLDEST optional folders first -- every dropped folder is older
    /// than every optional folder kept -- lists each in `dropped`, keeps the reported match and a
    /// crash marker, and the real upload body lands under the 64 MB edge cap.
    #[test]
    fn an_incompressible_oversize_tree_drops_the_oldest_folders_first_and_fits_the_cap() {
        let dir = fresh("oversize_noise");
        // Ten session folders of 8 MB of noise each: 80 MB that deflate cannot shrink.
        let mut names = Vec::new();
        for i in 0..10u32 {
            let name = format!("202609{:02}T090000Z_{:08x}_1_host", 10 + i, i);
            let s = dir.join("logs").join(&name);
            std::fs::create_dir_all(&s).unwrap();
            std::fs::write(
                s.join("capture.bin"),
                noise(8 * 1024 * 1024, u64::from(i) + 7),
            )
            .unwrap();
            std::fs::write(s.join("mh_net.log"), format!("; folder {i}\n")).unwrap();
            names.push(name);
        }
        std::fs::write(
            dir.join("logs").join("mh_crash_cafefeed00001111.marker"),
            "mh_crash=1\ncode=0xc0000005\n",
        )
        .unwrap();
        let newest = names.last().unwrap().clone();
        let oldest = names.first().unwrap().clone();

        let zip = dir.join("out").join("report.zip");
        let session = dir.join("logs").join(&newest);
        let built = build(&zip, &input_for(&dir, session, "too many matches")).unwrap();

        let meta: serde_json::Value = serde_json::from_str(&built.meta).unwrap();
        let included: Vec<String> = meta["included"]
            .as_array()
            .unwrap()
            .iter()
            .map(|v| v.as_str().unwrap().to_string())
            .collect();
        let dropped_dirs: Vec<String> = meta["dropped"]
            .as_array()
            .unwrap()
            .iter()
            .filter(|d| d.get("file").is_none())
            .map(|d| {
                assert!(d["bytes"].as_u64().unwrap() > 0, "{d:?}");
                assert!(!d["why"].as_str().unwrap().is_empty(), "{d:?}");
                d["dir"].as_str().unwrap().to_string()
            })
            .collect();
        assert!(included.contains(&newest), "{included:?}");
        assert!(dropped_dirs.contains(&oldest), "{dropped_dirs:?}");
        let oldest_kept = included.iter().min().unwrap();
        for d in &dropped_dirs {
            assert!(
                d < oldest_kept,
                "{d} was dropped while older {oldest_kept} was kept"
            );
        }
        // Seven 8 MB folders fit under 62 MB; the budget must not throw away more than it needs.
        assert!(included.len() >= 6, "{included:?}");

        let names_in_zip: Vec<String> = entries_of(&zip).into_iter().map(|(n, _)| n).collect();
        assert!(
            !names_in_zip
                .iter()
                .any(|n| n.starts_with(&format!("logs/{oldest}/"))),
            "the dropped directory leaked into the zip anyway: {names_in_zip:?}"
        );
        assert!(names_in_zip.contains(&format!("logs/{newest}/capture.bin")));
        assert!(names_in_zip.contains(&"crash/mh_crash_cafefeed00001111.marker".to_string()));

        let body = body_of(&built);
        assert!(body <= BODY_BUDGET, "upload body {body} over the budget");
        assert!(
            body < EDGE_CAP_BYTES,
            "upload body {body} over the edge cap"
        );
        std::fs::remove_dir_all(&dir).ok();
    }

    /// LA14 (3): inside the reported match, a text log is cut to its NEWEST part before the replay
    /// input goes, and the replay input is either byte-identical to the file or absent -- never a
    /// prefix. Two budgets on the same folder: one that cuts only the log, one too tight for the
    /// replay input at all.
    #[test]
    fn a_replay_input_is_whole_or_absent_and_the_log_keeps_its_tail() {
        let dir = fresh("replay_whole");
        let name = "20260926T130000Z_0badc0de_1_host";
        let s = dir.join("logs").join(name);
        std::fs::create_dir_all(&s).unwrap();
        let orders = noise(1024 * 1024, 99);
        std::fs::write(s.join("mh_match_orders.bin"), &orders).unwrap();
        let log = noisy_log(6 * 1024 * 1024, 5);
        std::fs::write(s.join("mh_match_harness.log"), &log).unwrap();
        let last_line = log.lines().last().unwrap().to_string();

        for (budget, replay_kept) in [(3 * 1024 * 1024u64, true), (600 * 1024, false)] {
            let zip = dir.join("out").join(format!("report_{budget}.zip"));
            let built = build_with_budget(
                &zip,
                &input_for(&dir, s.clone(), "desync near the end"),
                budget,
            )
            .unwrap();
            assert!(body_of(&built) <= budget, "{} > {budget}", body_of(&built));
            let entries = entries_of(&zip);

            let replay = entries
                .iter()
                .find(|(n, _)| n == &format!("logs/{name}/mh_match_orders.bin"));
            match replay {
                Some((_, b)) => assert!(b == &orders, "a replay input went in partially"),
                None => assert!(
                    !replay_kept,
                    "budget {budget}: the replay input was dropped"
                ),
            }
            if replay_kept {
                assert!(
                    replay.is_some(),
                    "budget {budget}: the replay input was dropped"
                );
            }

            let meta: serde_json::Value = serde_json::from_str(&built.meta).unwrap();
            let dropped = meta["dropped"].as_array().unwrap();
            let log_entry = entries
                .iter()
                .find(|(n, _)| n == &format!("logs/{name}/mh_match_harness.log"));
            if let Some((_, b)) = log_entry {
                let text = String::from_utf8_lossy(b);
                assert!(text.starts_with("; [report] the first "), "{}", &text[..80]);
                assert!(
                    text.trim_end().ends_with(&last_line),
                    "the newest line was lost"
                );
                let rec = dropped
                    .iter()
                    .find(|d| {
                        d["file"]
                            .as_str()
                            .is_some_and(|f| f.ends_with("mh_match_harness.log"))
                    })
                    .expect("the cut log is named in dropped");
                assert!(rec["kept_bytes"].as_u64().unwrap() > 0, "{rec:?}");
                assert!(
                    rec["kept_bytes"].as_u64().unwrap() < log.len() as u64,
                    "{rec:?}"
                );
            }
            if !replay_kept {
                assert!(
                    dropped.iter().any(|d| d["file"]
                        .as_str()
                        .is_some_and(|f| f.ends_with("mh_match_orders.bin"))),
                    "{}",
                    built.meta
                );
            }
        }
        std::fs::remove_dir_all(&dir).ok();
    }

    /// LA14's shed order: an optional older folder goes whole before anything in the reported
    /// match is touched.
    #[test]
    fn older_folders_go_before_the_reported_match_is_cut() {
        let dir = fresh("shed_order");
        let old = "20260926T080000Z_menu_host";
        let o = dir.join("logs").join(old);
        std::fs::create_dir_all(&o).unwrap();
        std::fs::write(o.join("mh_net.log"), noisy_log(2 * 1024 * 1024, 11)).unwrap();
        let name = "20260926T090000Z_12345678_1_host";
        let s = dir.join("logs").join(name);
        std::fs::create_dir_all(&s).unwrap();
        let log = noisy_log(1024 * 1024, 12);
        std::fs::write(s.join("mh_net.log"), &log).unwrap();

        let zip = dir.join("out").join("report.zip");
        let built = build_with_budget(&zip, &input_for(&dir, s, "lagged"), 1536 * 1024).unwrap();
        let meta: serde_json::Value = serde_json::from_str(&built.meta).unwrap();
        let dropped = meta["dropped"].as_array().unwrap();
        assert_eq!(dropped.len(), 1, "{}", built.meta);
        assert_eq!(dropped[0]["dir"].as_str().unwrap(), old);
        let e = entries_of(&zip)
            .into_iter()
            .find(|(n, _)| n == &format!("logs/{name}/mh_net.log"))
            .unwrap();
        assert!(e.1 == log.as_bytes(), "the reported match was cut");
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
            logs_root: None,
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
