//! egui_kittest checks of the settings page, and the review screenshots (dist RL12/RL13).
//!
//! Two tiers, so the plain `cargo test` stays GPU-free:
//!
//! * the interaction tests drive a real egui frame headlessly (no renderer) and read the page
//!   through its accessibility tree: rows appear under their sub-tab, a click dirties the model,
//!   a synthetic registry row shows up with no code change (the RL13 done_when, end to end);
//! * `review_screenshots` renders PNGs through wgpu (D3D/WARP, the launcher's own fallback path) and
//!   runs only when `MH_UI_REVIEW_DIR` names a directory, which it fills: every Settings sub-tab
//!   (compact and detailed, `standin_*` -- the real shell's own shots, app/pages/shots.rs, take the
//!   plain `settings_*` names) and the Diagnostics page in English and Russian, a row's hover
//!   description, an error state, the widget kit in a frame, and a modal. Those are what the user reviews (UI-layout rule); nothing is auto-diffed against a
//!   baseline yet because WU-C owns the page layout this stands in for.
//!
//! The stand-in launcher window below (menu column + game frame) is test scaffolding for exactly
//! that purpose; it is NOT the shell. WU-C builds the real one in `app/shell.rs`.

use egui::{pos2, vec2, Align, Color32, Layout, Rect, RichText, Ui, UiBuilder};
use egui_kittest::{kittest::Queryable, Harness};

use super::model::{MapStore, Model, ProviderOption};
use super::render::{self, RenderState};
use super::schema::{Provider, Schema, GAME_SCHEMA_JSON, LAUNCHER_SCHEMA_JSON};
use crate::i18n::{self, tr, trf, Locale};
use crate::{theme, widgets};

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Pg {
    Settings,
    Diagnostics,
    Kit,
}

struct Fx {
    model: Model,
    state: RenderState,
    store: MapStore,
    page: Pg,
    modal: bool,
}

const INI: &str = "; Mission Humanity configuration\r\n\
[video]\r\n\
window=borderless      ; borderless | windowed\r\n\
scale=fit\r\n\
filter=area\r\n\
vsync=1\r\n\
fps_limit=0\r\n\
mouse_clip=1\r\n\
picker=0\r\n\
[lang]\r\n\
pack=ru\r\n\
[net]\r\n\
force_relay=0\r\n\
port=6501\r\n\
[hud]\r\n\
net_indicator=1\r\n\
net_indicator_key=Ctrl+Alt+N\r\n\
[log]\r\n\
level=debug\r\n\
[video2]\r\n\
x=1\r\n";

fn model_from(schema: Schema) -> Model {
    let mut m = Model::new(schema);
    // A pinned 1920x1080 @ 100% desktop, so the review shots show the same list on every machine.
    m.set_provider_options(
        Provider::Resolutions,
        super::resolutions::options(
            Some(super::resolutions::Desktop {
                w: 1920,
                h: 1080,
                scale_pct: 100,
            }),
            tr("setting.video.resolution.auto"),
            tr("setting.video.resolution.classic"),
        ),
    );
    m.set_provider_options(
        Provider::LangPacks,
        vec![
            ProviderOption {
                value: String::new(),
                label: "English".into(),
            },
            ProviderOption {
                value: "ru".into(),
                label: "Русский".into(),
            },
            ProviderOption {
                value: "pl".into(),
                label: "Polski".into(),
            },
        ],
    );
    m
}

fn fixture(schema: Schema) -> Fx {
    let store = MapStore::with(&[("channel", "stable"), ("discord", "true")]);
    let mut model = model_from(schema);
    model.load(Some(INI.as_bytes()), &store);
    Fx {
        model,
        state: RenderState::default(),
        store,
        page: Pg::Settings,
        modal: false,
    }
}

fn menu_label(i: usize) -> &'static str {
    tr([
        "menu.play",
        "menu.settings",
        "menu.report",
        "menu.about",
        "menu.diagnostics",
    ][i])
}

/// The stand-in launcher window: starfield, menu column on the left, game frame on the right.
fn window(ui: &mut Ui, fx: &mut Fx) {
    theme::apply(ui.ctx());
    let full = ui.max_rect();
    theme::paint_background(ui.painter(), full);
    let pad = 18.0;
    let side = Rect::from_min_size(
        full.min + vec2(pad, pad),
        vec2(200.0, full.height() - 2.0 * pad),
    );
    let main = Rect::from_min_max(
        pos2(side.right() + 18.0, side.top()),
        full.max - vec2(pad, pad),
    );
    ui.scope_builder(UiBuilder::new().max_rect(side), |ui| {
        ui.spacing_mut().item_spacing.y = 10.0;
        ui.label(
            RichText::new(format!(
                "{}\n{}",
                tr("app.logo_line1"),
                tr("app.logo_line2")
            ))
            .size(20.0)
            .color(Color32::from_rgb(0xcc, 0xff, 0xee))
            .extra_letter_spacing(3.0),
        );
        ui.label(
            RichText::new(tr("app.logo_sub").to_uppercase())
                .size(10.0)
                .color(theme::TEXT_DIM)
                .extra_letter_spacing(3.0),
        );
        let current = match fx.page {
            Pg::Settings => 1,
            Pg::Diagnostics => 4,
            Pg::Kit => 0,
        };
        for i in 0..5 {
            let dot = i == 1 && fx.model.is_dirty();
            if widgets::menu_button(ui, menu_label(i), i == current, dot).clicked() {
                fx.page = match i {
                    1 => Pg::Settings,
                    4 => Pg::Diagnostics,
                    _ => Pg::Kit,
                };
            }
        }
        ui.with_layout(Layout::bottom_up(Align::Center), |ui| {
            ui.label(
                RichText::new(trf(
                    "app.version_line",
                    &[("version", "0.2.0"), ("channel", tr("channel.stable"))],
                ))
                .monospace()
                .size(11.0)
                .color(theme::TEXT_DIM),
            );
            let reason = fx.model.is_dirty().then(|| tr("start.dirty"));
            if widgets::start_button(ui, tr("button.start_game"), reason).clicked() {
                fx.modal = true;
            }
        });
    });
    ui.scope_builder(UiBuilder::new().max_rect(main), |ui| match fx.page {
        Pg::Settings => {
            widgets::game_frame(ui, Some(tr("page.settings.title")), true, |ui| {
                render::show_settings(ui, &mut fx.model, &mut fx.state);
                ui.add_space(6.0);
                // No Apply / Revert here: the real launcher saves from the shell's save bar only
                // (user 2026-10-09; the in-page strip was review-shot-only and removed 2026-10-11).
                widgets::hint(
                    ui,
                    &trf(
                        "settings.saved_to",
                        &[
                            ("path", "%LOCALAPPDATA%\\MissionHumanity\\games\\3f2a9c7e41d85b06\\mh_net.ini"),
                            ("source", tr("cfgdir.user")),
                        ],
                    ),
                );
            });
        }
        Pg::Diagnostics => {
            widgets::game_frame(ui, Some(tr("page.diagnostics.title")), true, |ui| {
                widgets::heading(ui, tr("diag.heading"));
                render::show_diagnostics(ui, &mut fx.model, &mut fx.state);
                widgets::group_header(ui, tr("diag.build"));
                ui.label(
                    RichText::new("mh.dll 0.2.0+20f0cf28 · launcher 0.2.0 · manifest 2026-10-08 · lang ru")
                        .monospace()
                        .size(12.0),
                );
                widgets::group_header(ui, tr("diag.last_launch"));
                ui.label(
                    RichText::new("exit 0 · 41 min · 3 sessions · no crash marker")
                        .monospace()
                        .size(12.0),
                );
                ui.add_space(6.0);
                ui.horizontal_wrapped(|ui| {
                    widgets::gbtn_dev(ui, tr("diag.reverify"));
                    widgets::gbtn_dev(ui, tr("diag.rollback"));
                    widgets::gbtn(ui, tr("diag.copy"));
                });
            });
        }
        Pg::Kit => {
            widgets::game_frame(ui, Some(tr("page.play.title")), true, |ui| {
                ui.columns(2, |cols| {
                    widgets::card(&mut cols[0], tr("play.game_version"), "0.2.0 · up to date", theme::OK);
                    widgets::card(&mut cols[1], tr("play.channel"), "Stable", theme::TEXT);
                });
                ui.columns(2, |cols| {
                    widgets::card(&mut cols[0], tr("play.relay"), "Automatic", theme::OK);
                    widgets::card(&mut cols[1], tr("play.discord"), "Off", theme::TEXT);
                });
                widgets::group_header(ui, tr("play.game_folder"));
                ui.label(RichText::new("C:\\Games\\Mission Humanity").monospace().size(12.0));
                widgets::hint(ui, "English retail found. Language pack: Russian (built from your files).");
                widgets::group_header(ui, "Pills and buttons");
                ui.horizontal_wrapped(|ui| {
                    widgets::pill(ui, tr("badge.experimental"), theme::ALERT);
                    widgets::pill(ui, tr("report.outcome.desync"), theme::ALERT);
                    widgets::pill(ui, tr("report.outcome.finished"), theme::TEXT);
                });
                ui.horizontal_wrapped(|ui| {
                    widgets::gbtn(ui, tr("btn.change"));
                    widgets::gbtn(ui, tr("report.review"));
                    widgets::gbtn_dev(ui, tr("diag.rollback"));
                });
                widgets::seg(ui, &["Quiet", "Normal", "Debug"], Some(1));
                widgets::toggle(ui, true);
            });
        }
    });
    if fx.modal {
        let out = widgets::modal(ui.ctx(), "shots_modal", tr("modal.dirty.title"), |ui| {
            ui.label(tr("modal.dirty.body"));
            ui.add_space(6.0);
            ui.horizontal(|ui| {
                widgets::gbtn(ui, tr("btn.apply"));
                widgets::gbtn(ui, tr("btn.discard"));
                widgets::gbtn(ui, tr("btn.cancel"))
            })
            .inner
        });
        if out.close_requested || out.inner.clicked() {
            fx.modal = false;
        }
    }
}

fn harness(fx: Fx) -> Harness<'static, Fx> {
    Harness::builder()
        .with_size(vec2(800.0, 700.0))
        .with_pixels_per_point(1.0)
        .build_ui_state(window, fx)
}

// ---- interaction (no GPU) ------------------------------------------------------------------------

#[test]
fn the_resolution_row_defaults_to_classic_sizes_and_keeps_an_explicit_auto() {
    // The shipped fixture ini has no `resolution` key: a fresh install reads the default, `stock`.
    let m = fixture(Schema::builtin()).model;
    assert_eq!(m.value("video.resolution"), "stock");
    // An existing ini that says `resolution=auto` keeps Auto.
    let mut m = model_from(Schema::builtin());
    let store = MapStore::with(&[]);
    m.load(
        Some(
            b"[video]
resolution=auto
",
        ),
        &store,
    );
    assert_eq!(m.value("video.resolution"), "auto");
}

#[test]
fn interface_size_is_greyed_with_a_hint_under_classic_sizes_and_live_again_under_auto() {
    i18n::set_locale(Locale::En);
    let mut h = harness(fixture(Schema::builtin())); // the fixture ini has no resolution key: Classic sizes only
    h.run();
    assert!(
        h.query_by_label("Interface size").is_some(),
        "greyed, not hidden"
    );
    assert!(
        h.query_by_label_contains("Not used with Classic sizes only")
            .is_some(),
        "the reason is drawn under the row"
    );
    assert!(!h.state().model.is_dirty(), "nothing was written or reset");
    h.state_mut().model.set("video.resolution", "auto");
    h.run();
    assert!(h.query_by_label("Interface size").is_some());
    assert!(
        h.query_by_label_contains("Not used with Classic sizes only")
            .is_none(),
        "the hint goes away with Auto"
    );
    i18n::set_locale(Locale::Ru);
    h.state_mut().model.set("video.resolution", "stock");
    h.run();
    assert!(
        h.query_by_label_contains("Не используется при").is_some(),
        "the Russian pack carries the hint too"
    );
    i18n::set_locale(Locale::En);
}

#[test]
fn display_rows_show_and_a_click_makes_the_page_dirty() {
    i18n::set_locale(Locale::En);
    let mut h = harness(fixture(Schema::builtin()));
    h.run();
    assert!(h.query_by_label("Window").is_some());
    assert!(h.query_by_label("V-sync").is_some());
    // The Resolution row is the page's first row, with its list from the pinned desktop.
    assert!(h.query_by_label("Game resolution").is_some());
    // (The pills of an experimental row are asserted by `a_new_registry_row_renders_with_no_launcher_change`:
    // no shipped Display row is experimental any more.)
    assert!(!h.state().model.is_dirty());
    h.get_by_label("Windowed").click();
    h.run();
    assert_eq!(h.state().model.value("video.window"), "windowed");
    assert_eq!(h.state().model.dirty_count(), 1);
    // The Settings menu entry now carries the unsaved marker via the Start button's disabled reason.
    // Revert lives in the shell's save bar (app/pages/shots.rs covers it); this fixture reverts directly.
    h.state_mut().model.revert();
    h.run();
    assert!(!h.state().model.is_dirty());
    assert_eq!(h.state().model.value("video.window"), "borderless");
}

#[test]
fn sub_tabs_swap_the_rows() {
    i18n::set_locale(Locale::En);
    let mut h = harness(fixture(Schema::builtin()));
    h.run();
    assert!(h.query_by_label("Host port").is_none());
    h.get_by_label("Multiplayer").click();
    h.run();
    assert!(h.query_by_label("Host port").is_some());
    assert!(h.query_by_label("Use relay server").is_some());
    assert!(h.query_by_label("Window").is_none());
    // The ini's own [net] relay is never offered.
    assert!(h.query_by_label("Relay address").is_none());
    h.get_by_label("Updates").click();
    h.run();
    assert!(h.query_by_label("Update channel").is_some());
    assert!(h.query_by_label("Discord status").is_some());
}

#[test]
fn a_bad_port_blocks_apply_until_fixed() {
    i18n::set_locale(Locale::En);
    let mut fx = fixture(Schema::builtin());
    fx.state.subtab = "multiplayer".into();
    fx.model.set("net.port", "22");
    let mut h = harness(fx);
    h.run();
    assert!(h
        .query_by_label("Enter a number from 1024 to 65535.")
        .is_some());
    assert!(h.state().model.has_problems());
    h.state_mut().model.set("net.port", "7000");
    h.run();
    assert!(h
        .query_by_label("Enter a number from 1024 to 65535.")
        .is_none());
    assert!(!h.state().model.has_problems());
}

#[test]
fn the_custom_relay_address_box_appears_only_in_custom_mode() {
    i18n::set_locale(Locale::En);
    let mut fx = fixture(Schema::builtin());
    fx.state.subtab = "multiplayer".into();
    let mut h = harness(fx);
    h.run();
    assert!(h.query_by_label("Address (host:port)").is_none());
    h.get_by_label("Custom").click();
    h.run();
    assert!(h.query_by_label("Address (host:port)").is_some());
    assert_eq!(h.state().model.value("launcher.relay_mode"), "custom");
}

#[test]
fn the_hotkey_row_captures_a_chord_and_esc_cancels() {
    i18n::set_locale(Locale::En);
    let mut fx = fixture(Schema::builtin());
    fx.state.subtab = "multiplayer".into();
    let mut h = harness(fx);
    h.run();
    h.get_by_label("Ctrl+Alt+N").click();
    h.run();
    assert!(h.query_by_label("Press a key...").is_some(), "capturing");
    h.key_press_modifiers(egui::Modifiers::CTRL | egui::Modifiers::SHIFT, egui::Key::K);
    h.run();
    assert_eq!(
        h.state().model.value("hud.net_indicator_key"),
        "Ctrl+Shift+K"
    );
    assert!(h.query_by_label("Press a key...").is_none());
    // Esc leaves the value alone.
    h.get_by_label("Ctrl+Shift+K").click();
    h.run();
    h.key_press(egui::Key::Escape);
    h.run();
    assert_eq!(
        h.state().model.value("hud.net_indicator_key"),
        "Ctrl+Shift+K"
    );
    assert!(h.query_by_label("Press a key...").is_none());
    // Clear writes `none`, which the game reads as "no hotkey".
    h.get_by_label("Clear").click();
    h.run();
    assert_eq!(h.state().model.value("hud.net_indicator_key"), "none");
}

/// The big-map key is documented as "hold Tab": Tab must be capturable (not eaten by focus
/// traversal), a different key must be settable, and Revert must restore the value AND the text.
#[test]
fn the_bigmap_hotkey_takes_tab_and_revert_restores_it() {
    i18n::set_locale(Locale::En);
    let mut fx = fixture(Schema::builtin());
    fx.model
        .load(Some(b"[ui]\r\nbigmap=1\r\nbigmap_key=F9\r\n"), &fx.store);
    fx.state.subtab = fx
        .model
        .schema()
        .row("ui.bigmap_key")
        .unwrap()
        .subtab
        .clone();
    let mut h = harness(fx);
    h.run();
    assert_eq!(h.state().model.value("ui.bigmap_key"), "F9");
    // Tab while listening is captured, not used to move focus.
    h.get_by_label("F9").click();
    h.run();
    h.run();
    assert!(h.query_by_label("Press a key...").is_some(), "capturing");
    h.key_press(egui::Key::Tab);
    h.run();
    assert_eq!(h.state().model.value("ui.bigmap_key"), "Tab");
    assert!(h.query_by_label("Press a key...").is_none());
    assert!(h.query_by_label("Tab").is_some());
    // Another key, then Revert: back to what is on disk, in the model and on the button.
    h.get_by_label("Tab").click();
    h.run();
    h.run();
    h.key_press(egui::Key::O);
    h.run();
    assert_eq!(h.state().model.value("ui.bigmap_key"), "O");
    assert!(h.query_by_label("O").is_some());
    h.state_mut().model.revert();
    h.run();
    assert_eq!(h.state().model.value("ui.bigmap_key"), "F9");
    assert!(h.query_by_label("F9").is_some(), "text refreshed");
    assert!(h.query_by_label("O").is_none());
    // Escape still cancels a capture.
    h.get_by_label("F9").click();
    h.run();
    h.run();
    h.key_press(egui::Key::Escape);
    h.run();
    assert!(h.query_by_label("Press a key...").is_none());
    assert_eq!(h.state().model.value("ui.bigmap_key"), "F9");
}

/// The shipped default (`Tab`, no key in the ini): set another key, Revert, and Tab is back.
#[test]
fn reverting_the_bigmap_hotkey_restores_the_default_tab() {
    i18n::set_locale(Locale::En);
    let mut fx = fixture(Schema::builtin());
    fx.model.load(
        Some(
            b"[ui]
bigmap=1
",
        ),
        &fx.store,
    );
    fx.state.subtab = fx
        .model
        .schema()
        .row("ui.bigmap_key")
        .unwrap()
        .subtab
        .clone();
    let mut h = harness(fx);
    h.run();
    assert!(h.query_by_label("Tab").is_some());
    h.get_by_label("Tab").click();
    h.run();
    h.run();
    h.key_press(egui::Key::O);
    h.run();
    assert_eq!(h.state().model.value("ui.bigmap_key"), "O");
    assert!(h.state().model.is_dirty());
    h.state_mut().model.revert();
    h.run();
    assert_eq!(h.state().model.value("ui.bigmap_key"), "Tab");
    assert!(h.query_by_label("Tab").is_some(), "text refreshed");
    assert!(h.query_by_label("O").is_none());
}

/// A hotkey row that does not opt in to named keys still refuses Tab (the game would read it as T).
#[test]
fn the_net_indicator_hotkey_still_refuses_tab_and_keeps_focus_traversal_off() {
    i18n::set_locale(Locale::En);
    let mut fx = fixture(Schema::builtin());
    fx.state.subtab = "multiplayer".into();
    let mut h = harness(fx);
    h.run();
    h.get_by_label("Ctrl+Alt+N").click();
    h.run();
    h.run();
    h.key_press(egui::Key::Tab);
    h.run();
    assert_eq!(h.state().model.value("hud.net_indicator_key"), "Ctrl+Alt+N");
}

/// RL13 done_when, rendered: a registry row nobody wrote code or strings for appears in its
/// sub-tab with a humanised label.
#[test]
fn a_new_registry_row_renders_with_no_launcher_change() {
    i18n::set_locale(Locale::En);
    let mut doc: serde_json::Value = serde_json::from_str(GAME_SCHEMA_JSON).unwrap();
    doc["settings"]
        .as_array_mut()
        .unwrap()
        .push(serde_json::json!({
            "section": "video", "key": "zoom_snap", "type": "bool", "default": 1,
            "label": "video.zoom_snap", "subtab": "display", "options": [],
            "experimental": true, "restart": true, "pending": false
        }));
    let schema = Schema::from_json(&doc.to_string(), Some(LAUNCHER_SCHEMA_JSON)).unwrap();
    let mut h = harness(fixture(schema));
    h.run();
    assert!(
        h.query_by_label("Zoom snap").is_some(),
        "humanised label of the new row"
    );
    // The synthetic experimental row carries the pill, next to the one shipped experimental Display row.
    assert_eq!(h.query_all_by_label("EXPERIMENTAL").count(), 2);
    // The shipped experimental Display rows are an explicit list, not "any row".
    let shipped: Vec<String> = h
        .state()
        .model
        .schema()
        .rows_in_subtab("display")
        .filter(|r| r.experimental && r.id != "video.zoom_snap")
        .map(|r| r.id.clone())
        .collect();
    assert_eq!(shipped, ["video.ui_scale"]);
}

#[test]
fn descriptions_are_compact_by_default_and_inline_when_detailed() {
    i18n::set_locale(Locale::En);
    let hint = tr("setting.video.fps_limit.hint");
    let mut h = harness(fixture(Schema::builtin()));
    h.run();
    assert!(!h.state().state.detailed);
    assert!(
        h.query_by_label(hint).is_none(),
        "compact: no inline description"
    );
    // The display page has restart rows: the one note above the rows says so.
    assert!(h
        .query_by_label("Changes apply at the next game start.")
        .is_some());
    h.get_by_label("Detailed").click();
    h.run();
    assert!(h.state().state.detailed);
    assert!(h.query_by_label(hint).is_some(), "detailed: inline");
    h.get_by_label("Compact").click();
    h.run();
    assert!(h.query_by_label(hint).is_none());
    assert!(
        !h.state().model.is_dirty(),
        "the view switch is not a setting edit"
    );
}

#[test]
fn the_label_column_fits_the_widest_label_and_values_line_up() {
    for loc in [Locale::En, Locale::Ru] {
        i18n::set_locale(loc);
        let mut fx = fixture(Schema::builtin());
        fx.state.subtab = "gameplay".into();
        let mut h = harness(fx);
        h.run();
        // Every row's first "Off" segment starts at the same x, right of the widest label.
        let offs: Vec<egui::Rect> = h
            .query_all_by_label(tr("value.off"))
            .map(|n| n.rect())
            .collect();
        assert!(offs.len() >= 5, "{loc:?}: {} Off segments", offs.len());
        let x0 = offs[0].left();
        assert!(
            offs.iter().all(|r| (r.left() - x0).abs() < 1.0),
            "{loc:?}: values not aligned: {offs:?}"
        );
        let labels: Vec<String> = h
            .state()
            .model
            .schema()
            .rows_in_subtab("gameplay")
            .filter(|r| h.state().model.is_visible(r))
            .map(|r| tr(&r.label_key()).to_string())
            .collect();
        for label in labels {
            let l = h.get_by_label(&label).rect();
            assert!(
                l.right() < x0,
                "{loc:?}: label {l:?} overlaps the values at {x0}"
            );
            // English gameplay labels all fit on one line at 800x600.
            if loc == Locale::En {
                assert!(l.height() < 24.0, "{label}: wrapped: {l:?}");
            }
        }
    }
    i18n::set_locale(Locale::En);
}

#[test]
fn russian_relabels_the_page() {
    i18n::set_locale(Locale::Ru);
    let mut h = harness(fixture(Schema::builtin()));
    h.run();
    assert!(h.query_by_label("Окно").is_some());
    assert!(h.query_by_label("Window").is_none());
    i18n::set_locale(Locale::En);
}

// ---- screenshots (opt-in) ------------------------------------------------------------------------

fn save(h: &mut Harness<'_, Fx>, dir: &std::path::Path, name: &str) {
    h.run();
    let img = h.render().expect("render");
    assert_eq!((img.width(), img.height()), (800, 600));
    // Not a blank frame: the phosphor green and the hazard yellow are both on it.
    let near = |p: [u8; 4], c: Color32| {
        (i32::from(p[0]) - i32::from(c.r())).abs() < 12
            && (i32::from(p[1]) - i32::from(c.g())).abs() < 12
            && (i32::from(p[2]) - i32::from(c.b())).abs() < 12
    };
    let has = |c: Color32| img.pixels().any(|p| near(p.0, c));
    assert!(has(theme::GRID), "{name}: no phosphor panel");
    img.save(dir.join(format!("{name}.png"))).expect("save png");
}

#[test]
fn review_screenshots() {
    let Some(dir) = std::env::var_os("MH_UI_REVIEW_DIR") else {
        eprintln!("MH_UI_REVIEW_DIR not set -- skipping the PNG render");
        return;
    };
    let dir = std::path::PathBuf::from(dir);
    std::fs::create_dir_all(&dir).unwrap();
    for loc in Locale::ALL {
        i18n::set_locale(loc);
        let code = loc.code();
        for tab in ["display", "gameplay", "multiplayer", "updates"] {
            for detailed in [false, true] {
                let mut fx = fixture(Schema::builtin());
                fx.state.subtab = tab.into();
                fx.state.detailed = detailed;
                let mut h = harness_wgpu(fx);
                let view = if detailed { "detailed" } else { "compact" };
                save(
                    &mut h,
                    &dir,
                    &format!("standin_settings_{tab}_{view}_{code}"),
                );
            }
        }
        // The compact view's description, as a tooltip over a hovered row.
        let mut fx = fixture(Schema::builtin());
        fx.state.subtab = "gameplay".into();
        let mut h = harness_wgpu(fx);
        h.run();
        h.get_by_label(tr("setting.net.improved_movement")).hover();
        for _ in 0..30 {
            h.step();
            std::thread::sleep(std::time::Duration::from_millis(20));
        }
        save(
            &mut h,
            &dir,
            &format!("standin_settings_gameplay_hover_{code}"),
        );
        let mut h = harness_wgpu(fixture(Schema::builtin()));
        h.state_mut().page = Pg::Diagnostics;
        save(&mut h, &dir, &format!("diagnostics_{code}"));
        let mut h = harness_wgpu(fixture(Schema::builtin()));
        h.state_mut().page = Pg::Kit;
        save(&mut h, &dir, &format!("kit_{code}"));
    }
    i18n::set_locale(Locale::En);
    // An error state: a rejected port, a custom relay, unsaved edits (Start disabled).
    let mut fx = fixture(Schema::builtin());
    fx.state.subtab = "multiplayer".into();
    fx.model.set("net.port", "22");
    fx.model.set("launcher.relay_mode", "custom");
    fx.model.set("video.window", "windowed");
    let mut h = harness_wgpu(fx);
    save(&mut h, &dir, "settings_multiplayer_errors_en");
    // The dirty-settings modal over the Display tab.
    let mut fx = fixture(Schema::builtin());
    fx.model.set("video.window", "windowed");
    fx.modal = true;
    let mut h = harness_wgpu(fx);
    save(&mut h, &dir, "modal_en");
    // The Resolution row opened on the pinned 1920x1080 desktop: classic sizes only (the default), Auto, then
    // the 4:3 sizes, the 16:9 sizes (RES3 list order).
    let mut h = harness_wgpu(fixture(Schema::builtin()));
    h.run();
    h.get_all_by_role(egui::accesskit::Role::ComboBox)
        .next()
        .unwrap()
        .click();
    h.run();
    save(&mut h, &dir, "settings_display_resolution_open_en");
    // mp:LSCEIL: the Max command delay dropdown (Multiplayer > Connection) opened.
    let mut fx = fixture(Schema::builtin());
    fx.state.subtab = "multiplayer".into();
    let mut h = harness_wgpu(fx);
    h.run();
    h.get_all_by_role(egui::accesskit::Role::ComboBox)
        .next()
        .unwrap()
        .click();
    h.run();
    save(&mut h, &dir, "settings_multiplayer_delay_open_en");
    // RES25: the Display tab with Classic sizes only (Interface size greyed, hint under it) and with Auto (live) -- English.
    for (res, name) in [("stock", "classic"), ("auto", "auto")] {
        let mut fx = fixture(Schema::builtin());
        fx.model.set("video.resolution", res);
        fx.state.subtab = "display".into();
        let mut h = harness_wgpu(fx);
        save(
            &mut h,
            &dir,
            &format!("settings_display_interface_size_{name}_en"),
        );
    }
    // A custom (hand-written) enum value and the Language combo opened is covered by the unit tests;
    // the picture of the language tab above shows the combo closed.
}

fn harness_wgpu(fx: Fx) -> Harness<'static, Fx> {
    Harness::builder()
        .with_size(vec2(800.0, 600.0))
        .with_pixels_per_point(1.0)
        // kittest's default (`PREDICTABLE`) filters textures in the shader, which ignores the
        // sampler's wrap mode -- the 16 px grid and hazard tiles would not repeat. The launcher's
        // real renderer samples in hardware, so the review pictures do too.
        .with_render_options(eframe::egui_wgpu::RendererOptions {
            predictable_texture_filtering: false,
            ..eframe::egui_wgpu::RendererOptions::PREDICTABLE
        })
        .wgpu()
        .build_ui_state(window, fx)
}

/// RES24 review frame: the Resolution dropdown open on a 2560x1440 desktop (the common 4:3 and 16:9 sizes that fit).
#[test]
fn resolution_list_review_frame() {
    let Some(dir) = std::env::var_os("MH_UI_REVIEW_DIR") else {
        return;
    };
    let dir = std::path::PathBuf::from(dir);
    std::fs::create_dir_all(&dir).unwrap();
    i18n::set_locale(Locale::En);
    let mut fx = fixture(Schema::builtin());
    fx.model.set_provider_options(
        Provider::Resolutions,
        super::resolutions::options(
            Some(super::resolutions::Desktop {
                w: 2560,
                h: 1440,
                scale_pct: 100,
            }),
            tr("setting.video.resolution.auto"),
            tr("setting.video.resolution.classic"),
        ),
    );
    fx.model.set("video.resolution", "1920x1080");
    let mut h = Harness::builder()
        .with_size(vec2(800.0, 900.0))
        .with_pixels_per_point(1.0)
        .with_render_options(eframe::egui_wgpu::RendererOptions {
            predictable_texture_filtering: false,
            ..eframe::egui_wgpu::RendererOptions::PREDICTABLE
        })
        .wgpu()
        .build_ui_state(window, fx);
    h.run();
    // The row's combo shows the current value; clicking it opens the list. The popup scrolls: one frame per page.
    h.get_by_value("1920x1080 (16:9)").click();
    h.run();
    h.run();
    for page in 1..=4 {
        let img = h.render().expect("render");
        img.save(dir.join(format!("resolution_list_2560x1440_en_p{page}.png")))
            .expect("save png");
        h.hover_at(egui::pos2(600.0, 330.0));
        h.run();
        h.event(egui::Event::MouseWheel {
            unit: egui::MouseWheelUnit::Point,
            delta: vec2(0.0, -170.0),
            phase: egui::TouchPhase::Move,
            modifiers: egui::Modifiers::NONE,
        });
        h.run();
    }
}

/// Review frames of the big-map hotkey row: default Tab, listening, O set, after Revert.
#[test]
fn bigmap_hotkey_review_frames() {
    let Some(dir) = std::env::var_os("MH_UI_REVIEW_DIR") else {
        return;
    };
    let dir = std::path::PathBuf::from(dir);
    std::fs::create_dir_all(&dir).unwrap();
    i18n::set_locale(Locale::En);
    let mut fx = fixture(Schema::builtin());
    fx.model.load(Some(b"[ui]\r\nbigmap=1\r\n"), &fx.store);
    fx.state.subtab = "display".into();
    let mut h = harness_wgpu(fx);
    save(&mut h, &dir, "bigmap_key_1_default_tab");
    h.get_by_label("Tab").click();
    h.run();
    h.run();
    save(&mut h, &dir, "bigmap_key_2_listening");
    h.key_press(egui::Key::O);
    save(&mut h, &dir, "bigmap_key_3_set_o");
    h.state_mut().model.revert();
    save(&mut h, &dir, "bigmap_key_4_after_revert");
    h.get_by_label("Tab").click();
    h.run();
    h.run();
    h.key_press(egui::Key::Tab);
    save(&mut h, &dir, "bigmap_key_5_tab_captured");
}
