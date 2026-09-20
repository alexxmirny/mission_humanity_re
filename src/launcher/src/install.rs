//! Installing a release zip: unpack it into `versions\<ver>\`, then put its files next to `mh.exe`.
//!
//! WHAT THIS AUTOMATES IS ONE PARAGRAPH OF INSTALL.md, and nothing more:
//!
//! > Installing one is unzipping it. Put every file next to the game executable, at that level --
//! > not in a subfolder.
//!
//! So the copy is flat, the destination is the game directory itself, and there is no registry key,
//! no service, no shortcut and no elevation. What the launcher adds over doing it by hand is a
//! record of what it did: `mh_launcher_installed.txt` in the game directory, holding one row per
//! file with its SHA-256 and whether it displaced something.
//!
//! THE FILE THAT MAKES THAT RECORD NECESSARY IS `mh.dll`. The retail game ships its own `mh.dll`
//! (it exports `DecompressLZWData`, `GetSightAreaFromRadius`, `MH_HostedPoolBase`) and this
//! project's `mh.dll` is a drop-in that re-exports those three and adds its own. So installing a
//! release OVERWRITES a game file -- the only one it does -- and an uninstall that merely deleted
//! what it copied would leave the game unable to start. Every displaced file is therefore parked as
//! `<name>.mhbak` first and put back on uninstall.
//!
//! And the digests are what stop the uninstall from being destructive in the other direction: a
//! file whose content no longer matches its row was replaced by someone else since, so it is left
//! alone and named in the result rather than deleted.

use crate::log;
use crate::paths::{
    Layout, BACKUP_SUFFIX, FIRST_RUN_MARKER, GAME_EXE, INSTALL_MANIFEST, VERSION_STAGING_SUFFIX,
};
use crate::relay::{self, Relay};
use sha2::{Digest, Sha256};
use std::io::Read;
use std::path::{Component, Path, PathBuf};

/// What `tools/release_package.py` calls every zip it writes.
const ZIP_PREFIX: &str = "mission_humanity_re-";

/// The three configurations that tool packages. A zip whose tag is not one of these is not one of
/// ours, and the version/tag split of `<version>-<tag>` cannot be guessed without the list: the
/// version itself may carry a `-` (`0.1.0-rc1`, `0.0.0-dev`), so splitting on the first or the last
/// hyphen is wrong in one direction or the other. Matching the KNOWN SUFFIX is what works.
const TAGS: [&str; 3] = ["net-debug", "brokered-debug", "net"];

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PackageName {
    pub version: String,
    pub tag: String,
}

/// Read `mission_humanity_re-<version>-<tag>.zip` apart.
///
/// `net-debug` is tried before `net` on purpose: a shorter tag tested first would strip only
/// `-debug` off the longer one and hand back a version of `0.1.0-net`.
pub fn parse_package_name(file_name: &str) -> Result<PackageName, String> {
    let stem = file_name
        .strip_suffix(".zip")
        .ok_or_else(|| format!("{file_name} is not a .zip"))?;
    let rest = stem.strip_prefix(ZIP_PREFIX).ok_or_else(|| {
        format!("{file_name} is not a Mission Humanity release zip (expected {ZIP_PREFIX}<version>-<tag>.zip)")
    })?;
    for tag in TAGS {
        if let Some(version) = rest.strip_suffix(&format!("-{tag}")) {
            if version.is_empty() {
                break;
            }
            return Ok(PackageName {
                version: version.to_string(),
                tag: tag.to_string(),
            });
        }
    }
    Err(format!(
        "{file_name} does not name one of the three release configurations ({})",
        TAGS.join(", ")
    ))
}

#[derive(Clone, Debug, Default)]
pub struct InstallReport {
    pub version: String,
    pub tag: String,
    pub package: String,
    pub game_dir: PathBuf,
    /// Files created in the game directory that were not there before.
    pub created: Vec<String>,
    /// Files that displaced an existing one, which is now `<name>.mhbak`.
    pub replaced: Vec<String>,
}

impl InstallReport {
    pub fn summary(&self) -> String {
        format!(
            "installed {} ({}) from {} into {} -- {} file(s) new, {} replaced (backed up as *{})",
            self.version,
            self.tag,
            self.package,
            self.game_dir.display(),
            self.created.len(),
            self.replaced.len(),
            BACKUP_SUFFIX
        )
    }
}

/// The canonical zip name for a version and a configuration.
///
/// The inverse of `parse_package_name`, and it exists because dist LA2 downloads a zip from a URL
/// rather than being handed a file: the name a release ASSET happens to have is somebody else's
/// string, while the name a version set is recorded under has to be this project's. Constructing it
/// means a manifest cannot rename a package by renaming a URL.
pub fn package_file_name(version: &str, tag: &str) -> String {
    format!("{ZIP_PREFIX}{version}-{tag}.zip")
}

/// Unzip `zip_path` into `versions\<ver>.staging\` and RENAME it to `versions\<ver>\`.
///
/// The rename is the point (dist LA2). Unpacking straight into the final directory leaves a window
/// in which it holds part of one release and part of another, and the machine that finds that
/// window is the one that lost power mid-update. A rename either happened or did not.
pub fn stage_zip(layout: &Layout, zip_path: &Path, version: &str) -> Result<PathBuf, String> {
    let staging = layout
        .versions()
        .join(format!("{version}{VERSION_STAGING_SUFFIX}"));
    if staging.exists() {
        std::fs::remove_dir_all(&staging)
            .map_err(|e| format!("cannot clear {}: {e}", staging.display()))?;
    }
    let files = extract(zip_path, &staging)?;
    let final_dir = layout.version_dir(version);
    if final_dir.exists() {
        std::fs::remove_dir_all(&final_dir)
            .map_err(|e| format!("cannot replace {}: {e}", final_dir.display()))?;
    }
    std::fs::rename(&staging, &final_dir).map_err(|e| {
        format!(
            "cannot move {} into place as {}: {e}",
            staging.display(),
            final_dir.display()
        )
    })?;
    log::line(format!(
        "install: staged {} entr(ies) and swapped them in as {}",
        files.len(),
        final_dir.display()
    ));
    Ok(final_dir)
}

/// Copy a staged version set beside `mh.exe`, backing up anything it displaces and writing the
/// receipt. The half of an install that does not care where the files came from -- a zip the player
/// chose (LA1) or a verified download (LA2).
///
/// `relay` (dist LA6): the signed manifest's relay, if any. The release zip carries an `mh_net.ini`
/// (the example ini verbatim -- `transport=udp` since 2026-09-20, so the relay edit below only
/// changes that line when a hand-edited ini says otherwise), so the copy is where the player's ini comes from --
/// and it is provisioned HERE, before its digest goes into the receipt, so the row records the file
/// as the launcher actually left it and an uninstall still recognises it as its own. `None` copies
/// the ini as shipped and writes no key file.
pub fn install_from_version_dir(
    version_dir: &Path,
    pkg: &PackageName,
    package: &str,
    game_dir: &Path,
    relay: Option<&Relay>,
) -> Result<InstallReport, String> {
    if !game_dir.join(GAME_EXE).is_file() {
        return Err(format!(
            "{} is not a game directory: no {GAME_EXE} in it",
            game_dir.display()
        ));
    }
    let mut files: Vec<String> = Vec::new();
    for entry in std::fs::read_dir(version_dir)
        .map_err(|e| format!("cannot read {}: {e}", version_dir.display()))?
        .flatten()
    {
        if !entry.file_type().map(|t| t.is_file()).unwrap_or(false) {
            continue;
        }
        let name = entry.file_name().to_string_lossy().to_string();
        // The launcher's own bookkeeping is not part of the release set and must never be copied
        // into somebody's game directory.
        if name == FIRST_RUN_MARKER {
            continue;
        }
        files.push(name);
    }
    files.sort();
    if files.is_empty() {
        return Err(format!("{} holds no files", version_dir.display()));
    }
    log::line(format!(
        "install: {} -> {} ({} file(s))",
        version_dir.display(),
        game_dir.display(),
        files.len()
    ));

    let staged = version_dir.to_path_buf();
    let name = package.to_string();
    let pkg = pkg.clone();
    let mut report = InstallReport {
        version: pkg.version.clone(),
        tag: pkg.tag.clone(),
        package: name.clone(),
        game_dir: game_dir.to_path_buf(),
        ..Default::default()
    };
    let mut rows = Vec::new();
    for file in &files {
        let src = staged.join(file);
        let dst = game_dir.join(file);
        let existed = dst.exists();
        if existed {
            // ONLY THE FIRST INSTALL MAKES A BACKUP. A second install would otherwise park OUR
            // previous file over the pristine game one and the original would be gone for good.
            let backup = game_dir.join(format!("{file}{BACKUP_SUFFIX}"));
            if !backup.exists() {
                std::fs::rename(&dst, &backup).map_err(|e| {
                    format!(
                        "cannot move {} aside to {}: {e}",
                        dst.display(),
                        backup.display()
                    )
                })?;
                log::line(format!(
                    "install: {file} displaced -> {file}{BACKUP_SUFFIX}"
                ));
            }
        }
        std::fs::copy(&src, &dst)
            .map_err(|e| format!("cannot copy {} -> {}: {e}", src.display(), dst.display()))?;
        if file == relay::INI_NAME {
            if let Some(r) = relay {
                let change = relay::provision_ini_file(&dst, &r.addr)?;
                log::line(format!(
                    "install: {file} {} with the manifest's relay ([net] transport=udp, relay=...)                      before its digest is recorded",
                    match change {
                        relay::Change::Unchanged => "already carried",
                        _ => "provisioned",
                    }
                ));
            }
        }
        let digest = sha256_file(&dst)?;
        if existed {
            report.replaced.push(file.clone());
            rows.push(format!("replaced\t{digest}\t{file}"));
        } else {
            report.created.push(file.clone());
            rows.push(format!("created\t{digest}\t{file}"));
        }
    }
    write_manifest(game_dir, &pkg, &name, &rows)?;
    // The key file, and -- for a zip that somehow shipped no ini -- the ini itself. Neither goes in
    // the receipt: mh_key.txt is a file the GAME owns (it mints one on first run when there is
    // none), and an uninstall that deleted the key would cut the player off from every relay game
    // they had been playing.
    relay::provision(game_dir, relay)?;
    log::line(format!("install: {}", report.summary()));
    Ok(report)
}

/// Install a release zip the player already has: stage it, swap it in, copy it beside `mh.exe`.
///
/// The LA1 entry point, now expressed as the two halves LA2 also uses -- so a zip chosen in the
/// file dialog and a zip downloaded from a signed manifest reach the game directory through exactly
/// the same code, and a bug in the copy cannot be fixed on one path only.
pub fn install(
    layout: &Layout,
    zip_path: &Path,
    game_dir: &Path,
    relay: Option<&Relay>,
) -> Result<InstallReport, String> {
    let name = zip_path
        .file_name()
        .and_then(|s| s.to_str())
        .ok_or_else(|| format!("{} has no usable file name", zip_path.display()))?
        .to_string();
    let pkg = parse_package_name(&name)?;
    if !game_dir.join(GAME_EXE).is_file() {
        return Err(format!(
            "{} is not a game directory: no {GAME_EXE} in it",
            game_dir.display()
        ));
    }
    let version_dir = stage_zip(layout, zip_path, &pkg.version)?;
    install_from_version_dir(&version_dir, &pkg, &name, game_dir, relay)
}

/// Extract a zip into `dest`, returning the entry names in zip order.
///
/// ZIP-SLIP IS REFUSED, not sanitised. Every entry must be a plain relative file name -- the three
/// release zips are flat by construction (`tools/release_package.py` asserts their exact entry
/// list), so an entry with a directory component, a `..`, a drive letter or a root is not a shape
/// this launcher has to support; it is an archive that is not ours, and the honest answer is to
/// stop rather than to quietly write a "cleaned" version of somebody else's path somewhere.
fn extract(zip_path: &Path, dest: &Path) -> Result<Vec<String>, String> {
    let file = std::fs::File::open(zip_path)
        .map_err(|e| format!("cannot open {}: {e}", zip_path.display()))?;
    let mut archive = zip::ZipArchive::new(file)
        .map_err(|e| format!("{} is not a readable zip: {e}", zip_path.display()))?;
    std::fs::create_dir_all(dest).map_err(|e| format!("cannot create {}: {e}", dest.display()))?;

    let mut names = Vec::new();
    for i in 0..archive.len() {
        let mut entry = archive
            .by_index(i)
            .map_err(|e| format!("cannot read entry {i} of {}: {e}", zip_path.display()))?;
        let raw = entry.name().to_string();
        if entry.is_dir() {
            return Err(format!(
                "{} holds a directory entry ({raw}); the release zips are flat",
                zip_path.display()
            ));
        }
        let candidate = Path::new(&raw);
        let flat = candidate.components().count() == 1
            && matches!(candidate.components().next(), Some(Component::Normal(_)))
            && !raw.contains('\\');
        if !flat {
            return Err(format!(
                "refusing {}: entry {raw:?} is not a plain file name",
                zip_path.display()
            ));
        }
        let out = dest.join(&raw);
        let mut buf = Vec::with_capacity(entry.size() as usize);
        entry
            .read_to_end(&mut buf)
            .map_err(|e| format!("cannot decompress {raw}: {e}"))?;
        std::fs::write(&out, &buf).map_err(|e| format!("cannot write {}: {e}", out.display()))?;
        names.push(raw);
    }
    if names.is_empty() {
        return Err(format!("{} is empty", zip_path.display()));
    }
    Ok(names)
}

fn write_manifest(
    game_dir: &Path,
    pkg: &PackageName,
    package: &str,
    rows: &[String],
) -> Result<(), String> {
    let path = game_dir.join(INSTALL_MANIFEST);
    let mut text = String::new();
    text.push_str("# Written by mh_launcher (dist LA1). Do not edit by hand.\n");
    text.push_str(
        "# Uninstalling reads this to remove exactly what the launcher added and to put\n",
    );
    text.push_str("# back every game file it displaced (those are the *.mhbak files beside it).\n");
    text.push_str(&format!("version\t{}\n", pkg.version));
    text.push_str(&format!("tag\t{}\n", pkg.tag));
    text.push_str(&format!("package\t{package}\n"));
    text.push_str(&format!("installed_at\t{}\n", log::stamp()));
    text.push_str("# <created|replaced>\t<sha256 as installed>\t<file>\n");
    for row in rows {
        text.push_str(row);
        text.push('\n');
    }
    std::fs::write(&path, text).map_err(|e| format!("cannot write {}: {e}", path.display()))
}

#[derive(Clone, Debug, Default)]
pub struct Manifest {
    pub version: String,
    pub tag: String,
    pub package: String,
    pub installed_at: String,
    /// `(disposition, sha256, file name)`.
    pub files: Vec<(String, String, String)>,
}

/// Read the install manifest out of a game directory, if the launcher put one there.
pub fn read_manifest(game_dir: &Path) -> Option<Manifest> {
    let text = std::fs::read_to_string(game_dir.join(INSTALL_MANIFEST)).ok()?;
    let mut m = Manifest::default();
    for raw in text.lines() {
        if raw.starts_with('#') || raw.trim().is_empty() {
            continue;
        }
        let parts: Vec<&str> = raw.split('\t').collect();
        match parts.as_slice() {
            ["version", v] => m.version = (*v).to_string(),
            ["tag", v] => m.tag = (*v).to_string(),
            ["package", v] => m.package = (*v).to_string(),
            ["installed_at", v] => m.installed_at = (*v).to_string(),
            [disposition, sha, file] => m.files.push((
                (*disposition).to_string(),
                (*sha).to_string(),
                (*file).to_string(),
            )),
            _ => {}
        }
    }
    Some(m)
}

#[derive(Clone, Debug, Default)]
pub struct UninstallReport {
    pub removed: Vec<String>,
    pub restored: Vec<String>,
    /// Files left alone because their content no longer matches what was installed.
    pub kept: Vec<String>,
}

impl UninstallReport {
    pub fn summary(&self) -> String {
        format!(
            "uninstalled -- {} removed, {} game file(s) restored, {} left alone (changed since install)",
            self.removed.len(),
            self.restored.len(),
            self.kept.len()
        )
    }
}

/// Undo an install, using the manifest as the authority on what to touch.
pub fn uninstall(game_dir: &Path) -> Result<UninstallReport, String> {
    let manifest = read_manifest(game_dir).ok_or_else(|| {
        format!(
            "no {INSTALL_MANIFEST} in {} -- nothing here was installed by this launcher",
            game_dir.display()
        )
    })?;
    let mut report = UninstallReport::default();
    for (disposition, sha, file) in &manifest.files {
        let path = game_dir.join(file);
        let backup = game_dir.join(format!("{file}{BACKUP_SUFFIX}"));
        if path.exists() && sha256_file(&path).ok().as_deref() != Some(sha.as_str()) {
            report.kept.push(file.clone());
            continue;
        }
        if path.exists() {
            std::fs::remove_file(&path)
                .map_err(|e| format!("cannot remove {}: {e}", path.display()))?;
            report.removed.push(file.clone());
        }
        if disposition == "replaced" && backup.exists() {
            std::fs::rename(&backup, &path)
                .map_err(|e| format!("cannot restore {}: {e}", path.display()))?;
            report.restored.push(file.clone());
        }
    }
    let manifest_path = game_dir.join(INSTALL_MANIFEST);
    std::fs::remove_file(&manifest_path)
        .map_err(|e| format!("cannot remove {}: {e}", manifest_path.display()))?;
    log::line(format!("uninstall: {}", report.summary()));
    Ok(report)
}

fn sha256_file(path: &Path) -> Result<String, String> {
    let bytes = std::fs::read(path).map_err(|e| format!("cannot hash {}: {e}", path.display()))?;
    let mut hasher = Sha256::new();
    hasher.update(&bytes);
    Ok(hasher
        .finalize()
        .iter()
        .map(|b| format!("{b:02x}"))
        .collect())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_three_release_names_split_correctly() {
        for (name, version, tag) in [
            ("mission_humanity_re-0.1.0-net.zip", "0.1.0", "net"),
            (
                "mission_humanity_re-0.1.0-net-debug.zip",
                "0.1.0",
                "net-debug",
            ),
            (
                "mission_humanity_re-0.1.0-brokered-debug.zip",
                "0.1.0",
                "brokered-debug",
            ),
        ] {
            let p = parse_package_name(name).expect(name);
            assert_eq!(
                (p.version.as_str(), p.tag.as_str()),
                (version, tag),
                "{name}"
            );
        }
    }

    /// The case that makes the KNOWN-SUFFIX rule necessary rather than merely tidy: the version
    /// itself contains hyphens, so neither `split('-').next()` nor `rsplit` gets these right.
    #[test]
    fn a_hyphenated_version_survives() {
        let p = parse_package_name("mission_humanity_re-0.1.0-rc1-net-debug.zip").unwrap();
        assert_eq!(p.version, "0.1.0-rc1");
        assert_eq!(p.tag, "net-debug");
        let p = parse_package_name("mission_humanity_re-0.0.0-dev-net.zip").unwrap();
        assert_eq!(p.version, "0.0.0-dev");
        assert_eq!(p.tag, "net");
    }

    #[test]
    fn foreign_archives_are_refused_by_name() {
        for bad in [
            "some_other_game-1.0-net.zip",
            "mission_humanity_re-0.1.0-server.zip",
            "mission_humanity_re-net.zip",
            "mission_humanity_re-0.1.0-net.7z",
        ] {
            assert!(parse_package_name(bad).is_err(), "{bad} should be refused");
        }
    }

    #[test]
    fn a_zip_with_a_path_entry_is_refused() {
        let dir = std::env::temp_dir().join("mh_launcher_test_zipslip");
        std::fs::create_dir_all(&dir).unwrap();
        let zip_path = dir.join("mission_humanity_re-0.1.0-net.zip");
        {
            let f = std::fs::File::create(&zip_path).unwrap();
            let mut w = zip::ZipWriter::new(f);
            let opts: zip::write::SimpleFileOptions = zip::write::SimpleFileOptions::default()
                .compression_method(zip::CompressionMethod::Deflated);
            w.start_file("../escaped.dll", opts).unwrap();
            std::io::Write::write_all(&mut w, b"nope").unwrap();
            w.finish().unwrap();
        }
        let err = extract(&zip_path, &dir.join("stage")).unwrap_err();
        assert!(err.contains("not a plain file name"), "{err}");
        std::fs::remove_dir_all(&dir).ok();
    }

    /// A release zip as the packager writes it (flat: a dll and the example ini), installed into a
    /// scratch game directory. Exercised both ways so the receipt's mh_net.ini row can be checked
    /// against the file the launcher actually left.
    fn fake_release(dir: &Path) -> PathBuf {
        let zip_path = dir.join("mission_humanity_re-0.1.0-net.zip");
        let f = std::fs::File::create(&zip_path).unwrap();
        let mut w = zip::ZipWriter::new(f);
        let opts: zip::write::SimpleFileOptions = zip::write::SimpleFileOptions::default()
            .compression_method(zip::CompressionMethod::Deflated);
        w.start_file("mh.dll", opts).unwrap();
        std::io::Write::write_all(&mut w, b"our dll").unwrap();
        w.start_file(relay::INI_NAME, opts).unwrap();
        std::io::Write::write_all(&mut w, SHIPPED_INI.as_bytes()).unwrap();
        w.finish().unwrap();
        zip_path
    }

    const SHIPPED_INI: &str = "; shipped
[net]
module=auto
transport=tcp
; relay=HOST:PORT
";

    /// dist LA6 done_when, on the install path: with a relay the copied ini gains exactly the two
    /// keys and the receipt digests THAT file; without one the ini is the zip's, byte for byte,
    /// and no key file exists.
    #[test]
    fn an_install_provisions_the_relay_only_when_the_manifest_has_one() {
        let root = std::env::temp_dir().join("mh_launcher_test_install_relay");
        let _ = std::fs::remove_dir_all(&root);
        std::fs::create_dir_all(&root).unwrap();
        let layout = Layout::rooted(root.join("state"));
        let zip_path = fake_release(&root);
        let relay = Relay {
            addr: "192.0.2.10:7100".into(),
            key: "ab".repeat(32),
        };

        for (name, with) in [("plain", false), ("relayed", true)] {
            let game = root.join(name);
            std::fs::create_dir_all(&game).unwrap();
            std::fs::write(game.join(GAME_EXE), b"exe").unwrap();
            std::fs::write(game.join("mh.dll"), b"the game's own dll").unwrap();
            let r = if with { Some(&relay) } else { None };
            let report = install(&layout, &zip_path, &game, r).unwrap();
            assert_eq!(report.version, "0.1.0");

            let ini = std::fs::read(game.join(relay::INI_NAME)).unwrap();
            if with {
                assert_eq!(
                    ini,
                    SHIPPED_INI
                        .replace("transport=tcp", "transport=udp")
                        .replace(
                            "[net]
",
                            "[net]
relay=192.0.2.10:7100
"
                        )
                        .as_bytes()
                );
                assert!(game.join(relay::KEY_NAME).is_file());
            } else {
                assert_eq!(ini, SHIPPED_INI.as_bytes(), "the zip's ini, untouched");
                assert!(!game.join(relay::KEY_NAME).exists());
            }
            // The receipt names the ini as it was LEFT, so uninstall recognises it.
            let receipt = read_manifest(&game).expect("a receipt was written");
            let (_, sha, _) = receipt
                .files
                .iter()
                .find(|(_, _, f)| f == relay::INI_NAME)
                .expect("the ini has a row");
            assert_eq!(*sha, sha256_file(&game.join(relay::INI_NAME)).unwrap());
            assert!(!receipt.files.iter().any(|(_, _, f)| f == relay::KEY_NAME));
            assert!(game.join(format!("mh.dll{BACKUP_SUFFIX}")).is_file());

            let un = uninstall(&game).unwrap();
            assert!(un.summary().contains("removed"), "{}", un.summary());
            assert!(
                !game.join(relay::INI_NAME).exists(),
                "the provisioned ini is ours to remove"
            );
            assert_eq!(
                std::fs::read(game.join("mh.dll")).unwrap(),
                b"the game's own dll"
            );
        }
        let _ = std::fs::remove_dir_all(&root);
    }
}
