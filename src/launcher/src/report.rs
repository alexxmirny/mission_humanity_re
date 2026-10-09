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
//! floor, then the state recordings below are tailed toward a keyframe, then the replay inputs go
//! -- whole, never cut -- and only then the text below the floor.
//! Every folder, file and cut log given up is named in `report.json`'s `dropped`.
//!
//! ## mp:D43: the state recordings are cut at a KEYFRAME, not a byte offset
//!
//! `mh_match_state.bin` (the whole-match `net-debug` recording, mp:D40) and `mh_desync_state.bin`
//! (the ship desync ring, mp:D41) are `docs/state-record.md` v1 files: a header, then `KEYF`/`STEP`/
//! `END ` chunks. They are neither a text log nor a `REPLAY_INPUTS` entry -- a cut copy STAYS
//! decodable because the format is built for exactly this ("Cutting a tail" in that doc): keep the
//! header plus every chunk from some `KEYF` chunk onward. When the budget must shrink one, this
//! packer walks the chunk headers (tag + length only, never a payload) to find every `KEYF` byte
//! offset, then picks the EARLIEST one whose header+tail still compresses under the target -- the
//! cut that throws away the least. Ranked BELOW the replay inputs (shed/tailed before those are ever
//! touched, per the tracker item's own wording) but above the reported match's floor-less text: a
//! desync/crash report is usually about a moment near the END of the file, so the newest keyframes
//! are exactly what is worth keeping. A file that fails the chunk walk (bad magic, unknown version,
//! or a first chunk that is not the `KEYF` the format guarantees) is left out WHOLE with its own
//! `dropped` reason -- never tailed at a guess -- and so is one whose own last keyframe still does
//! not fit.
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
use std::sync::atomic::{AtomicU64, AtomicUsize, Ordering};
use std::sync::mpsc::{self, Receiver};
use std::sync::{Arc, Mutex};

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
/// Bytes a staging zip has been given so far (high-water mark of the write position), readable
/// while the `ZipWriter` owns the file.
struct Counting<W> {
    inner: W,
    pos: u64,
    high: Arc<AtomicU64>,
}

impl<W> Counting<W> {
    fn new(inner: W, high: Arc<AtomicU64>) -> Self {
        Self {
            inner,
            pos: 0,
            high,
        }
    }
}

impl<W: Write> Write for Counting<W> {
    fn write(&mut self, buf: &[u8]) -> std::io::Result<usize> {
        let n = self.inner.write(buf)?;
        self.pos += n as u64;
        self.high.fetch_max(self.pos, Ordering::Relaxed);
        Ok(n)
    }

    fn flush(&mut self) -> std::io::Result<()> {
        self.inner.flush()
    }
}

impl<W: std::io::Seek> std::io::Seek for Counting<W> {
    fn seek(&mut self, to: std::io::SeekFrom) -> std::io::Result<u64> {
        self.pos = self.inner.seek(to)?;
        Ok(self.pos)
    }
}

/// Test hook (dist LA17): bytes handed to a DEFLATE encoder for a state recording, on this thread.
/// A thread-local so parallel tests do not see each other's counts.
#[cfg(test)]
mod hook {
    use std::cell::Cell;
    thread_local! {
        static DEFLATED: Cell<u64> = const { Cell::new(0) };
    }
    pub fn deflated(n: u64) {
        DEFLATED.with(|c| c.set(c.get() + n));
    }
    pub fn take() -> u64 {
        DEFLATED.with(|c| c.replace(0))
    }
}

const STAGE_UNCOMPRESSED_MAX: u64 = 512 * 1024 * 1024;

/// How many times the zip is rewritten to converge on `BODY_BUDGET`. Each rewrite copies the
/// already-compressed entries raw; only cut tails are compressed again.
const FIT_ATTEMPTS: usize = 6;

/// The replay inputs (`mp:SES7` match segment + the process recording). A cut replay input is
/// not replayable, so these are kept whole or left out whole -- never tailed.
const REPLAY_INPUTS: [&str; 5] = [
    "mh_match_orders.bin",
    "mh_match_clock.bin",
    "mh_match_seed.bin",
    "mh_orders.bin",
    "mh_clock.bin",
];

/// mp:D43. The state recordings (`docs/state-record.md` v1) -- unlike `REPLAY_INPUTS` a cut copy of
/// one of these STAYS decodable, because the format is built to be tailed at a `KEYF` chunk
/// boundary. So they get their own `Kind` and their own shed step (`shed_state_files`), ranked below
/// the replay inputs -- see the module doc's "mp:D43" section.
///
/// mp:D46: mh.dll gzips `mh_match_state.bin` after the match and deletes the raw file, so a state
/// recording is `mh_match_state[_<n>].bin`, `mh_match_state[_<n>].bin.gz` or the `mh_desync_state`
/// pair. An unfinished compress (`*.bin.gz.tmp`) is never a recording and never enters a report.
fn is_state_name(leaf: &str) -> bool {
    let stem = leaf.strip_suffix(".gz").unwrap_or(leaf);
    (stem.starts_with("mh_match_state") || stem.starts_with("mh_desync_state"))
        && stem.ends_with(".bin")
}

/// mp:D46. The state file is a gzip member (`.gz`), not the raw v1 byte stream.
fn is_gz_path(p: &Path) -> bool {
    p.extension().is_some_and(|e| e.eq_ignore_ascii_case("gz"))
}

/// mp:D46. The most a gzip state recording may unpack to before the packer refuses it. The raw
/// file of a 36-minute match was 242 MB; this leaves ~4x headroom and bounds the memory the keyframe
/// walk holds (it needs the whole raw stream in RAM, exactly as it always did for a raw file).
const STATE_GZ_MAX_RAW: u64 = 1024 * 1024 * 1024;

const WHY_OVER_BUDGET: &str = "over the report upload budget (oldest folders go first)";
const WHY_NOT_STAGED: &str = "past the report's staging limit (older than what could fit)";
const WHY_FILE_DROPPED: &str = "left out whole to fit the report upload budget";
const WHY_FILE_TAILED: &str = "only the newest part kept to fit the report upload budget";
/// mp:D43. A state recording tailed to its newest keyframes -- distinct wording from
/// `WHY_FILE_TAILED` because "the newest part" would be misleading (the cut lands on a `KEYF`
/// boundary, not an arbitrary byte offset).
const WHY_STATE_TAILED: &str = "only the newest keyframes kept to fit the report upload budget";
/// mp:D43. The chunk walk failed (bad magic, unknown version, or a first chunk that is not the
/// `KEYF` the format guarantees) -- never tailed at a guess.
const WHY_STATE_MALFORMED: &str =
    "not a valid state recording (docs/state-record.md v1) -- left out rather than tailed blindly";
/// mp:D43. The chunk walk was fine, but even the file's own LAST keyframe does not compress under
/// what the budget has left for it.
const WHY_STATE_NO_ROOM: &str = "even its newest keyframe does not fit the report upload budget";
/// mp:D46. A gzip state recording that cannot be unpacked (damaged stream with nothing salvageable,
/// or past `STATE_GZ_MAX_RAW`) and so cannot be tailed -- left out rather than cut at a guess.
const WHY_STATE_GZ_BAD: &str =
    "gzip state recording could not be unpacked to cut at a keyframe (damaged, or over the unpack limit)";

/// dist LA9. How many of the newest `logs\` directories to package when the launcher's own start
/// time is unknown (a `--report` invoked from a script that never called `--launch` in this same
/// process, or a test). Large enough to cover "the whole afternoon" of a normal play session without
/// degenerating into "every directory this game folder has ever produced".
const LOGS_FALLBACK_N: usize = 20;

/// dist LA16: is the game process still alive while a report is built? A process-wide flag rather
/// than an `Input` field: `upload.rs` (and every test) builds `Input` literally, and the App is the
/// only one who knows. Set by the App before each build; read into report.json's `game_running`.
static GAME_RUNNING: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);

pub fn set_game_running(running: bool) {
    GAME_RUNNING.store(running, Ordering::Relaxed);
}

/// What a report is built from. Everything is optional except the description, which is the point.
#[derive(Clone)]
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
    /// dist RL4: the game's `mh_net.ini` -- in the config directory now (`cfgdir::ini_path`), not
    /// beside the exe. `None` falls back to `<game_dir>\mh_net.ini` (portable mode, and a report
    /// built with no launcher state to resolve it from).
    pub ini_path: Option<PathBuf>,
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
    /// dist RL14: the crash marker files (absolute paths under the logs root) this report carries.
    /// Once the report is sent, each gets its `.reported` sibling (`mark_reported`).
    pub markers: Vec<PathBuf>,
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

/// dist LA17. What the build is doing right now, for a window that must keep repainting while it
/// runs. Written by the build (any thread), read by the UI every frame.
#[derive(Default)]
pub struct Progress {
    text: Mutex<String>,
    updates: AtomicUsize,
}

impl Progress {
    pub fn set(&self, text: impl Into<String>) {
        if let Ok(mut t) = self.text.lock() {
            *t = text.into();
        }
        self.updates.fetch_add(1, Ordering::Relaxed);
    }

    pub fn text(&self) -> String {
        self.text.lock().map(|t| t.clone()).unwrap_or_default()
    }

    #[cfg(test)]
    /// How many times `set` has been called: lets a test tell "the build reported" from "nothing yet".
    pub fn updates(&self) -> usize {
        self.updates.load(Ordering::Relaxed)
    }
}

/// `Input` with everything owned, so it can move to a worker thread (dist LA17).
pub struct OwnedInput {
    pub game_dir: Option<PathBuf>,
    pub logs_root: Option<PathBuf>,
    pub session_dir: Option<PathBuf>,
    pub launcher_started_utc: Option<String>,
    pub description: String,
    pub last_run: Option<Finished>,
    pub crash: Option<Marker>,
    pub minidump: Option<PathBuf>,
    pub launcher_log: Option<PathBuf>,
    pub ini_path: Option<PathBuf>,
}

impl OwnedInput {
    fn borrow(&self) -> Input<'_> {
        Input {
            game_dir: self.game_dir.as_deref(),
            logs_root: self.logs_root.clone(),
            session_dir: self.session_dir.clone(),
            launcher_started_utc: self.launcher_started_utc.clone(),
            description: &self.description,
            last_run: self.last_run.as_ref(),
            crash: self.crash.as_ref(),
            minidump: self.minidump.as_deref(),
            launcher_log: self.launcher_log.clone(),
            ini_path: self.ini_path.clone(),
        }
    }
}

/// dist LA17. One report build on its own thread. `report::build` deflates 100-250 MB state
/// recordings; run inside a frame callback it is a window that has stopped painting ("Not
/// responding"), which a player reads as a crash. The UI starts one of these, repaints while
/// `poll` says `Running`, and takes the result when it says `Done`.
pub struct Job {
    progress: Arc<Progress>,
    rx: Receiver<Result<Built, String>>,
    #[cfg(test)]
    worker: std::thread::ThreadId,
    finished: bool,
}

pub enum Poll {
    Running,
    Done(Result<Built, String>),
}

impl Job {
    pub fn start(dest: PathBuf, input: OwnedInput) -> Self {
        Self::start_with(dest, input, BODY_BUDGET)
    }

    fn start_with(dest: PathBuf, input: OwnedInput, body_budget: u64) -> Self {
        let progress = Arc::new(Progress::default());
        progress.set("starting");
        let (tx, rx) = mpsc::channel();
        let p = Arc::clone(&progress);
        let d = dest.clone();
        let _handle = std::thread::spawn(move || {
            let result = build_with_progress(&d, &input.borrow(), body_budget, &p);
            let _ = tx.send(result);
        });
        Self {
            progress,
            rx,
            #[cfg(test)]
            worker: _handle.thread().id(),
            finished: false,
        }
    }

    /// The id of the thread doing the work (a test asserts it is not the caller's).
    #[cfg(test)]
    pub fn worker_thread(&self) -> std::thread::ThreadId {
        self.worker
    }

    pub fn progress(&self) -> &Progress {
        &self.progress
    }

    /// Never blocks. After `Done` was returned once, later polls say `Done(Err(..))`.
    pub fn poll(&mut self) -> Poll {
        match self.rx.try_recv() {
            Ok(r) => {
                self.finished = true;
                Poll::Done(r)
            }
            Err(mpsc::TryRecvError::Empty) => Poll::Running,
            Err(mpsc::TryRecvError::Disconnected) => {
                let why = if self.finished {
                    "the report result was already taken"
                } else {
                    "the report thread stopped without answering"
                };
                self.finished = true;
                Poll::Done(Err(why.to_string()))
            }
        }
    }
}

/// Build the zip at `dest`, fitted to `BODY_BUDGET`, on the calling thread. The app goes through
/// `Job` (dist LA17); this is the same build for tests.
#[cfg(test)]
pub fn build(dest: &Path, input: &Input) -> Result<Built, String> {
    build_with_progress(dest, input, BODY_BUDGET, &Progress::default())
}

/// `build` with the body budget spelled out, so a test can exercise the trimming order on a few
/// megabytes instead of generating 70 MB of incompressible data for every case.
#[cfg(test)]
fn build_with_budget(dest: &Path, input: &Input, body_budget: u64) -> Result<Built, String> {
    build_with_progress(dest, input, body_budget, &Progress::default())
}

/// The build itself: `body_budget` is the upload-body cap, `progress` says what it is doing.
fn build_with_progress(
    dest: &Path,
    input_in: &Input,
    body_budget: u64,
    progress: &Progress,
) -> Result<Built, String> {
    if !description_ok(input_in.description) {
        return Err(NO_DESCRIPTION.to_string());
    }
    if let Some(parent) = dest.parent() {
        std::fs::create_dir_all(parent)
            .map_err(|e| format!("cannot create {}: {e}", parent.display()))?;
    }

    progress.set("looking through the log folders");
    // dist LA16: this run's own crash marker rides along only when it belongs to the session the
    // report is about -- an old crash must not make a clean match read as one.
    let facts = match (input_in.logs_root(), input_in.session_dir.as_deref()) {
        (Some(lr), Some(sd)) => Some(session_facts(&lr, sd)),
        _ => None,
    };
    let input_owned = Input {
        crash: input_in
            .crash
            .filter(|m| facts.as_ref().is_none_or(|f| marker_belongs(m, f))),
        ..input_in.clone()
    };
    let input = &input_owned;
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
        let ini = input
            .ini_path
            .clone()
            .unwrap_or_else(|| game_dir.join(relay::INI_NAME));
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
        facts.as_ref(),
    );

    let opts: zip::write::SimpleFileOptions = zip::write::SimpleFileOptions::default()
        .compression_method(zip::CompressionMethod::Deflated);
    let staging = dest.with_extension("staging");
    let result = (|| -> Result<Built, String> {
        stage(&mut set, &staging, opts, &scrub, body_budget, progress)?;
        let meta_for = |set: &Set| {
            meta_json(
                input,
                &machine,
                installed.as_ref(),
                &build_stamp,
                &match_id,
                &set.included(),
                &set.dropped_records(),
                &set.older_crashes,
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
            set.materialize_tails(opts, &scrub, progress)?;
            progress.set("writing the report");
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
            markers: set.markers.clone(),
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
#[allow(clippy::too_many_arguments)]
fn meta_json(
    input: &Input,
    machine: &Machine,
    installed: Option<&install::Manifest>,
    build_stamp: &str,
    match_id: &str,
    included: &[String],
    dropped: &[serde_json::Value],
    older_crashes: &[serde_json::Value],
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
        // dist LA16: the game was still running when this report was built (its logs end
        // mid-match), and any crash marker left out because it belongs to another match/run.
        "game_running": GAME_RUNNING.load(Ordering::Relaxed),
        "older_crashes": older_crashes,
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
/// `<stamp>_menu_<role>` (a process directory) or `<stamp>_<mid8>_<map>_<mode>` (a session one;
/// `<stamp>_<mid8>_<slot>_<role>` before SES8). `stamp` is the leading UTC prefix FOLDED to the
/// compact `YYYYMMDDTHHMMSSZ` form (either generation), which sorts exactly like the moment it names
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

/// The UTC stamp off the front of a directory name, as `20260917T164346Z`, or `None` if it does
/// not start with one -- `2026-09-17T16-43-46Z_...` (SES8) and `20260917T164346Z_...` (SES1) both,
/// returned compact so they compare with each other and with `launcher_started_utc`. The same
/// shape-check `paths::newest_session_dir` uses (kept as its own small copy here rather than a
/// cross-module dependency, the way `mh_common`'s OS-free headers accept some duplication for the
/// same reason): every real stamp is one fixed width and character class once folded, so string
/// comparison between two of them IS a comparison of instants.
fn utc_stamp_prefix(name: &str) -> Option<String> {
    let b = name.as_bytes();
    let digits = |r: std::ops::Range<usize>| b[r].iter().all(u8::is_ascii_digit);
    // SES8 (2026-09-29): `YYYY-MM-DDTHH-MM-SSZ` -- ISO 8601 with dashes for the colons a Windows
    // path cannot hold. Folded to the compact form, so the two generations compare as instants (a
    // raw compare puts every SES1 name after an SES8 one: `-` sorts before `0`).
    if b.len() >= 20
        && digits(0..4)
        && b[4] == b'-'
        && digits(5..7)
        && b[7] == b'-'
        && digits(8..10)
        && b[10] == b'T'
        && digits(11..13)
        && b[13] == b'-'
        && digits(14..16)
        && b[16] == b'-'
        && digits(17..19)
        && b[19] == b'Z'
    {
        return Some(format!(
            "{}{}{}T{}{}{}Z",
            &name[0..4],
            &name[5..7],
            &name[8..10],
            &name[11..13],
            &name[14..16],
            &name[17..19]
        ));
    }
    // SES1: `YYYYMMDDTHHMMSSZ`.
    if b.len() >= 16 && digits(0..8) && b[8] == b'T' && digits(9..15) && b[15] == b'Z' {
        return Some(name[..16].to_string());
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
    // NOT `list_log_dirs`: that sizes every directory (a walk of every file in it), and the page
    // asks for this list every couple of seconds. The order is the same one: stamp, then name,
    // newest first.
    let Ok(read) = std::fs::read_dir(logs_root) else {
        return Vec::new();
    };
    let mut out: Vec<(String, String, PathBuf)> = read
        .flatten()
        .filter(|e| e.file_type().map(|t| t.is_dir()).unwrap_or(false))
        .filter_map(|e| {
            let name = e.file_name().to_string_lossy().to_string();
            let stamp = utc_stamp_prefix(&name)?;
            (!is_process_dir_name(&name)).then(|| (stamp, name, e.path()))
        })
        .collect();
    out.sort_by(|a, b| b.0.cmp(&a.0).then_with(|| b.1.cmp(&a.1)));
    out.into_iter().map(|(_, _, p)| p).collect()
}

/// `session_dirs` over several logs roots (the launcher-owned one and the game's config-dir
/// `logs\`, where a hand launch writes and where the RL4 migration moved the old ones), merged
/// newest first and de-duplicated by directory name -- the first root listed wins a tie.
pub fn session_dirs_in(roots: &[PathBuf]) -> Vec<PathBuf> {
    let mut all: Vec<(String, String, PathBuf)> = roots
        .iter()
        .flat_map(|r| session_dirs(r))
        .filter_map(|p| {
            let name = p.file_name()?.to_string_lossy().to_string();
            Some((utc_stamp_prefix(&name)?, name, p))
        })
        .collect();
    all.sort_by(|a, b| b.0.cmp(&a.0).then_with(|| b.1.cmp(&a.1))); // stable: root order breaks ties
    all.dedup_by(|b, a| a.1.eq_ignore_ascii_case(&b.1));
    all.into_iter().map(|(_, _, p)| p).collect()
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
    /// mp:D43. A state recording (`STATE_FILES`): tailed at a `KEYF` chunk boundary when it must
    /// shrink, given up after the text-to-floor cut but before the replay inputs are touched.
    State,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Fate {
    Whole,
    /// Keep the newest part, at most this many compressed bytes. For `Kind::State` this is a
    /// TARGET the keyframe walk tries to land under, not a byte offset.
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
    /// mp:D43. The `u32 step` a `Kind::State` tail's kept `KEYF` chunk starts at -- cheap to read
    /// off the payload's first 4 bytes without decoding anything else. `None` for a text tail.
    kept_from_step: Option<u32>,
}

struct Item {
    name: String,
    source: Source,
    kind: Kind,
    group: usize,
    /// mp:D43. Overrides the generic `WHY_FILE_DROPPED` reason when this item was dropped for a
    /// `Kind::State`-specific cause (malformed, or no keyframe fits). `None` uses the generic one.
    drop_reason: Option<&'static str>,
    /// mp:D46. The entry name the CUT copy is written under, when it differs from `name`: a `.gz`
    /// state recording is unpacked to cut it, so the tail is a plain v1 file (`...bin`), not gzip.
    tail_name: Option<String>,
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
    /// dist LA16: crash markers found under `logs\` that belong to ANOTHER match or run than the
    /// one this report is about, left out and named in report.json (`older_crashes`).
    older_crashes: Vec<serde_json::Value>,
    /// dist RL14: the marker files that did go in (absolute paths), for `Built::markers`.
    markers: Vec<PathBuf>,
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
            drop_reason: None,
            tail_name: None,
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
        // mp:D46: an unfinished compress is not a recording; a raw file whose finished `.gz` sits
        // beside it (a kill between the rename and the delete) is the same bytes, so only the smaller
        // copy goes in.
        if leaf.ends_with(".bin.gz.tmp") {
            log::line(format!(
                "report: {leaf} is an unfinished compress, left out"
            ));
            return;
        }
        if is_state_name(&leaf) && !leaf.ends_with(".gz") {
            let mut twin = src.as_os_str().to_os_string();
            twin.push(".gz");
            if Path::new(&twin).is_file() {
                log::line(format!("report: {leaf} left out, its .gz twin is carried"));
                return;
            }
        }
        let kind = if is_text(&name) {
            Kind::Text
        } else if REPLAY_INPUTS.contains(&leaf.as_str()) {
            Kind::Replay
        } else if is_state_name(&leaf) {
            Kind::State
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
        progress: &Progress,
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
                continue; // only file source is ever in a Shrink group; nothing to cut
            };
            progress.set(format!("cutting {} to fit", it.name));
            if it.kind == Kind::State {
                // mp:D43. Never decode a payload -- only the header (which carries its own
                // `header_len`) and the chunk tags/lengths are read. `state_layout` returning
                // `None` means the chunk walk failed (bad magic/version, or a first chunk that is
                // not the `KEYF` the format guarantees): left out whole rather than tailed at a
                // guess. Otherwise `state_tail` picks the EARLIEST `KEYF` whose header+tail it
                // estimates still fits `target` (dist LA17: from the ratio staging measured, at most
                // one extra pass over the file); `None` means even the file's own newest keyframe
                // does not.
                let raw = match read_state_bytes(p) {
                    Ok(raw) => raw,
                    Err(why) => {
                        it.fate = Fate::Dropped;
                        it.drop_reason = Some(why);
                        continue;
                    }
                };
                // A `.gz` recording was unpacked to be cut, so its tail is the plain v1 stream.
                let out_name = if is_gz_path(p) {
                    it.name.strip_suffix(".gz").unwrap_or(&it.name).to_string()
                } else {
                    it.name.clone()
                };
                match state_layout(&raw) {
                    None => {
                        it.fate = Fate::Dropped;
                        it.drop_reason = Some(WHY_STATE_MALFORMED);
                    }
                    Some((header_len, keyframes)) => {
                        // The ratio staging measured: compressed bytes per raw byte of THIS file (for a
                        // `.gz` that is the stored gzip size over the unpacked length).
                        let ratio = it.comp as f64 / raw.len().max(1) as f64;
                        let tail = state_tail(
                            &raw, header_len, &keyframes, &out_name, target, ratio, opts,
                        )?;
                        match tail {
                            Some((zip, kept, step)) => {
                                it.tail_name = (out_name != it.name).then_some(out_name);
                                it.tail = Some(TailBuf {
                                    target,
                                    zip,
                                    kept,
                                    kept_from_step: Some(step),
                                });
                            }
                            None => {
                                it.fate = Fate::Dropped;
                                it.drop_reason = Some(WHY_STATE_NO_ROOM);
                            }
                        }
                    }
                }
                continue;
            }
            let text = read_scrubbed(p, scrub);
            let total = text.len() as u64;
            let ratio = target as f64 / it.comp.max(1) as f64;
            let mut keep = (total as f64 * ratio * 0.97) as u64;
            for _ in 0..6 {
                keep = keep.min(total);
                let (body, kept) = tail_of(&text, keep);
                let (zip, comp) = compress_one(&it.name, body.as_bytes(), opts)?;
                if comp <= target {
                    it.tail = Some(TailBuf {
                        target,
                        zip,
                        kept,
                        kept_from_step: None,
                    });
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
                        "dir": dir,
                        "file": it.name,
                        "bytes": it.raw,
                        "why": it.drop_reason.unwrap_or(WHY_FILE_DROPPED),
                    })),
                    Fate::Tail(_) => {
                        let why = if it.kind == Kind::State {
                            WHY_STATE_TAILED
                        } else {
                            WHY_FILE_TAILED
                        };
                        let mut rec = serde_json::json!({
                            "dir": dir,
                            "file": it.name,
                            "bytes": it.raw,
                            "kept_bytes": it.tail.as_ref().map(|t| t.kept).unwrap_or(0),
                            "why": why,
                        });
                        // mp:D43: the KEYF's own step, when the tail is a state recording's.
                        if let Some(step) = it.tail.as_ref().and_then(|t| t.kept_from_step) {
                            rec["kept_from_step"] = serde_json::json!(step);
                        }
                        // mp:D46: a gzip recording is unpacked to be cut; the tail is a plain v1 file.
                        if let Some(n) = it.tail_name.as_ref() {
                            rec["kept_as"] = serde_json::json!(n);
                        }
                        out.push(rec);
                    }
                }
            }
        }
        out
    }
}

/// Shrink one protected group by `excess` bytes, in this order: (a) plain binaries, largest
/// first, whole; (b) text logs cut from the front, largest first, down to `TEXT_TAIL_FLOOR` each
/// (a water level -- small logs stay whole); (c) state recordings (mp:D43), largest first, tailed
/// toward a keyframe; (d) replay inputs, whole; (e) text below the floor, down to nothing. Returns
/// what is still over.
fn shrink_group(items: &mut [Item], gi: usize, excess: u64) -> u64 {
    let excess = drop_largest(items, gi, Kind::Binary, excess);
    let excess = water_fill(items, gi, TEXT_TAIL_FLOOR, excess);
    let excess = shed_state_files(items, gi, excess);
    let excess = drop_largest(items, gi, Kind::Replay, excess);
    water_fill(items, gi, 0, excess)
}

/// mp:D43. Give up `Kind::State` items toward `excess`, largest (live-size) first: each is marked
/// with a compressed-byte TARGET to try to land under. The actual keyframe boundary is found later
/// in `Set::materialize_tails`, which is the only place that needs the file's bytes -- this step is
/// pure size bookkeeping, the same shape as `water_fill`'s level search but per item, since a state
/// file's valid cut points are discrete `KEYF` offsets rather than a continuous byte range.
fn shed_state_files(items: &mut [Item], gi: usize, mut excess: u64) -> u64 {
    let mut idx: Vec<usize> = (0..items.len())
        .filter(|&i| {
            items[i].group == gi && items[i].kind == Kind::State && items[i].fate != Fate::Dropped
        })
        .collect();
    idx.sort_by_key(|&i| std::cmp::Reverse(live_size(&items[i])));
    for i in idx {
        if excess == 0 {
            break;
        }
        let size = live_size(&items[i]);
        if size == 0 {
            continue;
        }
        let target = size.saturating_sub(excess);
        excess = excess.saturating_sub(size - target);
        items[i].fate = Fate::Tail(target);
    }
    excess
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
    compress_parts(name, &[data], opts)
}

/// `compress_one` over the concatenation of `parts`, without ever building the concatenation (dist
/// LA17: a state tail is the file header plus a slice of a 250 MB buffer; copying that per probe was
/// a second 250 MB allocation each time).
fn compress_parts(
    name: &str,
    parts: &[&[u8]],
    opts: zip::write::SimpleFileOptions,
) -> Result<(Vec<u8>, u64), String> {
    let mut w = zip::ZipWriter::new(std::io::Cursor::new(Vec::new()));
    w.start_file(name, opts)
        .map_err(|e| format!("cannot start {name}: {e}"))?;
    for data in parts {
        w.write_all(data)
            .map_err(|e| format!("cannot write {name}: {e}"))?;
        #[cfg(test)]
        if !is_text(name) {
            hook::deflated(data.len() as u64);
        }
    }
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

// ---- mp:D43: state recordings, tailed at a KEYF chunk boundary --------------------------------

/// `docs/state-record.md` v1: `"MHSR"` little-endian.
const STATE_MAGIC: u32 = 0x5253_484D;
/// The only version this packer knows how to walk. A file that claims a different one is left out
/// whole -- the header layout downstream of `version` is not this packer's to assume.
const STATE_VERSION: u16 = 1;
/// The `KEYF` chunk tag (`"KEYF"` little-endian), the only tag this packer looks for; `STEP`/`END `
/// chunks are skipped over by length, never inspected.
const STATE_TAG_KEYF: u32 = 0x4659_454B;
/// `magic(4) + version(2) + flags(2) + header_len(4) + manifest_fp(8) + keyframe_every(4) +
/// region_count(4)`: the fixed prefix before the region table. `header_len` already counts the
/// table, so nothing past this offset needs to be read at all.
const STATE_HEADER_MIN: usize = 28;

fn u32_at(buf: &[u8], off: u64) -> Option<u32> {
    let o = usize::try_from(off).ok()?;
    buf.get(o..o + 4)
        .map(|b| u32::from_le_bytes(b.try_into().unwrap()))
}

fn u16_at(buf: &[u8], off: u64) -> Option<u16> {
    let o = usize::try_from(off).ok()?;
    buf.get(o..o + 2)
        .map(|b| u16::from_le_bytes(b.try_into().unwrap()))
}

/// Read a state recording's header + `KEYF` chunk offsets, WITHOUT decoding any chunk's payload
/// (`docs/state-record.md` "Cutting a tail"). Returns `(header_len, keyframes)`, `keyframes` oldest
/// first as `(byte offset of the KEYF chunk's own header, its payload's leading u32 step)`.
///
/// `None` means the chunk walk failed outright -- bad magic, an unknown version, or (since the
/// format guarantees "the first chunk after the header is always a KEYF") a first chunk that is
/// not one. An incomplete TRAILING chunk (the recording was cut short by a crash or a kill) is not
/// a failure -- state-record.md says a reader "stops cleanly" there -- so the walk simply ends and
/// whatever keyframes were found before it are used.
fn state_layout(buf: &[u8]) -> Option<(u64, Vec<(u64, u32)>)> {
    if buf.len() < STATE_HEADER_MIN {
        return None;
    }
    if u32_at(buf, 0)? != STATE_MAGIC || u16_at(buf, 4)? != STATE_VERSION {
        return None;
    }
    let header_len = u64::from(u32_at(buf, 8)?);
    if header_len < STATE_HEADER_MIN as u64 || header_len > buf.len() as u64 {
        return None;
    }

    let mut keyframes = Vec::new();
    let mut off = header_len;
    let mut first = true;
    while off + 12 <= buf.len() as u64 {
        let tag = u32_at(buf, off)?;
        let payload_len = u64::from(u32_at(buf, off + 4)?);
        let payload_start = off + 12;
        let Some(payload_end) = payload_start.checked_add(payload_len) else {
            break;
        };
        if payload_end > buf.len() as u64 {
            break; // an incomplete trailing chunk: the file was cut short, not malformed
        }
        if tag == STATE_TAG_KEYF {
            let step = if payload_len >= 4 {
                u32_at(buf, payload_start)?
            } else {
                0
            };
            keyframes.push((off, step));
        } else if first {
            return None; // the format guarantees the first chunk after the header is a KEYF
        }
        first = false;
        off = payload_end;
    }
    if keyframes.is_empty() {
        return None;
    }
    Some((header_len, keyframes))
}

/// mp:D46. A state recording's raw v1 bytes: the file itself, or a `.gz` one unpacked (bounded by
/// `STATE_GZ_MAX_RAW`). A gzip stream that ends early or is damaged still yields the bytes decoded
/// before the fault -- the format is built to be read up to its last complete chunk -- unless there are
/// none. `Err` carries the `dropped` reason.
fn read_state_bytes(p: &Path) -> Result<Vec<u8>, &'static str> {
    if !is_gz_path(p) {
        return Ok(std::fs::read(p).unwrap_or_default());
    }
    // dist LA17: stream the gzip from the file (it is up to ~40-60 MB; holding it AND the unpacked
    // bytes was needless), and size the output from the gzip trailer's ISIZE (the unpacked length
    // mod 2^32, a hint only) so `read_to_end` does not double a 250 MB buffer on the way up.
    let Ok(mut file) = std::fs::File::open(p) else {
        return Err(WHY_STATE_GZ_BAD);
    };
    let hint = {
        use std::io::{Read, Seek, SeekFrom};
        let mut isize_le = [0u8; 4];
        let ok = file.seek(SeekFrom::End(-4)).is_ok() && file.read_exact(&mut isize_le).is_ok();
        file.seek(SeekFrom::Start(0)).ok();
        if ok {
            u64::from(u32::from_le_bytes(isize_le)).min(STATE_GZ_MAX_RAW)
        } else {
            0
        }
    };
    let mut out = Vec::with_capacity(usize::try_from(hint).unwrap_or(0));
    let mut dec = std::io::Read::take(
        flate2::read::GzDecoder::new(std::io::BufReader::with_capacity(1 << 20, file)),
        STATE_GZ_MAX_RAW + 1,
    );
    let res = std::io::Read::read_to_end(&mut dec, &mut out);
    if out.len() as u64 > STATE_GZ_MAX_RAW {
        return Err(WHY_STATE_GZ_BAD);
    }
    if let Err(e) = res {
        log::line(format!(
            "report: {} is damaged after {} unpacked bytes: {e}",
            p.display(),
            out.len()
        ));
    }
    if out.is_empty() {
        return Err(WHY_STATE_GZ_BAD);
    }
    Ok(out)
}

/// First guess's safety factor: aim a little under the target so a tail whose ratio is a touch worse
/// than the whole file's still fits on the first probe.
const TAIL_FIRST_SAFETY: f64 = 0.92;
/// Later guesses use a ratio measured on a tail of the same file, so they need less margin.
const TAIL_RETRY_SAFETY: f64 = 0.97;

/// mp:D43. Tail a state recording to a `KEYF` chunk whose header+tail compresses under `target`.
/// `keyframes` is oldest first (from `state_layout`). `Ok(None)` means even the LAST (newest,
/// smallest) keyframe does not fit -- the file is left out whole, never tailed past its own newest
/// keyframe.
///
/// dist LA17: bounded work. Staging already deflated the whole file once, so `ratio_hint`
/// (compressed bytes per raw byte, measured there) says how many raw bytes `target` buys: pick the
/// earliest keyframe whose header+tail is at most `target / ratio * TAIL_FIRST_SAFETY` and deflate
/// that ONE tail. If it misses, the probe measured a better ratio for this very region; step to the
/// next keyframe that ratio says fits. Every probe is strictly later (smaller) than the one before,
/// and the bytes deflated across all probes are capped at one pass over the file, so staging plus
/// this is at most two passes however the ratio behaves -- where mp:D46's bisection deflated up to
/// `log2(K)` near-whole tails (250 MB each) and D43's scan one per keyframe. The price is that the
/// kept tail can be a few percent shorter than the true optimum (the safety factor); the first probe
/// is almost always the only one. The bodies are never concatenated: `compress_parts` is fed the
/// header and the slice.
fn state_tail(
    raw: &[u8],
    header_len: u64,
    keyframes: &[(u64, u32)],
    name: &str,
    target: u64,
    ratio_hint: f64,
    opts: zip::write::SimpleFileOptions,
) -> Result<Option<(Vec<u8>, u64, u32)>, String> {
    let Some(last) = keyframes.len().checked_sub(1) else {
        return Ok(None);
    };
    let header = &raw[..header_len as usize];
    // Header + tail bytes if the cut is at keyframe `i`. Strictly decreasing in `i`.
    let size_at = |i: usize| header.len() as u64 + raw.len() as u64 - keyframes[i].0;
    // The earliest keyframe at index >= `from` that is at most `want` bytes, else the newest.
    let pick = |from: usize, want: f64| -> usize {
        let n = keyframes[from..].partition_point(|&(off, _)| {
            (header.len() as u64 + raw.len() as u64 - off) as f64 > want
        });
        (from + n).min(last)
    };
    let mut ratio = if ratio_hint.is_finite() && ratio_hint > 0.0 {
        ratio_hint
    } else {
        1.0
    };
    let mut safety = TAIL_FIRST_SAFETY;
    let mut from = 0usize;
    // The extra-pass allowance: all probes together deflate at most one whole file's worth.
    let mut allowance = raw.len() as u64;
    loop {
        let want = (target as f64 / ratio * safety).min(allowance as f64);
        let i = pick(from, want);
        let size = size_at(i);
        if size > allowance {
            return Ok(None); // not even the newest keyframe fits the allowance: leave it out whole
        }
        let (off, step) = keyframes[i];
        let (zip, comp) = compress_parts(name, &[header, &raw[off as usize..]], opts)?;
        allowance -= size;
        if comp <= target {
            return Ok(Some((zip, size, step)));
        }
        if i == last {
            return Ok(None);
        }
        ratio = comp as f64 / size as f64;
        safety = TAIL_RETRY_SAFETY;
        from = i + 1;
    }
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
    facts: Option<&SessionFacts>,
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
    // dist LA16: ...but only the ones that belong to the session the report is about. The others
    // are named in report.json (`older_crashes`) instead of being attached.
    let (crash_files, older) = split_crash_files(crash_files, facts);
    set.older_crashes = older;
    for p in crash_files {
        if p.extension().is_some_and(|e| e == "marker") {
            set.markers.push(p.clone());
        }
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
///
/// dist LA17: an Optional folder that comes AFTER enough staged material to fill the budget is not
/// staged at all. Optional folders are shed first and oldest first, and items are staged reported,
/// protected, then optional newest first -- so when the compressed bytes already written reach
/// `body_budget`, every later Optional folder is certain to be given up whole by `Set::shed`
/// (the same drop it would get after being deflated for nothing: a raw state recording costs seconds
/// per 250 MB). Such a folder is marked dropped here with `WHY_OVER_BUDGET`.
fn stage(
    set: &mut Set,
    staging: &Path,
    opts: zip::write::SimpleFileOptions,
    scrub: &Scrub,
    body_budget: u64,
    progress: &Progress,
) -> Result<(), String> {
    let file = std::fs::File::create(staging)
        .map_err(|e| format!("cannot create {}: {e}", staging.display()))?;
    let written = Arc::new(AtomicU64::new(0));
    let mut zip = zip::ZipWriter::new(Counting::new(file, Arc::clone(&written)));
    let mut n = 0usize;
    let total = set.items.len();
    for (idx, it) in set.items.iter_mut().enumerate() {
        let g = it.group;
        if set.groups[g].dropped_why.is_none()
            && set.groups[g].mode == Mode::DropWhole
            && set.groups[g].dir.is_some()
            && written.load(Ordering::Relaxed) >= body_budget
        {
            log::line(format!(
                "report: {} not staged, the newer material already fills the upload budget",
                set.groups[g].dir.as_deref().unwrap_or_default()
            ));
            set.groups[g].dropped_why = Some(WHY_OVER_BUDGET);
        }
        if set.groups[it.group].dropped_why.is_some() {
            it.fate = Fate::Dropped;
            continue;
        }
        progress.set(format!(
            "compressing {} ({} of {})",
            it.name,
            idx + 1,
            total
        ));
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
                // mp:D46: a gzip state recording is already deflated; deflating it again costs seconds and
                // saves nothing, so it is stored (the staged size IS its cost in the report).
                let o = if it.kind == Kind::State && is_gz_path(p) {
                    opts.compression_method(zip::CompressionMethod::Stored)
                } else {
                    opts
                };
                zip.start_file(it.name.as_str(), o)
                    .map_err(|e| format!("cannot start {} in the report: {e}", it.name))?;
                let n = std::io::copy(&mut f, &mut zip)
                    .map_err(|e| format!("cannot write {} into the report: {e}", it.name))?;
                #[cfg(test)]
                if it.kind == Kind::State && !is_gz_path(p) {
                    hook::deflated(n);
                }
                n
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
        entries.push(match (&it.fate, &it.tail_name) {
            (Fate::Tail(_), Some(n)) => n.clone(),
            _ => it.name.clone(),
        });
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

// ---- dist LA16: which crash markers belong to the session -----------------------------------

/// The `.reported` suffix: the RL5 retention contract with mh.dll's log prune
/// (`mh_common/include/mh_log_prune.h`). An empty `<marker file name>.reported` beside a marker
/// says the crash has been sent or dismissed, after which the folders it protected are prunable.
pub const REPORTED_SUFFIX: &str = ".reported";

/// Create the empty `<marker>.reported` sibling. Idempotent; the marker itself is left alone (the
/// game's prune reads both).
pub fn mark_reported(marker: &Path) -> Result<PathBuf, String> {
    let mut name = marker
        .file_name()
        .ok_or_else(|| format!("{} has no file name", marker.display()))?
        .to_os_string();
    name.push(REPORTED_SUFFIX);
    let dest = marker.with_file_name(name);
    std::fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(&dest)
        .map_err(|e| format!("cannot write {}: {e}", dest.display()))?;
    Ok(dest)
}

/// Has this marker been sent or dismissed already?
pub fn is_reported(marker: &Path) -> bool {
    let mut name = marker.file_name().unwrap_or_default().to_os_string();
    name.push(REPORTED_SUFFIX);
    marker.with_file_name(name).is_file()
}

/// What decides whether a crash marker belongs to the session a report is about: the match id, and
/// the time window of the game PROCESS that hosted the session (from its own process directory to
/// the next process directory). All stamps compact UTC (`20260926T175010Z`).
#[derive(Clone, Debug, Default)]
pub struct SessionFacts {
    pub match_id: String,
    /// The session directory's own stamp.
    pub start: String,
    /// The process ("menu") directory the session hangs off, per its `session.json`.
    pub process_start: Option<String>,
    /// The stamp of the next process directory: when that process was over.
    pub process_end: Option<String>,
}

/// `(name, compact stamp)` of every process directory under `logs_root`, without sizing them.
fn process_dir_stamps(logs_root: &Path) -> Vec<(String, String)> {
    let Ok(read) = std::fs::read_dir(logs_root) else {
        return Vec::new();
    };
    read.flatten()
        .filter(|e| e.file_type().map(|t| t.is_dir()).unwrap_or(false))
        .filter_map(|e| {
            let name = e.file_name().to_string_lossy().to_string();
            let stamp = utc_stamp_prefix(&name)?;
            is_process_dir_name(&name).then_some((name, stamp))
        })
        .collect()
}

pub fn session_facts(logs_root: &Path, session: &Path) -> SessionFacts {
    let name = session
        .file_name()
        .map(|n| n.to_string_lossy().to_string())
        .unwrap_or_default();
    let process_start = process_dir_of_session(session).and_then(|pd| utc_stamp_prefix(&pd));
    let process_end = process_start.as_deref().and_then(|ps| {
        process_dir_stamps(logs_root)
            .into_iter()
            .map(|(_, s)| s)
            .filter(|s| s.as_str() > ps)
            .min()
    });
    SessionFacts {
        match_id: match_id_from_session(session).unwrap_or_default(),
        start: utc_stamp_prefix(&name).unwrap_or_default(),
        process_start,
        process_end,
    }
}

/// Does `m` belong to the session `f` describes?
///
/// A marker that names a match belongs to the session of THAT match and no other. One that names
/// none (a crash in the menu, before any match) belongs to the game process the session ran in:
/// stamped from that process's directory up to the next process's. A marker whose own stamp cannot
/// be read, with no match to go by, cannot be shown to be another run's, so it stays (a marker
/// the game wrote always carries its `when=`; this is the hand-made and the damaged one).
pub fn marker_belongs(m: &Marker, f: &SessionFacts) -> bool {
    let mid = m.match_id.trim();
    if !mid.is_empty() && !f.match_id.is_empty() {
        return mid.eq_ignore_ascii_case(&f.match_id);
    }
    let Some(when) = utc_stamp_prefix(m.when.trim()) else {
        // Nothing to place it by: it cannot be shown to be ANOTHER run's, and evidence is not
        // thrown away on a guess.
        return true;
    };
    let lo = f.process_start.as_deref().unwrap_or(&f.start);
    when.as_str() >= lo && f.process_end.as_deref().is_none_or(|hi| when.as_str() < hi)
}

/// Split the `mh_crash_*` files swept from the logs root into the ones this report carries and a
/// record of the markers left out (`older_crashes` in report.json). `.reported` flags are never
/// carried. With no session to compare against everything but the flags stays (the old behaviour).
fn split_crash_files(
    files: Vec<PathBuf>,
    facts: Option<&SessionFacts>,
) -> (Vec<PathBuf>, Vec<serde_json::Value>) {
    let leaf = |p: &Path| {
        p.file_name()
            .unwrap_or_default()
            .to_string_lossy()
            .to_string()
    };
    let files: Vec<PathBuf> = files
        .into_iter()
        .filter(|p| !leaf(p).ends_with(REPORTED_SUFFIX))
        .collect();
    let Some(f) = facts else {
        return (files, Vec::new());
    };
    let mut keep: std::collections::BTreeSet<String> = std::collections::BTreeSet::new();
    let mut older = Vec::new();
    for p in files.iter().filter(|p| leaf(p).ends_with(".marker")) {
        match Marker::read(p) {
            Ok(m) if marker_belongs(&m, f) => {
                keep.insert(leaf(p));
            }
            Ok(m) => older.push(serde_json::json!({
                "file": leaf(p),
                "when": m.when,
                "build": m.build,
                "match_id": m.match_id,
            })),
            // A marker that cannot be read cannot be shown to belong to ANOTHER match either, and
            // evidence is not thrown away on a guess: it rides along.
            Err(_) => {
                keep.insert(leaf(p));
            }
        }
    }
    let kept = files
        .into_iter()
        .filter(|p| {
            let l = leaf(p);
            if l.ends_with(".marker") {
                keep.contains(&l)
            } else if let Some(stem) = l.strip_suffix(crate::crash::CTX32_SUFFIX) {
                keep.contains(stem)
            } else {
                true
            }
        })
        .collect();
    (kept, older)
}

/// The crash markers a report about `session` would carry (absolute paths of the `.marker`
/// files). What the Report page lists under "What goes in".
pub fn markers_for_session(logs_root: &Path, session: &Path) -> Vec<PathBuf> {
    let (_, crash) = root_files(logs_root);
    let facts = session_facts(logs_root, session);
    let (kept, _) = split_crash_files(crash, Some(&facts));
    kept.into_iter()
        .filter(|p| p.extension().is_some_and(|e| e == "marker"))
        .collect()
}

/// Every `mh_crash_*.marker` directly under the logs root with no `.reported` flag yet.
pub fn unreported_markers(logs_root: &Path) -> Vec<PathBuf> {
    let (_, crash) = root_files(logs_root);
    crash
        .into_iter()
        .filter(|p| p.extension().is_some_and(|e| e == "marker") && !is_reported(p))
        .collect()
}

/// `unreported_markers` over several logs roots.
pub fn unreported_markers_in(roots: &[PathBuf]) -> Vec<PathBuf> {
    roots.iter().flat_map(|r| unreported_markers(r)).collect()
}

// ---- dist RL14: the match list ---------------------------------------------------------------

/// How a match ended, from `session.json`'s `outcome` (RL15), with the launcher's one inference.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MatchOutcome {
    Finished,
    Quit,
    Desync,
    /// INFERRED: the file still says `running` and the game process is gone. The game never
    /// writes `crash` (a dying process is not trusted to rewrite a file).
    Crash,
    /// `running`, and the game is alive right now.
    Running,
    /// No `outcome` (a session from before RL15) or one we do not know.
    Unknown,
}

/// One line of the Report page's match list.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct MatchRow {
    /// The session directory's leaf name -- what selecting the row sets `session_pick` to.
    pub dir: String,
    pub match_id: String,
    /// UTC. From `began`, else the directory name's stamp.
    pub when: Option<chrono::NaiveDateTime>,
    /// `network`, `campaign`, `skirmish`, `tutorial`, `tactical`, or empty when unknown.
    pub mode: String,
    pub map: String,
    /// Human player names in slot order.
    pub players: Vec<String>,
    pub ai_count: u32,
    pub outcome: MatchOutcome,
    /// `host`, `client`, ... -- this peer's role in that session (empty when unknown).
    pub role: String,
}

fn json_str(v: &serde_json::Value, key: &str) -> String {
    v.get(key)
        .and_then(|x| x.as_str())
        .map(|s| s.trim().to_string())
        .unwrap_or_default()
}

fn parse_stamp(s: &str) -> Option<chrono::NaiveDateTime> {
    let compact = utc_stamp_prefix(s.trim())?;
    chrono::NaiveDateTime::parse_from_str(&compact, "%Y%m%dT%H%M%SZ").ok()
}

/// One row from a session directory name and its `session.json` text (if any). Tolerant by design:
/// every field may be missing, of the wrong type, or from a game older than RL15 -- a field that
/// cannot be read is simply empty, never a failure of the list. `running` is returned as
/// `Running`; deciding it is a crash needs to know whether the game is alive (`match_rows`).
pub fn parse_match_row(dir: &str, session_json: Option<&str>) -> MatchRow {
    let v: serde_json::Value = session_json
        .and_then(|t| serde_json::from_str(t).ok())
        .unwrap_or(serde_json::Value::Null);
    // The directory name is `<stamp>_<mid8>_<map>_<mode>`: a fallback for a missing file.
    let parts: Vec<&str> = dir.split('_').collect();
    let (name_map, name_mode) = if parts.len() == 4 && parts[1] != "menu" {
        (parts[2].to_string(), parts[3].to_string())
    } else {
        (String::new(), String::new())
    };
    let role = {
        let r = json_str(&v, "role");
        if r.is_empty() {
            json_str(&v, "mode")
        } else {
            r
        }
    };
    let raw_mode = {
        let m = json_str(&v, "mode");
        if m.is_empty() {
            name_mode
        } else {
            m
        }
    };
    let mode = match raw_mode.to_ascii_lowercase().as_str() {
        "host" | "client" => "network".to_string(),
        m @ ("campaign" | "skirmish" | "tutorial" | "tactical") => m.to_string(),
        _ => String::new(),
    };
    let map = {
        let m = json_str(&v, "map");
        // `map` is a path ("Maps\\krater.mpm"): the leaf, without its extension.
        let leaf = m.rsplit(['\\', '/']).next().unwrap_or("");
        let leaf = leaf.rsplit_once('.').map_or(leaf, |(stem, _)| stem);
        if leaf.is_empty() {
            name_map
        } else {
            leaf.to_string()
        }
    };
    let players: Vec<String> = v
        .get("players")
        .and_then(|p| p.as_array())
        .map(|a| {
            a.iter()
                .filter_map(|x| x.as_str())
                .map(|s| s.trim().to_string())
                .filter(|s| !s.is_empty())
                .collect()
        })
        .unwrap_or_default();
    let ai_count = v
        .get("ai_count")
        .and_then(|x| x.as_u64())
        .map_or(0, |n| n.min(64) as u32);
    let reason = json_str(&v, "reason");
    let outcome = match json_str(&v, "outcome").as_str() {
        "finished" => MatchOutcome::Finished,
        "quit" => MatchOutcome::Quit,
        "desync" => MatchOutcome::Desync,
        "running" => MatchOutcome::Running,
        // A session written before RL15 has no outcome, but a closed one has a reason.
        "" if !reason.is_empty() => {
            if reason == "gameover" {
                MatchOutcome::Finished
            } else {
                MatchOutcome::Quit
            }
        }
        _ => MatchOutcome::Unknown,
    };
    MatchRow {
        dir: dir.to_string(),
        match_id: json_str(&v, "match_id"),
        when: parse_stamp(&json_str(&v, "began")).or_else(|| parse_stamp(dir)),
        mode,
        map,
        players,
        ai_count,
        outcome,
        role,
    }
}

/// The match list: every session under `logs_root`, newest first, one row per MATCH.
///
/// * Two copies of one match (`match_id`) -- the host's and a client's, as when two peers share a
///   machine -- collapse to the HOST's: a client never reliably learns the host's name (its
///   `players` was seen as `["client", "client"]`), so the host's file is the one to believe.
/// * `running` with the game gone is a crash. `game_running` says whether a game process is alive;
///   then only the newest session can still be in progress, everything older is over.
pub fn match_rows(logs_roots: &[PathBuf], game_running: bool) -> Vec<MatchRow> {
    let mut rows: Vec<MatchRow> = session_dirs_in(logs_roots)
        .into_iter()
        .map(|dir| {
            let leaf = dir
                .file_name()
                .map(|n| n.to_string_lossy().to_string())
                .unwrap_or_default();
            let text = std::fs::read_to_string(dir.join("session.json")).ok();
            parse_match_row(&leaf, text.as_deref())
        })
        .collect();
    finish_match_rows(&mut rows, game_running);
    rows
}

/// The pure half of `match_rows`: host preference and crash inference over rows (newest first).
pub fn finish_match_rows(rows: &mut Vec<MatchRow>, game_running: bool) {
    // Crash inference first, on every file: the newest `running` one is alive only while a game is.
    for (i, r) in rows.iter_mut().enumerate() {
        if r.outcome == MatchOutcome::Running && !(game_running && i == 0) {
            r.outcome = MatchOutcome::Crash;
        }
    }
    // One row per match: prefer the host's copy, else the newest.
    let mut keep: Vec<MatchRow> = Vec::new();
    for r in rows.drain(..) {
        if r.match_id.is_empty() {
            keep.push(r);
            continue;
        }
        match keep.iter().position(|k| k.match_id == r.match_id) {
            None => keep.push(r),
            Some(i) => {
                if r.role == "host" && keep[i].role != "host" {
                    // The host's copy replaces the earlier one in the SAME place in the list.
                    keep[i] = r;
                }
            }
        }
    }
    *rows = keep;
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
                ini_path: None,
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
            ini_path: None,
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
            ini_path: None,
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
            ini_path: None,
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
            ini_path: None,
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
            ini_path: None,
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
            ini_path: None,
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

    // ---- mp:D43: the state recordings, tailed at a KEYF chunk boundary --------------------------

    /// One `{tag; payload_len; crc32=0; payload}` chunk (`docs/state-record.md`). The CRC is never
    /// checked by the launcher's chunk walk (it never decodes a payload), so a fixture can leave it
    /// zero.
    fn push_chunk(out: &mut Vec<u8>, tag: u32, payload: &[u8]) {
        out.extend_from_slice(&tag.to_le_bytes());
        out.extend_from_slice(&(payload.len() as u32).to_le_bytes());
        out.extend_from_slice(&0u32.to_le_bytes());
        out.extend_from_slice(payload);
    }

    /// A v1 state-record header: magic/version/flags, `header_len` filled in against its own
    /// length, one region (`HASHTBL`) of `region_len` bytes -- everything `state_layout` and
    /// `state_tail` need, and nothing a real writer's region table would add beyond it.
    fn state_header(region_len: u32, keyframe_every: u32) -> Vec<u8> {
        let name = b"HASHTBL";
        let mut h = Vec::new();
        h.extend_from_slice(&STATE_MAGIC.to_le_bytes());
        h.extend_from_slice(&STATE_VERSION.to_le_bytes());
        h.extend_from_slice(&0u16.to_le_bytes()); // flags
        let len_pos = h.len();
        h.extend_from_slice(&0u32.to_le_bytes()); // header_len, filled in below
        h.extend_from_slice(&0u64.to_le_bytes()); // manifest_fp
        h.extend_from_slice(&keyframe_every.to_le_bytes());
        h.extend_from_slice(&1u32.to_le_bytes()); // region_count
        h.extend_from_slice(&region_len.to_le_bytes());
        h.push(name.len() as u8);
        h.extend_from_slice(name);
        let total = h.len() as u32;
        h[len_pos..len_pos + 4].copy_from_slice(&total.to_le_bytes());
        h
    }

    /// A whole v1 state recording: `keyframe_count` `KEYF` chunks of `region_len` incompressible
    /// bytes each (the format's "first chunk after the header is always a KEYF", so this is a
    /// realistic fixture), a small `STEP` chunk between consecutive keyframes (so the chunk walk
    /// exercises skipping a non-`KEYF` tag, not just reading one), and an `END ` chunk. Each `KEYF`
    /// payload's leading `u32 step` counts up by one per keyframe, starting at 0.
    fn state_file(keyframe_count: u32, region_len: usize, seed: u64) -> Vec<u8> {
        let step_tag = u32::from_le_bytes(*b"STEP");
        let end_tag = u32::from_le_bytes(*b"END ");
        let mut out = state_header(region_len as u32, 3000);
        let mut step = 0u32;
        for k in 0..keyframe_count {
            let mut payload = Vec::with_capacity(4 + region_len);
            payload.extend_from_slice(&step.to_le_bytes());
            payload.extend_from_slice(&noise(region_len, seed.wrapping_add(u64::from(k)) | 1));
            push_chunk(&mut out, STATE_TAG_KEYF, &payload);
            if k + 1 < keyframe_count {
                let mut step_payload = Vec::with_capacity(8);
                step_payload.extend_from_slice(&step.to_le_bytes());
                step_payload.extend_from_slice(&0u32.to_le_bytes()); // run_count = 0
                push_chunk(&mut out, step_tag, &step_payload);
            }
            step += 1;
        }
        let mut end_payload = Vec::with_capacity(16);
        end_payload.extend_from_slice(&step.saturating_sub(1).to_le_bytes());
        end_payload.extend_from_slice(&keyframe_count.to_le_bytes());
        end_payload.extend_from_slice(&0u64.to_le_bytes());
        push_chunk(&mut out, end_tag, &end_payload);
        out
    }

    /// A state recording that fits under the budget as-is is copied byte for byte -- nothing about
    /// it is even parsed, since nothing forced a cut.
    #[test]
    fn a_state_recording_that_fits_is_kept_whole() {
        let dir = fresh("state_whole");
        let name = "20260927T090000Z_a1b2c3d4_1_host";
        let s = dir.join("logs").join(name);
        std::fs::create_dir_all(&s).unwrap();
        let state = state_file(3, 4 * 1024, 41);
        std::fs::write(s.join("mh_match_state.bin"), &state).unwrap();

        let zip = dir.join("out").join("report.zip");
        let built = build(&zip, &input_for(&dir, s.clone(), "fits fine")).unwrap();
        let entry = entries_of(&zip)
            .into_iter()
            .find(|(n, _)| n == &format!("logs/{name}/mh_match_state.bin"))
            .expect("the state recording is in the report");
        assert!(
            entry.1 == state,
            "the state recording was changed when nothing forced a cut"
        );

        let meta: serde_json::Value = serde_json::from_str(&built.meta).unwrap();
        assert!(
            meta["dropped"].as_array().unwrap().is_empty(),
            "{}",
            built.meta
        );
        std::fs::remove_dir_all(&dir).ok();
    }

    /// mp:D43's done_when (a): over budget, the replay input stays whole and the state recording is
    /// cut at a `KEYF` boundary -- the kept bytes are themselves a valid v1 file (header, then a
    /// suffix of the original chunk stream whose first chunk is a `KEYF`), and `report.json` names
    /// the cut with its own reason and the kept keyframe's step.
    #[test]
    fn an_over_budget_report_keeps_replay_inputs_whole_and_tails_the_state_file_at_a_keyframe() {
        let dir = fresh("state_tail");
        let name = "20260927T091000Z_deadbeef_1_host";
        let s = dir.join("logs").join(name);
        std::fs::create_dir_all(&s).unwrap();

        let orders = noise(256 * 1024, 5);
        std::fs::write(s.join("mh_match_orders.bin"), &orders).unwrap();

        // Six ~700 KB incompressible keyframes: a ~4.2 MB recording, nowhere near a 2 MB budget.
        let state = state_file(6, 700 * 1024, 77);
        std::fs::write(s.join("mh_match_state.bin"), &state).unwrap();

        let zip = dir.join("out").join("report.zip");
        let budget = 2 * 1024 * 1024u64;
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
            .find(|(n, _)| n == &format!("logs/{name}/mh_match_orders.bin"))
            .expect("the replay input was dropped");
        assert!(
            replay.1 == orders,
            "the replay input went in partially or changed"
        );

        let kept = &entries
            .iter()
            .find(|(n, _)| n == &format!("logs/{name}/mh_match_state.bin"))
            .expect("the state recording is gone entirely")
            .1;
        assert!(
            kept.len() < state.len(),
            "the state recording was kept whole despite the budget"
        );

        let (orig_header_len, _) = state_layout(&state).unwrap();
        assert!(
            kept.len() as u64 >= orig_header_len,
            "the kept bytes are shorter than the header alone"
        );
        assert_eq!(
            &kept[..orig_header_len as usize],
            &state[..orig_header_len as usize],
            "the header was not carried over verbatim"
        );

        // The kept bytes decode as a v1 file in their own right: same header, first chunk a KEYF.
        let (kept_header_len, kept_keyframes) =
            state_layout(kept).expect("the kept bytes are not a valid v1 state file");
        assert_eq!(kept_header_len, orig_header_len);
        assert_eq!(
            kept_keyframes[0].0, orig_header_len,
            "the first chunk after the header must be the kept KEYF"
        );
        // It really is a TAIL: everything past the header is a suffix of the original chunk stream.
        assert!(
            state.ends_with(&kept[orig_header_len as usize..]),
            "the kept chunks are not a suffix of the original file"
        );

        let meta: serde_json::Value = serde_json::from_str(&built.meta).unwrap();
        let rec = meta["dropped"]
            .as_array()
            .unwrap()
            .iter()
            .find(|d| {
                d["file"]
                    .as_str()
                    .is_some_and(|f| f.ends_with("mh_match_state.bin"))
            })
            .expect("the state cut is named in dropped");
        assert_eq!(rec["why"].as_str().unwrap(), WHY_STATE_TAILED, "{rec:?}");
        let kept_bytes = rec["kept_bytes"].as_u64().unwrap();
        assert!(kept_bytes > 0 && kept_bytes < state.len() as u64, "{rec:?}");
        let kept_from_step = rec["kept_from_step"].as_u64().unwrap();
        assert_eq!(
            kept_from_step, kept_keyframes[0].1 as u64,
            "the reported step must match the kept KEYF's own payload"
        );
        std::fs::remove_dir_all(&dir).ok();
    }

    /// A state file that fails the chunk walk -- here, a bad magic -- is dropped whole with its own
    /// reason rather than tailed at a guess, once the budget actually needs it to shrink.
    #[test]
    fn a_malformed_state_recording_is_dropped_with_reason() {
        let dir = fresh("state_malformed");
        let name = "20260927T092000Z_baadf00d_1_host";
        let s = dir.join("logs").join(name);
        std::fs::create_dir_all(&s).unwrap();

        let mut garbage = noise(2 * 1024 * 1024, 9);
        garbage[0..4].copy_from_slice(&[0, 0, 0, 0]); // never the "MHSR" magic
        std::fs::write(s.join("mh_desync_state.bin"), &garbage).unwrap();

        let zip = dir.join("out").join("report.zip");
        let budget = 1024 * 1024u64; // under the garbage file alone, so it must be shrunk
        let built =
            build_with_budget(&zip, &input_for(&dir, s.clone(), "weird file"), budget).unwrap();

        let names: Vec<String> = entries_of(&zip).into_iter().map(|(n, _)| n).collect();
        assert!(
            !names.contains(&format!("logs/{name}/mh_desync_state.bin")),
            "{names:?}"
        );

        let meta: serde_json::Value = serde_json::from_str(&built.meta).unwrap();
        let rec = meta["dropped"]
            .as_array()
            .unwrap()
            .iter()
            .find(|d| {
                d["file"]
                    .as_str()
                    .is_some_and(|f| f.ends_with("mh_desync_state.bin"))
            })
            .expect("the malformed state file is named in dropped");
        assert_eq!(rec["why"].as_str().unwrap(), WHY_STATE_MALFORMED, "{rec:?}");
        assert!(
            rec.get("kept_bytes").is_none(),
            "a whole drop has no kept_bytes: {rec:?}"
        );
        std::fs::remove_dir_all(&dir).ok();
    }

    /// A valid state recording whose only keyframe still does not fit under what the budget can
    /// give it is dropped whole -- never tailed past its own newest keyframe.
    #[test]
    fn a_state_recording_whose_only_keyframe_does_not_fit_is_dropped_whole() {
        let dir = fresh("state_no_room");
        let name = "20260927T093000Z_c0ffee00_1_host";
        let s = dir.join("logs").join(name);
        std::fs::create_dir_all(&s).unwrap();

        let state = state_file(1, 2 * 1024 * 1024, 13); // one ~2 MB keyframe
        std::fs::write(s.join("mh_match_state.bin"), &state).unwrap();

        let zip = dir.join("out").join("report.zip");
        let budget = 64 * 1024u64; // far under the header plus its single keyframe
        let built =
            build_with_budget(&zip, &input_for(&dir, s.clone(), "way too small"), budget).unwrap();

        let names: Vec<String> = entries_of(&zip).into_iter().map(|(n, _)| n).collect();
        assert!(
            !names.contains(&format!("logs/{name}/mh_match_state.bin")),
            "{names:?}"
        );

        let meta: serde_json::Value = serde_json::from_str(&built.meta).unwrap();
        let rec = meta["dropped"]
            .as_array()
            .unwrap()
            .iter()
            .find(|d| {
                d["file"]
                    .as_str()
                    .is_some_and(|f| f.ends_with("mh_match_state.bin"))
            })
            .expect("the oversize single-keyframe file is named in dropped");
        assert_eq!(rec["why"].as_str().unwrap(), WHY_STATE_NO_ROOM, "{rec:?}");
        std::fs::remove_dir_all(&dir).ok();
    }

    // ---- mp:D46: the state recording arrives gzip-compressed (`mh_match_state.bin.gz`) ----------

    fn gzip(bytes: &[u8]) -> Vec<u8> {
        let mut e = flate2::write::GzEncoder::new(Vec::new(), flate2::Compression::fast());
        e.write_all(bytes).unwrap();
        e.finish().unwrap()
    }

    fn compression_of(zip: &Path, entry: &str) -> zip::CompressionMethod {
        let f = std::fs::File::open(zip).unwrap();
        let mut a = zip::ZipArchive::new(f).unwrap();
        let e = a.by_name(entry).unwrap();
        e.compression()
    }

    /// A compressed recording of the size a real 36-minute match leaves (~39 MB) rides in whole under
    /// the REAL `BODY_BUDGET`, byte for byte, STORED (it is already deflated: deflating it again would
    /// only cost time). A leftover `.gz.tmp` is no recording and stays out; a raw file whose finished
    /// `.gz` twin sits beside it (a kill between the rename and the delete) is left out too.
    #[test]
    fn a_gzip_state_recording_of_field_size_is_carried_whole_within_the_body_budget() {
        let dir = fresh("state_gz_whole");
        let name = "20261004T090000Z_a1b2c3d4_1_host";
        let s = dir.join("logs").join(name);
        std::fs::create_dir_all(&s).unwrap();
        // ~39 MB of gzip: a valid gzip member whose payload deflate cannot shrink.
        let payload = noise(39 * 1024 * 1024, 4242);
        let gz = gzip(&payload);
        assert!(gz.len() as u64 > 38_000_000, "{}", gz.len());
        std::fs::write(s.join("mh_match_state.bin.gz"), &gz).unwrap();
        std::fs::write(s.join("mh_match_state.bin.gz.tmp"), b"unfinished").unwrap();
        std::fs::write(s.join("mh_desync_state.bin"), state_file(2, 4 * 1024, 3)).unwrap();
        // The twin rule: a raw file beside its finished .gz is the same recording.
        std::fs::write(s.join("mh_match_state_2.bin"), b"raw twin").unwrap();
        std::fs::write(s.join("mh_match_state_2.bin.gz"), gzip(b"raw twin")).unwrap();

        let zip = dir.join("out").join("report.zip");
        let built = build(&zip, &input_for(&dir, s.clone(), "a long match")).unwrap();
        assert!(body_of(&built) <= BODY_BUDGET, "{}", body_of(&built));
        let entries = entries_of(&zip);
        let entry = entries
            .iter()
            .find(|(n, _)| n == &format!("logs/{name}/mh_match_state.bin.gz"))
            .expect("the compressed recording is in the report");
        assert!(entry.1 == gz, "the compressed recording was changed");
        assert_eq!(
            compression_of(&zip, &format!("logs/{name}/mh_match_state.bin.gz")),
            zip::CompressionMethod::Stored
        );
        let names: Vec<&str> = entries.iter().map(|(n, _)| n.as_str()).collect();
        assert!(
            !names.iter().any(|n| n.ends_with(".gz.tmp")),
            "an unfinished compress went into the report: {names:?}"
        );
        assert!(
            !names.iter().any(|n| n.ends_with("mh_match_state_2.bin")),
            "the raw twin of a finished .gz went in as well: {names:?}"
        );
        assert!(
            names.iter().any(|n| n.ends_with("mh_match_state_2.bin.gz")),
            "{names:?}"
        );
        let meta: serde_json::Value = serde_json::from_str(&built.meta).unwrap();
        assert!(
            meta["dropped"].as_array().unwrap().is_empty(),
            "{}",
            meta["dropped"]
        );
        std::fs::remove_dir_all(&dir).ok();
    }

    /// Over budget, a gzip recording is unpacked, cut at a keyframe, and carried as a PLAIN v1 file
    /// (`...bin`, not `.gz`): same header, first chunk a `KEYF`, a suffix of the original stream. The
    /// replay inputs stay whole, and `dropped` names the cut with the entry it became (`kept_as`).
    #[test]
    fn an_over_budget_report_tails_a_gzip_state_recording_at_a_keyframe() {
        let dir = fresh("state_gz_tail");
        let name = "20261004T091000Z_deadbeef_1_host";
        let s = dir.join("logs").join(name);
        std::fs::create_dir_all(&s).unwrap();
        let orders = noise(256 * 1024, 5);
        std::fs::write(s.join("mh_match_orders.bin"), &orders).unwrap();
        let state = state_file(6, 700 * 1024, 77);
        std::fs::write(s.join("mh_match_state.bin.gz"), gzip(&state)).unwrap();

        let zip = dir.join("out").join("report.zip");
        let budget = 2 * 1024 * 1024u64;
        let built = build_with_budget(
            &zip,
            &input_for(&dir, s.clone(), "desync near the end"),
            budget,
        )
        .unwrap();
        assert!(body_of(&built) <= budget, "{} > {budget}", body_of(&built));

        let entries = entries_of(&zip);
        let names: Vec<&str> = entries.iter().map(|(n, _)| n.as_str()).collect();
        assert!(
            !names.contains(&format!("logs/{name}/mh_match_state.bin.gz").as_str()),
            "the over-budget gzip file was carried whole: {names:?}"
        );
        assert!(
            entries
                .iter()
                .find(|(n, _)| n == &format!("logs/{name}/mh_match_orders.bin"))
                .is_some_and(|(_, b)| *b == orders),
            "the replay input was not kept whole"
        );
        assert!(built
            .entries
            .contains(&format!("logs/{name}/mh_match_state.bin")));
        let kept = &entries
            .iter()
            .find(|(n, _)| n == &format!("logs/{name}/mh_match_state.bin"))
            .expect("the tail is carried as the plain .bin")
            .1;
        let (orig_header_len, _) = state_layout(&state).unwrap();
        assert!(kept.len() < state.len());
        assert_eq!(
            &kept[..orig_header_len as usize],
            &state[..orig_header_len as usize]
        );
        let (_, kept_keyframes) = state_layout(kept).expect("the tail is not a valid v1 file");
        assert_eq!(kept_keyframes[0].0, orig_header_len);
        assert!(state.ends_with(&kept[orig_header_len as usize..]));

        let meta: serde_json::Value = serde_json::from_str(&built.meta).unwrap();
        let rec = meta["dropped"]
            .as_array()
            .unwrap()
            .iter()
            .find(|d| {
                d["file"]
                    .as_str()
                    .is_some_and(|f| f.ends_with("mh_match_state.bin.gz"))
            })
            .expect("the cut is named in dropped under its on-disk (.gz) name");
        assert_eq!(rec["why"].as_str().unwrap(), WHY_STATE_TAILED, "{rec:?}");
        assert_eq!(
            rec["kept_as"].as_str().unwrap(),
            format!("logs/{name}/mh_match_state.bin"),
            "{rec:?}"
        );
        assert_eq!(
            rec["kept_from_step"].as_u64().unwrap(),
            kept_keyframes[0].1 as u64
        );
        std::fs::remove_dir_all(&dir).ok();
    }

    /// The defined behaviour when even the compressed recording cannot be used: a `.gz` that does not
    /// unpack is left out whole with its own reason (never cut at a guess), and a valid one whose
    /// newest keyframe still does not fit is left out with the same "no room" reason as a raw file.
    #[test]
    fn a_gzip_state_recording_that_cannot_be_cut_is_dropped_with_a_reason() {
        let dir = fresh("state_gz_drop");
        let name = "20261004T092000Z_baadf00d_1_host";
        let s = dir.join("logs").join(name);
        std::fs::create_dir_all(&s).unwrap();
        std::fs::write(s.join("mh_desync_state.bin.gz"), noise(2 * 1024 * 1024, 9)).unwrap();
        let state = state_file(1, 2 * 1024 * 1024, 13);
        std::fs::write(s.join("mh_match_state.bin.gz"), gzip(&state)).unwrap();

        let zip = dir.join("out").join("report.zip");
        let built = build_with_budget(&zip, &input_for(&dir, s.clone(), "nothing fits"), 64 * 1024)
            .unwrap();
        let names: Vec<String> = entries_of(&zip).into_iter().map(|(n, _)| n).collect();
        assert!(
            !names.iter().any(|n| n.contains("_state.bin")),
            "an unusable state file went in: {names:?}"
        );
        let meta: serde_json::Value = serde_json::from_str(&built.meta).unwrap();
        let why_of = |f: &str| {
            meta["dropped"]
                .as_array()
                .unwrap()
                .iter()
                .find(|d| d["file"].as_str().is_some_and(|x| x.ends_with(f)))
                .unwrap_or_else(|| panic!("{f} is not named in dropped"))["why"]
                .as_str()
                .unwrap()
                .to_string()
        };
        assert_eq!(why_of("mh_desync_state.bin.gz"), WHY_STATE_GZ_BAD);
        assert_eq!(why_of("mh_match_state.bin.gz"), WHY_STATE_NO_ROOM);
        std::fs::remove_dir_all(&dir).ok();
    }

    /// dist LA17: `state_tail` always lands under the target, never keeps a longer tail than the
    /// optimum a full scan would, and stays near that optimum. (The scan is the oracle; the search
    /// under test is an estimate, so "the same cut" is no longer the contract -- "fits, and not much
    /// shorter than it had to be" is.)
    #[test]
    fn the_keyframe_search_fits_and_stays_near_the_optimum() {
        let state = state_file(24, 96 * 1024, 31);
        let (header_len, keyframes) = state_layout(&state).unwrap();
        let opts = zip::write::SimpleFileOptions::default()
            .compression_method(zip::CompressionMethod::Deflated);
        let sizes: Vec<u64> = keyframes
            .iter()
            .map(|&(off, _)| {
                let mut body = state[..header_len as usize].to_vec();
                body.extend_from_slice(&state[off as usize..]);
                compress_one("x", &body, opts).unwrap().1
            })
            .collect();
        let whole = sizes[0] as f64 / state.len() as f64;
        for target in [
            sizes[0] + 1,
            sizes[0],
            sizes[7] + 100,
            sizes[7],
            sizes[7] - 1,
            sizes[23] + 1,
            sizes[23],
            sizes[23] - 1,
            0,
        ] {
            let want = sizes.iter().position(|&c| c <= target);
            let got = state_tail(&state, header_len, &keyframes, "x", target, whole, opts).unwrap();
            let Some((zip, _, step)) = got else {
                // Nothing fits only when the newest keyframe's own tail is over the target.
                assert!(
                    target < sizes[23],
                    "target {target}: gave up although the newest fits"
                );
                assert!(want.is_none());
                continue;
            };
            let idx = step as usize; // the fixture's steps count 0.. per keyframe
            let comp = zip::ZipArchive::new(std::io::Cursor::new(&zip[..]))
                .unwrap()
                .by_index_raw(0)
                .unwrap()
                .compressed_size();
            assert!(comp <= target, "target {target}: kept tail is {comp}");
            let best = want.expect("a fit exists");
            assert!(
                idx >= best,
                "target {target}: cut at {idx}, before the optimum {best}"
            );
            // Within ~15% of the file's keyframes of the optimum (the first guess aims 8% low).
            assert!(
                idx <= best + 4,
                "target {target}: cut at {idx}, optimum {best} -- too much thrown away"
            );
        }
    }

    /// dist LA17: a miss on the first estimate steps to a later keyframe by the ratio it measured,
    /// and the bytes deflated across all probes stay within one pass over the file.
    #[test]
    fn a_wrong_ratio_hint_costs_at_most_one_extra_pass() {
        let state = state_file(24, 96 * 1024, 53);
        let (header_len, keyframes) = state_layout(&state).unwrap();
        let opts = zip::write::SimpleFileOptions::default()
            .compression_method(zip::CompressionMethod::Deflated);
        // Noise: the true ratio is ~1.0. Claim 0.65 -- the estimate is 1.5x too generous.
        let target = state.len() as u64 / 2;
        hook::take();
        let got = state_tail(&state, header_len, &keyframes, "x", target, 0.65, opts)
            .unwrap()
            .expect("half the file's bytes buys a tail");
        let deflated = hook::take();
        let comp = zip::ZipArchive::new(std::io::Cursor::new(&got.0[..]))
            .unwrap()
            .by_index_raw(0)
            .unwrap()
            .compressed_size();
        assert!(comp <= target, "{comp} > {target}");
        assert!(
            deflated <= state.len() as u64,
            "{deflated} bytes deflated for a {} byte file",
            state.len()
        );
    }

    /// Does `entry`'s uncompressed stream equal `header + state[off..]`? Streamed, so a ~250 MB check
    /// does not hold a second copy.
    fn entry_is_header_plus_tail(zip: &Path, entry: &str, header: &[u8], tail: &[u8]) -> bool {
        let f = std::fs::File::open(zip).unwrap();
        let mut a = zip::ZipArchive::new(f).unwrap();
        let mut e = a.by_name(entry).unwrap();
        let mut want = header.iter().chain(tail.iter());
        let mut buf = vec![0u8; 1 << 20];
        loop {
            let n = std::io::Read::read(&mut e, &mut buf).unwrap();
            if n == 0 {
                break;
            }
            for b in &buf[..n] {
                if want.next() != Some(b) {
                    return false;
                }
            }
        }
        want.next().is_none()
    }

    /// A state recording of `keyframes` x `kf_len` bytes where `noise_len` of each keyframe is
    /// incompressible and the rest is zeros (a ratio of about `kf_len / noise_len`), written to `path`
    /// chunk by chunk. Returns (file length, header_len, keyframe offsets).
    fn write_big_state(
        path: &Path,
        keyframes: u32,
        kf_len: usize,
        noise_len: usize,
    ) -> (u64, u64, Vec<(u64, u32)>) {
        let header = state_header(kf_len as u32, 3000);
        let mut f = std::io::BufWriter::new(std::fs::File::create(path).unwrap());
        f.write_all(&header).unwrap();
        let mut pos = header.len() as u64;
        let blocks: Vec<Vec<u8>> = (0..4).map(|i| noise(noise_len, 1000 + i)).collect();
        let mut offs = Vec::new();
        for k in 0..keyframes {
            let mut payload = vec![0u8; kf_len];
            payload[..4].copy_from_slice(&k.to_le_bytes());
            let b = &blocks[k as usize % blocks.len()];
            payload[4..4 + b.len() - 4].copy_from_slice(&b[4..]);
            offs.push((pos, k));
            let mut chunk = Vec::new();
            push_chunk(&mut chunk, STATE_TAG_KEYF, &payload);
            f.write_all(&chunk).unwrap();
            pos += chunk.len() as u64;
        }
        let mut end = Vec::new();
        push_chunk(&mut end, u32::from_le_bytes(*b"END "), &[0u8; 16]);
        f.write_all(&end).unwrap();
        pos += end.len() as u64;
        f.flush().unwrap();
        (pos, header.len() as u64, offs)
    }

    /// dist LA17 done_when (b): a synthetic ~250 MB RAW state recording whose whole deflate is over
    /// the real `BODY_BUDGET` is staged once and tailed with at most one more pass -- two deflate passes
    /// over the file, counted by the hook -- the report fits `BODY_BUDGET`, and the kept bytes are the
    /// header plus an exact suffix of the original starting at a `KEYF` chunk.
    #[test]
    fn a_250_mb_raw_state_recording_over_budget_takes_at_most_two_deflate_passes() {
        let dir = fresh("state_250mb");
        let name = "20260927T095000Z_f00dfeed_1_host";
        let s = dir.join("logs").join(name);
        std::fs::create_dir_all(&s).unwrap();
        let state_path = s.join("mh_match_state.bin");
        // 40 keyframes x 6.25 MB = 250 MB; 2.5 MB of each is noise, so ~100 MB deflated: well over 62 MB.
        let (len, header_len, offs) = write_big_state(&state_path, 40, 6_250_000, 2_500_000);
        assert!(len > 250_000_000, "{len}");

        let zip = dir.join("out").join("report.zip");
        hook::take();
        let built = build(&zip, &input_for(&dir, s.clone(), "desync near the end")).unwrap();
        let deflated = hook::take();
        eprintln!(
            "deflate passes over the {len} byte state file: {:.2}",
            deflated as f64 / len as f64
        );
        assert!(
            deflated <= 2 * len,
            "{deflated} bytes deflated for a {len} byte file: more than two passes"
        );
        assert!(
            body_of(&built) <= BODY_BUDGET,
            "{} > {BODY_BUDGET}",
            body_of(&built)
        );

        let meta: serde_json::Value = serde_json::from_str(&built.meta).unwrap();
        let rec = meta["dropped"]
            .as_array()
            .unwrap()
            .iter()
            .find(|d| d["file"] == format!("logs/{name}/mh_match_state.bin").as_str())
            .unwrap_or_else(|| panic!("the cut is not in dropped: {}", built.meta))
            .clone();
        let step = rec["kept_from_step"].as_u64().expect("kept_from_step") as usize;
        assert!(step > 0 && step < 40, "{rec}");
        // The estimate should keep most of what the budget buys: the kept tail is ~60% of the file
        // at this ratio (62 MB of ~100 MB); allow a wide band, the point is "not nothing, not all".
        let kept_bytes = rec["kept_bytes"].as_u64().unwrap();
        assert!(kept_bytes > len / 4 && kept_bytes < len, "{rec}");

        let raw = std::fs::read(&state_path).unwrap();
        let entry = format!("logs/{name}/mh_match_state.bin");
        assert!(
            entry_is_header_plus_tail(
                &zip,
                &entry,
                &raw[..header_len as usize],
                &raw[offs[step].0 as usize..]
            ),
            "the kept bytes are not the header plus the suffix from keyframe {step}"
        );
        std::fs::remove_dir_all(&dir).ok();
    }

    /// dist LA17: a raw state recording in an OPTIONAL folder that the budget is certain to shed is
    /// not deflated at all -- it is marked dropped (the shed order is unchanged: that folder was the
    /// first thing `shed` would give up anyway).
    #[test]
    fn an_optional_folder_behind_a_full_budget_is_not_staged() {
        let dir = fresh("opt_not_staged");
        let logs = dir.join("logs");
        let mk = |stamp: &str| {
            let d = logs.join(format!("{stamp}_aaaa{stamp}_1_host"));
            std::fs::create_dir_all(&d).unwrap();
            d
        };
        let new = mk("20260927T100000Z");
        let old = mk("20260927T090000Z");
        // The reported folder alone is over the (small) budget with noise; the older folder holds
        // a raw state recording that must never be deflated.
        std::fs::write(new.join("mh_net.log"), noisy_log(900 * 1024, 5)).unwrap();
        let state = state_file(8, 512 * 1024, 7);
        std::fs::write(old.join("mh_match_state.bin"), &state).unwrap();

        let zip = dir.join("out").join("report.zip");
        hook::take();
        let built =
            build_with_budget(&zip, &input_for(&dir, new.clone(), "newest"), 256 * 1024).unwrap();
        assert_eq!(hook::take(), 0, "the shed folder's state file was deflated");
        let meta: serde_json::Value = serde_json::from_str(&built.meta).unwrap();
        let old_name = old.file_name().unwrap().to_string_lossy().to_string();
        assert!(
            meta["dropped"]
                .as_array()
                .unwrap()
                .iter()
                .any(|d| d["dir"] == old_name.as_str() && d["why"] == WHY_OVER_BUDGET),
            "{}",
            built.meta
        );
        assert!(!meta["included"]
            .as_array()
            .unwrap()
            .iter()
            .any(|v| v == old_name.as_str()));
        std::fs::remove_dir_all(&dir).ok();
    }

    /// dist LA17: the job is a state machine -- `Running` with a progress line until the thread
    /// answers, then `Done` once; the build ran on another thread; the result is what `build` returns.
    #[test]
    fn the_report_job_runs_off_the_caller_and_reports_progress() {
        let dir = fresh("job");
        let s = dir.join("logs").join("20260927T110000Z_1234abcd_1_host");
        std::fs::create_dir_all(&s).unwrap();
        std::fs::write(s.join("mh_net.log"), noisy_log(300 * 1024, 6)).unwrap();
        std::fs::write(s.join("mh_match_state.bin"), state_file(4, 256 * 1024, 3)).unwrap();
        let zip = dir.join("out").join("job.zip");
        let input = OwnedInput {
            game_dir: Some(dir.clone()),
            logs_root: None,
            session_dir: Some(s.clone()),
            launcher_started_utc: None,
            description: "it froze".to_string(),
            last_run: None,
            crash: None,
            minidump: None,
            launcher_log: None,
            ini_path: None,
        };
        let mut job = Job::start(zip.clone(), input);
        assert_ne!(job.worker_thread(), std::thread::current().id());
        let t0 = std::time::Instant::now();
        let built = loop {
            match job.poll() {
                Poll::Running => {
                    assert!(t0.elapsed().as_secs() < 60, "the job never finished");
                    std::thread::sleep(std::time::Duration::from_millis(2));
                }
                Poll::Done(r) => break r.expect("the build succeeds"),
            }
        };
        assert_eq!(built.zip, zip);
        assert!(zip.is_file());
        // "starting", the folder scan, one line per entry, "writing the report".
        assert!(
            job.progress().updates() >= 4,
            "{}",
            job.progress().updates()
        );
        assert_eq!(job.progress().text(), "writing the report");
        // A result is delivered once.
        assert!(matches!(job.poll(), Poll::Done(Err(_))));
        std::fs::remove_dir_all(&dir).ok();
    }

    /// A job whose build fails (no description) surfaces the same error `build` gives.
    #[test]
    fn a_failed_job_carries_the_builds_error() {
        let dir = fresh("job_err");
        let input = OwnedInput {
            game_dir: Some(dir.clone()),
            logs_root: None,
            session_dir: None,
            launcher_started_utc: None,
            description: "  ".to_string(),
            last_run: None,
            crash: None,
            minidump: None,
            launcher_log: None,
            ini_path: None,
        };
        let mut job = Job::start(dir.join("out").join("x.zip"), input);
        let t0 = std::time::Instant::now();
        loop {
            match job.poll() {
                Poll::Running => {
                    assert!(t0.elapsed().as_secs() < 60);
                    std::thread::sleep(std::time::Duration::from_millis(2));
                }
                Poll::Done(r) => {
                    assert_eq!(r.unwrap_err(), NO_DESCRIPTION);
                    break;
                }
            }
        }
        std::fs::remove_dir_all(&dir).ok();
    }
}

/// dist RL14: the match list, LA16's marker selection, and the `.reported` contract.
#[cfg(test)]
mod rl14_tests {
    use super::*;

    fn scratch(name: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("mh_launcher_test_rl14_{name}"));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        d
    }

    const FULL: &str = r#"{
      "match_id": "01a106fb5a4f4b0c8d6e2f3a4b5c6d7e", "slot": 0, "role": "host", "mode": "host",
      "map": "Maps\\Krater.mpm", "began": "20261008T211402Z", "ended": "20261008T213402Z",
      "reason": "gameover", "process_dir": "2026-10-08T21-10-00Z_menu_solo",
      "players": ["Alex", "Sasha", "Mark"], "ai_count": 1, "outcome": "finished"
    }"#;

    #[test]
    fn a_full_session_file_reads_into_one_row() {
        let r = parse_match_row("2026-10-08T21-14-02Z_aabbccdd_krater_host", Some(FULL));
        assert_eq!(r.mode, "network");
        assert_eq!(r.map, "Krater", "the leaf of the path, without .mpm");
        assert_eq!(r.players, ["Alex", "Sasha", "Mark"]);
        assert_eq!(r.ai_count, 1);
        assert_eq!(r.outcome, MatchOutcome::Finished);
        assert_eq!(r.role, "host");
        assert_eq!(
            r.when.unwrap().format("%Y-%m-%d %H:%M:%S").to_string(),
            "2026-10-08 21:14:02"
        );
    }

    #[test]
    fn every_other_mode_and_outcome_word_maps() {
        for (mode, want) in [
            ("client", "network"),
            ("campaign", "campaign"),
            ("skirmish", "skirmish"),
            ("tutorial", "tutorial"),
            ("tactical", "tactical"),
            ("nonsense", ""),
        ] {
            let j = format!(r#"{{"mode":"{mode}","outcome":"quit"}}"#);
            let r = parse_match_row("2026-10-08T21-14-02Z_aabbccdd_m_x", Some(&j));
            assert_eq!(r.mode, want, "{mode}");
            assert_eq!(r.outcome, MatchOutcome::Quit);
        }
        for (word, want) in [
            ("desync", MatchOutcome::Desync),
            ("running", MatchOutcome::Running),
            ("weird", MatchOutcome::Unknown),
        ] {
            let j = format!(r#"{{"outcome":"{word}"}}"#);
            assert_eq!(parse_match_row("x", Some(&j)).outcome, want, "{word}");
        }
    }

    /// A file from before RL15 (no outcome), a file that is not JSON, a field of the wrong type, and
    /// no file at all: each is a row, never a failure -- and the directory name fills in what it can.
    #[test]
    fn missing_or_wrong_fields_degrade_to_unknown_instead_of_failing() {
        let name = "2026-10-08T21-14-02Z_aabbccdd_krater_host";
        let none = parse_match_row(name, None);
        assert_eq!(
            (none.map.as_str(), none.mode.as_str()),
            ("krater", "network")
        );
        assert_eq!(none.outcome, MatchOutcome::Unknown);
        assert!(
            none.when.is_some(),
            "the stamp comes from the directory name"
        );
        let junk = parse_match_row(name, Some("{ not json"));
        assert_eq!(junk.outcome, MatchOutcome::Unknown);
        let wrong = parse_match_row(
            name,
            Some(r#"{"players": "Alex", "ai_count": "many", "outcome": 7, "map": 3}"#),
        );
        assert!(wrong.players.is_empty());
        assert_eq!(wrong.ai_count, 0);
        assert_eq!(wrong.outcome, MatchOutcome::Unknown);
        assert_eq!(wrong.map, "krater");
        // Pre-RL15: no `outcome`, but a closed session has a `reason`.
        let old = parse_match_row(name, Some(r#"{"reason":"gameover"}"#));
        assert_eq!(old.outcome, MatchOutcome::Finished);
        let old = parse_match_row(name, Some(r#"{"reason":"link_lost"}"#));
        assert_eq!(old.outcome, MatchOutcome::Quit);
    }

    fn row(dir: &str, mid: &str, role: &str, players: &[&str], outcome: MatchOutcome) -> MatchRow {
        MatchRow {
            dir: dir.into(),
            match_id: mid.into(),
            when: None,
            mode: "network".into(),
            map: "m".into(),
            players: players.iter().map(|s| s.to_string()).collect(),
            ai_count: 0,
            outcome,
            role: role.into(),
        }
    }

    /// RL15's note: a client's copy of a match may list `[client, client]`. When both copies are on
    /// disk the host's wins, whichever is newer, and it keeps the place in the list.
    #[test]
    fn the_hosts_copy_of_a_match_is_preferred_over_the_clients() {
        let mut rows = vec![
            row(
                "c",
                "M1",
                "client",
                &["client", "client"],
                MatchOutcome::Finished,
            ),
            row("other", "M2", "host", &["A"], MatchOutcome::Finished),
            row(
                "h",
                "M1",
                "host",
                &["Alex", "Sasha"],
                MatchOutcome::Finished,
            ),
        ];
        finish_match_rows(&mut rows, false);
        let dirs: Vec<&str> = rows.iter().map(|r| r.dir.as_str()).collect();
        assert_eq!(dirs, ["h", "other"]);
        assert_eq!(rows[0].players, ["Alex", "Sasha"]);
        // Host copy first: still one row. A match with no id is never merged with another.
        let mut rows = vec![
            row("h", "M1", "host", &["Alex"], MatchOutcome::Finished),
            row("c", "M1", "client", &["client"], MatchOutcome::Finished),
            row("a", "", "host", &[], MatchOutcome::Unknown),
            row("b", "", "host", &[], MatchOutcome::Unknown),
        ];
        finish_match_rows(&mut rows, false);
        assert_eq!(rows.len(), 3);
        assert_eq!(rows[0].dir, "h");
    }

    /// `running` with the game gone is a crash; with a game alive only the newest session can still
    /// be in progress.
    #[test]
    fn a_running_session_whose_process_is_gone_is_a_crash() {
        let mk = || {
            vec![
                row("new", "A", "host", &[], MatchOutcome::Running),
                row("old", "B", "host", &[], MatchOutcome::Running),
                row("done", "C", "host", &[], MatchOutcome::Desync),
            ]
        };
        let mut rows = mk();
        finish_match_rows(&mut rows, false);
        assert_eq!(rows[0].outcome, MatchOutcome::Crash);
        assert_eq!(rows[1].outcome, MatchOutcome::Crash);
        assert_eq!(
            rows[2].outcome,
            MatchOutcome::Desync,
            "desync is not rewritten"
        );
        let mut rows = mk();
        finish_match_rows(&mut rows, true);
        assert_eq!(
            rows[0].outcome,
            MatchOutcome::Running,
            "the game is up: in progress"
        );
        assert_eq!(
            rows[1].outcome,
            MatchOutcome::Crash,
            "but an older one is over"
        );
    }

    #[test]
    fn match_rows_reads_the_directories_newest_first_and_ignores_process_folders() {
        let root = scratch("rows");
        for (name, json) in [
            ("2026-10-08T10-00-00Z_menu_solo", None),
            (
                "2026-10-08T21-14-02Z_aabbccdd_krater_host",
                Some(FULL.replace("finished", "running")),
            ),
            (
                "2026-10-07T09-00-00Z_11223344_wyspy_host",
                Some(r#"{"match_id":"x","outcome":"quit","mode":"host"}"#.to_string()),
            ),
        ] {
            let d = root.join(name);
            std::fs::create_dir_all(&d).unwrap();
            if let Some(j) = json {
                std::fs::write(d.join("session.json"), j).unwrap();
            }
        }
        let rows = match_rows(std::slice::from_ref(&root), false);
        assert_eq!(rows.len(), 2, "{rows:#?}");
        assert!(rows[0].dir.contains("krater") && rows[1].dir.contains("wyspy"));
        assert_eq!(rows[0].outcome, MatchOutcome::Crash, "running, game gone");
        assert_eq!(rows[1].outcome, MatchOutcome::Quit);
        assert_eq!(
            match_rows(std::slice::from_ref(&root), true)[0].outcome,
            MatchOutcome::Running
        );
        let _ = std::fs::remove_dir_all(root);
    }

    // ---- .reported -------------------------------------------------------------------------

    /// The RL5 contract (`mh_log_prune.h`): `<marker file name>.reported`, empty, beside the marker.
    #[test]
    fn mark_reported_makes_the_empty_sibling_the_prune_looks_for() {
        let root = scratch("reported");
        let marker = root.join("mh_crash_4242.marker");
        std::fs::write(&marker, "mh_crash=1\n").unwrap();
        assert!(!is_reported(&marker));
        assert_eq!(unreported_markers(&root), std::slice::from_ref(&marker));
        let flag = mark_reported(&marker).unwrap();
        assert_eq!(flag, root.join("mh_crash_4242.marker.reported"));
        assert_eq!(std::fs::metadata(&flag).unwrap().len(), 0, "empty");
        assert!(is_reported(&marker));
        assert!(unreported_markers(&root).is_empty());
        // Again: still there, still empty, no error.
        mark_reported(&marker).unwrap();
        assert_eq!(std::fs::metadata(&flag).unwrap().len(), 0);
        assert!(
            marker.is_file(),
            "the marker itself is left for the prune to read"
        );
        let _ = std::fs::remove_dir_all(root);
    }

    // ---- LA16 ------------------------------------------------------------------------------

    fn marker_text(match_id: &str, when: &str, build: &str) -> String {
        format!(
            "mh_crash=1\ncode=c0000005\npid=1\ntid=2\nmodule=mh.dll\noffset=0x10\n\
             match_id={match_id}\nwhen={when}\nbuild={build}\n"
        )
    }

    fn facts() -> SessionFacts {
        SessionFacts {
            match_id: "01a106fb".into(),
            start: "20261004T100500Z".into(),
            process_start: Some("20261004T100000Z".into()),
            process_end: Some("20261004T180000Z".into()),
        }
    }

    #[test]
    fn a_marker_belongs_to_its_match_or_to_its_process_and_to_nothing_else() {
        let m = |id: &str, when: &str| Marker::parse(&marker_text(id, when, "b")).unwrap();
        // The report's own match.
        assert!(marker_belongs(&m("01A106FB", "20261004T101500Z"), &facts()));
        // LA16's player report: another match, eight days earlier.
        assert!(!marker_belongs(
            &m("01a0ded5", "20260926T175010Z"),
            &facts()
        ));
        // ... and another match in the SAME process window is still another match.
        assert!(!marker_belongs(
            &m("01a0ded5", "20261004T101500Z"),
            &facts()
        ));
        // A menu crash (no match) of this process belongs; an older or a later process's does not.
        assert!(marker_belongs(&m("", "20261004T170000Z"), &facts()));
        assert!(!marker_belongs(&m("", "20261003T170000Z"), &facts()));
        assert!(!marker_belongs(&m("", "20261004T190000Z"), &facts()));
        // No readable stamp and no match: it cannot be shown to be another run's, so it stays.
        assert!(marker_belongs(&m("", ""), &facts()));
    }

    /// LA16's done_when, end to end: an old marker in the logs root plus a new session -> the report
    /// omits the old marker (and names it in report.json); the new session's own marker rides along,
    /// the report built while the game runs says so, and `Built::markers` lists what to flag.
    #[test]
    fn an_old_marker_is_left_out_of_a_new_sessions_report_and_named() {
        let root = scratch("la16");
        let logs = root.join("logs");
        let pd = "2026-10-04T10-00-00Z_menu_solo";
        let sess = "2026-10-04T10-05-00Z_a106fb5a_krater_host";
        for d in [pd, sess] {
            std::fs::create_dir_all(logs.join(d)).unwrap();
        }
        std::fs::write(
            logs.join(sess).join("session.json"),
            format!(r#"{{"match_id":"01a106fb","process_dir":"{pd}","outcome":"running"}}"#),
        )
        .unwrap();
        std::fs::write(logs.join(sess).join("mh_net.log"), "; [build] mh 0.2.0\n").unwrap();
        std::fs::write(
            logs.join("mh_crash_000045f86ed5d831.marker"),
            marker_text("01a0ded5", "20260926T175010Z", "0.2.0-rc4+a6e57fb0"),
        )
        .unwrap();
        std::fs::write(
            logs.join("mh_crash_000045f86ed5d831.marker.ctx32"),
            [1u8, 2, 3],
        )
        .unwrap();
        std::fs::write(
            logs.join("mh_crash_0000aaaa00000001.marker"),
            marker_text("01a106fb", "20261004T101500Z", "0.2.0"),
        )
        .unwrap();
        std::fs::write(logs.join("mh_crash_0000aaaa00000001.marker.reported"), "").unwrap();

        let session = logs.join(sess);
        let preview = markers_for_session(&logs, &session);
        assert_eq!(
            preview
                .iter()
                .map(|p| p.file_name().unwrap().to_string_lossy().to_string())
                .collect::<Vec<_>>(),
            ["mh_crash_0000aaaa00000001.marker"]
        );

        set_game_running(true);
        let zip = root.join("out").join("r.zip");
        let input = Input {
            game_dir: None,
            logs_root: Some(logs.clone()),
            session_dir: Some(session.clone()),
            launcher_started_utc: None,
            description: "it froze",
            last_run: None,
            crash: None,
            minidump: None,
            ini_path: None,
            launcher_log: None,
        };
        let built = build(&zip, &input).unwrap();
        set_game_running(false);
        let names: Vec<&str> = built.entries.iter().map(String::as_str).collect();
        assert!(
            names.contains(&"crash/mh_crash_0000aaaa00000001.marker"),
            "{names:?}"
        );
        assert!(
            !names.iter().any(|n| n.contains("000045f86ed5d831")),
            "the old marker (or its sidecar) rode along: {names:?}"
        );
        assert!(
            !names.iter().any(|n| n.ends_with(".reported")),
            "a flag is not evidence: {names:?}"
        );
        assert_eq!(built.markers.len(), 1);
        let meta: serde_json::Value = serde_json::from_str(&built.meta).unwrap();
        assert_eq!(meta["game_running"], true);
        let older = meta["older_crashes"].as_array().unwrap();
        assert_eq!(older.len(), 1);
        assert_eq!(older[0]["file"], "mh_crash_000045f86ed5d831.marker");
        assert_eq!(older[0]["build"], "0.2.0-rc4+a6e57fb0");
        assert_eq!(older[0]["when"], "20260926T175010Z");
        let _ = std::fs::remove_dir_all(root);
    }

    /// This run's own marker (the live channel's) obeys the same rule.
    #[test]
    fn this_runs_marker_is_dropped_when_it_is_not_about_the_selected_session() {
        let root = scratch("la16_live");
        let logs = root.join("logs");
        let sess = "2026-10-04T10-05-00Z_a106fb5a_krater_host";
        std::fs::create_dir_all(logs.join(sess)).unwrap();
        std::fs::write(
            logs.join(sess).join("session.json"),
            r#"{"match_id":"01a106fb"}"#,
        )
        .unwrap();
        let other = Marker::parse(&marker_text("01a0ded5", "20260926T175010Z", "rc4")).unwrap();
        let zip = root.join("out").join("r.zip");
        let input = Input {
            game_dir: None,
            logs_root: Some(logs.clone()),
            session_dir: Some(logs.join(sess)),
            launcher_started_utc: None,
            description: "it froze",
            last_run: None,
            crash: Some(&other),
            minidump: None,
            ini_path: None,
            launcher_log: None,
        };
        let built = build(&zip, &input).unwrap();
        assert!(!built.entries.iter().any(|n| n == "crash/marker.txt"));
        let meta: serde_json::Value = serde_json::from_str(&built.meta).unwrap();
        assert!(meta.get("crash").is_none(), "{meta}");
        let _ = std::fs::remove_dir_all(root);
    }
}
