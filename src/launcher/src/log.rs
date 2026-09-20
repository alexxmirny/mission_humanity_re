//! The launcher's own log: `%LOCALAPPDATA%\MissionHumanity\logs\launcher.log`.
//!
//! One append-only UTF-8 file, one line per action, flushed on every line. Flushing per line is the
//! whole design constraint rather than a nicety: the events worth reading back are the ones next to
//! a crash -- the game's, and one day the launcher's own -- and a buffered logger loses exactly the
//! last few lines, which are the ones that said what was happening.
//!
//! The log is best-effort by construction. A launcher that refused to start because it could not
//! open its log would be trading a working game for a diagnostic, so every failure here degrades to
//! "no file, still running": `init` records the reason in memory and `line` keeps appending to the
//! in-memory ring the UI shows.

use std::fs::{self, File, OpenOptions};
use std::io::Write;
use std::path::{Path, PathBuf};
use std::sync::{Mutex, OnceLock};

/// How many lines the UI can show without reading the file back.
const RING: usize = 200;

struct Sink {
    file: Option<File>,
    path: Option<PathBuf>,
    /// Why there is no file, when there is none. Surfaced in the status view.
    problem: Option<String>,
    recent: Vec<String>,
}

fn sink() -> &'static Mutex<Sink> {
    static SINK: OnceLock<Mutex<Sink>> = OnceLock::new();
    SINK.get_or_init(|| {
        Mutex::new(Sink {
            file: None,
            path: None,
            problem: None,
            recent: Vec::new(),
        })
    })
}

/// UTC, second resolution, sortable. Local time would be friendlier to read and useless to compare
/// against the game's own logs from a machine in another zone, which is what a bug report is.
pub fn stamp() -> String {
    chrono::Utc::now().format("%Y-%m-%dT%H:%M:%SZ").to_string()
}

/// Open (or create) the log at `path`. Safe to call once; later calls replace the sink.
pub fn init(path: &Path) {
    let mut problem = None;
    let mut file = None;
    match path.parent() {
        Some(dir) => {
            if let Err(e) = fs::create_dir_all(dir) {
                problem = Some(format!("cannot create {}: {e}", dir.display()));
            }
        }
        None => problem = Some(format!("{} has no parent directory", path.display())),
    }
    if problem.is_none() {
        match OpenOptions::new().create(true).append(true).open(path) {
            Ok(f) => file = Some(f),
            Err(e) => problem = Some(format!("cannot open {}: {e}", path.display())),
        }
    }
    if let Ok(mut s) = sink().lock() {
        s.file = file;
        s.path = Some(path.to_path_buf());
        s.problem = problem;
    }
}

/// Append one line. Never fails, never panics, never blocks on anything but the sink mutex.
pub fn line(msg: impl AsRef<str>) {
    let text = format!("{} {}", stamp(), msg.as_ref());
    if let Ok(mut s) = sink().lock() {
        if let Some(f) = s.file.as_mut() {
            // A write that fails is recorded once, in the ring, and then ignored: the alternative
            // is a launcher that spends its time reporting that it cannot report.
            let _ = writeln!(f, "{text}");
            let _ = f.flush();
        }
        if s.recent.len() == RING {
            s.recent.remove(0);
        }
        s.recent.push(text);
    }
}

/// The last `n` lines, newest last. For the status view's activity box.
pub fn recent(n: usize) -> Vec<String> {
    sink()
        .lock()
        .map(|s| s.recent.iter().rev().take(n).rev().cloned().collect())
        .unwrap_or_default()
}

/// The log file's path, once `init` has been called.
pub fn path() -> Option<PathBuf> {
    sink().lock().ok().and_then(|s| s.path.clone())
}

/// The reason there is no log file, when there is none.
pub fn problem() -> Option<String> {
    sink().lock().ok().and_then(|s| s.problem.clone())
}
