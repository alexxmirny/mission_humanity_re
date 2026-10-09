//! The Play page: what is installed, whether there is an update, the game folder, what is new.
//! (The Start game button itself is in the shell.)

use egui::{Color32, RichText, Ui};

use crate::app::{App, UpdateAction, View};
use crate::discord;
use crate::i18n::{tr, trf};
use crate::install;
use crate::launch::Outcome;
use crate::paths;
use crate::settings::schema::Provider;
use crate::{theme, widgets};

pub fn show(app: &mut App, ui: &mut Ui) {
    app.ensure_model();
    let receipt = app.game_dir().as_deref().and_then(install::read_manifest);

    cards(app, ui, receipt.as_ref());
    if app.marker.is_some() && app.session.is_none() {
        crash_prompt(app, ui);
    }
    folder_block(app, ui);
    update_block(app, ui);
    whats_new(app, ui);
    run_block(app, ui);
}

fn cards(app: &App, ui: &mut Ui, receipt: Option<&install::Manifest>) {
    let (version, version_color) = match receipt {
        Some(m) => {
            let (state, color) = match app.offer.as_ref() {
                Some(o) if o.game.is_some() => (Some(tr("play.update_available")), theme::HAZARD),
                Some(_) => (Some(tr("play.up_to_date")), theme::OK),
                None => (None, theme::TEXT),
            };
            match state {
                Some(s) => (format!("{} · {s}", m.version), color),
                None => (m.version.clone(), color),
            }
        }
        None => (tr("play.not_installed").to_string(), theme::HAZARD),
    };
    let (relay, relay_color) = relay_value(app);
    let (discord, discord_color) = discord_value(app);
    ui.columns(2, |cols| {
        widgets::card(
            &mut cols[0],
            tr("play.game_version"),
            &version,
            version_color,
        );
        // The channel's name is its name in every language.
        widgets::card(
            &mut cols[1],
            tr("play.channel"),
            app.config.channel(),
            theme::TEXT,
        );
    });
    ui.columns(2, |cols| {
        widgets::card(&mut cols[0], tr("play.relay"), &relay, relay_color);
        widgets::card(&mut cols[1], tr("play.discord"), &discord, discord_color);
    });
    if app.config.relay_mode() == "auto" && app.relay.is_some() && !app.relay_from.is_empty() {
        widgets::hint(
            ui,
            &format!("{} {}", tr("play.relay_from.manifest"), app.relay_from),
        );
    }
}

fn relay_value(app: &App) -> (String, Color32) {
    match app.config.relay_mode() {
        "off" => (tr("play.relay.off").to_string(), theme::TEXT_DIM),
        "custom" => match app.effective_relay() {
            Some(r) => (
                format!("{} · {}", tr("play.relay.custom"), r.addr),
                theme::OK,
            ),
            None => (
                format!(
                    "{} · {}",
                    tr("play.relay.custom"),
                    tr("play.relay_from.invalid")
                ),
                theme::HAZARD,
            ),
        },
        _ => match app.relay.as_ref() {
            Some(r) => (format!("{} · {}", tr("play.relay.auto"), r.addr), theme::OK),
            None => (
                format!("{} · {}", tr("play.relay.auto"), tr("play.relay_from.none")),
                theme::HAZARD,
            ),
        },
    }
}

fn discord_value(app: &App) -> (String, Color32) {
    if !app.config.discord {
        return (tr("play.discord.off").to_string(), theme::TEXT_DIM);
    }
    // The effective id: launcher.toml's own, else the one built in (RL20). The raw field is empty on
    // every normal install, which made this card say "not set up" while presence worked.
    if app.config.effective_discord_client_id().is_empty() {
        return (tr("play.discord.unconfigured").to_string(), theme::TEXT_DIM);
    }
    // While the game runs, say what is on show (the writer is `discord.rs`'s worker; this reads the
    // same presence.json it does).
    if let (Some(s), Some(root)) = (app.session.as_ref(), app.log_root()) {
        if let Some(a) = discord::read_activity(&root.join(discord::PRESENCE_FILE), s.pid()) {
            return (
                trf("play.discord.showing", &[("state", &a.details)]),
                theme::OK,
            );
        }
    }
    (tr("play.discord.on").to_string(), theme::OK)
}

fn crash_prompt(app: &mut App, ui: &mut Ui) {
    let Some(m) = app.marker.clone() else {
        return;
    };
    widgets::group_header(ui, tr("play.crash.title"));
    ui.label(
        RichText::new(trf(
            "play.crash.body",
            &[
                ("where", &m.where_text()),
                ("code", &format!("0x{:08x}", m.code)),
            ],
        ))
        .color(theme::ALERT),
    );
    ui.horizontal_wrapped(|ui| {
        if widgets::gbtn(ui, tr("play.crash.report")).clicked() {
            app.view = View::Report;
        }
        if widgets::gbtn(ui, tr("play.crash.dismiss")).clicked() {
            app.dismiss_crash();
        }
    });
    widgets::hint(ui, tr("play.crash.hint"));
}

fn folder_block(app: &mut App, ui: &mut Ui) {
    widgets::group_header(ui, tr("play.game_folder"));
    if app.game_dir_ok() {
        if let Some(d) = app.game_dir() {
            ui.add(
                egui::Label::new(
                    RichText::new(d.display().to_string())
                        .monospace()
                        .size(12.0),
                )
                .wrap(),
            );
        }
        ui.horizontal_wrapped(|ui| {
            if widgets::gbtn(ui, tr("btn.change")).clicked() {
                app.browse_game_dir();
            }
        });
        let pack = app.model.baseline("lang.pack");
        let line = if pack.trim().is_empty() {
            tr("play.no_pack").to_string()
        } else {
            let label = app
                .model
                .provider_options(Provider::LangPacks)
                .iter()
                .find(|o| o.value == pack.trim())
                .map_or_else(|| pack.trim().to_string(), |o| o.label.clone());
            trf("play.pack_line", &[("pack", &label)])
        };
        widgets::hint(ui, &line);
    } else {
        widgets::hint(ui, tr("play.no_folder"));
        widgets::hint(ui, &trf("play.put_here", &[("exe", paths::GAME_EXE)]));
        ui.horizontal_wrapped(|ui| {
            if widgets::gbtn(ui, tr("btn.browse")).clicked() {
                app.browse_game_dir();
            }
        });
        let changed = ui
            .add(
                egui::TextEdit::singleline(&mut app.game_dir_edit)
                    .desired_width(f32::INFINITY)
                    .hint_text(trf("play.folder_hint", &[("exe", paths::GAME_EXE)])),
            )
            .changed();
        if changed {
            app.persist();
        }
        if let Some(d) = app.game_dir() {
            if !paths::is_game_dir(&d) {
                ui.label(
                    RichText::new(trf(
                        "play.no_exe_in",
                        &[("exe", paths::GAME_EXE), ("dir", &d.display().to_string())],
                    ))
                    .size(11.0)
                    .color(theme::ALERT),
                );
            }
        }
    }
}

/// The player's-language sentence for what the last check found.
fn offer_text(app: &App) -> String {
    match app.offer.as_ref() {
        None if app
            .job
            .as_ref()
            .is_some_and(|j| j.action == UpdateAction::AutoCheck) =>
        {
            tr("play.checking").to_string()
        }
        None => String::new(),
        Some(o) => match (&o.game, &o.launcher) {
            (None, None) if o.blocked.is_some() && o.no_launcher => {
                tr("play.launcher_needed_by_hand").to_string()
            }
            (None, None) if o.blocked.is_some() => tr("play.launcher_needed").to_string(),
            (None, None) if o.no_launcher => tr("play.current_no_launcher").to_string(),
            (None, None) => tr("play.all_current").to_string(),
            (Some(g), None) if o.no_launcher => {
                trf("play.offer.game_no_launcher", &[("version", g)])
            }
            (Some(g), None) => trf("play.offer.game", &[("version", g)]),
            (None, Some(l)) => trf("play.offer.launcher", &[("version", l)]),
            (Some(g), Some(l)) => trf("play.offer.both", &[("game", g), ("launcher", l)]),
        },
    }
}

fn update_block(app: &mut App, ui: &mut Ui) {
    widgets::group_header(ui, tr("play.update"));
    let busy = app.job.is_some();
    let running = app.session.is_some();
    let line = offer_text(app);
    if !line.is_empty() {
        ui.label(&line);
    }
    let has_offer = app.offer.as_ref().is_some_and(crate::update::Offer::any);
    let can = !busy && !running && app.game_dir_ok();
    ui.horizontal_wrapped(|ui| {
        ui.add_enabled_ui(can && has_offer, |ui| {
            if widgets::gbtn(ui, tr("play.install_update"))
                .on_hover_text(tr("play.update_hint"))
                .clicked()
            {
                app.start_update_from(UpdateAction::Update, Some(View::Play));
            }
        });
        ui.add_enabled_ui(!busy, |ui| {
            if widgets::gbtn(ui, tr("play.check_updates")).clicked() {
                app.start_update_from(UpdateAction::AutoCheck, None);
            }
        });
        if busy {
            ui.spinner();
        }
    });
    // What the install Play started is doing now, in place -- and, when it is over, what it said.
    if let Some(progress) = app.play_progress() {
        ui.horizontal_wrapped(|ui| {
            ui.spinner();
            ui.label(progress);
        });
    } else if app.pending_launch.is_none()
        && !app.update_line.is_empty()
        && !busy
        && app.last_job_from_play
    {
        let color = if app.update_is_error {
            theme::ALERT
        } else {
            theme::TEXT_DIM
        };
        ui.label(RichText::new(&app.update_line).size(11.0).color(color));
    }
    // The freeze mitigation that replaced the 30-day STALE refusal (dist RL8): say how old the
    // manifest the offer came from is, so a pinned file is visible to a player.
    if let Some(days) = app.offer.as_ref().and_then(|o| o.age_days) {
        widgets::hint(
            ui,
            &trf("play.manifest_age", &[("days", &days.to_string())]),
        );
    }
}

fn whats_new(app: &App, ui: &mut Ui) {
    let url = app.accepted.notes_url.trim();
    // The URL is inside the signed manifest, but a link the OS will open is checked anyway.
    if !url.starts_with("https://") {
        return;
    }
    widgets::group_header(
        ui,
        &trf("play.whats_new", &[("version", &app.accepted.version)]),
    );
    ui.hyperlink_to(tr("play.full_notes"), url);
}

/// The text for a finished run, in the player's language.
pub fn outcome_text(o: &Outcome) -> String {
    match o {
        Outcome::Normal => tr("play.run.normal").to_string(),
        Outcome::Nonzero(c) => trf("play.run.nonzero", &[("code", &format!("0x{c:08X}"))]),
        Outcome::Crash(c) => trf("play.run.crash", &[("code", &format!("0x{c:08X}"))]),
    }
}

fn run_block(app: &mut App, ui: &mut Ui) {
    if let Some(s) = app.session.as_ref() {
        widgets::group_header(ui, tr("play.run.title"));
        ui.label(trf(
            "play.run.running",
            &[
                ("pid", &s.pid().to_string()),
                ("secs", &format!("{:.0}", s.started.elapsed().as_secs_f64())),
            ],
        ));
        let token = match s.elevated {
            Some(true) => "play.run.token.elevated",
            Some(false) => "play.run.token.normal",
            None => "play.run.token.unknown",
        };
        widgets::hint(ui, tr(token));
        widgets::hint(ui, tr("play.run.running_hint"));
    } else if let Some(f) = app.last_run.as_ref() {
        widgets::group_header(ui, tr("play.run.last"));
        let text = outcome_text(&f.outcome);
        let color = if f.outcome.is_crash() {
            theme::ALERT
        } else {
            theme::TEXT
        };
        ui.label(RichText::new(text).color(color));
        widgets::hint(
            ui,
            &trf("play.run.after", &[("secs", &format!("{:.1}", f.seconds))]),
        );
    }
}
