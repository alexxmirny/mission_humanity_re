//! The UI-thread freeze watchdog (dist RL21 follow-up).
//!
//! A player's launcher froze right after the game exited -- Windows filed it as `AppHangXProcB1`
//! ("the UI thread waited on another process") with no stack, and `launcher.log` said nothing,
//! because a thread that is stuck cannot write the line that would say where. So the UI thread
//! leaves cheap breadcrumbs instead and a second thread reads them:
//!
//! * `frame_begin` / `frame_end` stamp a heartbeat around every egui frame, and `phase("...")`
//!   names the step the UI thread is in around the calls that can wait on something outside the
//!   process (the game-exit handling, the match-list scan, the RL4 migration, the update prune, the
//!   report page's file reads, the clipboard, an `explorer` spawn, the Win32 process snapshot).
//! * The watchdog thread polls twice a second. When the heartbeat is older than `FREEZE_MS` it
//!   writes ONE `ui: FROZEN for N s in phase <p>` line, and `ui: recovered after N s` once the beat
//!   moves again -- at most one pair per freeze, however long it lasts.
//! * The first freeze of a run also writes `launcher_freeze.dmp` (MiniDumpNormal: thread stacks +
//!   module list, with the UI thread as the "faulting" one) beside `launcher.log`, so the next
//!   report carries the stack Windows' own hang report did not.
//!
//! What is NOT a freeze: a modal phase (`modal:` prefix -- a file dialog pumps its own messages and
//! waits on the player, not on a process), and the gap between frames while the window is minimised
//! (eframe does not paint a minimised window). Between frames otherwise, the app asks for a repaint
//! at least once a second (`IDLE_REPAINT`), so an idle window still beats -- a stall OUTSIDE our
//! frame code (present, the event loop) shows up as phase `between frames`.
//!
//! Everything the UI thread does here is an atomic store or a short uncontended mutex: the
//! watchdog must never be what blocks the thread it watches.

use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};
use std::sync::{Mutex, OnceLock};
use std::time::{Duration, Instant};

use crate::log;

/// A heartbeat older than this is a freeze. Windows' own hang detector fires at 5 s.
pub const FREEZE_MS: u64 = 3000;
/// How often the watchdog looks.
const POLL: Duration = Duration::from_millis(500);
/// The idle repaint cadence that keeps an unused window beating.
pub const IDLE_REPAINT: Duration = Duration::from_secs(1);
/// The dump's file name, beside launcher.log. One per machine: a new freeze overwrites it.
pub const DUMP_FILE: &str = "launcher_freeze.dmp";
/// A dump bigger than this is deleted again (a Normal dump of this process is well under 5 MB).
const DUMP_MAX: u64 = 32 << 20;
/// The phase name of the gap between two frames.
pub const BETWEEN_FRAMES: &str = "between frames";

struct Beat {
    /// ms since `epoch()` of the last stamp.
    last_ms: AtomicU64,
    /// Number of stamps so far; a change means the UI thread moved.
    beats: AtomicU64,
    phase: Mutex<&'static str>,
    /// The window is minimised: no frames are expected between frames.
    idle_ok: AtomicBool,
    ui_tid: AtomicU32,
}

impl Beat {
    fn new() -> Beat {
        Beat {
            last_ms: AtomicU64::new(0),
            beats: AtomicU64::new(0),
            phase: Mutex::new(BETWEEN_FRAMES),
            idle_ok: AtomicBool::new(false),
            ui_tid: AtomicU32::new(0),
        }
    }

    fn stamp(&self) {
        self.last_ms.store(now_ms(), Ordering::Relaxed);
        self.beats.fetch_add(1, Ordering::Release);
    }

    fn set_phase(&self, p: &'static str) -> &'static str {
        match self.phase.lock() {
            Ok(mut g) => std::mem::replace(&mut *g, p),
            Err(_) => BETWEEN_FRAMES,
        }
    }

    fn enter(&'static self, name: &'static str) -> PhaseGuard {
        let prev = self.set_phase(name);
        self.stamp();
        PhaseGuard {
            beat: Some(self),
            prev,
        }
    }

    fn snapshot(&self) -> Snapshot {
        let beats = self.beats.load(Ordering::Acquire);
        Snapshot {
            now_ms: now_ms(),
            last_ms: self.last_ms.load(Ordering::Relaxed),
            beats,
            phase: self.phase.lock().map(|g| *g).unwrap_or(BETWEEN_FRAMES),
            idle_ok: self.idle_ok.load(Ordering::Relaxed),
        }
    }
}

fn beat() -> &'static Beat {
    static B: OnceLock<Beat> = OnceLock::new();
    B.get_or_init(Beat::new)
}

fn epoch() -> Instant {
    static E: OnceLock<Instant> = OnceLock::new();
    *E.get_or_init(Instant::now)
}

fn now_ms() -> u64 {
    epoch().elapsed().as_millis() as u64
}

/// Start of an egui frame (UI thread).
pub fn frame_begin() {
    let b = beat();
    if b.ui_tid.load(Ordering::Relaxed) == 0 {
        b.ui_tid.store(current_tid(), Ordering::Relaxed);
    }
    b.idle_ok.store(false, Ordering::Relaxed);
    b.set_phase("frame");
    b.stamp();
}

/// End of an egui frame (UI thread). `minimized`: no frames are expected until it is restored.
pub fn frame_end(minimized: bool) {
    let b = beat();
    b.set_phase(BETWEEN_FRAMES);
    b.idle_ok.store(minimized, Ordering::Relaxed);
    b.stamp();
}

/// Names what the UI thread is doing until the guard drops (then the previous phase is back).
/// Cheap enough to wrap anything; use a `modal:` prefix for a step that legitimately waits on the
/// player (a file dialog).
///
/// A no-op off the UI thread (some helpers, e.g. the process snapshot, also run on workers) and
/// before the first real frame -- so the kittest harness, which never calls `frame_begin`, leaves
/// the global heartbeat alone.
#[must_use]
pub fn phase(name: &'static str) -> PhaseGuard {
    let b = beat();
    let ui = b.ui_tid.load(Ordering::Relaxed);
    if ui == 0 || ui != current_tid() {
        return PhaseGuard {
            beat: None,
            prev: BETWEEN_FRAMES,
        };
    }
    b.enter(name)
}

pub struct PhaseGuard {
    beat: Option<&'static Beat>,
    prev: &'static str,
}

impl Drop for PhaseGuard {
    fn drop(&mut self) {
        if let Some(b) = self.beat {
            b.set_phase(self.prev);
            b.stamp();
        }
    }
}

// ---- the state machine (pure; unit-tested) -------------------------------------------------

/// One look at the heartbeat.
#[derive(Clone, Copy, Debug)]
pub struct Snapshot {
    pub now_ms: u64,
    pub last_ms: u64,
    pub beats: u64,
    pub phase: &'static str,
    pub idle_ok: bool,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Event {
    Frozen { secs: u64, phase: &'static str },
    Recovered { secs: u64 },
}

impl Event {
    pub fn line(&self) -> String {
        match self {
            Event::Frozen { secs, phase } => format!("ui: FROZEN for {secs} s in phase {phase}"),
            Event::Recovered { secs } => format!("ui: recovered after {secs} s"),
        }
    }
}

#[derive(Debug, Default)]
pub struct Detector {
    /// While a freeze is reported: (beat count at the freeze, the stale beat's time).
    frozen: Option<(u64, u64)>,
}

impl Detector {
    pub fn step(&mut self, s: Snapshot) -> Option<Event> {
        if let Some((beats, since)) = self.frozen {
            if s.beats != beats {
                self.frozen = None;
                let secs = s.last_ms.saturating_sub(since).div_ceil(1000);
                return Some(Event::Recovered { secs });
            }
            return None;
        }
        if s.beats == 0 {
            return None; // the UI has not drawn its first frame yet
        }
        let age = s.now_ms.saturating_sub(s.last_ms);
        let exempt = s.phase.starts_with("modal:") || (s.phase == BETWEEN_FRAMES && s.idle_ok);
        if age > FREEZE_MS && !exempt {
            self.frozen = Some((s.beats, s.last_ms));
            return Some(Event::Frozen {
                secs: age / 1000,
                phase: s.phase,
            });
        }
        None
    }
}

// ---- the thread ----------------------------------------------------------------------------

/// Start the watchdog thread. `log_dir` is where the freeze dump goes. Once per process.
pub fn start(log_dir: PathBuf) {
    static STARTED: AtomicBool = AtomicBool::new(false);
    if STARTED.swap(true, Ordering::SeqCst) {
        return;
    }
    let _ = epoch();
    let spawned = std::thread::Builder::new()
        .name("ui-watchdog".into())
        .spawn(move || {
            let mut det = Detector::default();
            let mut dumped = false;
            loop {
                std::thread::sleep(POLL);
                if let Some(ev) = det.step(beat().snapshot()) {
                    log::line(ev.line());
                    if matches!(ev, Event::Frozen { .. }) && !dumped {
                        dumped = true;
                        let tid = beat().ui_tid.load(Ordering::Relaxed);
                        let dest = log_dir.join(DUMP_FILE);
                        // Its own thread: dbghelp serialises on a process-wide lock, and if the
                        // UI thread is frozen INSIDE a dump of the game, this call waits with it.
                        // The watchdog must keep watching to log the recovery.
                        let _ = std::thread::Builder::new()
                            .name("ui-freeze-dump".into())
                            .spawn(move || write_dump(tid, &dest));
                    }
                }
            }
        });
    if let Err(e) = spawned {
        log::line(format!("ui: watchdog not started: {e}"));
    }
}

fn current_tid() -> u32 {
    // SAFETY: a plain syscall with no arguments.
    unsafe { windows_sys::Win32::System::Threading::GetCurrentThreadId() }
}

fn write_dump(ui_tid: u32, dest: &Path) {
    use minidump_writer::minidump_writer::MinidumpWriter;
    use minidump_writer::MinidumpType;
    if ui_tid == 0 {
        return;
    }
    let mut file = match std::fs::File::create(dest) {
        Ok(f) => f,
        Err(e) => {
            log::line(format!(
                "ui: freeze dump: cannot create {}: {e}",
                dest.display()
            ));
            return;
        }
    };
    // MiniDumpNormal (0): every thread's stack and the module list -- what a hang needs, and
    // small. The UI thread's context is the dump's exception context, so a debugger opens on it.
    let r = MinidumpWriter::dump_local_context(
        None,
        Some(ui_tid),
        Some(MinidumpType::from_bits_truncate(0)),
        &mut file,
    );
    drop(file);
    let size = std::fs::metadata(dest).map(|m| m.len()).unwrap_or(0);
    match r {
        Ok(()) if size <= DUMP_MAX => log::line(format!(
            "ui: freeze dump -> {} ({} KB)",
            dest.display(),
            size / 1024
        )),
        Ok(()) => {
            let _ = std::fs::remove_file(dest);
            log::line(format!(
                "ui: freeze dump dropped: {size} bytes is over the cap"
            ));
        }
        Err(e) => {
            let _ = std::fs::remove_file(dest);
            log::line(format!("ui: freeze dump failed: {e}"));
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn snap(now: u64, last: u64, beats: u64, phase: &'static str, idle_ok: bool) -> Snapshot {
        Snapshot {
            now_ms: now,
            last_ms: last,
            beats,
            phase,
            idle_ok,
        }
    }

    #[test]
    fn a_fresh_beat_is_quiet() {
        let mut d = Detector::default();
        assert_eq!(d.step(snap(1000, 900, 5, "frame", false)), None);
        assert_eq!(d.step(snap(3900, 900, 5, "frame", false)), None); // 3.0 s: not yet
    }

    #[test]
    fn one_line_per_freeze_then_one_recovery() {
        let mut d = Detector::default();
        let ev = d.step(snap(5000, 1000, 7, "refresh_matches", false));
        assert_eq!(
            ev,
            Some(Event::Frozen {
                secs: 4,
                phase: "refresh_matches"
            })
        );
        assert_eq!(
            ev.unwrap().line(),
            "ui: FROZEN for 4 s in phase refresh_matches"
        );
        // Still stuck: no more lines, however long.
        assert_eq!(d.step(snap(9000, 1000, 7, "refresh_matches", false)), None);
        assert_eq!(d.step(snap(60000, 1000, 7, "refresh_matches", false)), None);
        // The beat moved: one recovery line with the total stall.
        let ev = d.step(snap(61000, 60500, 8, "frame", false));
        assert_eq!(ev, Some(Event::Recovered { secs: 60 }));
        assert_eq!(ev.unwrap().line(), "ui: recovered after 60 s");
        assert_eq!(d.step(snap(61500, 61400, 9, "frame", false)), None);
        // A second freeze is reported again.
        assert!(matches!(
            d.step(snap(70000, 61400, 9, "migrate_legacy", false)),
            Some(Event::Frozen {
                phase: "migrate_legacy",
                ..
            })
        ));
    }

    #[test]
    fn modal_and_minimised_idle_are_not_freezes() {
        let mut d = Detector::default();
        assert_eq!(
            d.step(snap(60000, 0, 3, "modal: folder dialog", false)),
            None
        );
        assert_eq!(d.step(snap(60000, 0, 3, BETWEEN_FRAMES, true)), None);
        // ...but a stall between frames of a visible window is one.
        assert!(matches!(
            d.step(snap(60000, 0, 3, BETWEEN_FRAMES, false)),
            Some(Event::Frozen {
                phase: BETWEEN_FRAMES,
                ..
            })
        ));
    }

    #[test]
    fn nothing_before_the_first_frame() {
        let mut d = Detector::default();
        assert_eq!(d.step(snap(60000, 0, 0, BETWEEN_FRAMES, false)), None);
    }

    #[test]
    fn phase_guards_nest_and_restore() {
        // A private heartbeat: the global one is stamped by kittest frames running in parallel.
        let b: &'static Beat = Box::leak(Box::new(Beat::new()));
        {
            let _a = b.enter("outer");
            assert_eq!(b.snapshot().phase, "outer");
            {
                let _b = b.enter("inner");
                assert_eq!(b.snapshot().phase, "inner");
            }
            assert_eq!(b.snapshot().phase, "outer");
        }
        assert_eq!(b.snapshot().phase, BETWEEN_FRAMES);
        assert_eq!(b.snapshot().beats, 4);
    }
}
