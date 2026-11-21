//! Mod discovery for the launcher's Mods page (DATAEXT1).
//!
//! The launcher's half of the activation contract: it reads `<game>\mods\<id>\` folders and the
//! `[mods] enabled=a,b,c` list in the game's `mh_net.ini`, and it writes that ONE key back through
//! the byte-preserving ini editor (`settings::ini_io`). `mh.dll` stays the only authority on what
//! actually mounts at boot: it re-reads the same ini line and re-validates every folder when the
//! game starts. This module is metadata for the page -- names, versions, reasons a folder cannot
//! mount -- never a second loader, and it computes no multiplayer digest (the DLL owns that).
//!
//! The folder contract (frozen with the conductor, 2026-11-01):
//!
//! * a mod is a DIRECTORY under `<game>\mods\`; its name is its id: ASCII `a-z`, `0-9`, `-`, `_`,
//!   1..=32 characters, lowercase; anything else is shown as an invalid row, never hidden;
//! * `mod.ini` must be valid UTF-8 (a BOM is accepted) with a required `[mod]` section:
//!   `format=1`, `id=` equal to the folder name, `name=` and `version=` required,
//!   `description=` optional; every value must be free of control characters and within the
//!   metadata byte bounds the DLL's catalog enforces (frozen 2026-11-01: name/version 64
//!   bytes, description 256); values follow the game's ini rules -- the first `[mod]` section
//!   and the first occurrence of a key win, and a `;` starts a comment (so a value may not
//!   contain one). A file with no `[mod]` section is reported as its first missing line
//!   (`format`);
//! * the enabled list is the priority order: LATER wins -- the bottom entry's file is used when
//!   two active mods ship the same game-relative path;
//! * the list is written back EXACTLY as it stands (canonical comma form, no added or removed
//!   spaces): a blank token between commas, a duplicate, an unknown folder or a malformed id all
//!   stay visible with their reason until the player removes them -- nothing is silently
//!   dropped, deduped or reordered. A WHOLLY empty value means "no mods". No `-`/`_` folding:
//!   `a-b` and `a_b` are different mods.
//!
//! Scanning happens on a worker thread (the App starts it); this module is pure and synchronous.

use std::path::Path;

use crate::settings::ini_io;

/// The folder every mod lives in, under the game directory.
pub const DIR_NAME: &str = "mods";
/// A mod's metadata file.
pub const MANIFEST_NAME: &str = "mod.ini";
/// The manifest's section.
pub const MANIFEST_SECTION: &str = "mod";
/// The activation list: `[mods] enabled=a,b,c` in the game's `mh_net.ini` (later wins).
pub const INI_SECTION: &str = "mods";
pub const INI_KEY: &str = "enabled";

/// Metadata byte bounds and content rules, mirroring the DLL's ModCatalog exactly (frozen
/// 2026-11-01: name/version 64 bytes, description 256 bytes; valid UTF-8, no control
/// characters) so the launcher never shows a healthy row for a mod the game will refuse.
pub const NAME_MAX_BYTES: usize = 64;
pub const VERSION_MAX_BYTES: usize = 64;
pub const DESCRIPTION_MAX_BYTES: usize = 256;
/// Maximum active entries accepted by the DLL, including invalid or duplicate tokens.
pub const ENABLED_MAX: usize = 64;

/// Is `id` a well-formed mod id (and therefore a legal folder name)?
pub fn valid_id(id: &str) -> bool {
    (1..=32).contains(&id.chars().count())
        && id
            .chars()
            .all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || c == '-' || c == '_')
}

/// Why the launcher's catalog cannot vouch for a mod. The DLL decides at boot what an
/// unresolvable active set means (the whole set falls back to stock, with its own reason); the
/// page only shows WHY this row will not mount.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Problem {
    /// The folder name is not a valid id (uppercase, too long, bad characters).
    BadId,
    /// `mod.ini` is missing or could not be read.
    NoManifest,
    /// `mod.ini` exists but is not valid UTF-8.
    BadEncoding,
    /// `format` is present but is not `1`; carries what the file says.
    BadFormat(String),
    /// `id=` does not match the folder name; carries what the file says.
    IdMismatch(String),
    /// A required line is missing or empty; carries which one (`format`, `id`, `name`, `version`).
    MissingField(&'static str),
    /// A metadata line is over its byte bound; carries which one.
    TooLong(&'static str),
    /// A metadata line contains control characters (Unicode Cc: C0, DEL, C1); carries which one.
    ControlChars(&'static str),
}

impl Problem {
    /// The i18n key of the reason (`mods.problem.*`).
    pub fn key(&self) -> &'static str {
        match self {
            Problem::BadId => "mods.problem.bad_id",
            Problem::NoManifest => "mods.problem.no_manifest",
            Problem::BadEncoding => "mods.problem.bad_encoding",
            Problem::BadFormat(_) => "mods.problem.bad_format",
            Problem::IdMismatch(_) => "mods.problem.id_mismatch",
            Problem::MissingField(_) => "mods.problem.missing_field",
            Problem::TooLong(_) => "mods.problem.too_long",
            Problem::ControlChars(_) => "mods.problem.control_chars",
        }
    }

    /// The key's `{placeholder}` values, named as the strings use them.
    pub fn args(&self) -> Vec<(&'static str, String)> {
        match self {
            Problem::BadFormat(format) => vec![("format", format.clone())],
            Problem::IdMismatch(id) => vec![("id", id.clone())],
            Problem::MissingField(field)
            | Problem::TooLong(field)
            | Problem::ControlChars(field) => {
                vec![("field", (*field).to_string())]
            }
            _ => Vec::new(),
        }
    }
}

/// One discovered `mods\<id>\` folder. An unusable folder is still an entry, with its `problem`
/// set -- the page shows invalid rows, it does not hide them.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Entry {
    /// The folder name, exactly as on disk.
    pub id: String,
    /// `name=` for a healthy mod; the raw id otherwise.
    pub name: String,
    pub version: String,
    pub description: String,
    /// Why this folder cannot mount; `None` for a healthy mod.
    pub problem: Option<Problem>,
}

/// Everything discovered under `<game>\mods\`, sorted by id: discovery order must never become
/// priority -- the ini's list is the only priority.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Catalog {
    pub entries: Vec<Entry>,
}

impl Catalog {
    /// The entry for `id`, if this folder was discovered.
    pub fn find(&self, id: &str) -> Option<&Entry> {
        self.entries.iter().find(|e| e.id == id)
    }
}

/// Read every mod folder under `<game>\mods\`. A missing `mods` folder is an empty catalog --
/// "no mods installed" is a normal state, not an error. Stray files are ignored: a mod is a
/// directory with a manifest, and the DLL's own scan has the final word anyway.
pub fn scan(game_dir: &Path) -> Catalog {
    let Ok(dirs) = std::fs::read_dir(game_dir.join(DIR_NAME)) else {
        return Catalog::default();
    };
    let mut entries = Vec::new();
    for d in dirs.flatten() {
        let Ok(kind) = d.file_type() else { continue };
        if !kind.is_dir() {
            continue;
        }
        let file_name = d.file_name();
        let Some(id) = file_name.to_str() else {
            continue; // not spellable as text -- not a mod id in any encoding we read
        };
        entries.push(entry(id, &d.path()));
    }
    entries.sort_by(|a, b| a.id.cmp(&b.id));
    Catalog { entries }
}

/// Read one mod folder. Manifest validation only -- the fields the page needs to SHOW; payload
/// and layout verdicts stay with mh.dll.
fn entry(id: &str, dir: &Path) -> Entry {
    let bytes = ini_io::read_file(&dir.join(MANIFEST_NAME)).ok().flatten();
    let problem = if !valid_id(id) {
        Some(Problem::BadId)
    } else {
        manifest_problem(bytes.as_deref(), id)
    };
    let get = |key: &str| {
        bytes
            .as_deref()
            .and_then(|b| ini_io::read_value(b, MANIFEST_SECTION, key))
            .unwrap_or_default()
    };
    Entry {
        id: id.to_string(),
        name: if problem.is_none() {
            get("name")
        } else {
            id.to_string()
        },
        version: get("version"),
        description: get("description"),
        problem,
    }
}

/// The manifest-side reasons an otherwise well-named folder cannot mount. UTF-8 is checked
/// up front: a lossy re-decode would show a healthy-looking row the game refuses.
fn manifest_problem(bytes: Option<&[u8]>, id: &str) -> Option<Problem> {
    let Some(bytes) = bytes else {
        return Some(Problem::NoManifest);
    };
    let Ok(text) = std::str::from_utf8(bytes) else {
        return Some(Problem::BadEncoding);
    };
    let get = |key: &str| ini_io::read_value(text.as_bytes(), MANIFEST_SECTION, key);
    match get("format") {
        Some(f) if f == "1" => {}
        Some(f) => return Some(Problem::BadFormat(f)),
        None => return Some(Problem::MissingField("format")),
    }
    match get("id") {
        Some(v) if v == id => {}
        Some(v) => return Some(Problem::IdMismatch(v)),
        None => return Some(Problem::MissingField("id")),
    }
    let name = get("name").unwrap_or_default();
    if name.is_empty() {
        return Some(Problem::MissingField("name"));
    }
    if name.len() > NAME_MAX_BYTES {
        return Some(Problem::TooLong("name"));
    }
    let version = get("version").unwrap_or_default();
    if version.is_empty() {
        return Some(Problem::MissingField("version"));
    }
    if version.len() > VERSION_MAX_BYTES {
        return Some(Problem::TooLong("version"));
    }
    let description = get("description").unwrap_or_default();
    if description.len() > DESCRIPTION_MAX_BYTES {
        return Some(Problem::TooLong("description"));
    }
    for (field, value) in [
        ("name", name.as_str()),
        ("version", version.as_str()),
        ("description", description.as_str()),
    ] {
        if value.chars().any(char::is_control) {
            return Some(Problem::ControlChars(field));
        }
    }
    None
}

/// The `[mods] enabled=` list: the ids in priority order, later wins. Tokens are kept EXACTLY as
/// the file spelled them -- a blank token between commas, a duplicate, an unknown folder, a
/// malformed id -- so Apply can never silently drop, dedupe or reorder what the player (or a
/// hand edit) asked for. Only a WHOLLY empty value reads as "no mods".
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Selection {
    pub ids: Vec<String>,
}

impl Selection {
    /// Read the list out of `mh_net.ini` bytes. Each token is trimmed of surrounding spaces
    /// (that is display whitespace, not content); blank tokens are KEPT as invalid rows. A
    /// missing section/key or a wholly empty value is an empty list.
    pub fn from_ini(bytes: Option<&[u8]>) -> Selection {
        let raw = bytes
            .and_then(|b| ini_io::read_value(b, INI_SECTION, INI_KEY))
            .unwrap_or_default();
        let raw = raw.trim();
        Selection {
            ids: if raw.is_empty() {
                Vec::new()
            } else {
                raw.split(',').map(str::trim).map(str::to_string).collect()
            },
        }
    }

    /// The canonical file form: comma-separated, no added spaces.
    pub fn to_ini_value(&self) -> String {
        self.ids.join(",")
    }

    /// Is the row at `index` a repeat of an earlier occurrence? Shown as a diagnostic; the DLL
    /// refuses a whole set that names a mod twice, so the page flags it and drops nothing.
    pub fn is_duplicate(&self, index: usize) -> bool {
        match self.ids.get(index) {
            Some(id) => self.ids[..index].iter().any(|t| t == id),
            None => false,
        }
    }

    /// Enable `id` by appending it to the END of the list: later wins, so a freshly enabled mod
    /// becomes the top priority (and is easy to demote with the Down button). Already listed --
    /// including as a later duplicate -- is a no-op.
    pub fn enable(&mut self, id: &str) {
        if !self.ids.iter().any(|t| t == id) {
            self.ids.push(id.to_string());
        }
    }

    /// Move the row at `index` by `delta` rows. Moving DOWN the list RAISES its priority.
    /// Clamped at both ends; `false` when nothing moved.
    pub fn move_row(&mut self, index: usize, delta: isize) -> bool {
        let n = self.ids.len() as isize;
        if n == 0 || index as isize >= n {
            return false;
        }
        let to = (index as isize + delta).clamp(0, n - 1);
        if to == index as isize {
            return false;
        }
        self.ids.swap(index, to as usize);
        true
    }

    /// Remove whatever row sits at `index` (how a blank or missing row leaves the list).
    pub fn remove(&mut self, index: usize) {
        if index < self.ids.len() {
            self.ids.remove(index);
        }
    }
}

/// The Mods page's state: the last scan, and the pending selection against the ini's baseline.
///
/// `pending` starts as the file's list; every click edits it; Apply writes it (one
/// byte-preserving key splice) and re-reads the file, so `loaded` always names what IS on disk.
#[derive(Clone, Debug, Default)]
pub struct Mods {
    pub catalog: Catalog,
    /// What the page shows and Apply would write.
    pub pending: Selection,
    /// The ini's list as of the last read -- the Revert baseline.
    pub loaded: Selection,
}

impl Mods {
    /// Adopt a (re)read of the ini: the pending list becomes the file's list, dropping unsaved
    /// edits. The Revert path, the reload after Apply, and the load for a game directory.
    pub fn reloaded(&mut self, bytes: Option<&[u8]>) {
        self.loaded = Selection::from_ini(bytes);
        self.pending = self.loaded.clone();
    }

    /// True when Apply has something to write. Canonical comparison: a hand-spaced `a, b` in
    /// the file is the same list as `a,b` -- neither an edit, nor something an Apply rewrites.
    pub fn is_dirty(&self) -> bool {
        self.pending.to_ini_value() != self.loaded.to_ini_value()
    }

    /// The pending edit, `None` while the page agrees with the file.
    pub fn edit(&self) -> Option<ini_io::Edit> {
        self.is_dirty()
            .then(|| ini_io::Edit::new(INI_SECTION, INI_KEY, &self.pending.to_ini_value()))
    }

    /// Drop unsaved edits: back to the file's list.
    pub fn revert(&mut self) {
        self.pending = self.loaded.clone();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sel(ids: &[&str]) -> Selection {
        Selection {
            ids: ids.iter().map(|s| s.to_string()).collect(),
        }
    }

    #[test]
    fn ids_have_bounds_and_charset() {
        assert!(valid_id("a"));
        assert!(valid_id("bushes-2_x"));
        assert!(valid_id(&"a".repeat(32)));
        assert!(!valid_id(""));
        assert!(!valid_id(&"a".repeat(33)));
        assert!(!valid_id("Bushes"), "lowercase only");
        assert!(!valid_id("bushes mod"), "no spaces");
        assert!(!valid_id("bushes.mod"), "no dots");
        assert!(!valid_id("кусты"), "ASCII only");
        assert!(!valid_id("bushes;del"), "a ; starts an ini comment");
    }

    #[test]
    fn selection_reads_the_ini_list_and_writes_it_back_verbatim() {
        assert_eq!(Selection::from_ini(None).ids, Vec::<String>::new());
        let ini = b"[video]\r\nwindow=borderless\r\n[mods]\r\nenabled= a , b ; later wins\r\n";
        assert_eq!(Selection::from_ini(Some(ini)).ids, vec!["a", "b"]);
        // A blank token is an invalid entry: it is KEPT (visible, removable), never dropped.
        assert_eq!(
            Selection::from_ini(Some(b"[mods]\r\nenabled=a,,b\r\n".as_slice())).ids,
            vec!["a", "", "b"]
        );
        assert_eq!(
            Selection::from_ini(Some(b"[mods]\r\nenabled=\r\n"))
                .ids
                .len(),
            0
        );
        assert_eq!(sel(&["a", "", "b"]).to_ini_value(), "a,,b");
        assert_eq!(sel(&["a", "b"]).to_ini_value(), "a,b");
    }

    #[test]
    fn duplicates_missing_and_moves_behave_and_nothing_is_silently_dropped() {
        let mut s = Selection::from_ini(Some(b"[mods]\r\nenabled=a,ghost,a\r\n".as_slice()));
        assert_eq!(s.ids, vec!["a", "ghost", "a"]);
        assert!(!s.is_duplicate(0));
        assert!(!s.is_duplicate(1));
        assert!(s.is_duplicate(2), "the repeat is flagged, not dropped");
        s.enable("a"); // already listed: a no-op, not a third copy
        assert_eq!(s.ids, vec!["a", "ghost", "a"]);
        s.remove(1); // how a blank or missing row leaves the list
        assert_eq!(s.ids, vec!["a", "a"]);
        assert!(!s.move_row(0, -1), "clamped at the top");
        assert!(!s.move_row(1, 1), "clamped at the bottom");
        let mut t = sel(&["a", "b", "c"]);
        assert!(t.move_row(0, 1));
        assert_eq!(t.ids, vec!["b", "a", "c"], "down = higher priority");
    }

    #[test]
    fn applying_the_list_keeps_every_other_ini_byte() {
        let dir = std::env::temp_dir().join("mh_launcher_test_mods_ini");
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        let ini = dir.join("mh_net.ini");
        let original = "; Mission Humanity configuration\r\n\
            [video]\r\nwindow=borderless ; keep\r\n[mods]\r\n; my mods, newest first\r\n\
            enabled= a , b ; later wins\r\n[net]\r\nport=6501\r\n[log]\r\nlevel=debug\r\n";
        std::fs::write(&ini, original).unwrap();

        let mut m = Mods::default();
        let bytes = ini_io::read_file(&ini).unwrap();
        m.reloaded(bytes.as_deref());
        assert!(!m.is_dirty(), "hand-spaced a , b is canonically a,b");
        m.pending.enable("c");
        ini_io::apply_file(&ini, &[m.edit().unwrap()]).unwrap();
        let after = std::fs::read(&ini).unwrap();
        let text = String::from_utf8_lossy(&after).into_owned();
        // The activation list reads back in the written order, and the ONLY byte change is
        // the value itself: the original text with `a , b` swapped for `a,b,c`.
        assert_eq!(
            Selection::from_ini(Some(after.as_slice())).ids,
            vec!["a", "b", "c"]
        );
        assert_eq!(after, original.replace("a , b", "a,b,c").as_bytes());
        assert!(text.contains("; Mission Humanity configuration"), "{text}");
        assert!(text.contains("; my mods, newest first"), "{text}");
        assert!(text.contains("window=borderless ; keep"), "{text}");
        assert!(
            text.contains("port=6501") && text.contains("level=debug"),
            "{text}"
        );

        // The page's baseline moves to what is really on disk: the roundtrip closes.
        m.reloaded(Some(after.as_slice()));
        assert!(!m.is_dirty(), "{}", m.pending.to_ini_value());

        // Unticking everything writes an explicit empty list; the section survives.
        m.pending = Selection::default();
        ini_io::apply_file(&ini, &[m.edit().unwrap()]).unwrap();
        let text = std::fs::read_to_string(&ini).unwrap();
        assert!(
            text.contains("[mods]") && text.contains("enabled="),
            "{text}"
        );
        assert!(text.contains("port=6501"), "{text}");
    }

    #[test]
    fn scan_reads_manifests_and_flags_unusable_folders() {
        let game = std::env::temp_dir().join("mh_launcher_test_mods_scan");
        let _ = std::fs::remove_dir_all(&game);
        let mods_dir = game.join("mods");
        let w = |id: &str, body: &str| {
            std::fs::create_dir_all(mods_dir.join(id)).unwrap();
            std::fs::write(mods_dir.join(id).join("mod.ini"), body).unwrap();
        };
        w(
            "good",
            "[mod]\r\nformat=1\r\nid=good\r\nname=Good Mod\r\nversion=1.2\r\n\
             description=Does things ; nicely\r\n",
        );
        w(
            "bad-format",
            "[mod]\r\nformat=2\r\nid=bad-format\r\nname=x\r\nversion=1\r\n",
        );
        w(
            "id-mismatch",
            "[mod]\r\nformat=1\r\nid=other\r\nname=x\r\nversion=1\r\n",
        );
        w(
            "no-name",
            "[mod]\r\nformat=1\r\nid=no-name\r\nversion=1\r\n",
        );
        std::fs::create_dir_all(mods_dir.join("no-manifest")).unwrap();
        std::fs::create_dir_all(mods_dir.join("Broken")).unwrap();
        std::fs::write(mods_dir.join("stray.txt"), b"not a mod").unwrap();

        let cat = scan(&game);
        let good = cat.find("good").unwrap();
        assert_eq!(good.problem, None);
        assert_eq!(good.name, "Good Mod");
        assert_eq!(good.version, "1.2");
        assert_eq!(
            good.description, "Does things",
            "the ; cut, as the game cuts it"
        );
        assert_eq!(
            cat.find("bad-format").unwrap().problem,
            Some(Problem::BadFormat("2".into()))
        );
        assert_eq!(
            cat.find("id-mismatch").unwrap().problem,
            Some(Problem::IdMismatch("other".into()))
        );
        assert_eq!(
            cat.find("no-name").unwrap().problem,
            Some(Problem::MissingField("name"))
        );
        assert_eq!(
            cat.find("no-manifest").unwrap().problem,
            Some(Problem::NoManifest)
        );
        assert_eq!(cat.find("Broken").unwrap().problem, Some(Problem::BadId));
        assert!(cat.find("stray.txt").is_none(), "a file is not a mod");

        // Discovery order is sorted by id; it must never become priority.
        let ids: Vec<&str> = cat.entries.iter().map(|e| e.id.as_str()).collect();
        let mut sorted = ids.clone();
        sorted.sort();
        assert_eq!(ids, sorted);

        // No mods folder at all: an empty catalog, not an error.
        assert!(scan(&game.join("elsewhere")).entries.is_empty());
    }

    #[test]
    fn metadata_bounds_and_encoding_are_enforced_like_the_dll_enforces_them() {
        let game = std::env::temp_dir().join("mh_launcher_test_mods_bounds");
        let _ = std::fs::remove_dir_all(&game);
        let mods_dir = game.join("mods");
        let w = |id: &str, body: &[u8]| {
            std::fs::create_dir_all(mods_dir.join(id)).unwrap();
            std::fs::write(mods_dir.join(id).join("mod.ini"), body).unwrap();
        };
        let long_name = format!(
            "[mod]\r\nformat=1\r\nid=toolong\r\nname={}\r\nversion=1\r\n",
            "n".repeat(NAME_MAX_BYTES + 1)
        );
        w("toolong", long_name.as_bytes());
        w(
            "badenc",
            b"[mod]\r\nformat=1\r\nid=badenc\r\nname=\xFF\xFEbad\r\nversion=1\r\n",
        );
        // The bounds are byte bounds: a name AT the limit is still healthy...
        let at_limit = format!(
            "[mod]\r\nformat=1\r\nid=atlimit\r\nname={}\r\nversion=1\r\n",
            "n".repeat(NAME_MAX_BYTES)
        );
        w("atlimit", at_limit.as_bytes());
        // ...and control characters are refused wherever they hide.
        w(
            "ctrl",
            b"[mod]\r\nformat=1\r\nid=ctrl\r\nname=a\x01b\r\nversion=1\r\n",
        );

        let cat = scan(&game);
        assert_eq!(
            cat.find("toolong").unwrap().problem,
            Some(Problem::TooLong("name"))
        );
        assert_eq!(
            cat.find("badenc").unwrap().problem,
            Some(Problem::BadEncoding)
        );
        assert_eq!(cat.find("atlimit").unwrap().problem, None);
        assert_eq!(
            cat.find("ctrl").unwrap().problem,
            Some(Problem::ControlChars("name"))
        );
    }

    #[test]
    fn a_manifest_with_a_bom_still_parses() {
        let game = std::env::temp_dir().join("mh_launcher_test_mods_bom");
        let _ = std::fs::remove_dir_all(&game);
        let dir = game.join("mods").join("bom");
        std::fs::create_dir_all(&dir).unwrap();
        let mut body = b"\xEF\xBB\xBF".to_vec();
        body.extend_from_slice(b"[mod]\r\nformat=1\r\nid=bom\r\nname=Bom\r\nversion=1\r\n");
        std::fs::write(dir.join("mod.ini"), &body).unwrap();
        assert_eq!(scan(&game).find("bom").unwrap().problem, None);
    }
}
