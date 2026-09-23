//! dist LA13: the ONE step that may run elevated -- install / switch / provision into a game
//! directory a standard token cannot write -- and never the game.
//!
//! MEASURED, 2026-09-21, in both 09-20 players' launcher logs: the game lives under
//! `C:\Program Files (x86)\Mission Humanity`, and every install/switch/provision there failed with
//! `cannot move ...\LICENSE aside: Access is denied`, three times each, until the player re-ran the
//! launcher "as administrator" -- after which every GAME the launcher started inherited the full
//! administrator token. Two things wrong with that: a game does not need it, and a game that has it
//! is not UAC-virtualized any more, so its saves and `setup.dat` land in the real folder on those
//! runs and in `%LOCALAPPDATA%\VirtualStore\...` on the others (the "disappearing saves").
//!
//! THE RULE. The launcher stays a per-user, un-elevated program (plan decision D10). When a write
//! into the game directory is refused, it re-runs ITSELF, elevated (`ShellExecuteExW` with the
//! `runas` verb -- the standard UAC prompt), with a command line that names EXACTLY the step and
//! nothing else -- `--step install:<version>:<tag>` / `--step uninstall` / `--step provision` --
//! waits for it, reads its verdict back from a result file, and carries on un-elevated: the game is
//! then started by THIS process, with THIS token. The elevated child opens no window, launches
//! nothing and updates nothing; its argv cannot even express `--launch` (`step_argv` below is the
//! whole of what it is given, and `the_elevated_step_cannot_launch_the_game` pins that).
//!
//! WHY A PROBE FIRST. `install_from_version_dir` moves a foreign file aside and copies eight files;
//! a refusal on the fourth leaves the directory half-done. So `needs_elevation` asks Windows the
//! question directly -- can this token create a file here -- BEFORE the step, and the elevated
//! re-run is decided up front. An Access-denied that still slips through (a directory that became
//! read-only mid-step) is caught by `is_access_denied` and re-run the same way.
//!
//! ONE PROMPT PER INSTALL/SWITCH/PROVISION, NEVER TO PLAY: a game directory the token can write
//! (the ordinary case, anywhere but Program Files) never reaches this module at all.

use std::path::{Path, PathBuf};

use crate::log;
use crate::paths::Layout;

/// What the elevated child is asked to do. Parsed back out of `--step <spec>` by `StepSpec::parse`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum StepSpec {
    /// Copy `versions\<version>\` beside `mh.exe` as configuration `tag` (uninstalling a different
    /// configuration first), provision the accepted manifest's relay, write the receipt -- the
    /// second half of `update::apply`.
    Install { version: String, tag: String },
    /// Remove what the receipt lists and put retail's `mh.dll` back.
    Uninstall,
    /// Write the accepted manifest's relay into `mh_net.ini` + `mh_key.txt`.
    Provision,
}

impl StepSpec {
    /// The `--step` argument text: `install:<version>:<tag>`, `uninstall`, `provision`.
    pub fn to_arg(&self) -> String {
        match self {
            StepSpec::Install { version, tag } => format!("install:{version}:{tag}"),
            StepSpec::Uninstall => "uninstall".to_string(),
            StepSpec::Provision => "provision".to_string(),
        }
    }

    pub fn parse(text: &str) -> Result<StepSpec, String> {
        if text == "uninstall" {
            return Ok(StepSpec::Uninstall);
        }
        if text == "provision" {
            return Ok(StepSpec::Provision);
        }
        if let Some(rest) = text.strip_prefix("install:") {
            // The version may itself carry a `-` (0.1.1-rc1) but never a `:`; the tag is last.
            if let Some((version, tag)) = rest.rsplit_once(':') {
                if !version.is_empty() && !tag.is_empty() {
                    return Ok(StepSpec::Install {
                        version: version.to_string(),
                        tag: tag.to_string(),
                    });
                }
            }
        }
        Err(format!(
            "--step wants install:<version>:<tag>, uninstall or provision, got {text:?}"
        ))
    }

    pub fn describe(&self) -> String {
        match self {
            StepSpec::Install { version, tag } => format!("install {version} ({tag})"),
            StepSpec::Uninstall => "uninstall".to_string(),
            StepSpec::Provision => "provision the relay".to_string(),
        }
    }
}

/// The COMPLETE argv the elevated child is started with. State directory, game directory, the
/// step, and where to write its verdict -- and nothing that opens a window or starts a game.
///
/// `--app-dir` is passed explicitly even when this process used the default: "Run as
/// administrator" under a DIFFERENT administrator account has a different `%LOCALAPPDATA%`, and
/// the child must use ours (the staged version set is there, and so is the log it appends to).
pub fn step_argv(layout: &Layout, game_dir: &Path, step: &StepSpec, result: &Path) -> Vec<String> {
    vec![
        "--app-dir".to_string(),
        layout.root.display().to_string(),
        "--game-dir".to_string(),
        game_dir.display().to_string(),
        "--step".to_string(),
        step.to_arg(),
        "--result".to_string(),
        result.display().to_string(),
    ]
}

/// Can this process create a file in `dir`? The question the whole module turns on, asked of the
/// filesystem rather than guessed from the path (a game under Program Files with a loosened ACL
/// needs no elevation; one on a read-only share is not helped by it).
pub fn can_write(dir: &Path) -> Result<(), std::io::Error> {
    let probe = dir.join(format!(".mh_launcher_write_probe_{}", std::process::id()));
    std::fs::write(&probe, b"probe")?;
    let _ = std::fs::remove_file(&probe);
    Ok(())
}

/// True when a write into `dir` would be refused for lack of rights (and only then -- a missing
/// directory or a full disk is a different problem, not one elevation solves).
pub fn needs_elevation(dir: &Path) -> bool {
    match can_write(dir) {
        Ok(()) => false,
        Err(e) if e.kind() == std::io::ErrorKind::PermissionDenied => {
            log::line(format!(
                "elevate: {} is not writable by this token ({e}) -- the next install/switch/\
                 provision step runs elevated, the game does not",
                dir.display()
            ));
            true
        }
        Err(e) => {
            log::line(format!(
                "elevate: cannot probe {} ({e}); not an access problem, so not elevating",
                dir.display()
            ));
            false
        }
    }
}

/// Is this error text Windows' ERROR_ACCESS_DENIED, as `std::io::Error` prints it?
pub fn is_access_denied(err: &str) -> bool {
    err.contains("(os error 5)") || err.contains("Access is denied")
}

/// Where the elevated child writes its verdict: `<state>\elevated\<stamp>.result`, first line
/// `ok` or `err`, the rest the summary or the message.
fn result_path(layout: &Layout) -> PathBuf {
    layout
        .root
        .join("elevated")
        .join(format!("{}.result", log::stamp().replace([':', '-'], "")))
}

/// Run `step` in an elevated copy of this launcher and wait for it. Returns the child's summary.
///
/// `ShellExecuteExW` with `runas` is the one sanctioned way for a process to ask for elevation:
/// Windows shows the UAC consent dialog (or, on a machine whose policy is "elevate without
/// prompting", nothing) and starts the child with the full token. The parent keeps its own.
pub fn run_step_elevated(
    layout: &Layout,
    game_dir: &Path,
    step: &StepSpec,
) -> Result<String, String> {
    let result = result_path(layout);
    if let Some(parent) = result.parent() {
        std::fs::create_dir_all(parent)
            .map_err(|e| format!("cannot create {}: {e}", parent.display()))?;
    }
    let _ = std::fs::remove_file(&result);
    let exe = std::env::current_exe()
        .map_err(|e| format!("cannot find this executable's own path ({e})"))?;
    let argv = step_argv(layout, game_dir, step, &result);
    log::line(format!(
        "elevate: running {} ELEVATED: {} {}",
        step.describe(),
        exe.display(),
        argv.join(" ")
    ));
    let code = spawn_runas_and_wait(&exe, &argv)?;
    let text = std::fs::read_to_string(&result).map_err(|e| {
        format!(
            "the elevated step exited with code {code} and left no result at {} ({e})",
            result.display()
        )
    })?;
    let _ = std::fs::remove_file(&result);
    let (verdict, rest) = text.split_once('\n').unwrap_or((text.trim(), ""));
    let rest = rest.trim().to_string();
    match verdict.trim() {
        "ok" => {
            log::line(format!("elevate: the elevated step finished -- {rest}"));
            Ok(rest)
        }
        _ => Err(format!("the elevated step failed: {rest}")),
    }
}

/// `ShellExecuteExW(runas)` + wait. Returns the child's exit code.
fn spawn_runas_and_wait(exe: &Path, argv: &[String]) -> Result<u32, String> {
    use std::os::windows::ffi::OsStrExt;
    use windows_sys::Win32::Foundation::{CloseHandle, GetLastError, ERROR_CANCELLED};
    use windows_sys::Win32::System::Threading::{
        GetExitCodeProcess, WaitForSingleObject, INFINITE,
    };
    use windows_sys::Win32::UI::Shell::{
        ShellExecuteExW, SEE_MASK_NOCLOSEPROCESS, SHELLEXECUTEINFOW,
    };
    use windows_sys::Win32::UI::WindowsAndMessaging::SW_SHOWNORMAL;

    let wide = |s: &str| -> Vec<u16> {
        std::ffi::OsStr::new(s)
            .encode_wide()
            .chain(std::iter::once(0))
            .collect()
    };
    let params: String = argv
        .iter()
        .map(|a| {
            if a.contains(' ') || a.is_empty() {
                format!("\"{a}\"")
            } else {
                a.clone()
            }
        })
        .collect::<Vec<_>>()
        .join(" ");
    let verb = wide("runas");
    let file = wide(&exe.display().to_string());
    let params = wide(&params);
    // SAFETY: every pointer handed to ShellExecuteExW outlives the call; the process handle it
    // returns (SEE_MASK_NOCLOSEPROCESS) is waited on and closed once.
    unsafe {
        let mut info: SHELLEXECUTEINFOW = std::mem::zeroed();
        info.cbSize = std::mem::size_of::<SHELLEXECUTEINFOW>() as u32;
        info.fMask = SEE_MASK_NOCLOSEPROCESS;
        info.lpVerb = verb.as_ptr();
        info.lpFile = file.as_ptr();
        info.lpParameters = params.as_ptr();
        info.nShow = SW_SHOWNORMAL;
        if ShellExecuteExW(&mut info) == 0 {
            let err = GetLastError();
            return Err(if err == ERROR_CANCELLED {
                "the elevation prompt was declined -- the game directory needs administrator \
                 rights to install into, and nothing was changed"
                    .to_string()
            } else {
                format!("cannot start the elevated step (err {err})")
            });
        }
        if info.hProcess.is_null() {
            return Err("the elevated step started but left no process handle".to_string());
        }
        WaitForSingleObject(info.hProcess, INFINITE);
        let mut code: u32 = 0;
        GetExitCodeProcess(info.hProcess, &mut code);
        CloseHandle(info.hProcess);
        Ok(code)
    }
}

/// The child's side: perform `step` and write the verdict. Called from `main.rs` BEFORE any window
/// opens, exactly like `--verify-binary`. Returns the process exit code.
pub fn perform_step(layout: &Layout, game_dir: &Path, step: &StepSpec, result: &Path) -> i32 {
    log::line(format!(
        "step: {} in {} (token: {})",
        step.describe(),
        game_dir.display(),
        crate::launch::describe_elevation(crate::launch::current_process_elevation())
    ));
    let outcome = perform(layout, game_dir, step);
    let text = match &outcome {
        Ok(summary) => format!("ok\n{summary}\n"),
        Err(e) => format!("err\n{e}\n"),
    };
    if let Some(parent) = result.parent() {
        let _ = std::fs::create_dir_all(parent);
    }
    if let Err(e) = std::fs::write(result, text) {
        log::line(format!("step: cannot write {}: {e}", result.display()));
        return 1;
    }
    match outcome {
        Ok(summary) => {
            log::line(format!("step: done -- {summary}"));
            0
        }
        Err(e) => {
            log::line(format!("step: failed -- {e}"));
            1
        }
    }
}

fn perform(layout: &Layout, game_dir: &Path, step: &StepSpec) -> Result<String, String> {
    let relay = crate::update::load_accepted(layout).and_then(|m| m.relay);
    match step {
        StepSpec::Install { version, tag } => {
            let pkg = crate::install::PackageName {
                version: version.clone(),
                tag: tag.clone(),
            };
            let package = crate::install::package_file_name(version, tag);
            let staged = crate::update::Staged {
                version_dir: layout.version_dir(version),
                pkg,
                package,
            };
            if !staged.version_dir.is_dir() {
                return Err(format!(
                    "{} is not staged under {}",
                    version,
                    layout.versions().display()
                ));
            }
            let report = crate::update::install_staged_set(&staged, relay.as_ref(), game_dir)?;
            Ok(report.summary())
        }
        StepSpec::Uninstall => crate::install::uninstall(game_dir).map(|r| r.summary()),
        StepSpec::Provision => {
            crate::relay::provision(game_dir, relay.as_ref()).map(|p| p.summary())
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The done_when's "cargo test on the argv split": the elevated child is given the state
    /// directory, the game directory, the step and the result file -- and NOTHING that launches.
    #[test]
    fn the_elevated_step_cannot_launch_the_game() {
        let layout = Layout::rooted(r"C:\state\MissionHumanity");
        let game = Path::new(r"C:\Program Files (x86)\Mission Humanity");
        let step = StepSpec::Install {
            version: "0.1.2".into(),
            tag: "net".into(),
        };
        let result = Path::new(r"C:\state\MissionHumanity\elevated\x.result");
        let argv = step_argv(&layout, game, &step, result);
        assert_eq!(
            argv,
            vec![
                "--app-dir",
                r"C:\state\MissionHumanity",
                "--game-dir",
                r"C:\Program Files (x86)\Mission Humanity",
                "--step",
                "install:0.1.2:net",
                "--result",
                r"C:\state\MissionHumanity\elevated\x.result",
            ]
        );
        for forbidden in [
            "--launch",
            "--update",
            "--self-update",
            "--install",
            "--uninstall",
            "--exit-after-launch",
        ] {
            assert!(
                !argv.iter().any(|a| a == forbidden),
                "{forbidden} in {argv:?}"
            );
        }
    }

    /// The spec round-trips through its `--step` text, hyphenated versions included, and
    /// nonsense is refused rather than guessed at.
    #[test]
    fn step_specs_round_trip_and_nonsense_is_refused() {
        for step in [
            StepSpec::Install {
                version: "0.1.1-rc1".into(),
                tag: "net-debug".into(),
            },
            StepSpec::Install {
                version: "0.2.0".into(),
                tag: "brokered-debug".into(),
            },
            StepSpec::Uninstall,
            StepSpec::Provision,
        ] {
            assert_eq!(StepSpec::parse(&step.to_arg()).unwrap(), step);
        }
        for bad in [
            "install",
            "install:",
            "install:0.1.0",
            "install::net",
            "launch",
            "",
        ] {
            assert!(StepSpec::parse(bad).is_err(), "{bad:?} should be refused");
        }
    }

    /// A directory this token can write needs no elevation; the access-denied text is
    /// recognised in the shape `std::io::Error` prints it.
    #[test]
    fn a_writable_directory_needs_no_elevation() {
        let dir = std::env::temp_dir().join("mh_launcher_test_elevate_probe");
        std::fs::create_dir_all(&dir).unwrap();
        assert!(!needs_elevation(&dir));
        assert!(
            std::fs::read_dir(&dir).unwrap().flatten().next().is_none(),
            "the probe file is removed again"
        );
        std::fs::remove_dir_all(&dir).ok();
        assert!(is_access_denied(
            "cannot move C:\\x aside to C:\\y: Access is denied. (os error 5)"
        ));
        assert!(!is_access_denied(
            "cannot copy C:\\x: The system cannot find the file specified. (os error 2)"
        ));
    }
}
