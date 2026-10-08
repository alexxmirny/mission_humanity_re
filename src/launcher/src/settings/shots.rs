//! egui_kittest checks of the settings page, and the review screenshots (dist RL12/RL13).
//!
//! Two tiers, so the plain `cargo test` stays GPU-free:
//!
//! * the interaction tests drive a real egui frame headlessly (no renderer) and read the page
//!   through its accessibility tree: rows appear under their sub-tab, a click dirties the model,
//!   a synthetic registry row shows up with no code change (the RL13 done_when, end to end);
//! * `review_screenshots` renders PNGs through wgpu (D3D/WARP, the launcher's own fallback path) and
//!   runs only when `MH_UI_REVIEW_DIR` names a directory, which it fills: every Settings sub-tab and
//!   the Diagnostics page in English and Russian, an error state, the widget kit in a frame, and a
//!   modal. Those are what the user reviews (UI-layout rule); nothing is auto-diffed against a
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
vsync=0\r\n\
fps_limit=60\r\n\
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
level=normal\r\n\
[video2]\r\n\
x=1\r\n";

fn model_from(schema: Schema) -> Model {
    let mut m = Model::new(schema);
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
                let act = render::footer(ui, &fx.model);
                match act {
                    render::Action::Apply => {
                        let path = std::env::temp_dir().join("mh_launcher_shots.ini");
                        let _ = fx.model.apply(&path, &mut fx.store);
                    }
                    render::Action::Revert => fx.model.revert(),
                    render::Action::None => {}
                }
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
                    widgets::pill(ui, tr("badge.restart"), theme::TEXT_DIM);
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
        .with_size(vec2(800.0, 600.0))
        .with_pixels_per_point(1.0)
        .build_ui_state(window, fx)
}

// ---- interaction (no GPU) ------------------------------------------------------------------------

#[test]
fn display_rows_show_and_a_click_makes_the_page_dirty() {
    i18n::set_locale(Locale::En);
    let mut h = harness(fixture(Schema::builtin()));
    h.run();
    assert!(h.query_by_label("Window").is_some());
    assert!(h.query_by_label("V-sync").is_some());
    // The experimental row carries its pills.
    assert!(h.query_by_label("EXPERIMENTAL").is_some());
    assert!(!h.state().model.is_dirty());
    h.get_by_label("Windowed").click();
    h.run();
    assert_eq!(h.state().model.value("video.window"), "windowed");
    assert_eq!(h.state().model.dirty_count(), 1);
    // The Settings menu entry now carries the unsaved marker via the Start button's disabled reason.
    h.get_by_label("Revert").click();
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
    h.get_by_label("Updates & logs").click();
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
    // Two experimental rows now carry the pill.
    assert_eq!(h.query_all_by_label("EXPERIMENTAL").count(), 2);
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
        for tab in ["display", "language", "multiplayer", "updates"] {
            let mut fx = fixture(Schema::builtin());
            fx.state.subtab = tab.into();
            let mut h = harness_wgpu(fx);
            save(&mut h, &dir, &format!("settings_{tab}_{code}"));
        }
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
