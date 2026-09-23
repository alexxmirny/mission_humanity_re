//! Where the launcher keeps its own state: `%LOCALAPPDATA%\MissionHumanity\`.
//!
//! PER-USER, AND THAT IS THE POINT (plan decision D10). Everything the launcher writes lives under
//! the user's own local app-data directory, so it installs, updates and self-replaces without ever
//! asking for elevation. The alternative -- `%ProgramFiles%` -- buys nothing here and costs a UAC
//! prompt on every update, which is the thing dist LA1's acceptance clause forbids outright.
//!
//! ```text
//! %LOCALAPPDATA%\MissionHumanity\
//!     launcher.toml          the game directory, and what is installed there
//!     versions\<ver>\        one unzipped release set per version (dist LA2 swaps between them)
//!     accepted\              the last accepted manifest.json + .minisig (dist LA6 reads its relay)
//!     logs\launcher.log      this program's own log
//! ```
//!
//! The game directory is NOT under here. It is wherever the player installed the game, it holds
//! game data this project does not ship, and the launcher only ever copies files into it.

use std::path::{Path, PathBuf};

/// The single directory name this project owns under `%LOCALAPPDATA%`.
pub const APP_DIR: &str = "MissionHumanity";

/// The file the launcher writes into a game directory naming everything it put there.
pub const INSTALL_MANIFEST: &str = "mh_launcher_installed.txt";

/// The suffix a displaced game file is parked under, so an install is reversible.
pub const BACKUP_SUFFIX: &str = ".mhbak";

/// The one file whose presence makes a directory a game directory.
pub const GAME_EXE: &str = "mh.exe";

/// What a version directory is called while it is still being filled (dist LA2).
///
/// A download lands in `versions\<ver>.staging\` and becomes `versions\<ver>\` by a RENAME, so the
/// real name only ever exists over a complete set. The suffix is therefore load-bearing rather than
/// cosmetic: `update::version_dirs` skips it, so an update interrupted halfway leaves litter rather
/// than a version the launcher would offer to play.
pub const VERSION_STAGING_SUFFIX: &str = ".staging";

/// Written inside `versions\<ver>\` once that version has actually run (dist LA2).
///
/// It is what makes the previous version a last-known-good rather than garbage: nothing is pruned
/// while the newest version has never started, so an update that installs perfectly and then cannot
/// launch still has the directory behind it sitting on disk.
pub const FIRST_RUN_MARKER: &str = "first_run_ok";

#[derive(Clone, Debug)]
pub struct Layout {
    pub root: PathBuf,
}

impl Layout {
    /// The real layout, rooted at `%LOCALAPPDATA%\MissionHumanity`.
    ///
    /// `directories` asks the OS (`SHGetKnownFolderPath`/`FOLDERID_LocalAppData`) rather than
    /// joining `%USERPROFILE%` with a guess, which is what makes this correct on a machine with a
    /// redirected or relocated profile.
    pub fn discover() -> Result<Self, String> {
        let base = directories::BaseDirs::new()
            .ok_or_else(|| "cannot locate the user's local app-data directory".to_string())?;
        Ok(Self {
            root: base.data_local_dir().join(APP_DIR),
        })
    }

    /// A layout rooted somewhere explicit. `--app-dir` uses it; so do the tests.
    pub fn rooted(root: impl Into<PathBuf>) -> Self {
        Self { root: root.into() }
    }

    pub fn config(&self) -> PathBuf {
        self.root.join("launcher.toml")
    }

    pub fn versions(&self) -> PathBuf {
        self.root.join("versions")
    }

    pub fn version_dir(&self, version: &str) -> PathBuf {
        self.versions().join(version)
    }

    pub fn logs(&self) -> PathBuf {
        self.root.join("logs")
    }

    /// Where built reports and their dumps go, and where `reports\outbox\` sits under it (dist
    /// LA4 + RP1). Under the launcher's own state rather than the game folder: a report is the
    /// LAUNCHER's artefact, and a player who uninstalls the game should not lose the evidence of
    /// why they did.
    pub fn reports(&self) -> PathBuf {
        self.root.join("reports")
    }

    pub fn log_file(&self) -> PathBuf {
        self.logs().join("launcher.log")
    }

    /// dist LA13: THE LAUNCHER-OWNED LOGS ROOT for a game directory --
    /// `logs\<game-dir-hash>\` under the launcher's own state, handed to the game by environment
    /// (`launch::ENV_LOG_ROOT`) so its session directories, `mh_run.txt` and the crash marker
    /// land somewhere the launcher can read back.
    ///
    /// WHY NOT `<game>\logs\`. Both 09-20 players run the game from `C:\Program Files (x86)\`.
    /// Retail `mh.exe` carries no manifest, so a NON-elevated game process is UAC-virtualized:
    /// its writes beside the exe silently go to `%LOCALAPPDATA%\VirtualStore\Program Files
    /// (x86)\...`, while this 64-bit launcher (never virtualized) reads the real `<game>\logs\`
    /// and finds nothing -- no session in the report, no crash marker, an exit-code-only verdict.
    /// A root under the launcher's own per-user state is written by the game and read by the
    /// launcher through the same, un-virtualized path. Per game directory (hashed, since a path
    /// is not a directory name) so two installs do not interleave their sessions.
    pub fn game_log_root(&self, game_dir: &Path) -> PathBuf {
        self.logs().join(game_dir_hash(game_dir))
    }

    /// `accepted\` -- the last manifest that passed every gate, kept WITH its signature so a
    /// later launch can re-verify it and read its relay (dist LA6, `update::load_accepted`).
    pub fn accepted_dir(&self) -> PathBuf {
        self.root.join("accepted")
    }

    pub fn accepted_manifest(&self) -> PathBuf {
        self.accepted_dir().join(crate::update::MANIFEST_NAME)
    }

    pub fn accepted_signature(&self) -> PathBuf {
        self.accepted_dir().join(crate::update::SIGNATURE_NAME)
    }
}

/// The directory name `game_log_root` uses for a game directory: the first 16 hex digits of the
/// SHA-256 of the path, lower-cased and with trailing separators trimmed, so `C:\Games\MH` and
/// `c:\games\mh\` (the same folder to Windows) hash the same. A hash rather than a sanitised path
/// because the path can be longer than a directory name may be (dist LA10 was a ~230-char install).
pub fn game_dir_hash(game_dir: &Path) -> String {
    use sha2::{Digest, Sha256};
    let text = game_dir
        .to_string_lossy()
        .trim_end_matches(['\\', '/'])
        .to_ascii_lowercase();
    let digest = Sha256::digest(text.as_bytes());
    digest.iter().take(8).map(|b| format!("{b:02x}")).collect()
}

/// Is this a game directory? The test is `mh.exe` beside it and nothing more.
///
/// Deliberately weak. A stricter check (the data packs, a known size, a hash) would reject the
/// legitimate installs this project cannot enumerate -- retail, re-release, a copy made for
/// testing -- and the failure mode of a weak check is the launch button not working, which the
/// player sees immediately. Note this also accepts a directory whose `mh.exe` is something else
/// entirely; `launch.rs` reports whatever that process does rather than pretending to vet it.
pub fn is_game_dir(dir: &Path) -> bool {
    dir.join(GAME_EXE).is_file()
}

// --------------------------------------------------------------------------- dist LA7

/// Where the game directory the launcher is about to use came from.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum GameDirSource {
    /// The launcher executable's own directory holds `mh.exe` -- the "drop it into the game
    /// folder and run it" path, which is the one the player is expected to take.
    OwnDir,
    /// The process's current working directory holds `mh.exe` (a shortcut with "Start in", or a
    /// console opened in the game folder).
    Cwd,
    /// Neither of the above, and the directory `launcher.toml` remembers still holds `mh.exe`.
    Saved,
}

impl GameDirSource {
    pub fn describe(self) -> &'static str {
        match self {
            GameDirSource::OwnDir => "the launcher's own directory",
            GameDirSource::Cwd => "the current directory",
            GameDirSource::Saved => "the saved setting",
        }
    }
}

/// The outcome of `resolve_game_dir`.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct ResolvedGameDir {
    /// The directory to use, and where it came from. `None` when nothing offered holds `mh.exe`:
    /// the player is asked, and only then.
    pub found: Option<(PathBuf, GameDirSource)>,
    /// The saved directory no longer holds `mh.exe`. Named, so the Play view can say which
    /// folder went missing rather than silently carrying a path that no longer works.
    pub stale_saved: Option<PathBuf>,
}

impl ResolvedGameDir {
    /// The notice for the status line when the saved directory has lost its `mh.exe`.
    pub fn notice(&self) -> Option<String> {
        let stale = self.stale_saved.as_ref()?;
        Some(match &self.found {
            Some((dir, source)) => format!(
                "the saved game directory {} no longer holds {GAME_EXE}; using {} ({}) instead",
                stale.display(),
                dir.display(),
                source.describe()
            ),
            None => format!(
                "the saved game directory {} no longer holds {GAME_EXE} -- pick the folder that does",
                stale.display()
            ),
        })
    }
}

/// Find the game directory without asking (dist LA7): the launcher's own directory, then the
/// process CWD, then the directory `launcher.toml` remembers -- the FIRST that holds `mh.exe` wins.
///
/// THE ORDER IS THE USER'S PATH. "Download the launcher, drop it into the game folder, run it"
/// means the exe's own directory is the strongest signal there is, stronger than a setting written
/// on some earlier run against some other copy of the game; a saved directory only decides when
/// the launcher was started from somewhere that is not a game folder. A saved directory that no
/// longer holds `mh.exe` is reported by name (`stale_saved`) whether or not something else was
/// found, because a path that silently stopped working is the kind of thing a player only
/// discovers when Play does nothing.
///
/// `--game-dir` is not an input here: `main.rs` skips this function entirely when it is given,
/// which is what "overrides everything" means.
pub fn resolve_game_dir(
    own_dir: Option<&Path>,
    cwd: Option<&Path>,
    saved: &str,
) -> ResolvedGameDir {
    let saved = saved.trim();
    let saved_path = if saved.is_empty() {
        None
    } else {
        Some(PathBuf::from(saved))
    };
    let stale_saved = saved_path.as_ref().filter(|p| !is_game_dir(p)).cloned();
    let candidates = [
        (own_dir.map(Path::to_path_buf), GameDirSource::OwnDir),
        (cwd.map(Path::to_path_buf), GameDirSource::Cwd),
        (saved_path, GameDirSource::Saved),
    ];
    let found = candidates
        .into_iter()
        .filter_map(|(dir, source)| dir.map(|d| (d, source)))
        .find(|(dir, _)| is_game_dir(dir));
    ResolvedGameDir { found, stale_saved }
}

/// The newest `<UTC>_<mid8>_<slot>_<role>\` directory under a logs root, if any -- the
/// launcher-owned root (dist LA13, `Layout::game_log_root`) or a game's own `logs\`.
///
/// The game creates one per SESSION (mp SES1); dist LA4's report is a zip of it.
///
/// THE NAME DECIDES, NOT THE MODIFICATION TIME, and that is a correction to LA1's version rather
/// than a refinement of it. Every session directory is named `YYYYMMDDTHHMMSSZ_...` (see
/// `src/mh_dll/mh_common/include/mh_session_dir.h`), so lexicographic order over that prefix IS
/// chronological order, exactly, forever. An mtime is a different fact that usually agrees:
/// two sessions opened inside the same filesystem timestamp tick have EQUAL mtimes and the
/// comparison then picks whichever `read_dir` yielded first (measured -- it is what made this
/// function's own test flaky), and copying a logs folder off a rig VM rewrites every mtime to the
/// moment of the copy while leaving the names intact. Mtime remains the tie-breaker for a
/// directory whose name is not a stamp, because something has to order those and nothing else can.
pub fn newest_session_dir_in(logs: &Path) -> Option<PathBuf> {
    let mut best: Option<(String, std::time::SystemTime, PathBuf)> = None;
    for entry in std::fs::read_dir(logs).ok()?.flatten() {
        if !entry.file_type().map(|t| t.is_dir()).unwrap_or(false) {
            continue;
        }
        let name = entry.file_name().to_string_lossy().to_string();
        let stamp = utc_stamp_prefix(&name).unwrap_or_default();
        let when = entry
            .metadata()
            .and_then(|m| m.modified())
            .unwrap_or(std::time::SystemTime::UNIX_EPOCH);
        let better = match best.as_ref() {
            None => true,
            Some((bs, bw, _)) => (&stamp, &when) > (bs, bw),
        };
        if better {
            best = Some((stamp, when, entry.path()));
        }
    }
    best.map(|(_, _, p)| p)
}

/// `20260917T164346Z` off the front of a directory name, or `None` if it does not start with one.
///
/// Shape-checked rather than parsed: the only thing that matters is that every real stamp is the
/// same fixed width and character class, so string comparison between two of them is a comparison
/// of instants. A name that fails this is not "an old stamp", it is not a stamp.
fn utc_stamp_prefix(name: &str) -> Option<String> {
    let b = name.as_bytes();
    if b.len() < 16 {
        return None;
    }
    let digits = |r: std::ops::Range<usize>| b[r].iter().all(u8::is_ascii_digit);
    if digits(0..8) && b[8] == b'T' && digits(9..15) && b[15] == b'Z' {
        return Some(name[..16].to_string());
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    /// dist LA13: the same folder hashes the same however it is spelled, different folders differ,
    /// and the root sits under the launcher's own logs directory.
    #[test]
    fn the_game_log_root_is_per_folder_and_spelling_blind() {
        let a = game_dir_hash(Path::new(r"C:\Games\Mission Humanity"));
        assert_eq!(a.len(), 16);
        assert_eq!(a, game_dir_hash(Path::new(r"c:\games\mission humanity\")));
        assert_ne!(
            a,
            game_dir_hash(Path::new(r"C:\Program Files (x86)\Mission Humanity"))
        );
        let l = Layout::rooted("state");
        let root = l.game_log_root(Path::new(r"C:\Games\Mission Humanity"));
        assert_eq!(root, Path::new("state").join("logs").join(&a));
    }

    #[test]
    fn layout_is_a_pure_join() {
        let l = Layout::rooted("anywhere/at/all");
        assert!(l.config().ends_with("launcher.toml"));
        assert!(l.version_dir("0.1.0").ends_with("versions/0.1.0"));
        assert!(l.log_file().ends_with("logs/launcher.log"));
    }

    /// Two session directories created in the same instant -- the case that makes an mtime
    /// comparison pick at random -- and the one whose NAME is later must win every time.
    #[test]
    fn the_newest_session_is_chosen_by_its_utc_name_not_its_mtime() {
        let dir = std::env::temp_dir().join("mh_launcher_test_newest_session");
        std::fs::remove_dir_all(&dir).ok();
        for leaf in [
            "20260917T164346Z_dedd707c_1_client",
            "20260916T101010Z_menu_solo",
            "20260917T110000Z_menu_host",
            "not-a-session-directory",
        ] {
            std::fs::create_dir_all(dir.join("logs").join(leaf)).unwrap();
        }
        let got = newest_session_dir_in(&dir.join("logs")).unwrap();
        assert_eq!(
            got.file_name().unwrap().to_string_lossy(),
            "20260917T164346Z_dedd707c_1_client"
        );
        assert_eq!(
            utc_stamp_prefix("20260917T164346Z_x").as_deref(),
            Some("20260917T164346Z")
        );
        for bad in ["logs", "2026-09-17T16Z_x", "20260917X164346Z_x", "short"] {
            assert!(utc_stamp_prefix(bad).is_none(), "{bad}");
        }
        std::fs::remove_dir_all(&dir).ok();
    }

    /// dist LA7: the four cases of the resolution order -- own directory, CWD, saved, none -- and
    /// the stale-saved notice. Each candidate is a real temporary directory with or without an
    /// `mh.exe` in it, because `is_game_dir` is the whole test.
    fn game_dir_fixture(name: &str) -> (PathBuf, PathBuf, PathBuf, PathBuf) {
        let root = std::env::temp_dir().join(format!("mh_launcher_test_resolve_{name}"));
        let _ = std::fs::remove_dir_all(&root);
        let mk = |leaf: &str, with_exe: bool| {
            let d = root.join(leaf);
            std::fs::create_dir_all(&d).unwrap();
            if with_exe {
                std::fs::write(d.join(GAME_EXE), b"exe").unwrap();
            }
            d
        };
        (
            root.clone(),
            mk("own", name.contains("own")),
            mk("cwd", name.contains("cwd")),
            mk("saved", name.contains("saved")),
        )
    }

    #[test]
    fn the_launchers_own_directory_wins_over_everything() {
        let (root, own, cwd, saved) = game_dir_fixture("own_cwd_saved");
        let r = resolve_game_dir(Some(&own), Some(&cwd), &saved.display().to_string());
        assert_eq!(r.found, Some((own.clone(), GameDirSource::OwnDir)));
        assert!(
            r.stale_saved.is_none(),
            "the saved one is fine, just outranked"
        );
        assert!(r.notice().is_none());
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn the_cwd_is_next_when_the_launcher_sits_elsewhere() {
        let (root, own, cwd, saved) = game_dir_fixture("cwd_saved");
        let r = resolve_game_dir(Some(&own), Some(&cwd), &saved.display().to_string());
        assert_eq!(r.found, Some((cwd.clone(), GameDirSource::Cwd)));
        assert!(r.notice().is_none());
        // No own directory at all (current_exe failed) is the same answer.
        let r = resolve_game_dir(None, Some(&cwd), "");
        assert_eq!(r.found, Some((cwd, GameDirSource::Cwd)));
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn the_saved_directory_is_the_last_resort() {
        let (root, own, cwd, saved) = game_dir_fixture("saved");
        let r = resolve_game_dir(Some(&own), Some(&cwd), &saved.display().to_string());
        assert_eq!(r.found, Some((saved.clone(), GameDirSource::Saved)));
        assert!(r.stale_saved.is_none());
        assert!(r.notice().is_none());
        // Surrounding whitespace in the TOML value is not part of the path.
        let padded = format!("  {}  ", saved.display());
        assert_eq!(
            resolve_game_dir(None, None, &padded).found,
            Some((saved, GameDirSource::Saved))
        );
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn nothing_matching_means_the_player_is_asked_and_an_empty_setting_is_no_notice() {
        let (root, own, cwd, _saved) = game_dir_fixture("none");
        let r = resolve_game_dir(Some(&own), Some(&cwd), "");
        assert_eq!(r, ResolvedGameDir::default());
        assert!(r.found.is_none());
        assert!(r.notice().is_none(), "no setting, nothing to report");
        let _ = std::fs::remove_dir_all(&root);
    }

    /// The done_when clause: *a saved directory whose mh.exe is gone shows a named notice*. Both
    /// with and without a fallback, and the notice names the folder that went missing.
    #[test]
    fn a_saved_directory_that_lost_its_exe_is_named_not_silently_kept() {
        let (root, own, cwd, saved) = game_dir_fixture("stale_own");
        let saved_text = saved.display().to_string();
        // Own directory has the exe, the saved one does not: own wins AND the notice says so.
        let r = resolve_game_dir(Some(&own), Some(&cwd), &saved_text);
        assert_eq!(r.found, Some((own.clone(), GameDirSource::OwnDir)));
        assert_eq!(r.stale_saved.as_ref(), Some(&saved));
        let notice = r.notice().expect("a stale saved directory is reported");
        assert!(notice.contains(&saved_text), "{notice}");
        assert!(notice.contains(&own.display().to_string()), "{notice}");
        assert!(notice.contains("no longer holds mh.exe"), "{notice}");
        // Nothing else matches either: still named, and the player is asked.
        let r = resolve_game_dir(Some(&cwd), Some(&cwd), &saved_text);
        assert!(r.found.is_none());
        let notice = r.notice().unwrap();
        assert!(notice.contains(&saved_text), "{notice}");
        assert!(notice.contains("pick the folder"), "{notice}");
        // A saved path that does not exist at all is the same case as one that lost its exe.
        let gone = root.join("never_existed");
        let r = resolve_game_dir(None, None, &gone.display().to_string());
        assert_eq!(r.stale_saved, Some(gone));
        assert!(r.found.is_none());
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn a_directory_without_the_exe_is_not_a_game_directory() {
        let dir = std::env::temp_dir().join("mh_launcher_test_not_a_game_dir");
        std::fs::create_dir_all(&dir).unwrap();
        assert!(!is_game_dir(&dir));
        assert!(newest_session_dir_in(&dir.join("logs")).is_none());
        std::fs::remove_dir_all(&dir).ok();
    }
}
