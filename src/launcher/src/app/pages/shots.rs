//! egui_kittest checks of the whole window, and the review screenshots (dist RL12/RL14/RL16).
//!
//! Two tiers, so a plain `cargo test` stays GPU-free:
//!
//! * interaction tests drive real frames headlessly (no renderer) and read the page through its
//!   accessibility tree: every page draws in English and Russian, nothing sticks out of the frame
//!   at 800x600, Start with unsaved settings asks, a match row selects, Apply writes the ini;
//! * `review_screenshots` renders PNGs through wgpu (D3D/WARP, the launcher's own fallback path) and
//!   runs only when `MH_UI_REVIEW_DIR` names a directory, which it fills with every page in both
//!   languages. Those are what the user reviews (UI-layout rule). There is deliberately NO committed
//!   baseline: the user looks at the pictures, and a diff against a baseline nobody approved would
//!   only say "different".

use std::path::{Path, PathBuf};

use chrono::{DateTime, Duration as Span, Utc};
use egui::{vec2, Color32, Rect};
use egui_kittest::kittest::{NodeT, Queryable};
use egui_kittest::Harness;

use crate::app::{Accepted, App, Startup, View};
use crate::config::Config;
use crate::i18n::{self, Locale};
use crate::paths::Layout;
use crate::relay::Relay;
use crate::update::Offer;

const PAGES: [View; 5] = [
    View::Play,
    View::Settings,
    View::Report,
    View::About,
    View::Diagnostics,
];

fn stamp_dir(t: DateTime<Utc>) -> String {
    t.format("%Y-%m-%dT%H-%M-%SZ").to_string()
}

fn stamp_compact(t: DateTime<Utc>) -> String {
    t.format("%Y%m%dT%H%M%SZ").to_string()
}

struct Fixture {
    app: App,
    game: PathBuf,
    root: PathBuf,
}

#[allow(clippy::too_many_arguments)]
fn write_session(
    logs: &Path,
    ago: Span,
    mid8: &str,
    map: &str,
    mode: &str,
    players: &[&str],
    ai: u32,
    outcome: &str,
    process_dir: &str,
) -> String {
    let t = Utc::now() - ago;
    let name = format!("{}_{mid8}_{map}_{mode}", stamp_dir(t));
    let dir = logs.join(&name);
    std::fs::create_dir_all(&dir).unwrap();
    let json = serde_json::json!({
        "match_id": format!("01a1{mid8}0000000000000000{mid8}"),
        "slot": 0,
        "role": if mode == "client" { "client" } else { "host" },
        "mode": mode,
        "build": "0.2.0+test",
        "map": format!("Maps\\{map}.mpm"),
        "began": stamp_compact(t),
        "ended": if outcome == "running" { String::new() } else { stamp_compact(t + Span::minutes(20)) },
        "reason": "",
        "process_dir": process_dir,
        "players": players,
        "ai_count": ai,
        "outcome": outcome,
    });
    std::fs::write(
        dir.join("session.json"),
        serde_json::to_string_pretty(&json).unwrap(),
    )
    .unwrap();
    std::fs::write(
        dir.join("mh_net.log"),
        "; [build] mh 0.2.0+test\nhello\n".repeat(3),
    )
    .unwrap();
    name
}

/// A launcher over a scratch game folder: a stub `mh.exe`, an install receipt, a Russian pack, an
/// ini beside the exe (portable mode), five matches in the logs root, a relay, an offered update.
fn fixture(tag: &str, with_game: bool) -> Fixture {
    let root = std::env::temp_dir().join(format!("mh_launcher_pages_{tag}"));
    let _ = std::fs::remove_dir_all(&root);
    let game = root.join("Mission Humanity");
    std::fs::create_dir_all(game.join("lang").join("ru")).unwrap();
    std::fs::write(game.join("mh.exe"), b"x").unwrap();
    std::fs::write(
        game.join("lang").join("ru").join("pack.ini"),
        "codepage=1251\n",
    )
    .unwrap();
    std::fs::write(
        game.join(crate::paths::INSTALL_MANIFEST),
        "version\t0.2.0\ntag\tnet\npackage\tmission_humanity_re-0.2.0-net.zip\n\
         installed_at\t2026-10-08T10:00:00Z\n",
    )
    .unwrap();
    std::fs::write(
        game.join("mh_net.ini"),
        "; Mission Humanity configuration\r\n[video]\r\nwindow=borderless\r\nscale=fit\r\n\
         filter=area\r\nvsync=0\r\nfps_limit=60\r\nmouse_clip=1\r\npicker=0\r\n[lang]\r\npack=ru\r\n\
         [net]\r\nforce_relay=0\r\nport=6501\r\n[hud]\r\nnet_indicator=1\r\nnet_indicator_key=Ctrl+Alt+N\r\n\
         [log]\r\nlevel=normal\r\n",
    )
    .unwrap();
    let layout = Layout::rooted(root.join("state"));
    let cfg = Config {
        game_dir: if with_game {
            game.display().to_string()
        } else {
            String::new()
        },
        installed_version: "0.2.0".into(),
        installed_tag: "net".into(),
        discord_client_id: "42".into(),
        // The launcher speaks the language `ui_lang` names (`App::new` installs it), so a test
        // that asks for Russian says so here.
        ui_lang: i18n::locale().code().into(),
        ..Config::default()
    };
    let logs = layout.game_log_root(&game);
    std::fs::create_dir_all(&logs).unwrap();
    let pd = format!("{}_menu_solo", stamp_dir(Utc::now() - Span::days(7)));
    std::fs::create_dir_all(logs.join(&pd)).unwrap();
    write_session(
        &logs,
        Span::hours(1),
        "aabbccdd",
        "krater",
        "host",
        &["you", "Sasha", "Mark"],
        0,
        "desync",
        &pd,
    );
    // The same match as the client saw it: a client never learns the host's name.
    write_session(
        &logs,
        Span::hours(1),
        "aabbccdd",
        "krater",
        "client",
        &["client", "client"],
        0,
        "desync",
        &pd,
    );
    write_session(
        &logs,
        Span::hours(3),
        "11223344",
        "wyspy",
        "host",
        &["you", "Sasha"],
        0,
        "finished",
        &pd,
    );
    write_session(
        &logs,
        Span::hours(26),
        "55667788",
        "mission-6",
        "campaign",
        &["you"],
        0,
        "running",
        &pd,
    );
    write_session(
        &logs,
        Span::days(5),
        "99aabbcc",
        "delta",
        "skirmish",
        &["you"],
        3,
        "finished",
        &pd,
    );

    let mut app = App::new(layout, cfg, View::Play, Startup::default());
    // The start-up check would open a socket; the fixture answers for it.
    app.startup_done = true;
    // A release build bakes MH_UPDATE_BASE_URL in, which turns the RL9 silent update on: its fetch
    // job keeps the window repainting and kittest's run() hits max_steps (the launcher-v0.2.0-rc8 CI
    // red). The pictures are of a settled page, so the fixture switches it off.
    app.auto.enabled = false;
    app.relay = Some(Relay {
        addr: "203.0.113.9:7100".into(),
        key: "open".into(),
    });
    app.relay_from = "0.2.1".into();
    app.accepted = Accepted {
        version: "0.2.1".into(),
        issued_at: "2026-10-05T00:00:00Z".into(),
        notes_url: "https://example.invalid/notes/0.2.1".into(),
    };
    app.offer = Some(Offer {
        game: Some("0.2.1".into()),
        age_days: Some(3),
        ..Offer::default()
    });
    Fixture { app, game, root }
}

fn harness(app: App) -> Harness<'static, App> {
    Harness::builder()
        .with_size(vec2(800.0, 600.0))
        .with_pixels_per_point(1.0)
        .build_ui_state(|ui, app: &mut App| app.show(ui), app)
}

fn harness_wgpu(app: App) -> Harness<'static, App> {
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
        .build_ui_state(|ui, app: &mut App| app.show(ui), app)
}

fn cleanup(f: &Fixture) {
    let _ = std::fs::remove_dir_all(&f.root);
}

// ---- interaction (no GPU) ------------------------------------------------------------------------

#[test]
fn every_page_draws_in_both_languages() {
    for loc in Locale::ALL {
        i18n::set_locale(loc);
        for page in PAGES {
            let mut f = fixture(&format!("draw_{}_{page:?}", loc.code()), true);
            f.app.view = page;
            let root = f.root.clone();
            let mut h = harness(f.app);
            h.run();
            let _ = std::fs::remove_dir_all(root);
        }
    }
    i18n::set_locale(Locale::En);
}

/// The two review complaints, as an assertion: nothing the player can see or click sticks out past
/// the right edge of the frame (a Russian button used to hang out of it), on any page, either
/// language, at the smallest window the launcher allows.
#[test]
fn nothing_sticks_out_of_the_frame_at_800x600() {
    // The window is 800 wide, 18 px of space, then the frame's 10 px border: 772 is the last pixel
    // of the green panel's outer edge.
    const RIGHT: f32 = 800.0 - 18.0 - 10.0;
    for loc in Locale::ALL {
        i18n::set_locale(loc);
        for page in PAGES {
            let mut f = fixture(&format!("fit_{}_{page:?}", loc.code()), true);
            f.app.view = page;
            // Worst cases: unsaved edits, the longest sub-tab, a crash prompt, a report with a pick.
            f.app.model_for = None;
            let root = f.root.clone();
            let mut h = harness(f.app);
            h.run();
            if page == View::Settings {
                for tab in ["language", "multiplayer", "updates"] {
                    h.state_mut().settings_state.subtab = tab.into();
                    h.run();
                    assert_fits(&h, RIGHT, &format!("settings/{tab} {}", loc.code()));
                }
            }
            if page == View::Diagnostics {
                // Re-verify / Roll back are not drawn yet, but when RL9 supplies them they sit in
                // this row: the widest label in the widest language.
            }
            assert_fits(&h, RIGHT, &format!("{page:?} {}", loc.code()));
            let _ = std::fs::remove_dir_all(root);
        }
    }
    i18n::set_locale(Locale::En);
}

fn assert_fits(h: &Harness<'_, App>, right: f32, what: &str) {
    let mut bad = Vec::new();
    for n in h.root().children_recursive() {
        // `Node::rect` panics for a node with no bounds; those have no extent to check.
        let Some(b) = n.accesskit_node().bounding_box() else {
            continue;
        };
        let r = Rect::from_min_max(
            egui::pos2(b.x0 as f32, b.y0 as f32),
            egui::pos2(b.x1 as f32, b.y1 as f32),
        );
        // Only things with an extent: skip the window itself and zero-size accessibility nodes.
        if r.width() < 1.0 || r.height() < 1.0 || r.width() >= 799.0 {
            continue;
        }
        // The menu column is not in the frame.
        if r.right() <= 220.0 {
            continue;
        }
        if r.right() > right + 0.5 {
            bad.push(format!(
                "{:?} right edge {:.1} > {right} ({:?})",
                n.accesskit_node().label(),
                r.right(),
                r
            ));
        }
    }
    assert!(bad.is_empty(), "{what}: sticks out of the frame:\n{bad:#?}");
}

/// Steps the harness until the launch's background job has ended (bounded at ~5 s), then runs it
/// to idle: run() alone gives up after max_steps while a job is still repainting.
fn settle(h: &mut Harness<'static, App>) {
    for _ in 0..500 {
        h.step();
        if h.state().job.is_none() {
            break;
        }
        std::thread::sleep(std::time::Duration::from_millis(10));
    }
    h.run();
}

#[test]
fn start_game_with_unsaved_settings_asks_and_the_three_answers_do_what_they_say() {
    i18n::set_locale(Locale::En);
    let mut f = fixture("start_prompt", true);
    f.app.view = View::Settings;
    // This test is about the prompt, not RL4: keep the launch from moving the fixture's portable
    // ini (a legacy folder the migration would otherwise tidy) out from under the assertions.
    f.app.auto.migrate_failed = true;
    // A launch with an update source runs its manifest fetch as a job, which repaints until it ends
    // (a release build bakes MH_UPDATE_BASE_URL in). Keep that fetch off the network and step the
    // harness until that job has ended (settle) before asking for an idle frame.
    f.app.config.update_base_url = "http://127.0.0.1:9/".into();
    let ini = f.game.join("mh_net.ini");
    let root = f.root.clone();
    let mut h = harness(f.app);
    h.run();
    h.get_by_label("Windowed").click();
    h.run();
    assert!(h.state().model.is_dirty());
    // A game that cannot start (the stub `mh.exe` is not a program) -- what matters is that the
    // press asked first.
    h.get_by_label("Start game").click();
    h.run();
    assert!(h.state().start_prompt, "pressing Start with edits asks");
    assert!(h.query_by_label_contains("You changed settings").is_some());
    assert!(h.state().session.is_none(), "nothing started yet");

    // Cancel: back to the page, edits intact.
    h.get_by_label("Cancel").click();
    h.run();
    assert!(!h.state().start_prompt);
    assert!(h.state().model.is_dirty());

    // Apply from the prompt writes the ini, and then the launch is attempted.
    h.get_by_label("Start game").click();
    h.run();
    // Two "Apply" buttons now: the page's footer and the prompt's, which is drawn last.
    h.query_all_by_label("Apply").last().unwrap().click();
    settle(&mut h);
    assert!(!h.state().start_prompt);
    assert!(!h.state().model.is_dirty());
    let text = std::fs::read_to_string(&ini).unwrap();
    assert!(text.contains("window=windowed"), "{text}");
    assert!(text.contains("; Mission Humanity configuration"));

    // Discard drops the edit and launches.
    h.get_by_label("Borderless").click();
    h.run();
    assert!(h.state().model.is_dirty());
    h.get_by_label("Start game").click();
    h.run();
    h.get_by_label("Discard").click();
    settle(&mut h);
    assert!(!h.state().start_prompt);
    assert!(!h.state().model.is_dirty());
    assert!(
        std::fs::read_to_string(&ini)
            .unwrap()
            .contains("window=windowed"),
        "discard must not write"
    );
    let _ = std::fs::remove_dir_all(root);
}

#[test]
fn unsaved_settings_show_apply_on_every_page_and_closing_asks() {
    i18n::set_locale(Locale::En);
    let mut f = fixture("save_bar", true);
    f.app.view = View::Settings;
    f.app.auto.migrate_failed = true;
    let ini = f.game.join("mh_net.ini");
    let root = f.root.clone();
    let mut h = harness(f.app);
    h.run();
    assert!(
        h.query_by_label_contains("unsaved change").is_none(),
        "clean: no save bar"
    );
    h.get_by_label("Windowed").click();
    h.run();
    // Away from Settings, the edit is still one click from saved.
    h.state_mut().view = View::About;
    h.run();
    assert!(h.query_by_label_contains("1 unsaved change").is_some());
    h.get_by_label("Apply").click();
    h.run();
    assert!(!h.state().model.is_dirty());
    assert!(std::fs::read_to_string(&ini)
        .unwrap()
        .contains("window=windowed"));
    assert!(h.query_by_label_contains("unsaved change").is_none());

    // Closing with an edit pending asks; Cancel keeps it, Discard drops it and closes.
    h.state_mut().view = View::Settings;
    h.run();
    h.get_by_label("Borderless").click();
    h.run();
    h.state_mut().close_prompt = true;
    h.run();
    assert!(h
        .query_by_label_contains("before closing the launcher")
        .is_some());
    h.get_by_label("Cancel").click();
    h.run();
    assert!(!h.state().close_prompt && h.state().model.is_dirty());
    h.state_mut().close_prompt = true;
    h.run();
    h.get_by_label("Discard").click();
    h.run();
    assert!(h.state().close_confirmed && !h.state().model.is_dirty());
    let _ = std::fs::remove_dir_all(root);
}

#[test]
fn start_is_blocked_with_a_reason_while_busy_or_without_a_game_folder() {
    i18n::set_locale(Locale::En);
    let f = fixture("start_blocked", false);
    let h = harness(f.app);
    assert_eq!(
        h.state().start_blocked(),
        Some("Choose the game folder first.")
    );
    let _ = std::fs::remove_dir_all(f.root);
    let mut f = fixture("start_blocked2", true);
    assert_eq!(f.app.start_blocked(), None);
    f.app.model.load(
        Some(b"[net]\r\nport=6501\r\n"),
        &crate::settings::model::MapStore::default(),
    );
    f.app.model.set("net.port", "22");
    assert!(f.app.start_blocked().is_some(), "invalid edits block Start");
    cleanup(&f);
}

#[test]
fn the_report_page_lists_matches_host_first_and_a_click_picks_one() {
    i18n::set_locale(Locale::En);
    let mut f = fixture("report_pick", true);
    f.app.view = View::Report;
    let root = f.root.clone();
    let mut h = harness(f.app);
    h.run();
    let rows = h.state().matches.clone();
    // Five session folders, but the client's copy of the first match collapses into the host's.
    assert_eq!(rows.len(), 4, "{rows:#?}");
    let krater = rows.iter().find(|r| r.map == "krater").unwrap();
    assert_eq!(krater.players, ["you", "Sasha", "Mark"], "the host's names");
    assert_eq!(krater.outcome, crate::report::MatchOutcome::Desync);
    // `running`, no game alive: a crash.
    let m6 = rows.iter().find(|r| r.map == "mission-6").unwrap();
    assert_eq!(m6.outcome, crate::report::MatchOutcome::Crash);
    assert!(h.query_all_by_label_contains("desync").count() >= 1);
    assert!(h.query_all_by_label_contains("crash").count() >= 1);
    // Select the crash row.
    h.get_by_label_contains(&m6.dir).click();
    h.run();
    assert_eq!(h.state().session_pick.as_deref(), Some(m6.dir.as_str()));
    assert!(h.state().chosen_session_dir().unwrap().ends_with(&m6.dir));
    let _ = std::fs::remove_dir_all(root);
}

#[test]
fn a_reported_crash_gets_its_flag_when_dismissed() {
    i18n::set_locale(Locale::En);
    let mut f = fixture("dismiss", true);
    let logs = f.app.log_root().unwrap();
    let marker = logs.join("mh_crash_0000000000000001.marker");
    std::fs::write(
        &marker,
        "mh_crash=1\ncode=c0000005\npid=42\ntid=7\nmodule=mh.dll\noffset=0x10\nmatch_id=\nwhen=20261008T100000Z\nbuild=0.2.0\n",
    )
    .unwrap();
    f.app.marker = Some(crate::crash::Marker::read(&marker).unwrap());
    f.app.marker_path = Some(marker.clone());
    let root = f.root.clone();
    let mut h = harness(f.app);
    h.run();
    assert!(h.query_by_label("THE GAME CRASHED").is_some());
    h.get_by_label("Dismiss").click();
    h.run();
    assert!(crate::report::is_reported(&marker));
    assert!(logs
        .join("mh_crash_0000000000000001.marker.reported")
        .is_file());
    assert!(h.state().marker.is_none());
    assert!(h.query_by_label("THE GAME CRASHED").is_none());
    let _ = std::fs::remove_dir_all(root);
}

/// The RL5 contract, send half: when the report that carried a crash marker is sent, the marker gets
/// its `.reported` sibling -- and a report that did not carry it leaves it unflagged.
#[test]
fn a_sent_report_flags_the_markers_it_carried_and_only_those() {
    i18n::set_locale(Locale::En);
    let mut f = fixture("sent_flags", true);
    let logs = f.app.log_root().unwrap();
    let carried = logs.join("mh_crash_00000000000000a1.marker");
    let other = logs.join("mh_crash_00000000000000b2.marker");
    for p in [&carried, &other] {
        std::fs::write(p, "mh_crash=1\n").unwrap();
    }
    let zip = f.root.join("r.zip");
    f.app.built = Some(crate::report::Built {
        zip: zip.clone(),
        entries: Vec::new(),
        bytes: 0,
        meta: String::new(),
        markers: vec![carried.clone()],
    });
    // Another zip was sent: nothing is flagged.
    f.app.mark_sent_markers(&f.root.join("elsewhere.zip"));
    assert!(!crate::report::is_reported(&carried));
    f.app.mark_sent_markers(&zip);
    assert!(crate::report::is_reported(&carried));
    assert!(!crate::report::is_reported(&other));
    cleanup(&f);
}

#[test]
fn russian_relabels_every_menu_entry_and_the_version_line_keeps_the_channel_name() {
    i18n::set_locale(Locale::Ru);
    let f = fixture("ru_menu", true);
    let root = f.root.clone();
    let mut h = harness(f.app);
    h.run();
    for label in [
        "Играть",
        "Настройки",
        "Сообщить о проблеме",
        "О программе",
        "Диагностика",
    ] {
        assert!(h.query_by_label(label).is_some(), "{label}");
    }
    assert!(h.query_by_label("Start game").is_none());
    // Channel names are names: `stable` and `latest` in Russian too, version line included.
    for name in ["stable", "latest"] {
        assert_eq!(i18n::tr_in(Locale::Ru, &format!("channel.{name}")), name);
        assert_eq!(
            i18n::tr_in(Locale::Ru, &format!("setting.launcher.channel.opt.{name}")),
            name
        );
    }
    assert_eq!(
        i18n::trf_in(
            Locale::Ru,
            "app.version_line",
            &[("version", "0.2.0"), ("channel", "stable")]
        ),
        "v0.2.0 · stable"
    );
    // ... and the renamed setting.
    assert_eq!(
        i18n::tr_in(Locale::En, "setting.video.filter"),
        "Interpolation"
    );
    assert_eq!(
        i18n::tr_in(Locale::Ru, "setting.video.filter"),
        "Интерполяция"
    );
    i18n::set_locale(Locale::En);
    let _ = std::fs::remove_dir_all(root);
}

#[test]
fn the_badges_sit_on_the_row_of_their_setting() {
    i18n::set_locale(Locale::En);
    let mut f = fixture("badges", true);
    f.app.view = View::Settings;
    let root = f.root.clone();
    let mut h = harness(f.app);
    h.run();
    // "Extra resolutions in game options" is experimental and restart-flagged; its Off/On control
    // and both pills must be on the same line (their vertical centres within a few pixels).
    let pill = h.get_by_label("EXPERIMENTAL").rect();
    let restart = h.get_by_label("next game start").rect();
    let on = h
        .root()
        .children_recursive()
        .filter(|n| n.accesskit_node().label().as_deref() == Some("On"))
        .map(|n| n.rect())
        .min_by(|a, b| {
            (a.center().y - pill.center().y)
                .abs()
                .total_cmp(&(b.center().y - pill.center().y).abs())
        })
        .unwrap();
    assert!(
        (pill.center().y - on.center().y).abs() < 6.0,
        "EXPERIMENTAL {pill:?} is not on the line of its control {on:?}"
    );
    // Both pills follow the control straight away, in order, on the same line -- not in a column
    // at the far right edge.
    assert!(
        pill.left() > on.right() && pill.left() - on.right() < 40.0,
        "the pill hugs its control: control {on:?}, pill {pill:?}"
    );
    assert!(
        restart.left() > pill.right() && restart.left() - pill.right() < 20.0,
        "the second pill follows the first: {pill:?} {restart:?}"
    );
    assert!(
        (restart.center().y - pill.center().y).abs() < 2.0,
        "one line: {pill:?} {restart:?}"
    );
    let _ = std::fs::remove_dir_all(root);
}

#[test]
fn a_staged_launcher_asks_to_restart_and_later_answers_it() {
    i18n::set_locale(Locale::En);
    let mut f = fixture("restart_modal", true);
    let root = f.root.clone();
    f.app.auto.pending_restart = Some(crate::update::StagedLauncher {
        version: "0.2.1".into(),
        // not a file: the Drop commit at the end must find nothing to replace
        candidate: root.join("never_written.exe"),
    });
    let mut h = harness(f.app);
    h.run();
    assert!(h
        .query_by_label_contains("Launcher 0.2.1 is installed")
        .is_some());
    assert!(h.query_by_label("Restart now").is_some());
    h.get_by_label("Later").click();
    h.run();
    assert_eq!(h.state().restart_prompt(), None);
    assert!(h.query_by_label("Restart now").is_none());
    assert!(
        h.state().auto.pending_restart.is_some(),
        "Later keeps it staged"
    );
    let _ = std::fs::remove_dir_all(root);
}

#[test]
fn diagnostics_offers_reverify_for_an_installed_game() {
    i18n::set_locale(Locale::En);
    let mut f = fixture("diag_repair", true);
    f.app.view = View::Diagnostics;
    let root = f.root.clone();
    assert!(f.app.repair_actions().is_some_and(|r| r.reverify));
    let mut h = harness(f.app);
    h.run();
    assert!(h.query_by_label("Re-verify game files").is_some());
    let _ = std::fs::remove_dir_all(root);
}

#[test]
fn the_report_lists_sessions_from_both_log_roots_without_duplicates() {
    i18n::set_locale(Locale::En);
    let mut f = fixture("both_roots", true);
    let roots = f.app.log_roots();
    assert_eq!(roots.len(), 2, "{roots:?}");
    let before = crate::report::session_dirs_in(&roots).len();
    // A session only the config-dir logs has (a hand launch / a migrated one), and a copy of one
    // the launcher root already has.
    let extra = roots[1].join("2026-10-08T23-00-00Z_deadbeef_hand_host");
    std::fs::create_dir_all(&extra).unwrap();
    let existing = crate::report::session_dirs_in(&roots[..1])[0]
        .file_name()
        .unwrap()
        .to_owned();
    std::fs::create_dir_all(roots[1].join(existing)).unwrap();
    assert_eq!(crate::report::session_dirs_in(&roots).len(), before + 1);
    f.app.refresh_matches(true);
    assert!(f.app.matches.iter().any(|m| m.dir.contains("deadbeef")));
    let _ = std::fs::remove_dir_all(&f.root);
}

// ---- screenshots (opt-in) ------------------------------------------------------------------------

fn save(h: &mut Harness<'_, App>, dir: &Path, name: &str) {
    h.run();
    let img = h.render().expect("render");
    assert_eq!((img.width(), img.height()), (800, 600));
    let near = |p: [u8; 4], c: Color32| {
        (i32::from(p[0]) - i32::from(c.r())).abs() < 12
            && (i32::from(p[1]) - i32::from(c.g())).abs() < 12
            && (i32::from(p[2]) - i32::from(c.b())).abs() < 12
    };
    assert!(
        img.pixels().any(|p| near(p.0, crate::theme::GRID)),
        "{name}: no phosphor panel"
    );
    img.save(dir.join(format!("{name}.png"))).expect("save png");
}

#[test]
fn review_screenshots() {
    let Some(dir) = std::env::var_os("MH_UI_REVIEW_DIR") else {
        eprintln!("MH_UI_REVIEW_DIR not set -- skipping the PNG render");
        return;
    };
    let dir = PathBuf::from(dir);
    std::fs::create_dir_all(&dir).unwrap();
    for loc in Locale::ALL {
        i18n::set_locale(loc);
        let code = loc.code();
        let shot = |name: &str, with_game: bool, f: &dyn Fn(&mut App)| {
            let mut fx = fixture(&format!("shot_{name}_{code}"), with_game);
            f(&mut fx.app);
            let root = fx.root.clone();
            let mut h = harness_wgpu(fx.app);
            save(&mut h, &dir, &format!("{name}_{code}"));
            let _ = std::fs::remove_dir_all(root);
        };
        shot("play", true, &|_| {});
        shot("play_nogame", false, &|_| {});
        shot("play_crash", true, &|a| {
            let m = crate::crash::Marker::parse(
                "mh_crash=1\ncode=c0000005\npid=42\ntid=7\nmodule=mh.dll\noffset=0x175b0\nwhen=20261008T100000Z\nbuild=0.2.0\n",
            )
            .unwrap();
            a.marker = Some(m);
            a.last_run = Some(crate::launch::Finished {
                outcome: crate::launch::Outcome::Crash(0xC000_0005),
                seconds: 412.0,
            });
        });
        for tab in ["display", "language", "multiplayer", "updates"] {
            shot(&format!("settings_{tab}"), true, &|a| {
                a.view = View::Settings;
                a.settings_state.subtab = tab.into();
            });
        }
        shot("settings_errors", true, &|a| {
            a.view = View::Settings;
            a.settings_state.subtab = "multiplayer".into();
            a.ensure_model();
            a.model.set("net.port", "22");
            a.model.set("launcher.relay_mode", "custom");
            a.model.set("video.window", "windowed");
        });
        shot("start_prompt", true, &|a| {
            a.view = View::Settings;
            a.ensure_model();
            a.model.set("video.window", "windowed");
            a.start_prompt = true;
        });
        shot("save_bar_play", true, &|a| {
            a.ensure_model();
            a.model.set("video.window", "windowed");
        });
        shot("close_prompt", true, &|a| {
            a.view = View::Settings;
            a.ensure_model();
            a.model.set("video.window", "windowed");
            a.close_prompt = true;
        });
        shot("report", true, &|a| {
            a.view = View::Report;
            a.refresh_matches(true);
            if let Some(r) = a.matches.iter().find(|r| r.map == "mission-6") {
                a.session_pick = Some(r.dir.clone());
            }
            // The crash that belongs to the picked match rides along; an older one does not.
            let logs = a.log_root().unwrap();
            let mid = a
                .matches
                .iter()
                .find(|r| r.map == "mission-6")
                .map(|r| r.match_id.clone())
                .unwrap_or_default();
            std::fs::write(
                logs.join("mh_crash_00000000000000aa.marker"),
                format!("mh_crash=1\ncode=c0000005\npid=42\ntid=7\nmodule=mh.dll\noffset=0x10\nmatch_id={mid}\nwhen=20261007T090000Z\nbuild=0.2.0\n"),
            )
            .unwrap();
            a.description = if loc == Locale::Ru {
                "Игра вылетела на шестой миссии.".into()
            } else {
                "The game crashed on mission 6.".into()
            };
        });
        shot("report_consent", true, &|a| {
            a.view = View::Report;
            a.consent = Some(crate::upload::Prepared {
                zip: PathBuf::from("C:\\Users\\player\\AppData\\Local\\MissionHumanity\\reports\\mh_report_20261008T220000Z.zip"),
                bytes: vec![0; 2_400_000],
                sha256: "9f2c".repeat(16),
                entries: vec![
                    ("report.json".into(), 1_820),
                    ("description.txt".into(), 31),
                    ("logs/2026-10-08T20-10-02Z_aabbccdd_krater_host/mh_net.log".into(), 612_403),
                    ("config/mh_net.ini".into(), 402),
                ],
                description: "The game crashed on mission 6.".into(),
                meta: "{}".into(),
                destination: "https://reports.example.invalid/v1/reports".into(),
            });
        });
        shot("about", true, &|a| a.view = View::About);
        shot("about_license", true, &|a| {
            a.view = View::About;
            a.about_doc = Some(super::about::Doc::License);
        });
        shot("diagnostics", true, &|a| a.view = View::Diagnostics);
        shot("restart_prompt", true, &|a| {
            a.auto.pending_restart = Some(crate::update::StagedLauncher {
                version: "0.2.1".into(),
                candidate: PathBuf::from("never_written.exe"),
            });
        });
    }
    i18n::set_locale(Locale::En);
}
