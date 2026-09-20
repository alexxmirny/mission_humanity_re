//! Starting the game and saying what happened to it.
//!
//! The launcher STAYS RESIDENT (plan decision D10): it spawns `mh.exe`, keeps the handle, and waits.
//! That is not politeness -- it is the only place the exit code exists. Windows hands the code to
//! whoever holds a handle to the process, so a launcher that spawns and quits has thrown away the
//! single fact dist LA4's crash report is built on.
//!
//! READING THE CODE AS `u32` IS THE WHOLE TRICK. `ExitStatus::code()` is typed `i32` because POSIX
//! says so, but on Windows the value is an unsigned `DWORD` that has been reinterpreted: an access
//! violation arrives as `-1073741819`, which is `0xC0000005` with the sign bit read as a sign. A
//! launcher that printed the signed number would show players a negative integer nobody can look
//! up, and a launcher that tested `code > 0` would classify every crash as a clean exit.

use std::path::{Path, PathBuf};
use std::process::{Child, Command};
use std::time::Instant;

use crate::log;
use crate::paths::GAME_EXE;

/// What the child's exit code meant.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Outcome {
    /// Zero. The program chose to stop.
    Normal,
    /// Non-zero but not `STATUS_*`-shaped: the program chose to stop and complained.
    Nonzero(u32),
    /// An `NTSTATUS` with severity `ERROR` (the top two bits set). The program did not choose this.
    Crash(u32),
}

impl Outcome {
    pub fn is_crash(&self) -> bool {
        matches!(self, Outcome::Crash(_))
    }

    /// The line the Launch view shows and the log records. One sentence, with the hex code in it
    /// whenever there is one, because the hex code is what a bug report can be searched for.
    pub fn describe(&self) -> String {
        match self {
            Outcome::Normal => "the game exited normally (code 0)".to_string(),
            Outcome::Nonzero(c) => format!(
                "the game exited with code {c} (0x{c:08X}) -- not a crash, but not a clean exit"
            ),
            Outcome::Crash(c) => match status_name(*c) {
                Some(n) => format!("the game CRASHED: 0x{c:08X} ({n})"),
                None => format!("the game CRASHED: 0x{c:08X}"),
            },
        }
    }
}

/// Classify a Windows process exit code.
///
/// The rule is the NTSTATUS severity field: bits 31-30 are `0b11` (`STATUS_SEVERITY_ERROR`) for
/// every code the exception dispatcher kills a process with -- `0xC0000005`, `0xC000001D`,
/// `0xC0000374`, `0xC0000409`, `0xC0000135`. Testing the SHAPE rather than a list of constants is
/// what makes an unfamiliar fault still read as a fault.
///
/// Two boundaries this deliberately does NOT cross:
///   * `0x80000000` (severity WARNING, e.g. `STATUS_BREAKPOINT` 0x80000003) is not a crash here. A
///     process that exits on a breakpoint was being debugged; reporting that as a game crash would
///     be a false positive aimed at exactly the people able to file the best bug reports.
///   * a small non-zero code is `Nonzero`, not `Crash`. `mh.exe` returning 1 is the program's own
///     refusal, and flattening the two would make "it wouldn't start" indistinguishable from "it
///     faulted" in every report LA4 ever collects.
pub fn classify(code: u32) -> Outcome {
    if code == 0 {
        Outcome::Normal
    } else if code & 0xC000_0000 == 0xC000_0000 {
        Outcome::Crash(code)
    } else {
        Outcome::Nonzero(code)
    }
}

/// The handful of `STATUS_*` codes worth naming in a UI. Not a lookup table of all of them -- just
/// the ones a player of this game is realistically going to see, so the message says something more
/// useful than the hex alone.
fn status_name(code: u32) -> Option<&'static str> {
    Some(match code {
        0xC000_0005 => "STATUS_ACCESS_VIOLATION",
        0xC000_001D => "STATUS_ILLEGAL_INSTRUCTION",
        0xC000_0025 => "STATUS_NONCONTINUABLE_EXCEPTION",
        0xC000_0026 => "STATUS_INVALID_DISPOSITION",
        0xC000_008C => "STATUS_ARRAY_BOUNDS_EXCEEDED",
        0xC000_008E => "STATUS_FLOAT_DIVIDE_BY_ZERO",
        0xC000_0094 => "STATUS_INTEGER_DIVIDE_BY_ZERO",
        0xC000_00FD => "STATUS_STACK_OVERFLOW",
        0xC000_0135 => "STATUS_DLL_NOT_FOUND",
        0xC000_0139 => "STATUS_ENTRYPOINT_NOT_FOUND",
        0xC000_013A => "STATUS_CONTROL_C_EXIT",
        0xC000_0142 => "STATUS_DLL_INIT_FAILED",
        0xC000_0374 => "STATUS_HEAP_CORRUPTION",
        0xC000_0409 => "STATUS_STACK_BUFFER_OVERRUN",
        0xC000_041D => "STATUS_FATAL_USER_CALLBACK_EXCEPTION",
        _ => return None,
    })
}

/// A game process the launcher is waiting on.
pub struct Session {
    child: Child,
    pub exe: PathBuf,
    pub started: Instant,
}

/// The result of one finished run.
#[derive(Clone, Debug)]
pub struct Finished {
    pub outcome: Outcome,
    pub seconds: f64,
}

/// Start `mh.exe` in `game_dir`.
///
/// CWD IS THE GAME DIRECTORY, and that is load-bearing rather than conventional: the game resolves
/// its data packs, its `logs\<runid>_<role>\` directory and its save folder relative to where it
/// runs, so a launcher that inherited its own working directory would produce a run that writes its
/// logs next to the launcher and cannot find `Res`. (`mh.dll` resolves `mh_net.ini` next to the EXE
/// instead -- see tools/make_lane.py -- so the two are not the same rule and both have to be right.)
/// `extra_env` is how dist LA4's crash channel reaches the game: `MH_CRASH_CHANNEL` and
/// `MH_CRASH_MARKER`, read by `mh.dll` at load time. ENVIRONMENT AND NOT AN INI KEY, deliberately
/// -- "is a launcher listening for my crash" is a fact about how this process was started, and a
/// setting in a file could disagree with it (a game started from Explorer would then block for
/// twenty seconds on an acknowledgement nobody was ever going to send). An empty slice means the
/// game runs exactly as it did before LA4.
pub fn start(game_dir: &Path, extra_env: &[(&'static str, String)]) -> Result<Session, String> {
    let exe = game_dir.join(GAME_EXE);
    if !exe.is_file() {
        return Err(format!("no {GAME_EXE} in {}", game_dir.display()));
    }
    log::line(format!(
        "launch: starting {} with cwd {}",
        exe.display(),
        game_dir.display()
    ));
    let mut cmd = Command::new(&exe);
    cmd.current_dir(game_dir);
    for (k, v) in extra_env {
        log::line(format!("launch: {k}={v}"));
        cmd.env(k, v);
    }
    let child = cmd
        .spawn()
        .map_err(|e| format!("cannot start {}: {e}", exe.display()))?;
    log::line(format!("launch: pid {}", child.id()));
    Ok(Session {
        child,
        exe,
        started: Instant::now(),
    })
}

impl Session {
    pub fn pid(&self) -> u32 {
        self.child.id()
    }

    /// Has it finished? `None` means still running; the session is consumed either way once it has.
    pub fn poll(&mut self) -> Result<Option<Finished>, String> {
        match self.child.try_wait() {
            Ok(None) => Ok(None),
            Ok(Some(status)) => {
                // `code()` is None only when a signal killed the process, which cannot happen on
                // Windows; the fallback keeps the type honest rather than unwrapping.
                let raw = status.code().unwrap_or(0) as u32;
                let outcome = classify(raw);
                let seconds = self.started.elapsed().as_secs_f64();
                log::line(format!(
                    "launch: pid {} exited after {seconds:.1}s -- raw code {} (0x{raw:08X}) -> {}",
                    self.child.id(),
                    status.code().unwrap_or(0),
                    outcome.describe()
                ));
                Ok(Some(Finished { outcome, seconds }))
            }
            Err(e) => Err(format!("cannot wait on {}: {e}", self.exe.display())),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn zero_is_a_normal_quit() {
        assert_eq!(classify(0), Outcome::Normal);
        assert!(!classify(0).is_crash());
    }

    /// The clause dist LA1 is accepted on, in one assertion.
    #[test]
    fn an_access_violation_is_a_crash_with_its_hex_code() {
        let o = classify(0xC000_0005);
        assert!(o.is_crash());
        assert!(o.describe().contains("0xC0000005"), "{}", o.describe());
        assert!(
            o.describe().contains("STATUS_ACCESS_VIOLATION"),
            "{}",
            o.describe()
        );
    }

    /// The signed/unsigned trap this module exists to avoid: the number Rust hands back for an
    /// access violation is negative, and only the `u32` reinterpretation is a code anyone can look
    /// up. If this ever fails, the Launch view is showing players "-1073741819".
    #[test]
    fn the_signed_exit_code_reinterprets_to_the_status_value() {
        assert_eq!(-1_073_741_819_i32 as u32, 0xC000_0005);
        assert_eq!(
            classify(-1_073_741_819_i32 as u32),
            Outcome::Crash(0xC000_0005)
        );
    }

    #[test]
    fn other_status_errors_are_crashes_even_unnamed() {
        for code in [0xC000_0374_u32, 0xC000_0409, 0xC000_0135, 0xC0DE_BEEF] {
            assert!(classify(code).is_crash(), "0x{code:08X}");
            assert!(classify(code).describe().contains(&format!("0x{code:08X}")));
        }
    }

    #[test]
    fn a_warning_status_and_a_plain_failure_code_are_not_crashes() {
        // STATUS_BREAKPOINT: severity WARNING, a debugger's business, not a game crash.
        assert_eq!(classify(0x8000_0003), Outcome::Nonzero(0x8000_0003));
        assert_eq!(classify(1), Outcome::Nonzero(1));
        assert!(!classify(1).is_crash());
        assert!(classify(1).describe().contains("not a crash"));
    }
}
