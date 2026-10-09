//! The Report page: pick the match that went wrong from a list, say what happened, build the zip,
//! then the consent screen (nothing is sent without it -- dist RP1, plan D13).

use chrono::{Local, NaiveDateTime, TimeZone, Utc};
use egui::{
    epaint::StrokeKind, pos2, vec2, Color32, CornerRadius, FontId, Rect, RichText, Sense, Stroke,
    Ui,
};

use crate::app::App;
use crate::i18n::{tr, trf};
use crate::report::{self, MatchOutcome, MatchRow};
use crate::upload;
use crate::{log, theme, widgets};

pub fn show(app: &mut App, ui: &mut Ui) {
    // THE CONSENT SCREEN REPLACES THE PAGE rather than sitting inside it. While a report is waiting
    // to be agreed to there is exactly one decision in front of the player, and the form that built
    // it is not a second thing to fiddle with (plan D13).
    if app.consent.is_some() {
        consent_view(app, ui);
        return;
    }

    widgets::hint(ui, tr("report.intro"));
    outbox_block(app, ui);
    if !app.upload_line.is_empty() {
        let color = if app.upload_is_error {
            theme::ALERT
        } else {
            theme::TEXT
        };
        ui.label(RichText::new(&app.upload_line).color(color));
    }

    widgets::group_header(ui, tr("report.which_match"));
    app.refresh_matches(false);
    let chosen = app
        .chosen_session_dir()
        .and_then(|p| p.file_name().map(|n| n.to_string_lossy().to_string()))
        .unwrap_or_default();
    if app.matches.is_empty() {
        widgets::hint(ui, tr("report.no_matches"));
    } else if let Some(dir) = match_table(ui, &app.matches, &chosen) {
        app.session_pick = Some(dir);
    }

    widgets::group_header(ui, tr("report.what_happened"));
    ui.add(
        egui::TextEdit::multiline(&mut app.description)
            .desired_width(f32::INFINITY)
            .desired_rows(3)
            .hint_text(tr("report.description_hint")),
    );
    let have_description = report::description_ok(&app.description);
    if !have_description {
        ui.label(
            RichText::new(tr("report.description_required"))
                .size(11.0)
                .color(theme::ALERT),
        );
    }

    // The buttons come BEFORE the long "what goes in" text: at 800x600 the page scrolls, and the
    // two things a player came for -- build it, then look at it -- must not be below the fold.
    build_row(app, ui, have_description);
    built_block(app, ui);
    what_goes_in(app, ui);
}

fn outbox_block(app: &mut App, ui: &mut Ui) {
    if app.outbox_pending.is_empty() {
        return;
    }
    widgets::group_header(
        ui,
        &trf(
            "report.outbox",
            &[("n", &app.outbox_pending.len().to_string())],
        ),
    );
    for p in app.outbox_pending.clone() {
        ui.horizontal_wrapped(|ui| {
            ui.label(
                RichText::new(
                    p.file_name()
                        .unwrap_or_default()
                        .to_string_lossy()
                        .to_string(),
                )
                .monospace()
                .size(11.0),
            );
            if widgets::gbtn(ui, tr("report.outbox_send")).clicked() {
                app.ask_consent(p.clone());
            }
        });
    }
}

// ---- the match list ------------------------------------------------------------------------------

const ROW_H: f32 = 24.0;
/// Column starts as fractions of the table's width: when | mode | map | players | result.
const COLS: [f32; 5] = [0.0, 0.25, 0.42, 0.60, 0.80];

/// "Today 21:14" / "Yesterday 23:40" / "2026-10-04 18:20", from LOCAL naive times.
pub fn when_text(t: NaiveDateTime, now: NaiveDateTime) -> String {
    let hm = t.format("%H:%M");
    let days = (now.date() - t.date()).num_days();
    match days {
        0 => format!("{} {hm}", tr("report.today")),
        1 => format!("{} {hm}", tr("report.yesterday")),
        _ => format!("{} {hm}", t.format("%Y-%m-%d")),
    }
}

fn local_of(utc: NaiveDateTime) -> NaiveDateTime {
    Utc.from_utc_datetime(&utc)
        .with_timezone(&Local)
        .naive_local()
}

fn mode_text(mode: &str) -> &'static str {
    match mode {
        "network" => tr("report.mode.network"),
        "campaign" => tr("report.mode.campaign"),
        "skirmish" => tr("report.mode.skirmish"),
        "tutorial" => tr("report.mode.tutorial"),
        "tactical" => tr("report.mode.tactical"),
        _ => tr("report.mode.unknown"),
    }
}

fn outcome_style(o: MatchOutcome) -> (&'static str, Color32) {
    match o {
        MatchOutcome::Finished => (tr("report.outcome.finished"), theme::TEXT),
        MatchOutcome::Quit => (tr("report.outcome.quit"), theme::TEXT_DIM),
        MatchOutcome::Desync => (tr("report.outcome.desync"), theme::ALERT),
        MatchOutcome::Crash => (tr("report.outcome.crash"), theme::ALERT),
        MatchOutcome::Running => (tr("report.outcome.running"), theme::HAZARD),
        MatchOutcome::Unknown => (tr("report.outcome.unknown"), theme::TEXT_DIM),
    }
}

/// The players cell: names, then "+ N AI". A single "—" when nothing is known.
pub fn players_text(r: &MatchRow) -> String {
    let mut s = r.players.join(", ");
    if r.ai_count > 0 {
        let ai = trf("report.ai_count", &[("n", &r.ai_count.to_string())]);
        s = if s.is_empty() {
            ai
        } else {
            format!("{s}, {ai}")
        };
    }
    if s.is_empty() {
        tr("report.mode.unknown").to_string()
    } else {
        s
    }
}

/// One line of text, cut with an ellipsis at `width`, painted at `pos`.
fn cell(ui: &Ui, pos: egui::Pos2, width: f32, text: &str, size: f32, color: Color32) {
    let mut job = egui::text::LayoutJob::simple(
        text.to_string(),
        FontId::proportional(size),
        color,
        width.max(8.0),
    );
    job.wrap.max_rows = 1;
    job.wrap.break_anywhere = true;
    job.wrap.overflow_character = Some('…');
    let galley = ui.painter().layout_job(job);
    ui.painter().galley(pos, galley, color);
}

fn pill_at(ui: &Ui, left_center: egui::Pos2, max_w: f32, text: &str, color: Color32) {
    let galley = ui
        .painter()
        .layout_no_wrap(text.to_string(), FontId::proportional(10.0), color);
    let w = (galley.size().x + 12.0).min(max_w);
    let rect = Rect::from_min_size(pos2(left_center.x, left_center.y - 9.0), vec2(w, 18.0));
    ui.painter().rect_stroke(
        rect,
        CornerRadius::same(8),
        Stroke::new(1.0, color),
        StrokeKind::Inside,
    );
    ui.painter().with_clip_rect(rect).galley(
        pos2(rect.left() + 6.0, rect.center().y - galley.size().y / 2.0),
        galley,
        color,
    );
}

/// The match table. Returns the directory of the row clicked this frame.
fn match_table(ui: &mut Ui, rows: &[MatchRow], chosen: &str) -> Option<String> {
    let width = ui.available_width();
    let x_of = |i: usize, left: f32| left + COLS[i] * width;
    let col_w = |i: usize| {
        let end = if i + 1 < COLS.len() { COLS[i + 1] } else { 1.0 };
        (end - COLS[i]) * width - 8.0
    };
    // Header.
    let (hrect, _) = ui.allocate_exact_size(vec2(width, 18.0), Sense::hover());
    for (i, key) in [
        "report.col.when",
        "report.col.mode",
        "report.col.map",
        "report.col.players",
        "report.col.result",
    ]
    .iter()
    .enumerate()
    {
        cell(
            ui,
            pos2(x_of(i, hrect.left()) + 6.0, hrect.top() + 3.0),
            col_w(i) - 6.0,
            &tr(key).to_uppercase(),
            10.0,
            theme::TEXT_DIM,
        );
    }
    ui.painter().hline(
        hrect.x_range(),
        hrect.bottom(),
        Stroke::new(1.0, theme::GROUP_LINE),
    );

    let now = Local::now().naive_local();
    let mut clicked = None;
    egui::ScrollArea::vertical()
        .id_salt("match_list")
        .max_height(ROW_H * 4.5)
        .auto_shrink([false, true])
        .show(ui, |ui| {
            ui.spacing_mut().item_spacing.y = 0.0;
            for r in rows {
                let (rect, resp) = ui.allocate_exact_size(vec2(width, ROW_H), Sense::click());
                let selected = r.dir == chosen;
                let (outcome_label, outcome_color) = outcome_style(r.outcome);
                resp.widget_info(|| {
                    egui::WidgetInfo::selected(
                        egui::WidgetType::SelectableLabel,
                        true,
                        selected,
                        format!("{} {outcome_label}", r.dir),
                    )
                });
                if selected {
                    ui.painter()
                        .rect_filled(rect, CornerRadius::ZERO, theme::SEL_BG);
                } else if resp.hovered() {
                    ui.painter().rect_filled(
                        rect,
                        CornerRadius::ZERO,
                        Color32::from_rgba_unmultiplied(0x0c, 0x3a, 0x0c, 0x80),
                    );
                }
                ui.painter().hline(
                    rect.x_range(),
                    rect.bottom(),
                    Stroke::new(1.0, theme::GROUP_LINE),
                );
                let ty = rect.top() + 5.0;
                let when = r
                    .when
                    .map(|t| when_text(local_of(t), now))
                    .unwrap_or_else(|| tr("report.mode.unknown").to_string());
                let map = if r.map.is_empty() {
                    tr("report.mode.unknown").to_string()
                } else {
                    r.map.clone()
                };
                let cells = [when, mode_text(&r.mode).to_string(), map, players_text(r)];
                for (i, text) in cells.iter().enumerate() {
                    cell(
                        ui,
                        pos2(x_of(i, rect.left()) + 6.0, ty),
                        col_w(i) - 6.0,
                        text,
                        12.0,
                        theme::TEXT,
                    );
                }
                pill_at(
                    ui,
                    pos2(x_of(4, rect.left()) + 4.0, rect.center().y),
                    col_w(4),
                    outcome_label,
                    outcome_color,
                );
                if resp.clicked() {
                    clicked = Some(r.dir.clone());
                }
                resp.on_hover_cursor(egui::CursorIcon::PointingHand);
            }
        });
    clicked
}

// ---- what goes in --------------------------------------------------------------------------------

fn what_goes_in(app: &mut App, ui: &mut Ui) {
    widgets::group_header(ui, tr("report.what_goes_in"));
    if let Some(f) = app.last_run.as_ref() {
        ui.label(
            RichText::new(super::play::outcome_text(&f.outcome))
                .size(11.0)
                .monospace(),
        );
    }
    let session = app.chosen_session_dir();
    match session.as_ref() {
        Some(dir) => {
            let count = std::fs::read_dir(dir)
                .map(|r| r.flatten().count())
                .unwrap_or(0);
            ui.add(
                egui::Label::new(
                    RichText::new(format!(
                        "{}\\ {}",
                        dir.file_name().unwrap_or_default().to_string_lossy(),
                        trf("report.files_from", &[("n", &count.to_string())])
                    ))
                    .monospace()
                    .size(11.0),
                )
                .wrap(),
            );
        }
        None => {
            widgets::hint(ui, tr("report.no_session"));
        }
    }
    // dist LA16: the crash markers this report would carry -- only those of the selected session.
    if let Some((root, dir)) = session
        .as_deref()
        .and_then(|d| Some((d.parent()?.to_path_buf(), d)))
    {
        for m in report::markers_for_session(&root, dir) {
            ui.label(
                RichText::new(trf(
                    "report.crash_marker",
                    &[("file", &m.file_name().unwrap_or_default().to_string_lossy())],
                ))
                .size(11.0)
                .color(theme::ALERT),
            );
        }
    }
    if app.session.is_some() {
        ui.label(
            RichText::new(tr("report.game_running"))
                .size(11.0)
                .color(theme::HAZARD),
        );
    }
    widgets::hint(ui, tr("report.scope_hint"));
    widgets::hint(ui, tr("report.key_hint"));
    match app.dump.as_ref() {
        Some(p) => {
            let mb = std::fs::metadata(p).map(|m| m.len()).unwrap_or(0) as f64 / 1_048_576.0;
            ui.checkbox(
                &mut app.include_dump,
                trf("report.include_dump", &[("mb", &format!("{mb:.1}"))]),
            );
            widgets::hint(ui, tr("report.dump_hint"));
        }
        None => {
            widgets::hint(ui, tr("report.no_dump"));
        }
    }
}

fn build_row(app: &mut App, ui: &mut Ui, have_description: bool) {
    let building = app.report_job.is_some();
    ui.add_space(6.0);
    ui.horizontal_wrapped(|ui| {
        ui.add_enabled_ui(have_description && !building, |ui| {
            if widgets::gbtn(ui, tr("report.build")).clicked() {
                app.do_report(None);
            }
        });
        if let Some(job) = app.report_job.as_ref() {
            ui.spinner();
            ui.label(job.progress().text());
        }
        if app.built.is_some() && widgets::gbtn(ui, tr("report.show_file")).clicked() {
            if let Some(b) = app.built.as_ref() {
                let dir = b.zip.parent().unwrap_or(&b.zip).to_path_buf();
                // `explorer` rather than a shell association: the target is a folder, and a
                // launcher that opened the zip itself would hand it to whatever the player has
                // associated with .zip, which is not what "show me" means.
                let _ = std::process::Command::new("explorer").arg(dir).spawn();
            }
        }
    });
    widgets::hint(ui, tr("report.hint"));
}

fn built_block(app: &mut App, ui: &mut Ui) {
    let Some(b) = app.built.as_ref() else {
        return;
    };
    let zip = b.zip.clone();
    ui.add(
        egui::Label::new(
            RichText::new(zip.display().to_string())
                .monospace()
                .size(11.0),
        )
        .wrap(),
    );
    ui.collapsing(
        trf("report.n_files", &[("n", &b.entries.len().to_string())]),
        |ui| {
            for e in &b.entries {
                ui.small(e);
            }
        },
    );
    // The whole of report.json, on screen, before anything is sent. It is the one part of a report
    // that is ABOUT the player's machine rather than about the game, so showing it is not a
    // debugging affordance -- it is how a player finds out what they would be handing over without
    // having to open a zip to do it.
    ui.collapsing(tr("report.meta"), |ui| {
        ui.monospace(&b.meta);
    });

    // dist RP1: the send offer. It appears only once a zip EXISTS, and it does not send -- it
    // opens the consent screen, which is the thing that can.
    ui.add_space(6.0);
    match app.collector() {
        Ok(c) => {
            let can = app.upload_job.is_none();
            ui.horizontal_wrapped(|ui| {
                ui.add_enabled_ui(can, |ui| {
                    if widgets::gbtn(ui, tr("report.review"))
                        .on_hover_text(format!("-> {}", c.endpoint()))
                        .clicked()
                    {
                        app.ask_consent(zip.clone());
                    }
                });
                if !can {
                    ui.label(tr("report.sending"));
                }
            });
        }
        Err(e) => {
            widgets::hint(ui, &trf("report.cannot_send", &[("error", &e)]));
        }
    }
}

/// **The consent screen (dist RP1, plan decision D13).** Every entry in the zip, its digest, the
/// destination and the whole of `report.json`, then two buttons. It renders `upload::consent_text`
/// -- the same text `--send` prints -- so the window and the command line cannot describe the same
/// upload differently.
fn consent_view(app: &mut App, ui: &mut Ui) {
    let Some(p) = app.consent.clone() else {
        return;
    };
    ui.label(RichText::new(tr("report.consent.lead")).strong());
    ui.add_space(6.0);
    egui::ScrollArea::vertical()
        .id_salt("consent_text")
        .max_height(280.0)
        .auto_shrink([false, true])
        .show(ui, |ui| {
            ui.monospace(upload::consent_text(&p));
        });
    ui.add_space(8.0);
    ui.horizontal_wrapped(|ui| {
        if widgets::gbtn(ui, tr("report.send")).clicked() {
            app.send_consented();
        }
        if widgets::gbtn(ui, tr("report.dont_send")).clicked() {
            log::line("upload: the player declined to send");
            app.consent = None;
            let zip = p.zip.display().to_string();
            app.upload_say_t("msg.upload_declined", &[("file", &zip)], false);
        }
    });
    widgets::hint(ui, &trf("report.consent.to", &[("to", &p.destination)]));
}

#[cfg(test)]
mod tests {
    use super::*;

    fn dt(s: &str) -> NaiveDateTime {
        NaiveDateTime::parse_from_str(s, "%Y-%m-%d %H:%M").unwrap()
    }

    #[test]
    fn times_read_today_yesterday_then_a_date() {
        let now = dt("2026-10-08 22:00");
        assert_eq!(when_text(dt("2026-10-08 21:14"), now), "Today 21:14");
        assert_eq!(when_text(dt("2026-10-07 23:40"), now), "Yesterday 23:40");
        assert_eq!(when_text(dt("2026-10-04 18:20"), now), "2026-10-04 18:20");
    }

    #[test]
    fn the_players_cell_names_humans_then_the_ai_and_says_dash_when_unknown() {
        let mut r = report::parse_match_row("2026-10-08T10-00-00Z_aabbccdd_krater_host", None);
        assert_eq!(players_text(&r), "—");
        r.players = vec!["you".into(), "Sasha".into()];
        r.ai_count = 2;
        assert_eq!(players_text(&r), "you, Sasha, 2 AI");
        r.players.clear();
        assert_eq!(players_text(&r), "2 AI");
    }
}
