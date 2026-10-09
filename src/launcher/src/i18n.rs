//! Launcher string table (dist RL16): keyed strings, English as the reference, Russian first.
//!
//! * `tr(key)` -- the string in the current locale; falls back to English, then to a humanised form
//!   of the key (`setting.video.window.opt.point` -> "Point") so a missing string is ugly, never a
//!   crash or a blank. It returns `&'static str` because the tables are parsed once and leaked, which
//!   is what lets egui widgets take it without a per-frame allocation.
//! * `tr_opt(key)` -- the same, but `None` instead of the humanised fallback (hints are optional).
//! * `trf(key, &[("name", value)])` -- `{name}` placeholders filled.
//! * `tr_in(locale, key)` / `trf_in` -- pure forms for tests and for rendering a page in a locale
//!   other than the process-wide one.
//!
//! The tables are `i18n/<code>.toml`: a flat table of quoted dotted keys (see `en.toml`'s header for
//! the key families). THE RULES, all enforced by the tests below: every key the launcher uses exists
//! in `en.toml`; a language file may not contain a key `en.toml` lacks; every settings-schema label,
//! option and sub-tab has an English string; and `missing_in(locale)` lists what a language still
//! has to translate (the test fails on any, so a half-translated file cannot ship by accident).
//!
//! One language setting drives both the launcher and the game: the Language settings row stores the
//! pack id in `[lang] pack` and mirrors it into `launcher.toml` `ui_lang`; `Locale::from_code` turns
//! that into the locale here (unknown codes -> English, so a `de` pack still gets an English
//! launcher until a `de.toml` exists).

#![allow(dead_code)]

use std::collections::{BTreeMap, HashMap};
use std::sync::atomic::{AtomicU8, Ordering};
use std::sync::{Mutex, OnceLock};

const EN_TOML: &str = include_str!("i18n/en.toml");
const RU_TOML: &str = include_str!("i18n/ru.toml");

/// A launcher language.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub enum Locale {
    #[default]
    En,
    Ru,
}

impl Locale {
    pub const ALL: [Locale; 2] = [Locale::En, Locale::Ru];

    /// The code stored in `launcher.toml` `ui_lang` (and used as the lang pack id).
    pub fn code(self) -> &'static str {
        match self {
            Locale::En => "en",
            Locale::Ru => "ru",
        }
    }

    /// Unknown or empty codes are English.
    pub fn from_code(code: &str) -> Locale {
        match code.trim().to_ascii_lowercase().as_str() {
            "ru" => Locale::Ru,
            _ => Locale::En,
        }
    }

    fn index(self) -> usize {
        self as usize
    }
}

type Table = HashMap<&'static str, &'static str>;

struct Tables {
    by_locale: [Table; 2],
}

fn parse(src: &str, what: &str) -> Table {
    let raw: BTreeMap<String, String> =
        toml::from_str(src).unwrap_or_else(|e| panic!("{what}: {e}"));
    raw.into_iter()
        .map(|(k, v)| {
            let k: &'static str = Box::leak(k.into_boxed_str());
            let v: &'static str = Box::leak(v.into_boxed_str());
            (k, v)
        })
        .collect()
}

fn tables() -> &'static Tables {
    static T: OnceLock<Tables> = OnceLock::new();
    T.get_or_init(|| Tables {
        by_locale: [
            parse(EN_TOML, "i18n/en.toml"),
            parse(RU_TOML, "i18n/ru.toml"),
        ],
    })
}

static CURRENT: AtomicU8 = AtomicU8::new(0);

// Under `cargo test` the locale is per THREAD: the unit tests run in parallel, and one switching to
// Russian must not relabel another's assertions.
#[cfg(test)]
thread_local! {
    static TEST_LOCALE: std::cell::Cell<Option<Locale>> = const { std::cell::Cell::new(None) };
}

/// Switch the process-wide locale (the App does this when `ui_lang` changes).
pub fn set_locale(l: Locale) {
    #[cfg(test)]
    TEST_LOCALE.with(|c| c.set(Some(l)));
    #[cfg(not(test))]
    CURRENT.store(l as u8, Ordering::Relaxed);
}

pub fn locale() -> Locale {
    #[cfg(test)]
    if let Some(l) = TEST_LOCALE.with(std::cell::Cell::get) {
        return l;
    }
    match CURRENT.load(Ordering::Relaxed) {
        1 => Locale::Ru,
        _ => Locale::En,
    }
}

/// `Some` only when the locale's own table, or English, has the key.
pub fn tr_opt_in(l: Locale, key: &str) -> Option<&'static str> {
    let t = tables();
    t.by_locale[l.index()]
        .get(key)
        .or_else(|| t.by_locale[Locale::En.index()].get(key))
        .copied()
}

pub fn tr_opt(key: &str) -> Option<&'static str> {
    tr_opt_in(locale(), key)
}

/// `sample.foo_bar` -> `Foo bar` (the last dotted segment, underscores to spaces, capitalised).
pub fn humanise(key: &str) -> String {
    let last = key.rsplit('.').next().unwrap_or(key).replace('_', " ");
    let mut c = last.chars();
    match c.next() {
        Some(f) => f.to_uppercase().collect::<String>() + c.as_str(),
        None => String::new(),
    }
}

fn humanised_static(key: &str) -> &'static str {
    static CACHE: OnceLock<Mutex<HashMap<String, &'static str>>> = OnceLock::new();
    let mut m = CACHE
        .get_or_init(|| Mutex::new(HashMap::new()))
        .lock()
        .unwrap_or_else(|p| p.into_inner());
    if let Some(s) = m.get(key) {
        return s;
    }
    let s: &'static str = Box::leak(humanise(key).into_boxed_str());
    m.insert(key.to_string(), s);
    s
}

pub fn tr_in(l: Locale, key: &str) -> &'static str {
    tr_opt_in(l, key).unwrap_or_else(|| humanised_static(key))
}

pub fn tr(key: &str) -> &'static str {
    tr_in(locale(), key)
}

/// Fill `{name}` placeholders. A placeholder with no value is left as written.
pub fn fill(template: &str, args: &[(&str, &str)]) -> String {
    let mut out = template.to_string();
    for (k, v) in args {
        out = out.replace(&format!("{{{k}}}"), v);
    }
    out
}

pub fn trf_in(l: Locale, key: &str, args: &[(&str, &str)]) -> String {
    fill(tr_in(l, key), args)
}

pub fn trf(key: &str, args: &[(&str, &str)]) -> String {
    trf_in(locale(), key, args)
}

/// Keys English has that `l` lacks (they render in English). Sorted. Empty for English.
pub fn missing_in(l: Locale) -> Vec<&'static str> {
    let t = tables();
    let mut v: Vec<&'static str> = t.by_locale[Locale::En.index()]
        .keys()
        .copied()
        .filter(|k| !t.by_locale[l.index()].contains_key(k))
        .collect();
    v.sort_unstable();
    v
}

/// Keys `l` has that English lacks -- always a bug (a typo, or a key for a deleted string).
pub fn extra_in(l: Locale) -> Vec<&'static str> {
    let t = tables();
    let mut v: Vec<&'static str> = t.by_locale[l.index()]
        .keys()
        .copied()
        .filter(|k| !t.by_locale[Locale::En.index()].contains_key(k))
        .collect();
    v.sort_unstable();
    v
}

/// All English keys, sorted (tests and the review harness).
pub fn all_keys() -> Vec<&'static str> {
    let mut v: Vec<&'static str> = tables().by_locale[Locale::En.index()]
        .keys()
        .copied()
        .collect();
    v.sort_unstable();
    v
}

/// The `{placeholder}` names a template uses, sorted and de-duplicated.
fn placeholders(s: &str) -> Vec<String> {
    let mut out = Vec::new();
    let mut rest = s;
    while let Some(i) = rest.find('{') {
        let after = &rest[i + 1..];
        match after.find('}') {
            Some(j) => {
                out.push(after[..j].to_string());
                rest = &after[j + 1..];
            }
            None => break,
        }
    }
    out.sort_unstable();
    out.dedup();
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::settings::schema::{Kind, Schema};

    #[test]
    fn both_tables_parse_and_english_is_the_reference() {
        assert!(all_keys().len() > 150);
        for l in Locale::ALL {
            assert!(
                extra_in(l).is_empty(),
                "{} has keys English lacks (typo?): {:?}",
                l.code(),
                extra_in(l)
            );
        }
    }

    /// RL16 done_when: "a test lists any string key absent from a language".
    #[test]
    fn russian_is_complete() {
        let missing = missing_in(Locale::Ru);
        assert!(
            missing.is_empty(),
            "ru.toml lacks {} key(s): {missing:#?}",
            missing.len()
        );
    }

    #[test]
    fn placeholders_agree_between_languages() {
        let t = tables();
        for (k, en) in &t.by_locale[Locale::En.index()] {
            if let Some(ru) = t.by_locale[Locale::Ru.index()].get(k) {
                assert_eq!(
                    placeholders(en),
                    placeholders(ru),
                    "{k}: placeholder names differ between en and ru"
                );
            }
        }
    }

    #[test]
    fn every_schema_label_option_and_subtab_has_an_english_string() {
        let s = Schema::builtin();
        let mut missing = Vec::new();
        let en = |k: &str| tr_opt_in(Locale::En, k).is_some();
        for t in &s.subtabs {
            if !en(&t.label) {
                missing.push(t.label.clone());
            }
        }
        for r in &s.rows {
            if !en(&r.label_key()) {
                missing.push(r.label_key());
            }
            let wants_options =
                r.provider.is_none() && matches!(r.kind, Kind::Enum | Kind::Int) && !r.hidden;
            if wants_options {
                for o in &r.options {
                    if !en(&r.option_key(o)) {
                        missing.push(r.option_key(o));
                    }
                }
            }
        }
        assert!(missing.is_empty(), "no English string for: {missing:#?}");
    }

    /// RL16: "every visible string through i18n::tr". The other direction -- a key the CODE names
    /// that no table has -- would render as a humanised fallback instead of failing anywhere, so
    /// this reads the UI sources and checks every string literal shaped like a key.
    #[test]
    fn every_key_the_ui_code_names_has_an_english_string() {
        // (`relay.` is not a family here: `relay.example.org` is a literal in a test.)
        const FAMILIES: [&str; 19] = [
            "menu.",
            "page.",
            "msg.",
            "play.",
            "report.",
            "about.",
            "diag.",
            "btn.",
            "start.",
            "footer.",
            "modal.",
            "settings.",
            "badge.",
            "value.",
            "hotkey.",
            "combo.",
            "problem.",
            "action.",
            "dialog.",
        ];
        let src = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("src");
        let mut files = vec![src.join("app.rs"), src.join("widgets.rs")];
        for dir in ["app", "app/pages", "settings"] {
            for e in std::fs::read_dir(src.join(dir)).unwrap().flatten() {
                let p = e.path();
                // The screenshot harness names files and fixtures ("report.json"), not keys.
                if p.extension().is_some_and(|x| x == "rs") && !p.ends_with("shots.rs") {
                    files.push(p);
                }
            }
        }
        let en = |k: &str| tr_opt_in(Locale::En, k).is_some();
        let mut missing = Vec::new();
        for f in files {
            let text = std::fs::read_to_string(&f).unwrap();
            for lit in text.split('"').skip(1).step_by(2) {
                let keyish = lit.contains('.')
                    && !lit.ends_with('.')
                    && lit.chars().all(|c| {
                        c.is_ascii_lowercase() || c.is_ascii_digit() || c == '_' || c == '.'
                    })
                    && FAMILIES.iter().any(|p| lit.starts_with(p));
                if keyish && !en(lit) {
                    missing.push(format!(
                        "{}: {lit}",
                        f.file_name().unwrap().to_string_lossy()
                    ));
                }
            }
        }
        missing.sort();
        missing.dedup();
        assert!(
            missing.is_empty(),
            "keys the code names with no English string: {missing:#?}"
        );
    }

    #[test]
    fn a_missing_key_falls_back_to_english_then_to_a_readable_form() {
        // Present in both.
        assert_eq!(tr_in(Locale::En, "menu.play"), "Play");
        assert_eq!(tr_in(Locale::Ru, "menu.play"), "Играть");
        // Unknown everywhere: humanised, never empty, never a panic.
        assert_eq!(tr_in(Locale::Ru, "setting.zzz.new_thing"), "New thing");
        assert_eq!(
            tr_in(Locale::En, "setting.video.filter.opt.future_mode"),
            "Future mode"
        );
        assert_eq!(tr_opt_in(Locale::Ru, "setting.zzz.new_thing.hint"), None);
        // The fallback is stable: same pointer on a second call (cached, not re-leaked).
        assert!(std::ptr::eq(
            tr_in(Locale::En, "a.b_c"),
            tr_in(Locale::En, "a.b_c")
        ));
    }

    #[test]
    fn placeholders_are_filled() {
        assert_eq!(
            trf_in(
                Locale::En,
                "footer.launcher_updated",
                &[("from", "0.2.0"), ("to", "0.2.1")]
            ),
            "Updated launcher 0.2.0 to 0.2.1 in the background"
        );
        assert_eq!(fill("{a} {b}", &[("a", "x")]), "x {b}");
    }

    #[test]
    fn locale_codes_round_trip_and_unknown_is_english() {
        for l in Locale::ALL {
            assert_eq!(Locale::from_code(l.code()), l);
        }
        assert_eq!(Locale::from_code("de"), Locale::En);
        assert_eq!(Locale::from_code(""), Locale::En);
        assert_eq!(Locale::from_code(" RU "), Locale::Ru);
    }

    /// RL16: "Cyrillic and Polish glyphs render". egui's bundled fonts (Ubuntu-Light first, then
    /// Hack and the emoji fonts as fallbacks) are asked, glyph by glyph, whether they can draw every
    /// character the string tables use, plus the full Polish alphabet. A character no font has would
    /// paint as a hollow box -- this fails instead, naming it.
    #[test]
    fn the_bundled_fonts_cover_every_character_the_tables_use() {
        let ctx = egui::Context::default();
        let mut out = ctx.run_ui(egui::RawInput::default(), |_ui| {});
        out.textures_delta.clear();
        let mut wanted: std::collections::BTreeSet<char> = "ĄĆĘŁŃÓŚŹŻąćęłńóśźż".chars().collect();
        for src in [EN_TOML, RU_TOML] {
            let table: BTreeMap<String, String> = toml::from_str(src).unwrap();
            for v in table.values() {
                wanted.extend(v.chars());
            }
        }
        wanted.extend("↺•«»".chars());
        let font = egui::FontId::proportional(13.0);
        let missing: Vec<char> = ctx.fonts_mut(|f| {
            wanted
                .iter()
                .copied()
                .filter(|c| !c.is_control() && !f.has_glyph(&font, *c))
                .collect()
        });
        assert!(
            missing.is_empty(),
            "no glyph in the bundled fonts for: {missing:?}"
        );
    }
}
