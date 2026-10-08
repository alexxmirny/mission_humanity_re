//! The settings model (dist RL13): loaded values, pending edits, validation and Apply.
//!
//! Pure data, no egui. `render.rs` draws it; the App owns one `Model` and hands it a
//! `LauncherStore` (the `launcher.toml` side) when it loads and applies. Nothing here names a
//! setting: every behaviour comes from the `Schema` row.
//!
//! ## Value text
//!
//! Values are text. Bools are canonically `"0"`/`"1"` here (the ini spelling; the launcher store gets
//! `"true"`/`"false"`), and the loaded text is kept as found, so a hand-written `vsync=on` or an
//! enum spelled in another case is *not* dirty and is *not* rewritten unless the player changes it.
//! An edit stores exactly what was typed (so a text box can hold `06501` mid-edit); comparison and
//! writing go through `canon`.

use std::collections::BTreeMap;
use std::path::Path;

use super::ini_io::{self, Change, Edit};
use super::schema::{Control, Kind, Provider, Row, Schema, Store};

/// The `launcher.toml` side of the settings. The App implements it over `Config` (WU-C); the tests
/// use `MapStore`. Bools travel as `"true"`/`"false"`.
pub trait LauncherStore {
    fn get(&self, field: &str) -> Option<String>;
    fn set(&mut self, field: &str, value: &str) -> Result<(), String>;
    /// Called once after a batch of `set`s (the App saves `launcher.toml` here).
    fn flush(&mut self) -> Result<(), String> {
        Ok(())
    }
}

/// A trivial in-memory store: the test double, and what a snapshot test seeds.
#[derive(Clone, Debug, Default)]
pub struct MapStore {
    pub fields: BTreeMap<String, String>,
    pub flushes: usize,
}

impl MapStore {
    pub fn with(pairs: &[(&str, &str)]) -> Self {
        Self {
            fields: pairs
                .iter()
                .map(|(k, v)| (k.to_string(), v.to_string()))
                .collect(),
            flushes: 0,
        }
    }
}

impl LauncherStore for MapStore {
    fn get(&self, field: &str) -> Option<String> {
        self.fields.get(field).cloned()
    }
    fn set(&mut self, field: &str, value: &str) -> Result<(), String> {
        self.fields.insert(field.to_string(), value.to_string());
        Ok(())
    }
    fn flush(&mut self) -> Result<(), String> {
        self.flushes += 1;
        Ok(())
    }
}

/// One entry of a run-time option list (`Provider`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ProviderOption {
    /// What is stored.
    pub value: String,
    /// What the player sees (a language's own name -- not translated).
    pub label: String,
}

/// Why a value cannot be applied. `render.rs` turns these into strings.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Problem {
    NotInteger,
    OutOfRange {
        min: Option<i64>,
        max: Option<i64>,
    },
    BadHotkey,
    /// `;`, a line break -- would corrupt the ini line.
    BadChars,
    /// The custom relay needs `host:port`.
    BadAddress,
}

/// What an Apply did.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ApplyReport {
    pub ini: Change,
    /// Launcher fields written (including mirrored ones).
    pub launcher_fields: Vec<String>,
    /// A changed row is flagged `restart`: the running game keeps the old value.
    pub restart_needed: bool,
}

// ---- value helpers -------------------------------------------------------------------------------

/// `0/1/true/false/on/off/yes/no` -> `"1"`/`"0"`; anything else `None`.
pub fn parse_bool(s: &str) -> Option<bool> {
    match s.trim().to_ascii_lowercase().as_str() {
        "1" | "true" | "on" | "yes" => Some(true),
        "0" | "false" | "off" | "no" => Some(false),
        _ => None,
    }
}

/// A parsed hotkey: `[Ctrl+][Alt+][Shift+](A-Z | 0-9 | F1-F12)`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Hotkey {
    pub ctrl: bool,
    pub alt: bool,
    pub shift: bool,
    /// Upper-case letter, digit, or `F1`..`F12`.
    pub key: String,
}

impl Hotkey {
    /// Canonical spelling: `Ctrl+Alt+Shift+Key`.
    pub fn format(&self) -> String {
        let mut s = String::new();
        if self.ctrl {
            s.push_str("Ctrl+");
        }
        if self.alt {
            s.push_str("Alt+");
        }
        if self.shift {
            s.push_str("Shift+");
        }
        s.push_str(&self.key);
        s
    }
}

/// `Ok(None)` for "no hotkey" (empty or `none`), `Ok(Some)` for a valid chord, `Err` otherwise.
/// Modifier order and case are free (the game accepts any), duplicates are not.
pub fn parse_hotkey(s: &str) -> Result<Option<Hotkey>, ()> {
    let t = s.trim();
    if t.is_empty() || t.eq_ignore_ascii_case("none") {
        return Ok(None);
    }
    let parts: Vec<&str> = t.split('+').map(str::trim).collect();
    let (key, mods) = parts.split_last().ok_or(())?;
    let mut hk = Hotkey {
        ctrl: false,
        alt: false,
        shift: false,
        key: String::new(),
    };
    for m in mods {
        let slot = match m.to_ascii_lowercase().as_str() {
            "ctrl" => &mut hk.ctrl,
            "alt" => &mut hk.alt,
            "shift" => &mut hk.shift,
            _ => return Err(()),
        };
        if *slot {
            return Err(());
        }
        *slot = true;
    }
    let k = key.to_ascii_uppercase();
    let ok = match k.as_bytes() {
        [c] => c.is_ascii_uppercase() || c.is_ascii_digit(),
        [b'F', rest @ ..] => rest
            .iter()
            .all(u8::is_ascii_digit)
            .then(|| std::str::from_utf8(rest).ok()?.parse::<u32>().ok())
            .flatten()
            .is_some_and(|n| (1..=12).contains(&n)),
        _ => false,
    };
    if !ok {
        return Err(());
    }
    hk.key = k;
    Ok(Some(hk))
}

/// The comparison/writing form of `raw` for `row`.
pub fn canon(row: &Row, raw: &str) -> String {
    let t = raw.trim();
    match row.kind {
        Kind::Bool => match parse_bool(t) {
            Some(true) => "1".to_string(),
            _ => "0".to_string(),
        },
        Kind::Int => t
            .parse::<i64>()
            .map_or_else(|_| t.to_string(), |n| n.to_string()),
        Kind::Enum => row
            .options
            .iter()
            .find(|o| o.eq_ignore_ascii_case(t))
            .cloned()
            .unwrap_or_else(|| t.to_string()),
        Kind::Hotkey => match parse_hotkey(t) {
            Ok(Some(h)) => h.format(),
            Ok(None) => "none".to_string(),
            Err(()) => t.to_string(),
        },
        Kind::Str => t.to_string(),
    }
}

/// The address shape the custom relay must have (`host:port`, port 1..65535, no `;#=`/space).
pub fn valid_relay_addr(addr: &str) -> bool {
    crate::relay::Relay {
        addr: addr.to_string(),
        key: "open".to_string(),
    }
    .validate()
    .is_ok()
}

fn bad_chars(s: &str) -> bool {
    s.contains([';', '\n', '\r'])
}

// ---- the model -----------------------------------------------------------------------------------

#[derive(Clone, Debug)]
pub struct Model {
    schema: Schema,
    /// Text as found: ini value, or launcher field. `None` = absent.
    loaded: BTreeMap<String, Option<String>>,
    /// Pending edits, exactly as typed.
    edits: BTreeMap<String, String>,
    providers: Vec<(Provider, Vec<ProviderOption>)>,
}

impl Model {
    pub fn new(schema: Schema) -> Self {
        Self {
            schema,
            loaded: BTreeMap::new(),
            edits: BTreeMap::new(),
            providers: Vec::new(),
        }
    }

    pub fn schema(&self) -> &Schema {
        &self.schema
    }

    /// (Re)load every row from the ini bytes (or none) and the launcher store; drops all edits.
    pub fn load(&mut self, ini: Option<&[u8]>, store: &dyn LauncherStore) {
        self.edits.clear();
        self.loaded.clear();
        for r in &self.schema.rows {
            let v = match &r.store {
                Store::Ini { section, key } => {
                    ini.and_then(|b| ini_io::read_value(b, section, key))
                }
                Store::Launcher { field } => store.get(field).map(|v| {
                    // The store speaks true/false; keep one spelling in `loaded`.
                    if r.kind == Kind::Bool {
                        canon(r, &v)
                    } else {
                        v
                    }
                }),
            };
            self.loaded.insert(r.id.clone(), v);
        }
    }

    pub fn set_provider_options(&mut self, p: Provider, options: Vec<ProviderOption>) {
        self.providers.retain(|(q, _)| *q != p);
        self.providers.push((p, options));
    }

    pub fn provider_options(&self, p: Provider) -> &[ProviderOption] {
        self.providers
            .iter()
            .find(|(q, _)| *q == p)
            .map_or(&[], |(_, v)| v.as_slice())
    }

    fn row(&self, id: &str) -> Option<&Row> {
        self.schema.row(id)
    }

    /// The text found on disk, `None` when the key/field is absent.
    pub fn raw(&self, id: &str) -> Option<&str> {
        self.loaded.get(id).and_then(|v| v.as_deref())
    }

    /// The value without pending edits: what is on disk, else the schema default.
    pub fn baseline(&self, id: &str) -> String {
        let Some(r) = self.row(id) else {
            return String::new();
        };
        match self.raw(id) {
            Some(v) => v.to_string(),
            None => r.default.clone(),
        }
    }

    /// The value the page shows: the pending edit, else the baseline.
    pub fn value(&self, id: &str) -> String {
        self.edits
            .get(id)
            .cloned()
            .unwrap_or_else(|| self.baseline(id))
    }

    pub fn is_edited(&self, id: &str) -> bool {
        self.edits.contains_key(id)
    }

    pub fn dirty_count(&self) -> usize {
        self.edits.len()
    }

    pub fn is_dirty(&self) -> bool {
        !self.edits.is_empty()
    }

    /// Does the shown value differ from the schema default? (The per-row reset button.)
    pub fn differs_from_default(&self, id: &str) -> bool {
        let Some(r) = self.row(id) else { return false };
        canon(r, &self.value(id)) != canon(r, &r.default)
    }

    /// Record an edit. Setting a row back to what is on disk clears its edit.
    pub fn set(&mut self, id: &str, value: &str) {
        let Some(r) = self.row(id) else { return };
        if canon(r, value) == canon(r, &self.baseline(id)) {
            self.edits.remove(id);
        } else {
            self.edits.insert(id.to_string(), value.to_string());
        }
    }

    /// Edit a row back to its schema default.
    pub fn reset_row(&mut self, id: &str) {
        if let Some(r) = self.row(id) {
            let d = r.default.clone();
            self.set(id, &d);
        }
    }

    pub fn revert_row(&mut self, id: &str) {
        self.edits.remove(id);
    }

    pub fn revert(&mut self) {
        self.edits.clear();
    }

    /// Is this row shown right now (its `visible_if` holds, it is not hidden)?
    pub fn is_visible(&self, r: &Row) -> bool {
        if r.hidden {
            return false;
        }
        match &r.visible_if {
            Some((other, eq)) => self
                .row(other)
                .is_some_and(|o| canon(o, &self.value(other)) == *eq),
            None => true,
        }
    }

    /// Is the shown value one the row does not list (an enum/int the player typed into the ini)?
    pub fn is_custom_choice(&self, r: &Row) -> bool {
        let v = self.value(&r.id);
        let listed = match (r.kind, r.provider) {
            (Kind::Enum | Kind::Int, None) if !r.options.is_empty() => {
                r.options.iter().any(|o| canon(r, o) == canon(r, &v))
            }
            (_, Some(p)) => self.provider_options(p).iter().any(|o| o.value == v.trim()),
            _ => true,
        };
        !listed
    }

    /// Validate the SHOWN value of one row. Only edits are ever checked by `problems`; a value the
    /// player already had in the ini is preserved, whatever it says.
    pub fn validate(&self, id: &str) -> Result<(), Problem> {
        let Some(r) = self.row(id) else { return Ok(()) };
        let v = self.value(id);
        let t = v.trim();
        if bad_chars(t) {
            return Err(Problem::BadChars);
        }
        match r.kind {
            Kind::Bool | Kind::Enum | Kind::Str => Ok(()),
            Kind::Int => {
                let n: i64 = t.parse().map_err(|_| Problem::NotInteger)?;
                if r.min.is_some_and(|m| n < m) || r.max.is_some_and(|m| n > m) {
                    // A listed option (fps_limit 0 = unlimited) is always allowed.
                    if r.options.iter().any(|o| o.trim().parse::<i64>() == Ok(n)) {
                        return Ok(());
                    }
                    return Err(Problem::OutOfRange {
                        min: r.min,
                        max: r.max,
                    });
                }
                Ok(())
            }
            Kind::Hotkey => parse_hotkey(t).map(|_| ()).map_err(|()| Problem::BadHotkey),
        }
    }

    /// Everything wrong with the pending edits, as (row id, problem).
    pub fn problems(&self) -> Vec<(String, Problem)> {
        let mut out = Vec::new();
        for id in self.edits.keys() {
            if let Err(p) = self.validate(id) {
                out.push((id.clone(), p));
            }
        }
        // The relay control: a custom mode needs a usable address.
        for r in &self.schema.rows {
            if r.control != Control::Relay {
                continue;
            }
            let Some(comp) = r.companion.as_deref() else {
                continue;
            };
            let touched = self.is_edited(&r.id) || self.is_edited(comp);
            if touched
                && canon(r, &self.value(&r.id)) == "custom"
                && !valid_relay_addr(self.value(comp).trim())
                && !out.iter().any(|(i, _)| i == comp)
            {
                out.push((comp.to_string(), Problem::BadAddress));
            }
        }
        out
    }

    pub fn has_problems(&self) -> bool {
        !self.problems().is_empty()
    }

    pub fn problem_for(&self, id: &str) -> Option<Problem> {
        self.problems()
            .into_iter()
            .find(|(i, _)| i == id)
            .map(|(_, p)| p)
    }

    /// The ini edits an Apply would make: changed ini rows only, canonical text.
    pub fn ini_edits(&self) -> Vec<Edit> {
        let mut out = Vec::new();
        for r in &self.schema.rows {
            let (Store::Ini { section, key }, Some(v)) = (&r.store, self.edits.get(&r.id)) else {
                continue;
            };
            let c = canon(r, v);
            out.push(Edit::new(section, key, &c));
        }
        out
    }

    /// The launcher fields an Apply would write: (field, text), including mirrored fields.
    pub fn launcher_edits(&self) -> Vec<(String, String)> {
        let mut out = Vec::new();
        for r in &self.schema.rows {
            let Some(v) = self.edits.get(&r.id) else {
                continue;
            };
            let c = canon(r, v);
            if let Store::Launcher { field } = &r.store {
                let text = if r.kind == Kind::Bool {
                    (if c == "1" { "true" } else { "false" }).to_string()
                } else {
                    c.clone()
                };
                out.push((field.clone(), text));
            }
            if let Some(m) = &r.mirror {
                // An empty value (the stock language) mirrors as `en`.
                out.push((m.clone(), if c.is_empty() { "en".to_string() } else { c }));
            }
        }
        out
    }

    /// Does any pending edit touch a `restart` row?
    pub fn restart_pending(&self) -> bool {
        self.edits
            .keys()
            .filter_map(|id| self.row(id))
            .any(|r| r.restart)
    }

    /// Apply: re-read the ini bytes, splice only the changed values, write atomically; then write
    /// the launcher fields and flush the store; then reload (which clears the edits).
    ///
    /// Refuses (without touching anything) while there are problems.
    pub fn apply(
        &mut self,
        ini_path: &Path,
        store: &mut dyn LauncherStore,
    ) -> Result<ApplyReport, String> {
        if self.has_problems() {
            return Err("there are invalid values".to_string());
        }
        let ini_edits = self.ini_edits();
        let launcher_edits = self.launcher_edits();
        let restart_needed = self.restart_pending();
        let ini = if ini_edits.is_empty() {
            Change::Unchanged
        } else {
            ini_io::apply_file(ini_path, &ini_edits)?
        };
        let mut written = Vec::new();
        for (f, v) in &launcher_edits {
            store.set(f, v)?;
            written.push(f.clone());
        }
        if !launcher_edits.is_empty() {
            store.flush()?;
        }
        let bytes = ini_io::read_file(ini_path)?;
        self.load(bytes.as_deref(), store);
        Ok(ApplyReport {
            ini,
            launcher_fields: written,
            restart_needed,
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn model() -> (Model, MapStore) {
        let m = Model::new(Schema::builtin());
        (m, MapStore::default())
    }

    const INI: &str = "; my notes\r\n[video]\r\nwindow=borderless ; keep\r\nvsync=on\r\n[net]\r\nport=6501\r\nsecret_dev_knob=7\r\n[mystery]\r\nx=1\r\n";

    #[test]
    fn loaded_values_default_and_not_dirty() {
        let (mut m, store) = model();
        m.load(Some(INI.as_bytes()), &store);
        assert_eq!(m.value("video.window"), "borderless");
        assert_eq!(
            m.value("video.scale"),
            "fit",
            "absent key shows the default"
        );
        assert_eq!(m.raw("video.scale"), None);
        // `on` is a hand-written spelling of 1: shown as on, not dirty, not rewritten.
        assert_eq!(
            canon(
                m.schema().row("video.vsync").unwrap(),
                &m.value("video.vsync")
            ),
            "1"
        );
        assert!(!m.is_dirty());
        assert_eq!(m.value("launcher.channel"), "stable");
        assert_eq!(m.value("launcher.discord"), "1");
    }

    #[test]
    fn dirty_revert_and_reset() {
        let (mut m, store) = model();
        m.load(Some(INI.as_bytes()), &store);
        m.set("video.window", "windowed");
        assert!(m.is_edited("video.window") && m.dirty_count() == 1);
        m.set("video.window", "borderless");
        assert!(
            !m.is_dirty(),
            "setting it back to the disk value clears the edit"
        );
        // Equal in another spelling is also clean.
        m.set("video.vsync", "1");
        assert!(!m.is_dirty());
        m.set("video.vsync", "0");
        m.set("net.port", "7000");
        assert_eq!(m.dirty_count(), 2);
        m.revert_row("net.port");
        assert_eq!(m.dirty_count(), 1);
        m.revert();
        assert!(!m.is_dirty());
        assert!(m.differs_from_default("video.window"));
        m.reset_row("video.window");
        assert_eq!(m.value("video.window"), "windowed");
        assert!(!m.differs_from_default("video.window"));
    }

    #[test]
    fn int_rows_are_range_checked_and_listed_options_pass() {
        let (mut m, store) = model();
        m.load(Some(INI.as_bytes()), &store);
        m.set("net.port", "80");
        assert_eq!(
            m.validate("net.port"),
            Err(Problem::OutOfRange {
                min: Some(1024),
                max: Some(65535)
            })
        );
        m.set("net.port", "70000");
        assert!(m.validate("net.port").is_err());
        m.set("net.port", "banana");
        assert_eq!(m.validate("net.port"), Err(Problem::NotInteger));
        assert!(m.has_problems());
        m.set("net.port", "1024");
        assert_eq!(m.validate("net.port"), Ok(()));
        m.set("net.port", "65535");
        assert!(!m.has_problems());
        // fps_limit has options 60|0 and no range: any integer is fine, text is not.
        m.set("video.fps_limit", "144");
        assert_eq!(m.validate("video.fps_limit"), Ok(()));
        m.set("video.fps_limit", "x");
        assert_eq!(m.validate("video.fps_limit"), Err(Problem::NotInteger));
    }

    #[test]
    fn hotkey_grammar() {
        let ok = |s: &str| parse_hotkey(s).unwrap().map(|h| h.format());
        assert_eq!(ok("Ctrl+Alt+N").as_deref(), Some("Ctrl+Alt+N"));
        assert_eq!(ok("alt+ctrl+n").as_deref(), Some("Ctrl+Alt+N"));
        assert_eq!(ok("F12").as_deref(), Some("F12"));
        assert_eq!(ok("shift+f1").as_deref(), Some("Shift+F1"));
        assert_eq!(ok("7").as_deref(), Some("7"));
        assert_eq!(ok(""), None);
        assert_eq!(ok("none"), None);
        for bad in [
            "Ctrl+",
            "Ctrl+Ctrl+N",
            "Win+N",
            "F13",
            "F0",
            "NN",
            "Ctrl+Alt",
            "+N",
            "F1F2",
            "-",
        ] {
            assert!(parse_hotkey(bad).is_err(), "{bad:?} must be refused");
        }
        let (mut m, store) = model();
        m.load(Some(b"[hud]\r\nnet_indicator_key=Ctrl+Alt+N\r\n"), &store);
        m.set("hud.net_indicator_key", "Ctrl+Q+Z");
        assert_eq!(m.validate("hud.net_indicator_key"), Err(Problem::BadHotkey));
        m.set("hud.net_indicator_key", "none");
        assert_eq!(m.validate("hud.net_indicator_key"), Ok(()));
        assert_eq!(
            m.ini_edits(),
            [Edit::new("hud", "net_indicator_key", "none")]
        );
    }

    #[test]
    fn unknown_and_unedited_ini_content_is_preserved_through_apply() {
        let dir = std::env::temp_dir().join("mh_launcher_test_model_apply");
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        let ini = dir.join("mh_net.ini");
        std::fs::write(&ini, INI).unwrap();
        let (mut m, mut store) = model();
        m.load(Some(INI.as_bytes()), &store);
        m.set("video.window", "windowed");
        m.set("video.scale", "integer"); // absent key: inserted
        m.set("launcher.channel", "latest");
        m.set("launcher.discord", "0");
        let report = m.apply(&ini, &mut store).unwrap();
        assert_eq!(report.ini, Change::Written);
        assert!(!report.restart_needed);
        let after = std::fs::read_to_string(&ini).unwrap();
        let expect = INI
            .replace("window=borderless ; keep", "window=windowed ; keep")
            .replace("vsync=on\r\n", "vsync=on\r\nscale=integer\r\n");
        assert_eq!(
            after, expect,
            "comments, `on`, dev knob and unknown sections untouched"
        );
        assert_eq!(store.fields["channel"], "latest");
        assert_eq!(store.fields["discord"], "false");
        assert_eq!(store.flushes, 1);
        assert!(!m.is_dirty(), "apply reloads");
        assert_eq!(m.value("video.window"), "windowed");
        assert_eq!(m.value("launcher.channel"), "latest");
        // An Apply with nothing pending does not touch the file at all.
        let before = std::fs::metadata(&ini).unwrap().modified().unwrap();
        let r = m.apply(&ini, &mut store).unwrap();
        assert_eq!(r.ini, Change::Unchanged);
        assert_eq!(std::fs::metadata(&ini).unwrap().modified().unwrap(), before);
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn apply_refuses_invalid_values_and_touches_nothing() {
        let dir = std::env::temp_dir().join("mh_launcher_test_model_refuse");
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        let ini = dir.join("mh_net.ini");
        std::fs::write(&ini, INI).unwrap();
        let (mut m, mut store) = model();
        m.load(Some(INI.as_bytes()), &store);
        m.set("net.port", "22");
        assert!(m.apply(&ini, &mut store).is_err());
        assert_eq!(std::fs::read_to_string(&ini).unwrap(), INI);
        assert!(
            m.is_edited("net.port"),
            "the edit survives so the player can fix it"
        );
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn restart_rows_set_the_flag_and_pending_text_cannot_carry_a_comment() {
        let (mut m, store) = model();
        m.load(None, &store);
        m.set("net.port", "7777");
        assert!(m.restart_pending());
        m.set("lang.pack", "ru;oops");
        assert_eq!(m.validate("lang.pack"), Err(Problem::BadChars));
    }

    #[test]
    fn the_language_row_mirrors_into_ui_lang() {
        let (mut m, store) = model();
        m.load(None, &store);
        m.set("lang.pack", "ru");
        assert!(m
            .launcher_edits()
            .contains(&("ui_lang".to_string(), "ru".to_string())));
        m.set("lang.pack", "");
        assert!(!m.is_dirty());
        m.load(Some(b"[lang]\r\npack=ru\r\n"), &store);
        m.set("lang.pack", "");
        assert!(m
            .launcher_edits()
            .contains(&("ui_lang".to_string(), "en".to_string())));
        assert_eq!(m.ini_edits(), [Edit::new("lang", "pack", "")]);
    }

    #[test]
    fn a_custom_relay_needs_an_address_and_the_row_stays_hidden_otherwise() {
        let (mut m, store) = model();
        m.load(None, &store);
        m.set("launcher.relay_mode", "custom");
        assert_eq!(
            m.problem_for("launcher.relay_custom"),
            Some(Problem::BadAddress)
        );
        m.set("launcher.relay_custom", "relay.example.org:7100");
        assert!(!m.has_problems());
        m.set("launcher.relay_custom", "relay.example.org");
        assert!(m.has_problems());
        m.set("launcher.relay_mode", "off");
        m.revert_row("launcher.relay_custom");
        assert!(
            !m.has_problems(),
            "no address needed unless the mode is custom"
        );
        assert!(valid_relay_addr("203.0.113.9:7100"));
        assert!(!valid_relay_addr("a b:1"));
        assert!(!valid_relay_addr("host:0"));
    }

    #[test]
    fn out_of_options_values_are_custom_and_preserved() {
        let (mut m, store) = model();
        m.load(
            Some(b"[video]\r\nfilter=bicubic\r\nfps_limit=144\r\n"),
            &store,
        );
        let f = m.schema().row("video.filter").unwrap().clone();
        assert!(m.is_custom_choice(&f));
        assert_eq!(m.value("video.filter"), "bicubic");
        assert!(!m.is_dirty());
        m.set("video.filter", "sharp");
        assert!(!m.is_custom_choice(&f));
        let fps = m.schema().row("video.fps_limit").unwrap().clone();
        assert!(m.is_custom_choice(&fps), "144 is not 60|0");
    }

    #[test]
    fn visible_if_follows_the_other_row() {
        let schema = Schema::from_json(
            r#"{"settings":[
              {"section":"a","key":"mode","type":"enum","default":"x","label":"a.mode","subtab":"t","options":["x","y"]},
              {"section":"a","key":"extra","type":"int","default":1,"label":"a.extra","subtab":"t","visible_if":{"row":"a.mode","equals":"y"}}]}"#,
            None,
        )
        .unwrap();
        let mut m = Model::new(schema);
        m.load(None, &MapStore::default());
        let extra = m.schema().row("a.extra").unwrap().clone();
        assert!(!m.is_visible(&extra));
        m.set("a.mode", "y");
        assert!(m.is_visible(&extra));
    }
}
