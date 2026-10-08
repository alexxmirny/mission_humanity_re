//! Where the game's `mh_net.ini` lives (dist RL3 mirror, used by the settings page).
//!
//! This is the launcher's half of `mh::cfgdir` (`src/mh_dll/mh_common/include/mh_config_dir.h`):
//! the DLL decides the directory when the game starts, the launcher decides it when the player
//! edits settings, and the two MUST agree or the page writes a file the game never reads. The rule
//! order is the DLL's:
//!
//! 1. `MH_CONFIG_DIR` -- an explicit directory (non-empty and short enough to append leaf names to).
//! 2. PORTABLE -- `mh_net.ini` already exists BESIDE `mh.exe`: the game directory itself. Every rig
//!    lane and every pre-0.2.0 install is in this mode, and it needs no profile.
//! 3. USER STORAGE -- `<layout root>\games\<hash16>\`, the same hash the launcher uses for its log
//!    root (`paths::game_dir_hash`, shared with the DLL by test vectors).
//!
//! The DLL has a fourth rule (the exe directory, when user storage cannot be created or spelled in
//! the ANSI code page). The launcher cannot know that without creating directories, which
//! resolution must not do, so it answers rule 3 and the write that follows creates the folder; the
//! rare machine where the DLL fell back would see the game read the exe-side folder instead. The
//! `Source` is shown on the Settings page so that case is visible, not silent.
//!
//! **Resolution never creates anything**, and in particular never an `mh_net.ini` beside the exe:
//! that file's mere existence flips the game into portable mode (RL3's trap), so only the player
//! (or a lane builder) may put one there.

#![allow(dead_code)]

use std::path::{Path, PathBuf};

use crate::paths::{game_dir_hash, Layout};

/// The configuration file's name. Same as `relay::INI_NAME`.
pub const INI_NAME: &str = "mh_net.ini";
/// The environment override, read by both the DLL and the launcher.
pub const ENV_CONFIG_DIR: &str = "MH_CONFIG_DIR";
/// Longest accepted override (`kMaxDirLen` in the C++ header: `MAX_PATH - 64`).
pub const MAX_DIR_LEN: usize = 260 - 64;

/// Which rule produced the directory. Mirrors `mh::cfgdir::source_t`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Source {
    /// `MH_CONFIG_DIR`.
    Env,
    /// `mh_net.ini` beside the exe: the game directory.
    Portable,
    /// `<layout root>\games\<hash16>\`.
    User,
}

impl Source {
    /// Short English description for logs and the debug-info dump (the page uses i18n keys
    /// `cfgdir.env` / `cfgdir.portable` / `cfgdir.user` via `i18n_key`).
    pub fn describe(self) -> &'static str {
        match self {
            Source::Env => "env MH_CONFIG_DIR",
            Source::Portable => "portable (mh_net.ini beside the exe)",
            Source::User => "user storage (%LOCALAPPDATA%)",
        }
    }

    pub fn i18n_key(self) -> &'static str {
        match self {
            Source::Env => "cfgdir.env",
            Source::Portable => "cfgdir.portable",
            Source::User => "cfgdir.user",
        }
    }
}

/// The directory that holds the game's `mh_net.ini`, and why.
///
/// `env` is the value of `MH_CONFIG_DIR` (use `env_config_dir()` for the process value); tests pass
/// their own.
pub fn resolve(layout: &Layout, game_dir: &Path, env: Option<&str>) -> (PathBuf, Source) {
    if let Some(e) = env.map(str::trim).filter(|e| !e.is_empty()) {
        if e.len() <= MAX_DIR_LEN {
            return (PathBuf::from(e), Source::Env);
        }
        // Too long to append leaf names to: the DLL ignores it too, and takes the next rule.
    }
    if game_dir.join(INI_NAME).is_file() {
        return (game_dir.to_path_buf(), Source::Portable);
    }
    (
        layout.root.join("games").join(game_dir_hash(game_dir)),
        Source::User,
    )
}

/// `MH_CONFIG_DIR` from the process environment.
pub fn env_config_dir() -> Option<String> {
    std::env::var(ENV_CONFIG_DIR).ok()
}

/// `game_config_dir(layout, game_dir)` -- the design's entry point: the directory, resolved against
/// the real environment.
pub fn game_config_dir(layout: &Layout, game_dir: &Path) -> (PathBuf, Source) {
    resolve(layout, game_dir, env_config_dir().as_deref())
}

/// The ini file itself, and the rule that placed it.
pub fn ini_path(layout: &Layout, game_dir: &Path) -> (PathBuf, Source) {
    let (dir, src) = game_config_dir(layout, game_dir);
    (dir.join(INI_NAME), src)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn scratch(name: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("mh_launcher_test_cfgdir_{name}"));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        d
    }

    #[test]
    fn no_ini_beside_the_exe_means_user_storage_under_the_hash() {
        let root = scratch("user_root");
        let game = scratch("user_game");
        let layout = Layout::rooted(&root);
        let (dir, src) = resolve(&layout, &game, None);
        assert_eq!(src, Source::User);
        assert_eq!(dir, root.join("games").join(game_dir_hash(&game)));
        // Resolution is pure: nothing was created, here or beside the exe.
        assert!(!dir.exists());
        assert!(!game.join(INI_NAME).exists());
        let _ = std::fs::remove_dir_all(&root);
        let _ = std::fs::remove_dir_all(&game);
    }

    #[test]
    fn an_ini_beside_the_exe_makes_the_game_directory_the_config_directory() {
        let root = scratch("port_root");
        let game = scratch("port_game");
        std::fs::write(game.join(INI_NAME), "[net]\r\n").unwrap();
        let layout = Layout::rooted(&root);
        let (dir, src) = resolve(&layout, &game, None);
        assert_eq!((dir.as_path(), src), (game.as_path(), Source::Portable));
        let (ini, _) = ini_path_for(&layout, &game, None);
        assert_eq!(ini, game.join(INI_NAME));
        // A DIRECTORY called mh_net.ini is not a config file.
        let game2 = scratch("port_game2");
        std::fs::create_dir(game2.join(INI_NAME)).unwrap();
        assert_eq!(resolve(&layout, &game2, None).1, Source::User);
        for d in [&root, &game, &game2] {
            let _ = std::fs::remove_dir_all(d);
        }
    }

    fn ini_path_for(layout: &Layout, game: &Path, env: Option<&str>) -> (PathBuf, Source) {
        let (d, s) = resolve(layout, game, env);
        (d.join(INI_NAME), s)
    }

    #[test]
    fn the_env_override_beats_portable_and_a_blank_or_overlong_one_is_ignored() {
        let root = scratch("env_root");
        let game = scratch("env_game");
        std::fs::write(game.join(INI_NAME), "").unwrap();
        let layout = Layout::rooted(&root);
        let (dir, src) = resolve(&layout, &game, Some("D:\\cfg\\mine"));
        assert_eq!((dir, src), (PathBuf::from("D:\\cfg\\mine"), Source::Env));
        assert_eq!(resolve(&layout, &game, Some("   ")).1, Source::Portable);
        assert_eq!(resolve(&layout, &game, Some("")).1, Source::Portable);
        let long = "x".repeat(MAX_DIR_LEN + 1);
        assert_eq!(resolve(&layout, &game, Some(&long)).1, Source::Portable);
        let _ = std::fs::remove_dir_all(&root);
        let _ = std::fs::remove_dir_all(&game);
    }

    #[test]
    fn the_user_folder_matches_the_launchers_log_hash_for_the_same_game_dir() {
        // Same 16-hex hash the DLL derives (paths.rs pins the cross-language vectors); a trailing
        // separator and ASCII case do not change the answer.
        let layout = Layout::rooted("R:\\root");
        let a = resolve(&layout, Path::new("C:\\Games\\MH"), None).0;
        let b = resolve(&layout, Path::new("c:\\games\\mh\\"), None).0;
        assert_eq!(a, b);
        assert_eq!(a.parent().unwrap(), Path::new("R:\\root\\games"));
        assert_eq!(a.file_name().unwrap().len(), 16);
    }
}
