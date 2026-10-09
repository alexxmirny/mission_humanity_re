//! dist RL4: the one-time move from "everything beside `mh.exe`" to user storage.
//!
//! Before v0.2.0 the game folder was where everything lived: `mh_net.ini`, `mh_key.txt`, `logs\`,
//! the shipped README/LICENSE, the harness DLLs. v0.2.0's `mh.dll` resolves its own files through
//! `mh::cfgdir` (RL3) -- `%LOCALAPPDATA%\MissionHumanity\games\<hash16>\` unless an `mh_net.ini`
//! sits beside the exe, which means PORTABLE MODE. So a leftover ini is not harmless clutter: it pins
//! the install to the old layout forever. This module moves the player's things out and the clutter
//! away, once, and says what it did in the log.
//!
//! ```text
//!   <game>\mh_net.ini   user-class keys (settings_schema.json) ──► <config>\mh_net.ini   (fill-only)
//!   <game>\mh_key.txt   ───────────────────────────────────────► <config>\mh_key.txt   (fill-only)
//!   <game>\logs\        ───────────────────────────────────────► <config>\logs\
//!   <game>\{mh_net.ini, mh_key.txt, README.txt, LICENSE, THIRD_PARTY.md, mh_run.txt,
//!           mh_harness.dll, libmh.dll, mh_config_refused.log}  ──► <config>\migrated\<date>\  (backup)
//!   kept: mh_launcher_installed.txt (the receipt), *.mhbak, binaries, lang\, retail data
//! ```
//!
//! RULES, each one a decision rather than a default:
//!
//! * **Only for a game that can use the new layout.** The receipt must exist and name a version
//!   that reads user storage (`reads_user_storage`: `>= 0.2.0-rc8`, so release candidates on the
//!   `latest` channel migrate too). Moving the ini out from under an rc7.1 `mh.dll` would make it
//!   read defaults and mint a new key; a folder with no receipt (a rig lane, a hand install) is not
//!   ours to tidy at all.
//! * **Never remove what the receipt lists.** The 0.2.0 zip decides which of the nine names are
//!   still part of the install (a `net-debug` set legitimately ships `mh_harness.dll`); only a name
//!   the current receipt does NOT list is a leftover.
//! * **Fill-only merges.** A key the config-dir ini already has is not overwritten: it is newer than
//!   anything in the old file, and a half-finished earlier run must not undo the player's edit.
//!   Only USER-class rows move (`Schema::builtin()` -- the generated registry's `C_USER` rows);
//!   every dev knob in the old ini is dropped, and `[net] relay` is dropped too because the launcher
//!   derives it from `relay_mode` at provision time.
//! * **Nothing is destroyed.** Every removed file is moved into `migrated\<date>\` first. Logs are
//!   MOVED, not copied (they can be hundreds of MB); a name that already exists at the destination
//!   goes to the backup instead.
//! * **Idempotent.** A second run finds nothing, changes nothing, and logs that.
//! * **Elevate only when the game folder needs it** (`route`): the whole step runs as
//!   `--step migrate` in an elevated copy of the launcher, exactly like LA13's install step.

use std::path::{Path, PathBuf};

use crate::cfgdir;
use crate::elevate;
use crate::install;
use crate::log;
use crate::paths::Layout;
use crate::relay::{INI_NAME, KEY_NAME};
use crate::settings::ini_io;
use crate::settings::schema::{Schema, Store};

/// The first game version that resolves its own files through `mh::cfgdir`: the 0.2.0 release
/// candidate 8 (a release candidate on the `latest` channel must migrate too), and every later
/// candidate and release. Compared by `reads_user_storage`, NOT as semver: semver orders the
/// pre-release identifiers `rc10` < `rc8` as text.
pub const MIN_GAME_VERSION: &str = "0.2.0-rc8";

/// Does this game version read its files from user storage? `0.2.0-rc<N>` for N >= 8, any other
/// pre-release of 0.2.0 only if it is named after rc8 would be (unknown spellings are NOT
/// assumed new), and every version >= 0.2.0.
pub fn reads_user_storage(version: &str) -> bool {
    let Ok(v) = semver::Version::parse(version) else {
        return false;
    };
    let release = semver::Version::new(v.major, v.minor, v.patch);
    let base = semver::Version::new(0, 2, 0);
    if release > base {
        return true;
    }
    if release < base {
        return false;
    }
    if v.pre.is_empty() {
        return true;
    }
    // 0.2.0-rc<N>[.x]: N is the leading digits after "rc".
    let pre = v.pre.as_str();
    let digits: String = pre
        .strip_prefix("rc")
        .map(|r| r.chars().take_while(char::is_ascii_digit).collect())
        .unwrap_or_default();
    digits.parse::<u32>().is_ok_and(|n| n >= 8)
}

/// Files removed from the game folder (after a backup), in the order they are handled. The ini and
/// the key are merged into the config directory first; the rest are simply clutter of older zips.
pub const LEGACY_FILES: [&str; 9] = [
    INI_NAME,
    KEY_NAME,
    "README.txt",
    "LICENSE",
    "THIRD_PARTY.md",
    "mh_run.txt",
    "mh_harness.dll",
    "libmh.dll",
    "mh_config_refused.log",
];

/// `[section] key` pairs that are user-class but are NOT migrated: derived at provision time.
const DERIVED: [(&str, &str); 1] = [("net", "relay")];

/// What the survey of a game folder found.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Survey {
    pub ini: bool,
    pub key: bool,
    pub logs: bool,
    /// The other `LEGACY_FILES` that are present and not part of the current receipt.
    pub strays: Vec<String>,
}

impl Survey {
    pub fn is_empty(&self) -> bool {
        !self.ini && !self.key && !self.logs && self.strays.is_empty()
    }

    pub fn describe(&self) -> String {
        let mut parts: Vec<String> = Vec::new();
        if self.ini {
            parts.push(INI_NAME.to_string());
        }
        if self.key {
            parts.push(KEY_NAME.to_string());
        }
        if self.logs {
            parts.push("logs\\".to_string());
        }
        parts.extend(self.strays.iter().cloned());
        parts.join(", ")
    }
}

/// Is this game folder one the migration may touch? `Ok` carries the receipt.
pub fn eligible(game_dir: &Path) -> Result<install::Manifest, String> {
    let receipt = install::read_manifest(game_dir).ok_or_else(|| {
        "no install record here -- not a folder this launcher installed into".to_string()
    })?;
    if semver::Version::parse(&receipt.version).is_err() {
        return Err(format!(
            "the install record's version {:?} is unreadable",
            receipt.version
        ));
    }
    if !reads_user_storage(&receipt.version) {
        return Err(format!(
            "the installed game is {} -- older than {MIN_GAME_VERSION}, which still reads its files \
             from this folder",
            receipt.version
        ));
    }
    Ok(receipt)
}

/// What is there to migrate in `game_dir`, given its receipt. Reads only.
pub fn survey(game_dir: &Path, receipt: &install::Manifest) -> Survey {
    let listed = |name: &str| {
        receipt
            .files
            .iter()
            .any(|(_, _, f)| f.eq_ignore_ascii_case(name))
    };
    let mut s = Survey {
        ini: game_dir.join(INI_NAME).is_file(),
        key: game_dir.join(KEY_NAME).is_file(),
        logs: dir_has_entries(&game_dir.join("logs")),
        strays: Vec::new(),
    };
    for name in &LEGACY_FILES[2..] {
        if game_dir.join(name).is_file() && !listed(name) {
            s.strays.push((*name).to_string());
        }
    }
    s
}

fn dir_has_entries(dir: &Path) -> bool {
    std::fs::read_dir(dir).is_ok_and(|mut d| d.next().is_some())
}

/// What the launcher should do about a survey.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Verdict {
    /// Not eligible, with the reason (logged once per start at most).
    Skip(String),
    /// Eligible and clean: nothing to do.
    Clean,
    /// Something to migrate.
    Needed(Survey),
}

pub fn assess(game_dir: &Path) -> Verdict {
    match eligible(game_dir) {
        Err(why) => Verdict::Skip(why),
        Ok(receipt) => {
            let s = survey(game_dir, &receipt);
            if s.is_empty() {
                Verdict::Clean
            } else {
                Verdict::Needed(s)
            }
        }
    }
}

/// How a `Needed` verdict is carried out.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Route {
    /// Nothing to do (skip or clean).
    None,
    /// This process, this token.
    Direct,
    /// An elevated re-run (`--step migrate`): the game folder is not this token's to write.
    Elevated,
    /// The game is running from the folder: wait.
    Wait,
}

/// The decision, separated from the filesystem so it can be unit-tested (Program Files cannot be):
/// `writable` is `!elevate::needs_elevation(game_dir)`, `game_running` is `procs::game_running_here`.
pub fn route(verdict: &Verdict, writable: bool, game_running: bool) -> Route {
    match verdict {
        Verdict::Skip(_) | Verdict::Clean => Route::None,
        Verdict::Needed(_) if game_running => Route::Wait,
        Verdict::Needed(_) if writable => Route::Direct,
        Verdict::Needed(_) => Route::Elevated,
    }
}

/// What a run did.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Report {
    pub changed: bool,
    pub ini_keys: Vec<String>,
    pub key_moved: bool,
    pub logs_moved: usize,
    pub backed_up: Vec<String>,
    pub backup_dir: Option<PathBuf>,
}

impl Report {
    pub fn summary(&self) -> String {
        if !self.changed {
            return "migration: nothing to do -- the game folder is already clean".to_string();
        }
        format!(
            "migrated {} setting(s), {} key, {} log item(s); {} file(s) backed up to {}",
            self.ini_keys.len(),
            if self.key_moved { "the" } else { "no" },
            self.logs_moved,
            self.backed_up.len(),
            self.backup_dir
                .as_ref()
                .map(|p| p.display().to_string())
                .unwrap_or_else(|| "-".to_string())
        )
    }
}

/// The config directory migration writes to: the `MH_CONFIG_DIR` override if set, else user
/// storage for this game folder. Deliberately NOT `cfgdir::game_config_dir`: with the legacy ini
/// still beside the exe that function answers "portable" -- the very state being left.
pub fn target_dir(layout: &Layout, game_dir: &Path, env: Option<&str>) -> PathBuf {
    if let Some(e) = env.map(str::trim).filter(|e| !e.is_empty()) {
        if e.len() <= cfgdir::MAX_DIR_LEN {
            return PathBuf::from(e);
        }
    }
    layout
        .root
        .join("games")
        .join(crate::paths::game_dir_hash(game_dir))
}

/// Where a backed-up file goes: `<backup>\<name>`, with a numeric suffix if that exists.
fn backup_path(backup_dir: &Path, name: &str) -> PathBuf {
    let first = backup_dir.join(name);
    if !first.exists() {
        return first;
    }
    (1..)
        .map(|n| backup_dir.join(format!("{name}.{n}")))
        .find(|p| !p.exists())
        .expect("an unbounded range")
}

/// Rename, falling back to copy + delete (a different volume: `%LOCALAPPDATA%` is often on C: and
/// the game on D:). A directory is copied recursively.
fn move_path(from: &Path, to: &Path) -> Result<(), String> {
    if let Some(parent) = to.parent() {
        std::fs::create_dir_all(parent)
            .map_err(|e| format!("cannot create {}: {e}", parent.display()))?;
    }
    if std::fs::rename(from, to).is_ok() {
        return Ok(());
    }
    if from.is_dir() {
        copy_dir(from, to)?;
        std::fs::remove_dir_all(from)
            .map_err(|e| format!("cannot remove {} after copying it: {e}", from.display()))
    } else {
        std::fs::copy(from, to)
            .map_err(|e| format!("cannot copy {} -> {}: {e}", from.display(), to.display()))?;
        std::fs::remove_file(from)
            .map_err(|e| format!("cannot remove {} after copying it: {e}", from.display()))
    }
}

fn copy_dir(from: &Path, to: &Path) -> Result<(), String> {
    std::fs::create_dir_all(to).map_err(|e| format!("cannot create {}: {e}", to.display()))?;
    for entry in std::fs::read_dir(from)
        .map_err(|e| format!("cannot read {}: {e}", from.display()))?
        .flatten()
    {
        let src = entry.path();
        let dst = to.join(entry.file_name());
        if src.is_dir() {
            copy_dir(&src, &dst)?;
        } else {
            std::fs::copy(&src, &dst)
                .map_err(|e| format!("cannot copy {} -> {}: {e}", src.display(), dst.display()))?;
        }
    }
    Ok(())
}

/// The user-class rows that move: every ini row of the committed schema except the derived ones.
fn user_keys() -> Vec<(String, String)> {
    Schema::builtin()
        .rows
        .iter()
        .filter_map(|r| match &r.store {
            Store::Ini { section, key } => Some((section.clone(), key.clone())),
            Store::Launcher { .. } => None,
        })
        .filter(|(s, k)| !DERIVED.iter().any(|(ds, dk)| ds == s && dk == k))
        .collect()
}

/// The edits that carry the old ini's user-class values into the new one, FILL-ONLY: a key the new
/// ini already has is left as it is. Pure over the two byte strings.
pub fn ini_edits(old: &[u8], new: Option<&[u8]>) -> Vec<ini_io::Edit> {
    user_keys()
        .into_iter()
        .filter_map(|(section, key)| {
            let value = ini_io::read_value(old, &section, &key)?;
            if value.trim().is_empty() {
                return None;
            }
            if new
                .and_then(|n| ini_io::read_value(n, &section, &key))
                .is_some()
            {
                return None;
            }
            Some(ini_io::Edit::new(&section, &key, value.trim()))
        })
        .collect()
}

/// Do the migration for `game_dir`. The caller has checked the route; this re-checks eligibility,
/// because the elevated `--step migrate` child reaches here with nothing but the folder name.
pub fn run(layout: &Layout, game_dir: &Path) -> Result<Report, String> {
    run_with(
        layout,
        game_dir,
        cfgdir::env_config_dir().as_deref(),
        &chrono::Utc::now().format("%Y-%m-%d").to_string(),
    )
}

pub fn run_with(
    layout: &Layout,
    game_dir: &Path,
    env: Option<&str>,
    date: &str,
) -> Result<Report, String> {
    let receipt = eligible(game_dir).map_err(|e| format!("migration refused: {e}"))?;
    let found = survey(game_dir, &receipt);
    let mut report = Report::default();
    if found.is_empty() {
        log::line(format!("{} ({})", report.summary(), game_dir.display()));
        return Ok(report);
    }
    let config = target_dir(layout, game_dir, env);
    let backup_dir = config.join("migrated").join(date);
    log::line(format!(
        "migration: {} -> {} (found: {})",
        game_dir.display(),
        config.display(),
        found.describe()
    ));
    std::fs::create_dir_all(&config)
        .map_err(|e| format!("cannot create {}: {e}", config.display()))?;

    // 1. the ini: user-class values, fill-only, then the original goes to the backup.
    let old_ini = game_dir.join(INI_NAME);
    if found.ini {
        let old = std::fs::read(&old_ini)
            .map_err(|e| format!("cannot read {}: {e}", old_ini.display()))?;
        let new_path = config.join(INI_NAME);
        let new = ini_io::read_file(&new_path)?;
        let edits = ini_edits(&old, new.as_deref());
        for e in &edits {
            log::line(format!(
                "migration: {INI_NAME} [{}] {} = {}",
                e.section, e.key, e.value
            ));
            report.ini_keys.push(format!("{}.{}", e.section, e.key));
        }
        if !edits.is_empty() {
            ini_io::apply_file(&new_path, &edits)?;
        }
        log::line(format!(
            "migration: {INI_NAME}: {} user setting(s) carried over; the dev keys in it are dropped",
            edits.len()
        ));
    }

    // 2. the key: fill-only (an existing new key is the live one).
    let old_key = game_dir.join(KEY_NAME);
    if found.key {
        let new_key = config.join(KEY_NAME);
        if new_key.exists() {
            log::line(format!(
                "migration: {KEY_NAME}: the config directory already has one -- the old file is \
                 backed up only"
            ));
        } else {
            std::fs::copy(&old_key, &new_key).map_err(|e| {
                format!(
                    "cannot copy {} -> {}: {e}",
                    old_key.display(),
                    new_key.display()
                )
            })?;
            log::line(format!(
                "migration: {KEY_NAME} copied to {}",
                new_key.display()
            ));
            report.key_moved = true;
        }
    }

    // 3. logs: moved entry by entry; a name already there goes to the backup instead.
    if found.logs {
        let from = game_dir.join("logs");
        let to = config.join("logs");
        std::fs::create_dir_all(&to).map_err(|e| format!("cannot create {}: {e}", to.display()))?;
        for entry in std::fs::read_dir(&from)
            .map_err(|e| format!("cannot read {}: {e}", from.display()))?
            .flatten()
        {
            let name = entry.file_name().to_string_lossy().to_string();
            let dst = to.join(&name);
            if dst.exists() {
                let b = backup_path(&backup_dir.join("logs"), &name);
                move_path(&entry.path(), &b)?;
                log::line(format!(
                    "migration: logs\\{name} already exists in the new logs -- backed up to {}",
                    b.display()
                ));
                report.backed_up.push(format!("logs\\{name}"));
            } else {
                move_path(&entry.path(), &dst)?;
                report.logs_moved += 1;
            }
        }
        // The (now empty) directory goes too; a non-empty one (something raced us) stays.
        let _ = std::fs::remove_dir(&from);
        log::line(format!(
            "migration: logs\\ moved to {} ({} item(s))",
            to.display(),
            report.logs_moved
        ));
    }

    // 4. the clutter and the originals of the ini and key: backed up, then gone.
    let mut remove: Vec<String> = Vec::new();
    if found.ini {
        remove.push(INI_NAME.to_string());
    }
    if found.key {
        remove.push(KEY_NAME.to_string());
    }
    remove.extend(found.strays.iter().cloned());
    for name in &remove {
        let from = game_dir.join(name);
        let to = backup_path(&backup_dir, name);
        move_path(&from, &to)?;
        log::line(format!(
            "migration: {name} removed (backup {})",
            to.display()
        ));
        report.backed_up.push(name.clone());
    }
    if !remove.is_empty() || !report.backed_up.is_empty() {
        report.backup_dir = Some(backup_dir);
    }
    report.changed = true;
    log::line(format!("migration: {}", report.summary()));
    Ok(report)
}

/// The launcher's entry point (dist RL4): assess, route, and run -- directly or as an elevated
/// `--step migrate`. `Ok(None)` is "nothing happened" (not eligible, clean, or the game is
/// running); `Ok(Some(line))` is a status line for the log.
pub fn migrate_if_needed(
    layout: &Layout,
    game_dir: &Path,
    game_running: bool,
) -> Result<Option<String>, String> {
    let verdict = assess(game_dir);
    if let Verdict::Skip(why) = &verdict {
        log::line(format!("migration: skipped -- {why}"));
    }
    let writable = match &verdict {
        Verdict::Needed(_) => !elevate::needs_elevation(game_dir),
        _ => true,
    };
    match route(&verdict, writable, game_running) {
        Route::None => {
            if verdict == Verdict::Clean {
                log::line("migration: nothing to do -- the game folder is already clean");
            }
            Ok(None)
        }
        Route::Wait => {
            log::line("migration: waiting -- the game is running from this folder");
            Ok(None)
        }
        Route::Direct => run(layout, game_dir).map(|r| Some(r.summary())),
        Route::Elevated => {
            log::line("migration: the game folder needs administrator rights -- running that step elevated");
            elevate::run_step_elevated(layout, game_dir, &elevate::StepSpec::Migrate).map(Some)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const OLD_INI: &str = "; shipped\r\n\
        [config]\r\n\
        mode=brokered\r\n\
        [video]\r\n\
        window=borderless\r\n\
        vsync=1\r\n\
        fps_limit=0\r\n\
        [net]\r\n\
        enable=1\r\n\
        transport=udp\r\n\
        relay=192.0.2.10:7100\r\n\
        port=6600\r\n\
        d45_restore_boot=0\r\n\
        force_relay=1\r\n\
        [hud]\r\n\
        net_indicator_key=Ctrl+Alt+M\r\n\
        [log]\r\n\
        level=debug\r\n\
        sp_clock_log=1\r\n";

    fn scratch(name: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("mh_launcher_test_migrate_{name}"));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        d
    }

    /// A game folder as an rc7.1 install updated to 0.2.0 leaves it: binaries, the 0.2.0 receipt,
    /// retail data, a parked retail dll -- plus every legacy leftover.
    fn legacy_game(root: &Path, version: &str) -> PathBuf {
        let game = root.join("game");
        std::fs::create_dir_all(
            game.join("logs")
                .join("2026-10-01T10-00-00Z_abc12345_m1_mp"),
        )
        .unwrap();
        std::fs::create_dir_all(game.join("lang")).unwrap();
        std::fs::write(game.join("mh.exe"), b"exe").unwrap();
        std::fs::write(game.join("mh.dll"), b"our dll").unwrap();
        std::fs::write(game.join("mh.dll.mhbak"), b"retail dll").unwrap();
        std::fs::write(game.join("setup.dat"), b"retail setup").unwrap();
        std::fs::write(game.join("lang").join("pack.ini"), b"x").unwrap();
        std::fs::write(game.join(INI_NAME), OLD_INI).unwrap();
        std::fs::write(game.join(KEY_NAME), b"aa".repeat(32)).unwrap();
        std::fs::write(
            game.join("logs")
                .join("2026-10-01T10-00-00Z_abc12345_m1_mp")
                .join("mh.log"),
            b"a session log",
        )
        .unwrap();
        for f in [
            "README.txt",
            "LICENSE",
            "THIRD_PARTY.md",
            "mh_run.txt",
            "mh_harness.dll",
            "libmh.dll",
            "mh_config_refused.log",
        ] {
            std::fs::write(game.join(f), format!("legacy {f}")).unwrap();
        }
        std::fs::write(
            game.join(crate::paths::INSTALL_MANIFEST),
            format!(
                "version\t{version}\ntag\tnet\npackage\tp.zip\ninstalled_at\tx\n\
                 ours\t{}\tmh.dll\n",
                install::sha256_file(&game.join("mh.dll")).unwrap()
            ),
        )
        .unwrap();
        game
    }

    fn run_in(root: &Path, game: &Path) -> Report {
        let layout = Layout::rooted(root.join("state"));
        run_with(&layout, game, None, "2026-10-08").unwrap()
    }

    #[test]
    fn only_user_class_values_are_carried_and_dev_keys_and_the_relay_are_dropped() {
        let edits = ini_edits(OLD_INI.as_bytes(), None);
        let got: Vec<String> = edits
            .iter()
            .map(|e| format!("{}.{}={}", e.section, e.key, e.value))
            .collect();
        for want in [
            "video.window=borderless",
            "video.vsync=1",
            "video.fps_limit=0",
            "net.port=6600",
            "net.force_relay=1",
            "hud.net_indicator_key=Ctrl+Alt+M",
            "log.level=debug",
        ] {
            assert!(
                got.contains(&want.to_string()),
                "{want} missing from {got:?}"
            );
        }
        for dropped in [
            "config.mode",
            "net.enable",
            "net.transport",
            "net.relay",
            "net.d45_restore_boot",
            "log.sp_clock_log",
        ] {
            assert!(
                !got.iter().any(|g| g.starts_with(&format!("{dropped}="))),
                "{dropped} must be dropped: {got:?}"
            );
        }
    }

    #[test]
    fn a_key_the_new_ini_already_has_is_not_overwritten() {
        let new = b"[video]\nwindow=windowed\n";
        let edits = ini_edits(OLD_INI.as_bytes(), Some(new));
        assert!(!edits.iter().any(|e| e.key == "window"));
        assert!(edits.iter().any(|e| e.key == "vsync"));
    }

    /// done_when: after the migration the game folder holds only binaries, lang\, the receipt and
    /// retail files; settings, key and logs are in user storage; the backup exists.
    #[test]
    fn a_legacy_folder_is_migrated_and_a_second_run_changes_nothing() {
        let root = scratch("full");
        let game = legacy_game(&root, "0.2.0");
        let layout = Layout::rooted(root.join("state"));
        let cfg = target_dir(&layout, &game, None);

        let verdict = assess(&game);
        let Verdict::Needed(found) = &verdict else {
            panic!("{verdict:?}")
        };
        assert!(found.ini && found.key && found.logs);
        assert_eq!(found.strays.len(), 7, "{found:?}");

        let r = run_in(&root, &game);
        assert!(r.changed);
        assert!(r.key_moved);
        assert_eq!(r.logs_moved, 1);

        // the game folder is clean
        let mut left: Vec<String> = std::fs::read_dir(&game)
            .unwrap()
            .flatten()
            .map(|e| e.file_name().to_string_lossy().to_string())
            .collect();
        left.sort();
        assert_eq!(
            left,
            vec![
                "lang".to_string(),
                "mh.dll".to_string(),
                "mh.dll.mhbak".to_string(),
                "mh.exe".to_string(),
                "mh_launcher_installed.txt".to_string(),
                "setup.dat".to_string(),
            ]
        );
        // the user's things arrived
        let ini = std::fs::read_to_string(cfg.join(INI_NAME)).unwrap();
        assert!(
            ini.contains("window=borderless") && ini.contains("level=debug"),
            "{ini}"
        );
        assert!(
            !ini.contains("d45_restore_boot") && !ini.contains("sp_clock_log"),
            "{ini}"
        );
        assert!(!ini.contains("\nrelay="), "{ini}");
        assert_eq!(std::fs::read(cfg.join(KEY_NAME)).unwrap(), b"aa".repeat(32));
        assert_eq!(
            std::fs::read(cfg.join("logs/2026-10-01T10-00-00Z_abc12345_m1_mp/mh.log")).unwrap(),
            b"a session log"
        );
        // the backup exists and holds what was removed
        let backup = cfg.join("migrated").join("2026-10-08");
        for f in [
            INI_NAME,
            KEY_NAME,
            "README.txt",
            "LICENSE",
            "THIRD_PARTY.md",
            "mh_run.txt",
            "mh_harness.dll",
            "libmh.dll",
            "mh_config_refused.log",
        ] {
            assert!(backup.join(f).is_file(), "{f} not backed up");
        }
        assert_eq!(
            std::fs::read_to_string(backup.join(INI_NAME)).unwrap(),
            OLD_INI
        );

        // a second run changes nothing
        let before = std::fs::read(cfg.join(INI_NAME)).unwrap();
        assert_eq!(assess(&game), Verdict::Clean);
        let again = run_in(&root, &game);
        assert!(!again.changed);
        assert!(again.summary().contains("nothing to do"));
        assert_eq!(std::fs::read(cfg.join(INI_NAME)).unwrap(), before);
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn nothing_the_receipt_lists_is_removed() {
        let root = scratch("receipted");
        let game = legacy_game(&root, "0.2.0");
        // a net-debug-style set: the receipt lists mh_harness.dll and LICENSE
        let receipt = game.join(crate::paths::INSTALL_MANIFEST);
        let mut text = std::fs::read_to_string(&receipt).unwrap();
        for f in ["mh_harness.dll", "LICENSE"] {
            text.push_str(&format!(
                "created\t{}\t{f}\n",
                install::sha256_file(&game.join(f)).unwrap()
            ));
        }
        std::fs::write(&receipt, text).unwrap();
        run_in(&root, &game);
        assert!(game.join("mh_harness.dll").is_file());
        assert!(game.join("LICENSE").is_file());
        assert!(!game.join("libmh.dll").exists());
        assert!(!game.join("README.txt").exists());
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn user_storage_starts_at_rc8_and_orders_candidates_numerically() {
        for v in [
            "0.2.0-rc8",
            "0.2.0-rc9",
            "0.2.0-rc10",
            "0.2.0-rc12.1",
            "0.2.0",
            "0.2.1",
            "0.3.0-rc1",
            "1.0.0",
        ] {
            assert!(reads_user_storage(v), "{v}");
        }
        for v in [
            "0.2.0-rc7",
            "0.2.0-rc7.1",
            "0.2.0-rc1",
            "0.2.0-beta",
            "0.1.9",
            "0.2.0-x",
            "garbage",
            "",
        ] {
            assert!(!reads_user_storage(v), "{v}");
        }
    }

    /// A release candidate of 0.2.0 (rc8 and later) migrates, and a second run finds it clean.
    #[test]
    fn a_release_candidate_game_migrates_once() {
        let root = scratch("rc8");
        let game = legacy_game(&root, "0.2.0-rc8");
        assert!(matches!(assess(&game), Verdict::Needed(_)));
        let layout = Layout::rooted(root.join("state"));
        assert!(run_with(&layout, &game, None, "d").is_ok());
        assert!(!game.join(INI_NAME).exists() && !game.join("logs").exists());
        assert!(matches!(assess(&game), Verdict::Clean));
        let _ = std::fs::remove_dir_all(&root);
    }

    /// An older game still reads its files from the folder; a folder without a receipt is not ours.
    #[test]
    fn an_old_game_or_an_unreceipted_folder_is_never_touched() {
        let root = scratch("ineligible");
        let old = legacy_game(&root, "0.2.0-rc7.1");
        assert!(matches!(assess(&old), Verdict::Skip(_)));
        assert!(run_with(&Layout::rooted(root.join("state")), &old, None, "d").is_err());
        assert!(old.join(INI_NAME).is_file() && old.join("logs").is_dir());

        std::fs::remove_file(old.join(crate::paths::INSTALL_MANIFEST)).unwrap();
        assert!(matches!(assess(&old), Verdict::Skip(_)));
        assert!(old.join(INI_NAME).is_file());
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn existing_new_settings_and_key_win_over_the_old_ones() {
        let root = scratch("fillonly");
        let game = legacy_game(&root, "0.2.0");
        let layout = Layout::rooted(root.join("state"));
        let cfg = target_dir(&layout, &game, None);
        std::fs::create_dir_all(&cfg).unwrap();
        std::fs::write(cfg.join(INI_NAME), "[video]\nwindow=windowed\n").unwrap();
        std::fs::write(cfg.join(KEY_NAME), "open\n").unwrap();
        let r = run_in(&root, &game);
        assert!(!r.key_moved);
        let ini = std::fs::read_to_string(cfg.join(INI_NAME)).unwrap();
        assert!(ini.contains("window=windowed"), "{ini}");
        assert!(!ini.contains("window=borderless"), "{ini}");
        assert!(
            ini.contains("vsync=1"),
            "the missing keys were filled: {ini}"
        );
        assert_eq!(
            std::fs::read_to_string(cfg.join(KEY_NAME)).unwrap(),
            "open\n"
        );
        assert_eq!(
            std::fs::read(cfg.join("migrated/2026-10-08").join(KEY_NAME)).unwrap(),
            b"aa".repeat(32),
            "the old key is still backed up"
        );
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn a_log_name_that_already_exists_goes_to_the_backup_not_over_it() {
        let root = scratch("logclash");
        let game = legacy_game(&root, "0.2.0");
        let layout = Layout::rooted(root.join("state"));
        let cfg = target_dir(&layout, &game, None);
        let name = "2026-10-01T10-00-00Z_abc12345_m1_mp";
        std::fs::create_dir_all(cfg.join("logs").join(name)).unwrap();
        std::fs::write(cfg.join("logs").join(name).join("mh.log"), b"the live one").unwrap();
        let r = run_in(&root, &game);
        assert_eq!(r.logs_moved, 0);
        assert_eq!(
            std::fs::read(cfg.join("logs").join(name).join("mh.log")).unwrap(),
            b"the live one"
        );
        assert_eq!(
            std::fs::read(
                cfg.join("migrated/2026-10-08/logs")
                    .join(name)
                    .join("mh.log")
            )
            .unwrap(),
            b"a session log"
        );
        assert!(!game.join("logs").exists());
        let _ = std::fs::remove_dir_all(&root);
    }

    /// The elevation DECISION (Program Files cannot be a test fixture): elevate only when there is
    /// work AND the folder is not writable; wait while the game runs; do nothing when clean.
    #[test]
    fn elevation_is_chosen_only_when_there_is_work_and_the_folder_is_not_writable() {
        let needed = Verdict::Needed(Survey {
            ini: true,
            ..Default::default()
        });
        assert_eq!(route(&needed, true, false), Route::Direct);
        assert_eq!(route(&needed, false, false), Route::Elevated);
        assert_eq!(route(&needed, false, true), Route::Wait);
        assert_eq!(route(&needed, true, true), Route::Wait);
        assert_eq!(route(&Verdict::Clean, false, false), Route::None);
        assert_eq!(route(&Verdict::Skip("x".into()), false, false), Route::None);
    }

    #[test]
    fn the_target_is_the_env_override_or_the_user_storage_hash_dir() {
        let layout = Layout::rooted(r"C:\state");
        let game = Path::new(r"C:\Games\MH");
        assert_eq!(
            target_dir(&layout, game, Some(r"C:\cfg")),
            PathBuf::from(r"C:\cfg")
        );
        assert_eq!(
            target_dir(&layout, game, Some("  ")),
            Path::new(r"C:\state")
                .join("games")
                .join(crate::paths::game_dir_hash(game))
        );
    }
}
