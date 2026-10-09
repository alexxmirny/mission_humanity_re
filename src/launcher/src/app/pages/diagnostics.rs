//! The Diagnostics page: the settings that are for support rather than for play (log level, the
//! Compatibility group), build facts, the last launch, Copy debug info, and -- when RL9 supplies
//! them -- Re-verify and Roll back. The old Status tab's maintenance bits (install from a zip,
//! uninstall, the update source) live under "Advanced".

use std::path::PathBuf;

use egui::{RichText, Ui};

use crate::app::App;
use crate::i18n::{tr, trf};
use crate::install;
use crate::settings::render;
use crate::{log, widgets};

pub fn show(app: &mut App, ui: &mut Ui) {
    app.ensure_model();
    if let Some((ini, source)) = app.settings_ini() {
        render::show_diagnostics(ui, &mut app.model, &mut app.settings_state);
        ui.add_space(4.0);
        // Apply / Revert only: where the file is is the Settings page's hint, and this page is
        // already long for 600 px.
        super::settings::footer(app, ui, &ini, source, false);
    } else {
        widgets::hint(ui, tr("play.no_folder"));
    }

    widgets::group_header(ui, tr("diag.build"));
    for line in build_lines(app) {
        ui.add(egui::Label::new(RichText::new(line).monospace().size(12.0)).wrap());
    }

    widgets::group_header(ui, tr("diag.last_launch"));
    ui.add(egui::Label::new(RichText::new(last_launch_line(app)).monospace().size(12.0)).wrap());

    ui.add_space(6.0);
    ui.horizontal_wrapped(|ui| {
        // RL9: the actions exist only when the back end can do them.
        if let Some(r) = app.repair_actions() {
            if r.reverify && widgets::gbtn_dev(ui, tr("diag.reverify")).clicked() {
                app.reverify();
            }
            if r.rollback && widgets::gbtn_dev(ui, tr("diag.rollback")).clicked() {
                app.rollback();
            }
        }
        if widgets::gbtn(ui, tr("diag.copy")).clicked() {
            let text = debug_info(app);
            ui.ctx().copy_text(text);
            app.last_copied = Some(std::time::Instant::now());
            app.say_t("msg.debug_copied", &[], false);
        }
    });

    advanced(app, ui);
}

/// The build facts, one per line: launcher, game build (from the install receipt), the signed
/// manifest the offer came from and how old it is, and the language pack.
pub fn build_lines(app: &App) -> Vec<String> {
    let receipt = app.game_dir().as_deref().and_then(install::read_manifest);
    let game = match &receipt {
        Some(m) => format!("{} ({})", m.version, m.tag),
        None => tr("play.not_installed").to_string(),
    };
    let manifest = if app.accepted.issued_at.is_empty() {
        tr("diag.no_manifest").to_string()
    } else {
        let age = app.offer.as_ref().and_then(|o| o.age_days).or_else(|| {
            crate::update::issued_age_days(&app.accepted.issued_at, chrono::Utc::now())
        });
        match age {
            Some(d) => trf(
                "diag.manifest_age",
                &[
                    (
                        "issued",
                        app.accepted.issued_at.split('T').next().unwrap_or(""),
                    ),
                    ("days", &d.to_string()),
                ],
            ),
            None => app.accepted.issued_at.clone(),
        }
    };
    let pack = {
        let p = app.model.baseline("lang.pack");
        if p.trim().is_empty() {
            "en".to_string()
        } else {
            p.trim().to_string()
        }
    };
    let no_launcher = app.offer.as_ref().is_some_and(|o| o.no_launcher);
    vec![
        format!(
            "{} {} · {} {game}{}",
            tr("diag.launcher"),
            crate::version::VERSION,
            tr("diag.game"),
            if no_launcher {
                format!(" · {}", tr("diag.no_launcher"))
            } else {
                String::new()
            }
        ),
        format!("{} {manifest}", tr("diag.manifest")),
        format!(
            "{} {pack} · {} {}",
            tr("diag.lang"),
            tr("diag.channel"),
            app.config.channel()
        ),
    ]
}

pub fn last_launch_line(app: &App) -> String {
    let roots = app.log_roots();
    let sessions = crate::report::session_dirs_in(&roots).len();
    let unreported = crate::report::unreported_markers_in(&roots).len();
    match app.last_run.as_ref() {
        Some(f) => {
            let mut s = format!(
                "{} · {}",
                super::play::outcome_text(&f.outcome),
                trf("play.run.after", &[("secs", &format!("{:.0}", f.seconds))])
            );
            s.push_str(&format!(
                " · {}",
                trf("diag.sessions", &[("n", &sessions.to_string())])
            ));
            if unreported > 0 {
                s.push_str(&format!(
                    " · {}",
                    trf("diag.crash_markers", &[("n", &unreported.to_string())])
                ));
            }
            s
        }
        None => format!(
            "{} · {}",
            tr("diag.not_launched"),
            trf("diag.sessions", &[("n", &sessions.to_string())])
        ),
    }
}

/// The text Copy debug info puts on the clipboard: English on purpose (it is pasted into a bug
/// report), and free of secrets (no key, no token).
pub fn debug_info(app: &App) -> String {
    let mut out = String::new();
    out.push_str(&format!("launcher {}\n", crate::version::VERSION));
    let m = crate::machine::Machine::detect();
    out.push_str(&format!(
        "os {}\ncpu {}\ngpu {}\nram_mb {}\n",
        m.os, m.cpu, m.gpu, m.ram_mb
    ));
    out.push_str(&format!("channel {}\n", app.config.channel()));
    out.push_str(&format!("language {}\n", app.config.ui_lang_code()));
    out.push_str(&format!("relay_mode {}\n", app.config.relay_mode()));
    match app.game_dir() {
        Some(d) => {
            out.push_str(&format!("game_dir {}\n", d.display()));
            let (dir, source) = crate::cfgdir::game_config_dir(&app.layout, &d);
            out.push_str(&format!(
                "config_dir {} [{}]\n",
                dir.display(),
                source.describe()
            ));
            match install::read_manifest(&d) {
                Some(r) => out.push_str(&format!(
                    "installed {} {} ({} files, {})\n",
                    r.tag,
                    r.version,
                    r.files.len(),
                    r.installed_at
                )),
                None => out.push_str("installed (nothing)\n"),
            }
            out.push_str(&format!(
                "logs {}\n",
                app.layout.game_log_root(&d).display()
            ));
        }
        None => out.push_str("game_dir (none)\n"),
    }
    out.push_str(&format!(
        "manifest {} issued {}\n",
        app.accepted.version, app.accepted.issued_at
    ));
    match app.last_run.as_ref() {
        Some(f) => out.push_str(&format!(
            "last_run {} after {:.1}s\n",
            f.outcome.describe(),
            f.seconds
        )),
        None => out.push_str("last_run (none)\n"),
    }
    if let Some(p) = log::path() {
        out.push_str(&format!("launcher_log {}\n", p.display()));
    }
    out
}

fn advanced(app: &mut App, ui: &mut Ui) {
    ui.add_space(6.0);
    ui.collapsing(tr("diag.advanced"), |ui| {
        widgets::hint(ui, tr("diag.source"));
        let changed = ui
            .add(
                egui::TextEdit::singleline(&mut app.update_base_edit)
                    .desired_width(f32::INFINITY)
                    .hint_text(if crate::update::DEFAULT_BASE_URL.is_empty() {
                        "https://<your pages site>/"
                    } else {
                        crate::update::DEFAULT_BASE_URL
                    }),
            )
            .changed();
        if changed {
            app.config.update_base_url = app.update_base_edit.trim().to_string();
            if let Err(e) = app.config.save(&app.layout.config()) {
                log::line(format!("config: {e}"));
            }
        }
        widgets::hint(
            ui,
            &trf("diag.source_hint", &[("channel", app.config.channel())]),
        );

        widgets::group_header(ui, tr("diag.zip"));
        ui.add(
            egui::TextEdit::singleline(&mut app.zip_edit)
                .desired_width(f32::INFINITY)
                .hint_text("mission_humanity_re-<version>-<configuration>.zip"),
        );
        let installed = app.game_dir().as_deref().and_then(install::read_manifest);
        ui.horizontal_wrapped(|ui| {
            if widgets::gbtn(ui, tr("diag.choose_zip")).clicked() {
                if let Some(p) = rfd::FileDialog::new()
                    .add_filter("zip", &["zip"])
                    .set_title(tr("diag.choose_zip"))
                    .pick_file()
                {
                    app.zip_edit = p.display().to_string();
                }
            }
            let can = !app.zip_edit.trim().is_empty() && app.game_dir_ok();
            ui.add_enabled_ui(can, |ui| {
                if widgets::gbtn(ui, tr("diag.install_zip")).clicked() {
                    let zip = PathBuf::from(app.zip_edit.trim());
                    app.do_install(zip);
                }
            });
            ui.add_enabled_ui(installed.is_some(), |ui| {
                if widgets::gbtn_dev(ui, tr("diag.uninstall")).clicked() {
                    app.do_uninstall();
                }
            });
        });

        widgets::group_header(ui, tr("diag.files"));
        ui.add(
            egui::Label::new(
                RichText::new(app.layout.root.display().to_string())
                    .monospace()
                    .size(11.0),
            )
            .wrap(),
        );
        if let Some(p) = log::path() {
            ui.add(
                egui::Label::new(
                    RichText::new(format!("log: {}", p.display()))
                        .monospace()
                        .size(11.0),
                )
                .wrap(),
            );
        }
        if let Some(problem) = log::problem() {
            ui.label(RichText::new(problem).size(11.0).color(crate::theme::ALERT));
        }
        for line in log::recent(8) {
            ui.add(egui::Label::new(RichText::new(line).size(10.0)).wrap());
        }
    });
}
