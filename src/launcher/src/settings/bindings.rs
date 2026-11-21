//! Strategic binding metadata and validation. Defaults and physical vocabulary come from C++'s generated catalog.
use super::model::Model;
use super::schema::{Control, Kind, Page, Row, Schema, Store, SubTab};
use crate::i18n::{self, tr, trf, Locale};
use serde::Deserialize;
use std::sync::LazyLock;

#[derive(Debug, Deserialize)]
pub struct Catalog {
    pub schema: u32,
    pub actions: Vec<Action>,
    pub roles: Vec<Role>,
    pub keys: Vec<PhysicalKey>,
    pub reserved: Vec<Reserved>,
    pub exclusive_roles: Vec<[String; 2]>,
    pub legacy_navigation: Vec<LegacyNavigation>,
    pub legacy_shift_keys: Vec<String>,
}
#[derive(Debug, Deserialize)]
pub struct LegacyNavigation {
    pub key: String,
    pub default: String,
}
#[derive(Debug, Deserialize)]
pub struct PhysicalKey {
    pub token: String,
    pub scan: u8,
    pub e0: bool,
    pub vk: u32,
}
#[derive(Debug, Deserialize)]
pub struct Reserved {
    pub key: String,
    pub context: String,
    pub reason: String,
}
#[derive(Debug, Deserialize)]
pub struct Action {
    pub id: String,
    pub label_en: String,
    pub label_ru: String,
    pub category: String,
    pub context: String,
    pub kind: String,
    pub editable: bool,
    pub bindings: Vec<BindingSlot>,
    #[serde(default)]
    pub ignored_modifiers: u8,
}

#[derive(Debug, Deserialize)]
pub struct BindingSlot {
    pub section: String,
    pub key: String,
    pub default: String,
}

impl BindingSlot {
    pub fn row_id(&self) -> String {
        format!("{}.{}", self.section, self.key)
    }
}
#[derive(Debug, Deserialize)]
pub struct Role {
    pub id: String,
    pub label_en: String,
    pub label_ru: String,
    pub default: String,
    pub section: String,
    pub key: String,
    pub kind: String,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Chord {
    pub scan: u8,
    pub e0: bool,
    pub modifiers: u8,
}
#[derive(Clone, Debug)]
pub struct Issue {
    pub ids: Vec<String>,
    pub text: String,
}

static CATALOG: LazyLock<Catalog> = LazyLock::new(|| {
    let c: Catalog = serde_json::from_str(include_str!("../strategic_bindings.gen.json"))
        .expect("generated strategic bindings must parse");
    assert_eq!(c.schema, 2, "unsupported strategic binding schema");
    c
});
pub fn catalog() -> &'static Catalog {
    &CATALOG
}
pub fn label<'a>(en: &'a str, ru: &'a str) -> &'a str {
    if i18n::locale() == Locale::Ru {
        ru
    } else {
        en
    }
}
impl Action {
    pub fn label(&self) -> &str {
        label(&self.label_en, &self.label_ru)
    }
}
impl Role {
    pub fn label(&self) -> &str {
        label(&self.label_en, &self.label_ru)
    }
    pub fn row_id(&self) -> String {
        format!("{}.{}", self.section, self.key)
    }
}

pub const MAX_TEXT_BYTES: usize = 94;
fn normalized_spec(text: &str) -> Option<&str> {
    let text = text.trim_matches([' ', '\t']);
    (text.len() <= MAX_TEXT_BYTES).then_some(text)
}
pub fn parse(text: &str) -> Result<Option<Chord>, ()> {
    let text = normalized_spec(text).ok_or(())?;
    if text.is_empty() || text.eq_ignore_ascii_case("none") {
        return Ok(None);
    }
    let parts: Vec<_> = text.split('+').collect();
    let (key, modifiers) = parts.split_last().ok_or(())?;
    let mut mask = 0;
    for modifier in modifiers {
        let bit = match modifier.to_ascii_lowercase().as_str() {
            "ctrl" | "control" => 1,
            "alt" => 2,
            "shift" => 4,
            _ => return Err(()),
        };
        if mask & bit != 0 {
            return Err(());
        }
        mask |= bit;
    }
    let key = key.to_ascii_lowercase();
    let (scan, e0) = if let Some(hex) = key.strip_prefix("scan:e0:") {
        if hex.len() != 2 || !hex.bytes().all(|byte| byte.is_ascii_hexdigit()) {
            return Err(());
        }
        (u8::from_str_radix(hex, 16).map_err(|_| ())?, true)
    } else if let Some(hex) = key.strip_prefix("scan:") {
        if hex.len() != 2 || !hex.bytes().all(|byte| byte.is_ascii_hexdigit()) {
            return Err(());
        }
        (u8::from_str_radix(hex, 16).map_err(|_| ())?, false)
    } else {
        // Existing VK_ aliases prefer named tokens: VK_F1 is F1, not virtual key 0xF1.
        let named = key.strip_prefix("vk_").unwrap_or(&key);
        let physical = if let Some(physical) = catalog()
            .keys
            .iter()
            .find(|physical| physical.token.eq_ignore_ascii_case(named))
        {
            physical
        } else {
            let hex = key
                .strip_prefix("vk_")
                .map(|suffix| suffix.strip_prefix("0x").unwrap_or(suffix))
                .or_else(|| key.strip_prefix("0x"))
                .ok_or(())?;
            if hex.is_empty() || !hex.bytes().all(|byte| byte.is_ascii_hexdigit()) {
                return Err(());
            }
            let vk = u32::from_str_radix(hex, 16).map_err(|_| ())?;
            if !(1..=255).contains(&vk) {
                return Err(());
            }
            catalog()
                .keys
                .iter()
                .find(|physical| physical.vk == vk)
                .ok_or(())?
        };
        (physical.scan, physical.e0)
    };
    if !(1..=0x7f).contains(&scan) {
        return Err(());
    }
    // Bare modifier keys are roles, never a keyboard action.
    if matches!(scan, 0x1d | 0x38 | 0x2a | 0x36) || (e0 && matches!(scan, 0x5b..=0x5d)) {
        return Err(());
    }
    Ok(Some(Chord {
        scan,
        e0,
        modifiers: mask,
    }))
}
pub fn format(chord: Chord) -> String {
    let mut text = String::new();
    if chord.modifiers & 1 != 0 {
        text.push_str("Ctrl+");
    }
    if chord.modifiers & 2 != 0 {
        text.push_str("Alt+");
    }
    if chord.modifiers & 4 != 0 {
        text.push_str("Shift+");
    }
    if let Some(key) = catalog()
        .keys
        .iter()
        .find(|p| p.scan == chord.scan && p.e0 == chord.e0)
    {
        text.push_str(&key.token);
    } else if chord.e0 {
        text.push_str(&format!("Scan:E0:{:02X}", chord.scan));
    } else {
        text.push_str(&format!("Scan:{:02X}", chord.scan));
    }
    text
}
pub fn canonical(text: &str) -> Option<String> {
    parse(text)
        .ok()
        .map(|c| c.map_or_else(|| "none".into(), format))
}
pub fn modifier(text: &str) -> Option<u8> {
    let text = normalized_spec(text)?;
    if text.eq_ignore_ascii_case("none") {
        Some(0)
    } else if text.eq_ignore_ascii_case("ctrl") || text.eq_ignore_ascii_case("control") {
        Some(1)
    } else if text.eq_ignore_ascii_case("alt") {
        Some(2)
    } else if text.eq_ignore_ascii_case("shift") {
        Some(4)
    } else {
        None
    }
}
pub fn button(text: &str) -> Option<u8> {
    let text = normalized_spec(text)?;
    if text.eq_ignore_ascii_case("MouseLeft") {
        Some(1)
    } else if text.eq_ignore_ascii_case("MouseRight") {
        Some(2)
    } else {
        None
    }
}
pub fn normalize_row(id: &str, value: &str) -> Option<String> {
    let c = catalog();
    if c.actions
        .iter()
        .filter(|action| action.editable)
        .flat_map(|action| &action.bindings)
        .any(|slot| slot.row_id() == id)
    {
        return Some(canonical(value).unwrap_or_else(|| value.to_string()));
    }
    let role = c.roles.iter().find(|r| r.row_id() == id)?;
    Some(if role.kind == "modifier" {
        match modifier(value) {
            Some(0) => "none".into(),
            Some(1) => "Ctrl".into(),
            Some(2) => "Alt".into(),
            Some(4) => "Shift".into(),
            _ => value.into(),
        }
    } else {
        match button(value) {
            Some(1) => "MouseLeft".into(),
            Some(2) => "MouseRight".into(),
            _ => value.into(),
        }
    })
}
pub fn validate_value(id: &str, value: &str) -> Option<Result<(), ()>> {
    if catalog()
        .actions
        .iter()
        .filter(|action| action.editable)
        .flat_map(|action| &action.bindings)
        .any(|slot| slot.row_id() == id)
    {
        return Some(parse(value).map(|_| ()));
    }
    catalog()
        .roles
        .iter()
        .find(|r| r.row_id() == id)
        .map(|role| {
            let valid = if role.kind == "modifier" {
                modifier(value).is_some()
            } else {
                button(value).is_some()
            };
            if valid {
                Ok(())
            } else {
                Err(())
            }
        })
}
/// Relocate registered rows even when a caller parses schemas directly (e.g. the generic renderer harness).
/// Do not add catalog rows to arbitrary synthetic schemas.
pub fn move_registered_rows(schema: &mut Schema) {
    for row in &mut schema.rows {
        if row.id == VERSION_ID || normalize_row(&row.id, &row.default).is_some() {
            row.page = Page::Bindings;
            row.subtab = "bindings".into();
            row.hidden = true;
            row.restart = true;
            row.label = "bindings".into();
        }
    }
    if schema.rows.iter().any(|row| row.page == Page::Bindings)
        && !schema.subtabs.iter().any(|tab| tab.id == "bindings")
    {
        schema.subtabs.insert(
            2.min(schema.subtabs.len()),
            SubTab {
                id: "bindings".into(),
                label: "subtab.bindings".into(),
            },
        );
    }
}
/// Merge metadata into the ONE model. Existing ui.bigmap_key / hud.net_indicator_key rows are reused.
pub fn extend_schema(schema: &mut Schema) {
    let mut add = |id: String, section: &str, key: &str, default: &str| {
        if let Some(row) = schema.rows.iter_mut().find(|r| r.id == id) {
            row.page = Page::Bindings;
            row.subtab = "bindings".into();
            // Keep the registry's declared kind; binding validation/canonicalization owns its expanded vocabulary.
            row.default = default.into();
            row.restart = true;
            row.hidden = true;
            row.label = "bindings".into();
        } else {
            schema.rows.push(Row {
                id,
                store: Store::Ini {
                    section: section.into(),
                    key: key.into(),
                },
                kind: Kind::Str,
                default: default.into(),
                label: "bindings".into(),
                subtab: "bindings".into(),
                options: Vec::new(),
                experimental: false,
                restart: true,
                pending: false,
                group: None,
                control: Control::Auto,
                min: None,
                max: None,
                allow: Vec::new(),
                provider: None,
                page: Page::Bindings,
                hidden: true,
                companion: None,
                visible_if: None,
                disabled_if: None,
                mirror: None,
                host: false,
            });
        }
    };
    for action in catalog().actions.iter().filter(|action| action.editable) {
        for slot in &action.bindings {
            add(slot.row_id(), &slot.section, &slot.key, &slot.default);
        }
    }
    for role in &catalog().roles {
        add(role.row_id(), &role.section, &role.key, &role.default);
    }
    add(VERSION_ID.into(), "keys", "bindings_version", "2");
    move_registered_rows(schema);
}
fn overlaps(a: &str, b: &str) -> bool {
    a.split('|').any(|c| b.split('|').any(|d| c == d))
}
fn ignored(action: &Action, slot: &BindingSlot, text: &str) -> u8 {
    if parse(text).ok() == parse(&slot.default).ok() {
        action.ignored_modifiers
    } else {
        0
    }
}
fn collision(a: Chord, ignored_a: u8, b: Chord, ignored_b: u8) -> bool {
    a.scan == b.scan
        && a.e0 == b.e0
        && a.modifiers.count_ones() == b.modifiers.count_ones()
        && (0..8).any(|mask| {
            mask & a.modifiers == a.modifiers
                && mask & !(ignored_a | a.modifiers) == 0
                && mask & b.modifiers == b.modifiers
                && mask & !(ignored_b | b.modifiers) == 0
        })
}
pub fn issues(model: &Model) -> Vec<Issue> {
    let mut out = Vec::new();
    let mut bindings = Vec::new();
    for action in &catalog().actions {
        for (index, slot) in action.bindings.iter().enumerate() {
            let id = if action.editable {
                slot.row_id()
            } else {
                format!("fixed.{}.{}", action.id, index)
            };
            if action.editable && model.schema().row(&id).is_none() {
                continue;
            }
            let value = if action.editable {
                model.value(&id)
            } else {
                slot.default.clone()
            };
            match parse(&value) {
                Err(()) => out.push(Issue {
                    ids: vec![id],
                    text: trf(
                        "keys.unsupported_value",
                        &[("action", action.label()), ("value", &value)],
                    ),
                }),
                Ok(Some(chord)) => {
                    for reserved in &catalog().reserved {
                        if !action.editable {
                            continue;
                        }
                        if reserved.context != "*" && !overlaps(&reserved.context, &action.context)
                        {
                            continue;
                        }
                        if let Some(key) = parse(&reserved.key).ok().flatten() {
                            if chord.scan == key.scan
                                && chord.e0 == key.e0
                                && chord.modifiers & key.modifiers == key.modifiers
                            {
                                out.push(Issue {
                                    ids: vec![id.clone()],
                                    text: trf(
                                        "keys.reserved",
                                        &[
                                            ("action", action.label()),
                                            ("key", &format(chord)),
                                            (
                                                "reason",
                                                tr(&format!("keys.reserved.{}", reserved.reason)),
                                            ),
                                        ],
                                    ),
                                });
                            }
                        }
                    }
                    bindings.push((action, id, chord, ignored(action, slot, &value)));
                }
                Ok(None) => (),
            }
        }
    }
    for (index, (a, id_a, chord_a, ignored_a)) in bindings.iter().enumerate() {
        for (b, id_b, chord_b, ignored_b) in bindings.iter().skip(index + 1) {
            // An action's own slots never double-dispatch.
            if a.id != b.id
                && overlaps(&a.context, &b.context)
                && collision(*chord_a, *ignored_a, *chord_b, *ignored_b)
            {
                out.push(Issue {
                    ids: vec![id_a.clone(), id_b.clone()],
                    text: trf(
                        "keys.conflict",
                        &[
                            ("a", a.label()),
                            ("b", b.label()),
                            ("key", &format(*chord_a)),
                        ],
                    ),
                });
            }
        }
    }
    for role in &catalog().roles {
        let id = role.row_id();
        if model.schema().row(&id).is_none() {
            continue;
        }
        let value = model.value(&id);
        let valid = if role.kind == "modifier" {
            modifier(&value).is_some()
        } else {
            button(&value).is_some()
        };
        if !valid {
            out.push(Issue {
                ids: vec![id],
                text: trf(
                    "keys.unsupported_value",
                    &[("action", role.label()), ("value", &value)],
                ),
            });
        }
    }
    for pair in &catalog().exclusive_roles {
        let a = catalog()
            .roles
            .iter()
            .find(|r| r.id == pair[0])
            .expect("catalog role");
        let b = catalog()
            .roles
            .iter()
            .find(|r| r.id == pair[1])
            .expect("catalog role");
        if model.schema().row(&a.row_id()).is_none() || model.schema().row(&b.row_id()).is_none() {
            continue;
        }
        let va = normalize_row(&a.row_id(), &model.value(&a.row_id()));
        let vb = normalize_row(&b.row_id(), &model.value(&b.row_id()));
        if va == vb && va.as_deref() != Some("none") {
            out.push(Issue {
                ids: vec![a.row_id(), b.row_id()],
                text: trf("keys.role_conflict", &[("a", a.label()), ("b", b.label())]),
            });
        }
    }
    if !model.binding_navigation_changes().is_empty() && model.value(VERSION_ID) != "2" {
        out.push(Issue {
            ids: vec![VERSION_ID.into()],
            text: trf(
                "keys.navigation_migration",
                &[("bindings", &model.binding_navigation_changes().join(", "))],
            ),
        });
    }
    out
}
pub fn reset_all(model: &mut Model) {
    for action in catalog().actions.iter().filter(|action| action.editable) {
        for slot in &action.bindings {
            model.reset_row(&slot.row_id());
        }
    }
    for role in &catalog().roles {
        model.reset_row(&role.row_id());
    }
    // Explicit acknowledgement of fixed-navigation migration.
    model.set(VERSION_ID, "2");
}
pub fn role_enabled(model: &Model, id: &str) -> bool {
    let role = catalog()
        .roles
        .iter()
        .find(|r| r.id == id)
        .expect("catalog role");
    role.kind != "modifier" || modifier(&model.value(&role.row_id())) != Some(0)
}
pub fn role_value(model: &Model, id: &str) -> String {
    let role = catalog()
        .roles
        .iter()
        .find(|r| r.id == id)
        .expect("catalog role");
    let id = role.row_id();
    let raw = model.value(&id);
    display(&normalize_row(&id, &raw).unwrap_or(raw))
}

pub const VERSION_ID: &str = "keys.bindings_version";
const SHIFT_ID: &str = "keys.role_shift_modifier";

fn profile_version(ini: Option<&[u8]>) -> Option<String> {
    ini.and_then(|bytes| super::ini_io::read_value(bytes, "keys", "bindings_version"))
}

/// Named evidence retained by the model; reading never stages an edit.
pub fn navigation_migration_changes(ini: Option<&[u8]>) -> Vec<String> {
    let version = profile_version(ini);
    if version.as_deref() == Some("2") {
        return Vec::new();
    }

    let mut changed = Vec::new();
    if let Some(version) = version.as_deref() {
        if version != "1" {
            changed.push(format!("bindings_version={version}"));
        }
    }

    let Some(bytes) = ini else {
        return changed;
    };
    for legacy in &catalog().legacy_navigation {
        if let Some(value) = super::ini_io::read_value(bytes, "keys", &legacy.key) {
            if canonical(&value).is_none() || canonical(&value) != canonical(&legacy.default) {
                changed.push(format!("{}={value}", legacy.key));
            }
        }
    }
    changed
}

/// Import only into the loaded baseline. Canonical values always win.
/// Version 2 never resurrects a stale legacy key.
pub fn loaded_value(id: &str, ini: Option<&[u8]>, direct: Option<String>) -> Option<String> {
    if direct.is_some() {
        return direct;
    }
    if profile_version(ini).as_deref() == Some("2") {
        return None;
    }
    let bytes = ini?;
    if id == VERSION_ID {
        return (!navigation_migration_changes(ini).is_empty()).then(|| "1".into());
    }
    if id == SHIFT_ID {
        let keys = &catalog().legacy_shift_keys;
        let values: Vec<String> = keys
            .iter()
            .map(|key| {
                let value =
                    super::ini_io::read_value(bytes, "keys", key).unwrap_or_else(|| "Shift".into());
                normalize_row(SHIFT_ID, &value).unwrap_or(value)
            })
            .collect();

        assert_eq!(values.len(), 4, "legacy Shift role metadata");
        if values.iter().all(|value| value == &values[0]) {
            return Some(values[0].clone());
        }
        return Some(
            keys.iter()
                .zip(&values)
                .map(|(key, value)| format!("{key}={value}"))
                .collect::<Vec<_>>()
                .join(", "),
        );
    }
    let legacy_key = match id {
        "keys.chat_team_2" => "chat_team_numpad",
        "keys.chat_team_3" => "chat_team_numpad_alt",
        "keys.chat_all_2" => "chat_all_numpad",
        "keys.chat_all_3" => "chat_all_numpad_alt",
        _ => return None,
    };
    super::ini_io::read_value(bytes, "keys", legacy_key)
}

/// Applying one binding edit commits the complete canonical profile in one atomic splice.
pub fn snapshot_edits(model: &Model) -> Vec<super::ini_io::Edit> {
    let mut edits = Vec::new();
    for action in catalog().actions.iter().filter(|action| action.editable) {
        for slot in &action.bindings {
            let id = slot.row_id();
            let value = model.value(&id);
            let value = normalize_row(&id, &value).unwrap_or(value);
            edits.push(super::ini_io::Edit::new(&slot.section, &slot.key, &value));
        }
    }
    for role in &catalog().roles {
        let id = role.row_id();
        let value = model.value(&id);
        let value = normalize_row(&id, &value).unwrap_or(value);
        edits.push(super::ini_io::Edit::new(&role.section, &role.key, &value));
    }
    edits.push(super::ini_io::Edit::new("keys", "bindings_version", "2"));
    for legacy in &catalog().legacy_navigation {
        edits.push(super::ini_io::Edit::remove("keys", &legacy.key));
    }
    for key in &catalog().legacy_shift_keys {
        edits.push(super::ini_io::Edit::remove("keys", key));
    }
    for key in [
        "chat_team_numpad",
        "chat_team_numpad_alt",
        "chat_all_numpad",
        "chat_all_numpad_alt",
    ] {
        edits.push(super::ini_io::Edit::remove("keys", key));
    }
    edits
}
pub fn display(value: &str) -> String {
    if value.eq_ignore_ascii_case("none") || value.is_empty() {
        return tr("keys.unbound").into();
    }
    match value {
        "MouseLeft" => tr("keys.mouse_left").into(),
        "MouseRight" => tr("keys.mouse_right").into(),
        _ => canonical(value).unwrap_or_else(|| value.to_string()),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::settings::model::MapStore;
    fn model(bytes: Option<&[u8]>) -> Model {
        let mut model = Model::new(Schema::builtin());
        model.load(bytes, &MapStore::default());
        model
    }
    #[test]
    fn generated_defaults_aliases_and_single_storage_rows_are_complete() {
        let model = model(None);
        assert!(issues(&model).is_empty(), "{:?}", issues(&model));
        for action in &catalog().actions {
            for slot in &action.bindings {
                assert!(parse(&slot.default).is_ok(), "{}", action.id);
                let id = slot.row_id();
                if !action.editable {
                    assert!(model.schema().row(&id).is_none());
                    continue;
                }
                assert_eq!(
                    model
                        .schema()
                        .rows
                        .iter()
                        .filter(|row| row.id == id)
                        .count(),
                    1
                );
                assert_eq!(model.value(&id), slot.default);
                assert_eq!(model.schema().row(&id).unwrap().page, Page::Bindings);
            }
        }
        for role in &catalog().roles {
            assert_eq!(validate_value(&role.row_id(), &role.default), Some(Ok(())));
        }
        assert_eq!(
            model
                .schema()
                .rows
                .iter()
                .filter(|row| row.id == "ui.bigmap_key")
                .count(),
            1
        );
        assert_eq!(
            model
                .schema()
                .rows
                .iter()
                .filter(|row| row.id == "hud.net_indicator_key")
                .count(),
            1
        );
    }
    #[test]
    fn physical_aliases_keep_e0_and_reject_modifiers_windows_and_unknown_vk() {
        assert_eq!(parse("Up"), parse("Scan:E0:48"));
        assert_eq!(parse("Numpad8"), parse("Scan:48"));
        assert_ne!(parse("Up"), parse("Numpad8"));
        assert_eq!(parse("control+alt+n"), parse("Ctrl+Alt+Scan:31"));
        for (legacy, canonical) in [
            ("VK_F1", "F1"),
            ("VK_A", "A"),
            ("VK_Tab", "Tab"),
            ("VK_41", "A"),
            ("VK_31", "1"),
            ("VK_0x41", "A"),
            ("0x41", "A"),
            ("0x9", "Tab"),
            ("0x09", "Tab"),
            ("0x0009", "Tab"),
        ] {
            assert_eq!(parse(legacy), parse(canonical), "{legacy}");
        }
        for value in [
            "Ctrl+Ctrl+Q",
            "Scan:80",
            "Scan:E0:5B",
            "Ctrl",
            "Win+Q",
            "VK_FF",
            "Scan:1",
            "Scan:001",
            "Scan:E0:2A",
            "Scan:E0:36",
            "Ctrl +A",
            "Ctrl+ A",
            "0x9junk",
        ] {
            assert!(parse(value).is_err(), "{value}");
        }
    }
    #[test]
    fn collisions_are_contextual_and_never_unbind_another_action() {
        let mut model = model(None);
        model.set("keys.follow_selection", "Ctrl+F3");
        model.set("keys.build_panel", "Ctrl+F3");
        assert!(issues(&model).iter().any(|issue| {
            issue.ids.contains(&"keys.follow_selection".into())
                && issue.ids.contains(&"keys.build_panel".into())
        }));
        assert_eq!(model.value("keys.build_panel"), "Ctrl+F3");
        assert_eq!(model.value("keys.follow_selection"), "Ctrl+F3");
        assert!(model.has_problems());
    }
    #[test]
    fn role_sharing_is_allowed_only_when_semantics_are_disjoint() {
        let mut model = model(None);
        model.set("keys.role_shift_modifier", "Ctrl");
        assert!(!issues(&model).is_empty());
        model.revert();
        model.set("keys.role_patrol", "Shift");
        assert!(!issues(&model).is_empty());
        model.revert();
        model.set("keys.role_command_button", "MouseLeft");
        assert!(!issues(&model).is_empty());
        assert_eq!(model.value("keys.role_select_button"), "MouseLeft");
        model.revert();
        model.set("keys.role_shift_modifier", "none");
        model.set("keys.role_confirm", "none");
        assert!(
            issues(&model).is_empty(),
            "disabled roles do not conflict or become unmodified"
        );
        assert!(!role_enabled(&model, "shift_modifier") && !role_enabled(&model, "confirm"));
        model.set("keys.role_select_button", "none");
        assert!(!issues(&model).is_empty(), "button roles remain mandatory");
    }
    #[test]
    fn reserved_chords_block_without_rejecting_native_modifier_wildcards() {
        let mut model = model(None);
        assert!(
            issues(&model).is_empty(),
            "Tab/Delete defaults retain their native wildcard policy"
        );
        model.set("keys.follow_selection", "F11");
        assert!(!issues(&model).is_empty());
        model.revert();
        model.set("keys.follow_selection", "Shift+Enter");
        assert!(!issues(&model).is_empty());
        assert_eq!(model.value("keys.chat_team"), "Enter");
    }
    #[test]
    fn reset_all_stages_and_revert_restores_saved_aliases() {
        let mut model = model(Some(
            b"; untouched\r\n[ui]\r\nbigmap_key=F2\r\n[keys]\r\ncamera_up_alt=F3\r\n",
        ));
        reset_all(&mut model);
        assert!(model.is_dirty());
        assert_eq!(model.value("ui.bigmap_key"), "Tab");
        assert_eq!(model.value("keys.camera_up_alt"), "Numpad8");
        assert_eq!(
            model.raw("ui.bigmap_key"),
            Some("F2"),
            "no eager disk replacement"
        );
        model.revert();
        assert_eq!(model.value("ui.bigmap_key"), "F2");
        assert_eq!(model.value("keys.camera_up_alt"), "F3");
    }
    #[test]
    fn shared_atomic_apply_preserves_unrelated_bytes_and_authoritative_keys() {
        let root = std::env::temp_dir().join(format!("mh_binding_model_{}", std::process::id()));
        std::fs::create_dir_all(&root).unwrap();
        let path = root.join("mh_net.ini");
        let before =
            b"; keep\r\n[unknown]\r\nopaque=xyz ; exact\r\n[ui]\r\nbigmap_key=Tab ; old alias\r\n";
        std::fs::write(&path, before).unwrap();
        let mut model = model(Some(before));
        model.set("ui.bigmap_key", "F2");
        model.set("keys.bigmap_key_alt", "F3");
        model.apply(&path, &mut MapStore::default()).unwrap();
        let text = std::fs::read_to_string(&path).unwrap();
        assert!(text.contains("; keep\r\n[unknown]\r\nopaque=xyz ; exact\r\n"));
        assert!(text.contains("bigmap_key=F2 ; old alias"));
        assert_eq!(text.matches("bigmap_key=").count(), 1);
        assert!(text.contains("[keys]") && text.contains("bigmap_key_alt=F3"));
        assert!(!model.is_dirty());
        std::fs::remove_dir_all(root).unwrap();
    }
    #[test]
    fn legacy_chat_slots_import_losslessly_without_dirtying() {
        let model = model(Some(
            b"[keys]\r\n\
chat_team=F1\r\n\
chat_team_alt=F2\r\n\
chat_team_numpad=F3\r\n\
chat_team_numpad_alt=F4\r\n\
chat_all=F5\r\n\
chat_all_alt=F6\r\n\
chat_all_numpad=F7\r\n\
chat_all_numpad_alt=F8\r\n",
        ));
        for (id, expected) in [
            ("keys.chat_team", "F1"),
            ("keys.chat_team_alt", "F2"),
            ("keys.chat_team_2", "F3"),
            ("keys.chat_team_3", "F4"),
            ("keys.chat_all", "F5"),
            ("keys.chat_all_alt", "F6"),
            ("keys.chat_all_2", "F7"),
            ("keys.chat_all_3", "F8"),
        ] {
            assert_eq!(model.value(id), expected);
        }
        assert!(!model.is_dirty());
    }

    #[test]
    fn divergent_legacy_shift_requires_explicit_choice_and_revert_restores_it() {
        let mut model = model(Some(b"[keys]\r\nrole_queue=Ctrl\r\n"));
        assert!(!model.is_dirty());
        assert!(issues(&model)
            .iter()
            .any(|issue| issue.ids.contains(&SHIFT_ID.into())));
        model.set(SHIFT_ID, "Shift");
        assert!(model.is_dirty());
        assert!(issues(&model).is_empty());
        model.revert();
        assert!(!model.is_dirty());
        assert!(!issues(&model).is_empty());
    }

    #[test]
    fn navigation_acknowledgement_preserves_other_custom_bindings() {
        let mut model = model(Some(
            b"[keys]\r\nsave_confirm=F3\r\nfollow_selection=F4\r\n",
        ));
        assert!(!model.is_dirty());
        assert!(issues(&model)
            .iter()
            .any(|issue| issue.ids.contains(&VERSION_ID.into())));

        model.set(VERSION_ID, "2");
        assert!(model.is_dirty());
        assert_eq!(model.value("keys.follow_selection"), "F4");
        assert!(issues(&model).is_empty());

        let edits = model.ini_edits();
        assert!(edits
            .iter()
            .any(|edit| { !edit.remove && edit.key == "follow_selection" && edit.value == "F4" }));
        assert!(edits
            .iter()
            .any(|edit| { edit.remove && edit.key == "save_confirm" }));

        model.revert();
        assert!(!model.is_dirty());
        assert_eq!(model.value("keys.follow_selection"), "F4");
        assert!(!issues(&model).is_empty());
    }

    #[test]
    fn canonical_version_never_resurrects_legacy_roles_or_slots() {
        let model = model(Some(
            b"[keys]\r\n\
bindings_version=2\r\n\
role_queue=Ctrl\r\n\
chat_all_numpad=F3\r\n\
save_confirm=F4\r\n",
        ));
        assert_eq!(model.value(SHIFT_ID), "Shift");
        assert_eq!(model.value("keys.chat_all_2"), "Ctrl+NumpadEnter");
        assert!(model.binding_navigation_changes().is_empty());
        assert!(!model.is_dirty());
    }

    #[test]
    fn one_binding_edit_emits_complete_canonical_snapshot() {
        let mut model = model(None);
        model.set("keys.follow_selection", "F3");
        let edits = model.ini_edits();
        assert!(edits.iter().any(|edit| {
            edit.section == "keys" && edit.key == "bindings_version" && edit.value == "2"
        }));
        for action in catalog().actions.iter().filter(|action| action.editable) {
            for slot in &action.bindings {
                assert!(edits
                    .iter()
                    .any(|edit| { edit.section == slot.section && edit.key == slot.key }));
            }
        }
        for role in &catalog().roles {
            assert!(edits
                .iter()
                .any(|edit| { edit.section == role.section && edit.key == role.key }));
        }
    }
}

#[cfg(test)]
mod shared_parse_corpus {
    use super::*;
    #[derive(Deserialize)]
    struct Cases {
        schema: u32,
        max_text_bytes: usize,
        cases: Vec<Case>,
        role_cases: Vec<RoleCase>,
    }
    #[derive(Deserialize)]
    struct Case {
        text: String,
        accepted: bool,
        #[serde(default)]
        physical: u16,
        #[serde(default)]
        modifiers: u8,
    }
    #[derive(Deserialize)]
    struct RoleCase {
        text: String,
        kind: String,
        accepted: bool,
        value: Option<u8>,
    }
    #[test]
    fn rust_matches_the_common_acceptance_and_physical_identity_corpus() {
        let cases: Cases = serde_json::from_str(include_str!(
            "../../../../tools/data/strategic_bindings_parse_cases.json"
        ))
        .unwrap();
        assert_eq!(cases.schema, 1);
        assert_eq!(cases.max_text_bytes, MAX_TEXT_BYTES);
        for case in cases.cases {
            let actual = parse(&case.text);
            assert_eq!(actual.is_ok(), case.accepted, "{:?}", case.text);
            if let Ok(actual) = actual {
                let (physical, modifiers) = actual.map_or((0, 0), |chord| {
                    (
                        u16::from(chord.scan) | if chord.e0 { 0x80 } else { 0 },
                        chord.modifiers,
                    )
                });
                assert_eq!(
                    (physical, modifiers),
                    (case.physical, case.modifiers),
                    "{:?}",
                    case.text
                );
            }
        }
        for case in cases.role_cases {
            let actual = match case.kind.as_str() {
                "modifier" => modifier(&case.text),
                "button" => button(&case.text),
                kind => panic!("unknown corpus role kind {kind}"),
            };
            assert_eq!(actual.is_some(), case.accepted, "{:?}", case.text);
            if case.accepted {
                assert_eq!(actual, case.value, "{:?}", case.text);
            }
        }
    }
}
