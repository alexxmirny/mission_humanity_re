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
//!
//! dist LA13 added two things: the launcher-owned logs root the game is handed by environment
//! (`ENV_LOG_ROOT`), and the rule that the game is NEVER started elevated -- the child's token is
//! queried and logged on every launch, and an elevated launcher starts the game from the desktop
//! shell's token instead of its own.

use std::os::windows::io::AsRawHandle;
use std::path::{Path, PathBuf};
use std::process::{Child, Command};
use std::time::Instant;

use windows_sys::Win32::Foundation::{CloseHandle, GetLastError, HANDLE, WAIT_OBJECT_0};
use windows_sys::Win32::Security::{
    DuplicateTokenEx, GetTokenInformation, SecurityImpersonation, TokenElevation, TokenPrimary,
    TOKEN_ASSIGN_PRIMARY, TOKEN_DUPLICATE, TOKEN_ELEVATION, TOKEN_QUERY,
};
use windows_sys::Win32::System::SystemServices::MAXIMUM_ALLOWED;
use windows_sys::Win32::System::Threading::{
    CreateProcessWithTokenW, GetCurrentProcess, GetExitCodeProcess, OpenProcess, OpenProcessToken,
    WaitForSingleObject, CREATE_UNICODE_ENVIRONMENT, PROCESS_INFORMATION,
    PROCESS_QUERY_INFORMATION, STARTUPINFOW,
};
use windows_sys::Win32::UI::WindowsAndMessaging::{GetShellWindow, GetWindowThreadProcessId};

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

/// dist LA13: the environment variable that hands the game its logs root.
///
/// `mh_common/run_context.cpp` reads it once at start: set, the game's session directories,
/// `mh_run.txt` and (by default) the crash marker go under that directory; unset -- a hand launch
/// -- they go under `<exe>\logs\` exactly as before. The launcher sets it to `Layout::game_log_root`
/// (see there for why a game under Program Files cannot be read back through its own folder).
pub const ENV_LOG_ROOT: &str = "MH_LOG_ROOT";

/// A game process the launcher is waiting on.
pub struct Session {
    proc: Proc,
    pub exe: PathBuf,
    pub started: Instant,
    /// dist LA13: what the child's token said at start (`None` when the query failed).
    pub elevated: Option<bool>,
}

/// The process handle, in one of two shapes: `std`'s, from the ordinary spawn, or a raw one from
/// `CreateProcessWithTokenW` (dist LA13's de-elevated launch), which `std` cannot wrap.
enum Proc {
    Std(Child),
    Raw { handle: HANDLE, pid: u32 },
}

impl Drop for Proc {
    fn drop(&mut self) {
        if let Proc::Raw { handle, .. } = self {
            // SAFETY: the handle was returned by CreateProcessWithTokenW and is closed once, here.
            unsafe {
                CloseHandle(*handle);
            }
        }
    }
}

/// The result of one finished run.
#[derive(Clone, Debug)]
pub struct Finished {
    pub outcome: Outcome,
    pub seconds: f64,
}

/// The game's own record of where it decided to put THIS run's logs: `run_context.cpp`'s
/// breadcrumb, `mh_run.txt`, written on every launch (and rewritten on every session rollover
/// within it) -- beside the exe for a hand launch, INSIDE the launcher-owned logs root when the
/// launcher set one (dist LA13), which is why the caller passes the directory it expects it in.
///
/// dist LA10: a game whose install sits at a long enough path can have Windows' `CreateDirectory`
/// refuse the stamped `logs\<name>\` subdirectory the DLL asks for; the DLL degrades rather than
/// losing the run, but the degraded location is not the one a player (or a report) would think to
/// look under `logs\` for. Reading this file back after every run and logging it, unconditionally,
/// is what would have turned that afternoon's two-day log-mining dig into reading one line: either
/// it names a real `logs\...` directory, or it is missing/empty, which is itself the finding.
pub fn resolved_log_root(breadcrumb_dir: &Path) -> Option<String> {
    std::fs::read_to_string(breadcrumb_dir.join("mh_run.txt"))
        .ok()
        .map(|s| s.trim().to_string())
        .filter(|s| !s.is_empty())
}

/// Is the process behind `handle` running with an elevated (full administrator) token?
///
/// `TokenElevation` is the one fact UAC exposes directly: 1 for a full admin token, 0 for a
/// standard user AND for the filtered token an administrator normally runs with. `None` when the
/// token cannot be opened, which for a child we just spawned should not happen.
pub fn process_elevation(handle: HANDLE) -> Option<bool> {
    let mut token: HANDLE = std::ptr::null_mut();
    // SAFETY: plain Win32 calls with out-pointers to locals of the documented types.
    unsafe {
        if OpenProcessToken(handle, TOKEN_QUERY, &mut token) == 0 {
            return None;
        }
        let mut elevation = TOKEN_ELEVATION { TokenIsElevated: 0 };
        let mut returned: u32 = 0;
        let ok = GetTokenInformation(
            token,
            TokenElevation,
            &mut elevation as *mut TOKEN_ELEVATION as *mut core::ffi::c_void,
            std::mem::size_of::<TOKEN_ELEVATION>() as u32,
            &mut returned,
        );
        CloseHandle(token);
        if ok == 0 {
            return None;
        }
        Some(elevation.TokenIsElevated != 0)
    }
}

/// This launcher's own token.
pub fn current_process_elevation() -> Option<bool> {
    // SAFETY: GetCurrentProcess returns a pseudo-handle that needs no closing.
    process_elevation(unsafe { GetCurrentProcess() })
}

pub fn describe_elevation(e: Option<bool>) -> &'static str {
    match e {
        Some(true) => "ELEVATED (full administrator token)",
        Some(false) => "not elevated",
        None => "unknown (token query failed)",
    }
}

/// Start `mh.exe` in `game_dir`.
///
/// CWD IS THE GAME DIRECTORY, and that is load-bearing rather than conventional: the game resolves
/// its data packs, its save folder and (without `MH_LOG_ROOT`) its `logs\<runid>_<role>\` directory
/// relative to where it runs, so a launcher that inherited its own working directory would produce a
/// run that writes its logs next to the launcher and cannot find `Res`. (`mh.dll` resolves
/// `mh_net.ini` next to the EXE instead -- see tools/make_lane.py -- so the two are not the same
/// rule and both have to be right.)
///
/// `log_root` (dist LA13) is the launcher-owned logs root, created here and handed over as
/// `MH_LOG_ROOT`. `extra_env` is how dist LA4's crash channel reaches the game: `MH_CRASH_CHANNEL`
/// and `MH_CRASH_MARKER`, read by `mh.dll` at load time. ENVIRONMENT AND NOT AN INI KEY,
/// deliberately -- "is a launcher listening for my crash" is a fact about how this process was
/// started, and a setting in a file could disagree with it (a game started from Explorer would then
/// block for twenty seconds on an acknowledgement nobody was ever going to send). An empty slice and
/// no root means the game runs exactly as it did before LA4.
///
/// NEVER ELEVATED (dist LA13). Both 09-20 players ended up running the launcher "as administrator"
/// (the install under Program Files had been refused, see `elevate.rs`), and every game it then
/// started inherited the full token -- saves in the real folder for those runs, in VirtualStore for
/// the others, and a game process with more rights than a game needs. So when THIS process is
/// elevated the game is started from the desktop shell's token instead (`start_unelevated`), and
/// the child's token is queried and logged either way, so the log says which it got.
pub fn start(
    game_dir: &Path,
    log_root: Option<&Path>,
    extra_env: &[(&'static str, String)],
) -> Result<Session, String> {
    let exe = game_dir.join(GAME_EXE);
    if !exe.is_file() {
        return Err(format!("no {GAME_EXE} in {}", game_dir.display()));
    }
    let mut env: Vec<(&'static str, String)> = Vec::new();
    if let Some(root) = log_root {
        std::fs::create_dir_all(root)
            .map_err(|e| format!("cannot create the logs root {}: {e}", root.display()))?;
        // A human-readable pointer beside the hashed name, so a directory listing of the launcher's
        // logs\ says which game folder each root belongs to.
        let _ = std::fs::write(
            root.join("game_dir.txt"),
            format!("{}\r\n", game_dir.display()),
        );
        let mut text = root.display().to_string();
        if !text.ends_with('\\') {
            text.push('\\');
        }
        env.push((ENV_LOG_ROOT, text));
    }
    env.extend(extra_env.iter().cloned());
    log::line(format!(
        "launch: starting {} with cwd {}",
        exe.display(),
        game_dir.display()
    ));
    for (k, v) in &env {
        log::line(format!("launch: {k}={v}"));
    }

    let own = current_process_elevation();
    let proc = if own == Some(true) {
        log::line(
            "launch: this launcher is running ELEVATED -- starting the game from the desktop \
             shell's token instead, so the game is not (dist LA13)",
        );
        match start_unelevated(&exe, game_dir, &env) {
            Ok(p) => p,
            Err(e) => {
                log::line(format!(
                    "launch: cannot start the game un-elevated ({e}); starting it the ordinary \
                     way -- IT WILL INHERIT THIS LAUNCHER'S ELEVATION. Start the launcher without \
                     'Run as administrator'."
                ));
                start_std(&exe, game_dir, &env)?
            }
        }
    } else {
        start_std(&exe, game_dir, &env)?
    };
    let (pid, handle) = match &proc {
        Proc::Std(c) => (c.id(), c.as_raw_handle() as HANDLE),
        Proc::Raw { handle, pid } => (*pid, *handle),
    };
    let elevated = process_elevation(handle);
    log::line(format!(
        "launch: pid {pid} -- token: {}",
        describe_elevation(elevated)
    ));
    Ok(Session {
        proc,
        exe,
        started: Instant::now(),
        elevated,
    })
}

fn start_std(exe: &Path, game_dir: &Path, env: &[(&'static str, String)]) -> Result<Proc, String> {
    let mut cmd = Command::new(exe);
    cmd.current_dir(game_dir);
    for (k, v) in env {
        cmd.env(k, v);
    }
    let child = cmd
        .spawn()
        .map_err(|e| format!("cannot start {}: {e}", exe.display()))?;
    Ok(Proc::Std(child))
}

fn wide(s: &std::ffi::OsStr) -> Vec<u16> {
    use std::os::windows::ffi::OsStrExt;
    s.encode_wide().chain(std::iter::once(0)).collect()
}

/// The child's environment block: this process's environment plus `extra`, as the
/// `CREATE_UNICODE_ENVIRONMENT` layout (`K=V\0...\0`). Built by hand because
/// `CreateProcessWithTokenW` given a NULL block builds one from the TOKEN's profile -- which is
/// what we want for everything except the three `MH_*` variables that ARE the point.
fn environment_block(extra: &[(&'static str, String)]) -> Vec<u16> {
    let mut vars: Vec<(std::ffi::OsString, std::ffi::OsString)> = std::env::vars_os()
        .filter(|(k, _)| !extra.iter().any(|(ek, _)| k.eq_ignore_ascii_case(ek)))
        .collect();
    for (k, v) in extra {
        vars.push((k.into(), v.into()));
    }
    vars.sort_by(|a, b| a.0.cmp(&b.0));
    let mut block: Vec<u16> = Vec::new();
    for (k, v) in vars {
        let mut entry = k;
        entry.push("=");
        entry.push(v);
        block.extend(wide(&entry));
    }
    block.push(0);
    block
}

/// Start the game with the DESKTOP SHELL's token -- the medium-integrity, un-elevated token the
/// player's Explorer runs with -- when this launcher itself is elevated.
///
/// The recipe is the documented one: the shell window's process owns the token every "Run as
/// administrator" started from, `DuplicateTokenEx` makes a primary copy, and
/// `CreateProcessWithTokenW` starts the game under it. It needs `SeImpersonatePrivilege`, which a
/// full administrator token has. Any failure is returned, not papered over: the caller logs it
/// loudly and falls back to the ordinary spawn, because a game that starts elevated is a lesser
/// evil than a Play button that does nothing.
fn start_unelevated(
    exe: &Path,
    game_dir: &Path,
    env: &[(&'static str, String)],
) -> Result<Proc, String> {
    // SAFETY: Win32 handle plumbing; every handle opened here is closed on every path below.
    unsafe {
        let shell = GetShellWindow();
        if shell.is_null() {
            return Err("no desktop shell window (is Explorer running?)".into());
        }
        let mut shell_pid: u32 = 0;
        GetWindowThreadProcessId(shell, &mut shell_pid);
        if shell_pid == 0 {
            return Err("the shell window has no process".into());
        }
        let shell_proc = OpenProcess(PROCESS_QUERY_INFORMATION, 0, shell_pid);
        if shell_proc.is_null() {
            return Err(format!(
                "cannot open the shell process {shell_pid} (err {})",
                GetLastError()
            ));
        }
        let mut shell_token: HANDLE = std::ptr::null_mut();
        let ok = OpenProcessToken(
            shell_proc,
            TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY,
            &mut shell_token,
        );
        CloseHandle(shell_proc);
        if ok == 0 {
            return Err(format!(
                "cannot open the shell's token (err {})",
                GetLastError()
            ));
        }
        let mut primary: HANDLE = std::ptr::null_mut();
        let ok = DuplicateTokenEx(
            shell_token,
            MAXIMUM_ALLOWED,
            std::ptr::null(),
            SecurityImpersonation,
            TokenPrimary,
            &mut primary,
        );
        CloseHandle(shell_token);
        if ok == 0 {
            return Err(format!(
                "cannot duplicate the shell's token (err {})",
                GetLastError()
            ));
        }

        let app = wide(exe.as_os_str());
        // The command line is what the game sees as argv[0]; quoted, since the path has spaces.
        let mut cmdline = wide(std::ffi::OsStr::new(&format!("\"{}\"", exe.display())));
        let cwd = wide(game_dir.as_os_str());
        let block = environment_block(env);
        let si = STARTUPINFOW {
            cb: std::mem::size_of::<STARTUPINFOW>() as u32,
            ..Default::default()
        };
        let mut pi = PROCESS_INFORMATION::default();
        let ok = CreateProcessWithTokenW(
            primary,
            0,
            app.as_ptr(),
            cmdline.as_mut_ptr(),
            CREATE_UNICODE_ENVIRONMENT,
            block.as_ptr() as *const core::ffi::c_void,
            cwd.as_ptr(),
            &si,
            &mut pi,
        );
        let err = GetLastError();
        CloseHandle(primary);
        if ok == 0 {
            return Err(format!("CreateProcessWithTokenW failed (err {err})"));
        }
        CloseHandle(pi.hThread);
        Ok(Proc::Raw {
            handle: pi.hProcess,
            pid: pi.dwProcessId,
        })
    }
}

impl Session {
    pub fn pid(&self) -> u32 {
        match &self.proc {
            Proc::Std(c) => c.id(),
            Proc::Raw { pid, .. } => *pid,
        }
    }

    /// Has it finished? `None` means still running; the session is consumed either way once it has.
    pub fn poll(&mut self) -> Result<Option<Finished>, String> {
        let raw: u32 = match &mut self.proc {
            Proc::Std(child) => match child.try_wait() {
                Ok(None) => return Ok(None),
                // `code()` is None only when a signal killed the process, which cannot happen on
                // Windows; the fallback keeps the type honest rather than unwrapping.
                Ok(Some(status)) => status.code().unwrap_or(0) as u32,
                Err(e) => return Err(format!("cannot wait on {}: {e}", self.exe.display())),
            },
            Proc::Raw { handle, .. } => {
                // SAFETY: a live process handle we own; a zero timeout makes this a poll.
                unsafe {
                    if WaitForSingleObject(*handle, 0) != WAIT_OBJECT_0 {
                        return Ok(None);
                    }
                    let mut code: u32 = 0;
                    if GetExitCodeProcess(*handle, &mut code) == 0 {
                        return Err(format!(
                            "cannot read the exit code of {} (err {})",
                            self.exe.display(),
                            GetLastError()
                        ));
                    }
                    code
                }
            }
        };
        let outcome = classify(raw);
        let seconds = self.started.elapsed().as_secs_f64();
        log::line(format!(
            "launch: pid {} exited after {seconds:.1}s -- raw code {} (0x{raw:08X}) -> {}",
            self.pid(),
            raw as i32,
            outcome.describe()
        ));
        Ok(Some(Finished { outcome, seconds }))
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

    // ---- dist LA13: the token, and the environment block ---------------------------------------

    /// The query works on this very process, whatever the answer is; a launcher that could not
    /// tell would log "unknown" for every game it started.
    #[test]
    fn the_launchers_own_token_elevation_is_readable() {
        let e = current_process_elevation();
        assert!(
            e.is_some(),
            "TokenElevation query failed on our own process"
        );
        assert!(!describe_elevation(e).starts_with("unknown"));
    }

    /// The block hands the child THIS process's environment plus the MH_* variables -- the
    /// variables win over an inherited value of the same name, every entry is `K=V\0`, and the
    /// block ends with the second NUL `CREATE_UNICODE_ENVIRONMENT` requires.
    #[test]
    fn the_environment_block_carries_the_mh_variables_over_the_inherited_ones() {
        std::env::set_var("MH_LOG_ROOT", "inherited-and-wrong");
        let block = environment_block(&[
            (ENV_LOG_ROOT, "C:\\state\\logs\\abc\\".to_string()),
            (
                "MH_CRASH_MARKER",
                "C:\\state\\logs\\abc\\m.marker".to_string(),
            ),
        ]);
        std::env::remove_var("MH_LOG_ROOT");
        assert_eq!(&block[block.len() - 2..], &[0, 0], "double NUL terminated");
        let text = String::from_utf16_lossy(&block[..block.len() - 1]);
        let entries: Vec<&str> = text.split('\0').collect();
        assert!(
            entries.contains(&"MH_LOG_ROOT=C:\\state\\logs\\abc\\"),
            "{entries:?}"
        );
        assert!(entries.contains(&"MH_CRASH_MARKER=C:\\state\\logs\\abc\\m.marker"));
        assert_eq!(
            entries
                .iter()
                .filter(|e| e.starts_with("MH_LOG_ROOT="))
                .count(),
            1,
            "the inherited value is replaced, not doubled"
        );
        assert!(
            entries
                .iter()
                .any(|e| e.to_ascii_uppercase().starts_with("PATH=")),
            "the rest of the environment is inherited"
        );
    }

    // ---- dist LA10: the resolved log root, read back after every launch ------------------------

    #[test]
    fn resolved_log_root_reads_the_breadcrumb_and_trims_it() {
        let dir = std::env::temp_dir().join("mh_launcher_test_log_root_present");
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(
            dir.join("mh_run.txt"),
            "C:\\Games\\MH\\logs\\20260101T000000Z_menu_solo\\\r\n",
        )
        .unwrap();
        assert_eq!(
            resolved_log_root(&dir).as_deref(),
            Some("C:\\Games\\MH\\logs\\20260101T000000Z_menu_solo\\")
        );
        std::fs::remove_dir_all(&dir).ok();
    }

    /// The done_when clause this test stands for: a launch whose game never wrote a breadcrumb (or
    /// wrote an empty one) must not be silently confused with one that did -- `None` is itself the
    /// signal LA10 needs, not a missing feature.
    #[test]
    fn resolved_log_root_is_none_when_the_breadcrumb_is_missing_or_blank() {
        let dir = std::env::temp_dir().join("mh_launcher_test_log_root_missing");
        std::fs::remove_dir_all(&dir).ok();
        std::fs::create_dir_all(&dir).unwrap();
        assert_eq!(resolved_log_root(&dir), None, "no mh_run.txt at all");
        std::fs::write(dir.join("mh_run.txt"), "   \r\n").unwrap();
        assert_eq!(resolved_log_root(&dir), None, "whitespace-only breadcrumb");
        std::fs::remove_dir_all(&dir).ok();
    }
}
