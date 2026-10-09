//! dist RL9: "is the game running from THIS folder?" -- the gate under silent auto-update.
//!
//! WHY NOT JUST `self.session`. The launcher only knows about a game IT started. A player who
//! double-clicks `mh.exe` (or starts it from a shortcut, or from a second launcher window) has a
//! game running in the very folder an update is about to overwrite, and `install_staged_set`
//! replacing `mh.dll` under a live process fails halfway or, worse, succeeds on a file the game has
//! mapped. So the answer comes from the OS: a Toolhelp process snapshot, every `mh.exe` in it, and
//! the full image path of each compared with the game directory.
//!
//! FAIL SAFE = RUNNING. Three things can go wrong and each one answers "running": the snapshot
//! cannot be taken, an `mh.exe` process cannot be opened (an elevated game seen from a standard
//! token, a protected process), or its path cannot be read. An update that waits ten more seconds
//! costs nothing; an update that races a live game costs a corrupt install. The one process this
//! does NOT count is an `mh.exe` whose path WAS read and names another folder -- a second install
//! on the same machine must not hold this one's updates hostage.
//!
//! The decision is a pure function (`running_from`) over a list of `ProcInfo`, so the comparison
//! rules (case, trailing separators, a `\\?\` prefix) are unit-tested without a process; the
//! Toolhelp half (`snapshot`) is proven against a REAL process in the tests below.

use std::path::{Path, PathBuf};

use crate::paths::GAME_EXE;

/// One process of the snapshot, reduced to what the decision needs.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ProcInfo {
    /// The image file name from the snapshot (`mh.exe`), as Toolhelp gives it.
    pub exe_name: String,
    /// The full image path, or `None` when the process could not be opened or queried.
    pub image: Option<PathBuf>,
}

/// Lower-cased, `\\?\`-stripped, separator-normalised, trailing-separator-trimmed text of a path.
/// Not a canonicalisation (nothing touches the disk): both sides of the comparison go through the
/// same function, which is all "the same folder spelled two ways" needs.
fn norm(p: &Path) -> String {
    let mut s = p.to_string_lossy().replace('/', "\\");
    if let Some(rest) = s.strip_prefix(r"\\?\") {
        s = rest.to_string();
    }
    s.trim_end_matches('\\').to_lowercase()
}

/// The pure half: is any `mh.exe` of `procs` running from `dir`? An `mh.exe` with no readable path
/// counts (fail safe); an `mh.exe` whose path is known counts only when its folder is `dir`.
pub fn running_from(dir: &Path, procs: &[ProcInfo]) -> bool {
    let want = norm(dir);
    procs.iter().any(|p| {
        if !p.exe_name.eq_ignore_ascii_case(GAME_EXE) {
            return false;
        }
        match &p.image {
            None => true,
            Some(img) => img.parent().is_some_and(|parent| norm(parent) == want),
        }
    })
}

/// Is `mh.exe` running from `dir` right now? `true` whenever that cannot be established.
pub fn game_running_here(dir: &Path) -> bool {
    match snapshot() {
        Ok(procs) => running_from(dir, &procs),
        Err(e) => {
            crate::log::line(format!(
                "procs: cannot list processes ({e}) -- treating the game as running"
            ));
            true
        }
    }
}

/// Every process named `mh.exe` right now, with its image path where it can be read.
pub fn snapshot() -> Result<Vec<ProcInfo>, String> {
    use windows_sys::Win32::Foundation::{CloseHandle, GetLastError, INVALID_HANDLE_VALUE};
    use windows_sys::Win32::System::Diagnostics::ToolHelp::{
        CreateToolhelp32Snapshot, Process32FirstW, Process32NextW, PROCESSENTRY32W,
        TH32CS_SNAPPROCESS,
    };

    // SAFETY: the snapshot handle is owned here and closed once; the PROCESSENTRY32W is zeroed
    // with `dwSize` set as the API requires, and only read after a successful call fills it.
    unsafe {
        let snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if snap == INVALID_HANDLE_VALUE {
            return Err(format!(
                "CreateToolhelp32Snapshot failed (err {})",
                GetLastError()
            ));
        }
        let mut out = Vec::new();
        let mut entry: PROCESSENTRY32W = std::mem::zeroed();
        entry.dwSize = std::mem::size_of::<PROCESSENTRY32W>() as u32;
        let mut more = Process32FirstW(snap, &mut entry) != 0;
        while more {
            let len = entry
                .szExeFile
                .iter()
                .position(|c| *c == 0)
                .unwrap_or(entry.szExeFile.len());
            let name = String::from_utf16_lossy(&entry.szExeFile[..len]);
            if name.eq_ignore_ascii_case(GAME_EXE) {
                out.push(ProcInfo {
                    exe_name: name,
                    image: image_path(entry.th32ProcessID),
                });
            }
            more = Process32NextW(snap, &mut entry) != 0;
        }
        CloseHandle(snap);
        Ok(out)
    }
}

/// The full image path of process `pid`, or `None` when it cannot be opened or queried.
fn image_path(pid: u32) -> Option<PathBuf> {
    use std::os::windows::ffi::OsStringExt;
    use windows_sys::Win32::Foundation::CloseHandle;
    use windows_sys::Win32::System::Threading::{
        OpenProcess, QueryFullProcessImageNameW, PROCESS_QUERY_LIMITED_INFORMATION,
    };

    // SAFETY: the handle is closed on every path; the buffer is `size` UTF-16 units and `size` is
    // updated by the call to the number actually written.
    unsafe {
        let h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, pid);
        if h.is_null() {
            return None;
        }
        let mut buf = vec![0u16; 1024];
        let mut size = buf.len() as u32;
        let ok = QueryFullProcessImageNameW(h, 0, buf.as_mut_ptr(), &mut size);
        CloseHandle(h);
        if ok == 0 {
            return None;
        }
        Some(PathBuf::from(std::ffi::OsString::from_wide(
            &buf[..size as usize],
        )))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn info(name: &str, image: Option<&str>) -> ProcInfo {
        ProcInfo {
            exe_name: name.to_string(),
            image: image.map(PathBuf::from),
        }
    }

    #[test]
    fn only_an_mh_exe_in_this_folder_counts_and_spelling_does_not_matter() {
        let dir = Path::new(r"C:\Games\Mission Humanity");
        // Another folder, another program: not running here.
        assert!(!running_from(
            dir,
            &[
                info("mh.exe", Some(r"C:\Games\Other\mh.exe")),
                info(
                    "notepad.exe",
                    Some(r"C:\Games\Mission Humanity\notepad.exe")
                ),
            ]
        ));
        // Same folder under case, slash, trailing-separator and \\?\ spellings.
        for dir_spelling in [
            r"C:\Games\Mission Humanity",
            r"c:\games\mission humanity\",
            "C:/Games/Mission Humanity",
            r"\\?\C:\Games\Mission Humanity",
        ] {
            assert!(
                running_from(
                    Path::new(dir_spelling),
                    &[info("MH.EXE", Some(r"C:\Games\Mission Humanity\mh.exe"))]
                ),
                "{dir_spelling}"
            );
        }
        // A subfolder is not the folder.
        assert!(!running_from(
            dir,
            &[info(
                "mh.exe",
                Some(r"C:\Games\Mission Humanity\sub\mh.exe")
            )]
        ));
        assert!(!running_from(dir, &[]));
    }

    #[test]
    fn an_mh_exe_whose_path_cannot_be_read_counts_as_running() {
        let dir = Path::new(r"C:\Games\Mission Humanity");
        assert!(running_from(dir, &[info("mh.exe", None)]));
        // ... but an unreadable process of another NAME is none of our business.
        assert!(!running_from(dir, &[info("other.exe", None)]));
    }

    /// The helper a real-process test starts: the test binary itself, copied to `mh.exe`, runs
    /// this ignored test, which just stays alive for a while.
    #[test]
    #[ignore = "helper process for the real-process test below"]
    fn sleeper_helper() {
        std::thread::sleep(std::time::Duration::from_secs(60));
    }

    /// RL9's first clause, against the OS: a real `mh.exe` in folder A is "running" for A and not
    /// for B, and stops being running once it exits.
    #[test]
    fn game_running_here_sees_a_real_process_in_its_own_folder_only() {
        let base =
            std::env::temp_dir().join(format!("mh_launcher_test_procs_{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&base);
        let a = base.join("a");
        let b = base.join("b");
        std::fs::create_dir_all(&b).unwrap();
        let mut child = testing::spawn_copy_as_mh_exe(&a);
        // The process exists as soon as spawn returns, but the snapshot needs it to have a name
        // and image; poll briefly rather than sleep a fixed time.
        let mut seen = false;
        for _ in 0..100 {
            if game_running_here(&a) {
                seen = true;
                break;
            }
            std::thread::sleep(std::time::Duration::from_millis(50));
        }
        assert!(seen, "the copied mh.exe was never seen in its own folder");
        // The other folder holds no mh.exe process. (Any other mh.exe on this box whose path is
        // readable and elsewhere does not count; an UNREADABLE one would -- fail safe -- so this
        // asserts only when nothing unreadable is around.)
        let all = snapshot().unwrap();
        if all.iter().all(|p| p.image.is_some()) {
            assert!(!game_running_here(&b));
        }
        child.kill().unwrap();
        child.wait().unwrap();
        let mut gone = false;
        for _ in 0..100 {
            if !game_running_here(&a) {
                gone = true;
                break;
            }
            std::thread::sleep(std::time::Duration::from_millis(50));
        }
        assert!(gone, "an exited mh.exe still counts as running");
        let _ = std::fs::remove_dir_all(&base);
    }
}

/// Test helper shared with `app.rs`'s tests: a REAL process named `mh.exe` in `dir`.
#[cfg(test)]
pub(crate) mod testing {
    use super::GAME_EXE;
    use std::path::Path;
    use std::process::{Child, Command, Stdio};

    /// Copy the running test binary to `dir\mh.exe` and start it on the ignored `sleeper_helper`
    /// test, which just stays alive. Kill and `wait()` the child when done.
    pub(crate) fn spawn_copy_as_mh_exe(dir: &Path) -> Child {
        std::fs::create_dir_all(dir).unwrap();
        let exe = dir.join(GAME_EXE);
        std::fs::copy(std::env::current_exe().unwrap(), &exe).unwrap();
        Command::new(&exe)
            .args([
                "procs::tests::sleeper_helper",
                "--exact",
                "--ignored",
                "--nocapture",
            ])
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .spawn()
            .unwrap()
    }
}
