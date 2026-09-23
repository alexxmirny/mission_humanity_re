//! `mh_launcher` -- the per-user launcher for Mission Humanity. dist LA1 + **LA2** + **LA6**.
//!
//! Five verbs now: it **updates itself and the game** from a signed manifest (LA2), installs a
//! release zip you already have next to your `mh.exe`, starts the game, and tells you whether the
//! process chose to stop or was stopped by a fault (LA1). The crash report is dist LA4 and still has
//! its place in the window so that arriving does not move anything. Since dist LA6 the front page
//! is **Play**: the signed manifest may name a relay, and the launcher writes it into the
//! game's `mh_net.ini` + `mh_key.txt` on install, on update and before every launch (`relay.rs`).
//!
//! THE ONLY NETWORK CODE IS `update.rs`, and it talks to two things: the manifest's base URL and the
//! absolute asset URLs that manifest carries, once its signature has verified against the key
//! compiled into this binary. Never `api.github.com` -- see that module's header for the
//! measurement behind the rule.
//!
//! WHY THERE IS A COMMAND LINE ON A GUI PROGRAM. Every clause dist LA1 is accepted on is about what
//! the program DOES -- installs, launches, classifies an exit, renders at two sizes -- and a
//! verification that can only be performed by a person clicking is a verification that is run once.
//! The flags below are how the acceptance run is scripted end to end; `--size` and `--view` are
//! also ordinary features (a small screen; coming back to the report view), and `--app-dir` is what
//! lets a test run against a throwaway state directory instead of the real `%LOCALAPPDATA%`.

// A GUI program with no console in release, and a console in debug. Losing stdout is the price of
// not flashing a black window at a player; during development the panic text is worth more than the
// tidiness, and `log::line` writes to the file in both.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod app;
mod config;
mod crash;
mod elevate;
mod install;
mod launch;
mod log;
mod machine;
mod paths;
mod relay;
mod report;
mod update;
mod upload;
mod version;

use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicI32, Ordering};

use app::{App, Startup, UpdateAction, View};
use config::Config;
use paths::Layout;

const USAGE: &str = "\
mh_launcher -- Mission Humanity launcher (dist LA1 + LA2)

usage: mh_launcher [options]

  --game-dir <path>     use this game directory (the folder holding mh.exe) and remember it
  --install <zip>       install this release zip into the game directory at startup
  --uninstall           undo the launcher's install in the game directory at startup
  --launch              start the game at startup -- exactly what the Play button does
                        (the picked configuration is installed first if it is missing, then the
                        relay from the accepted manifest is written into mh_net.ini +
                        mh_key.txt; the lobby is the game's own)
  --exit-after-launch   close the launcher once the game it started has exited
  --view <name>         open on play | status | report (default: play, the Play page)
  --size <W>x<H>        initial window size in points (default 1280x720)
  --app-dir <path>      keep launcher state here instead of %LOCALAPPDATA%\\MissionHumanity

  --check-update        fetch the signed manifest, verify it, and say what it offers
  --update              the above, then download and install the game when the manifest is
                        newer than what is installed (or the chosen configuration is missing)
  --self-update         the above, but for the launcher executable itself
                        BOTH FLAGS TOGETHER = the Update button: the launcher first, then the
                        game in the replacement launcher, in the same run (dist LA11/LA12)
  --exit-after-update   close the launcher once the startup update work has finished
  --update-url <url>    fetch the manifest from here instead, and remember it
  --verify-binary       print this build's version and exit 0 -- the health gate a NEW launcher
                        must pass before it is allowed to replace a running one
  --step <what>         perform ONE install step into the game directory and exit, no window:
                        install:<version>:<tag> (the staged set), uninstall, or provision.
                        This is what the launcher re-runs ELEVATED when the game directory is
                        not writable (Program Files) -- never the game itself (dist LA13)
  --result <file>       where --step writes its verdict (first line ok|err, then the summary)

  --report <zip>        build a report zip here once any startup work has finished, then say where
  --description <text>  the report's description. REQUIRED for --report; an empty one is refused
  --with-minidump       include the memory snapshot in that report, when the run produced one
  --exit-after-report   close the launcher once --report has written its zip

  --send <zip>          send this report to the collector and exit. Prints EXACTLY what would be
                        sent and sends nothing without --consent
  --send-outbox         the same, for every report waiting in reports\\outbox
  --consent             yes, send what was just listed. Without it nothing leaves the machine
  --outbox              list the reports waiting to be sent, and exit
  --report-url <url>    send to this collector instead of the one compiled in (https only)
  --report-token-file <path>  read the report token from here instead of the one compiled in
  --report-ca <path>    trust this root certificate (PEM) instead of the one compiled in
  --report-pin <hex>    the SHA-256 of that root's SubjectPublicKeyInfo. Required with --report-ca
  -h, --help            this text

Exit code: 0 when the requested startup work succeeded, 1 when an update or an upload was refused
or failed, 2 when a send was described and --consent was not given.
";

/// The process's exit code, decided by whatever the startup work did.
///
/// It lives here as a static because the one place that KNOWS the verdict is the eframe callback
/// that receives the update job's result, and the one place that can act on it is `main` after
/// `run_native` has returned -- and eframe hands the `App` back to nobody. A GUI program that
/// scripts cannot read is a GUI program whose acceptance test is a person clicking, which is the
/// thing LA1's command line exists to avoid.
pub static EXIT_CODE: AtomicI32 = AtomicI32::new(0);

pub fn set_exit_code(code: i32) {
    EXIT_CODE.store(code, Ordering::SeqCst);
}

struct Args {
    game_dir: Option<String>,
    app_dir: Option<PathBuf>,
    update_url: Option<String>,
    verify_binary: bool,
    /// dist LA13: the one elevated verb, and where it writes its verdict.
    step: Option<elevate::StepSpec>,
    result: Option<PathBuf>,
    view: View,
    size: [f32; 2],
    startup: Startup,
    /// dist RP1: the console-side upload verbs. They run before any window opens, for the same
    /// reason `--verify-binary` does -- they print, they exit, and a person clicking is not the
    /// thing that proves them.
    send: Option<PathBuf>,
    send_outbox: bool,
    consent: bool,
    list_outbox: bool,
    collector: CollectorArgs,
}

/// The four `--report-*` overrides. Present so a local collector can be tested against the real
/// code path instead of a second one -- the BAKED configuration is what a release uses, and these
/// only ever replace it wholesale (a half-override is refused in `resolve`).
#[derive(Default)]
struct CollectorArgs {
    url: Option<String>,
    token_file: Option<PathBuf>,
    ca: Option<PathBuf>,
    pin: Option<String>,
}

impl CollectorArgs {
    fn any(&self) -> bool {
        self.url.is_some() || self.token_file.is_some() || self.ca.is_some() || self.pin.is_some()
    }

    /// The destination this run sends to: the baked one, or the overrides when any were given.
    ///
    /// An override set is taken WHOLE. Mixing a baked certificate with a `--report-url` somewhere
    /// else would be a build trusting one server's CA while talking to another, which is either a
    /// mistake or an attempt; either way it is not a configuration this program assembles for you.
    fn resolve(&self) -> Result<upload::Collector, String> {
        if !self.any() {
            return upload::Collector::baked();
        }
        let (Some(url), Some(ca), Some(pin)) = (&self.url, &self.ca, &self.pin) else {
            return Err(
                "--report-url, --report-ca and --report-pin go together: an overridden collector \
                 needs its own trust anchor and the pin that confirms it"
                    .to_string(),
            );
        };
        let token = match &self.token_file {
            Some(p) => std::fs::read_to_string(p)
                .map_err(|e| format!("cannot read the token file {}: {e}", p.display()))?,
            None => upload::BAKED_TOKEN.to_string(),
        };
        let pem = std::fs::read_to_string(ca)
            .map_err(|e| format!("cannot read the certificate {}: {e}", ca.display()))?;
        upload::Collector::new(url, token.trim(), &pem, pin)
    }
}

fn parse_args() -> Result<Option<Args>, String> {
    parse_args_from(std::env::args().skip(1))
}

/// dist LA11: which update flags were given. They used to overwrite one `Option<UpdateAction>` in
/// parse order, so `--update --self-update` MEANT `--self-update` and the game half of the request
/// was silently dropped before the restart ever happened. Now the set is collected and resolved
/// once: both = the launcher first, then the game (`UpdateAction::Update`).
#[derive(Default)]
struct UpdateFlags {
    check: bool,
    game: bool,
    launcher: bool,
}

impl UpdateFlags {
    fn resolve(&self) -> Option<UpdateAction> {
        match (self.launcher, self.game, self.check) {
            (true, true, _) => Some(UpdateAction::Update),
            (true, false, _) => Some(UpdateAction::SelfUpdate),
            (false, true, _) => Some(UpdateAction::Apply),
            (false, false, true) => Some(UpdateAction::Check),
            (false, false, false) => None,
        }
    }
}

fn parse_args_from(argv: impl Iterator<Item = String>) -> Result<Option<Args>, String> {
    let mut update_flags = UpdateFlags::default();
    let mut args = Args {
        game_dir: None,
        app_dir: None,
        update_url: None,
        verify_binary: false,
        step: None,
        result: None,
        // dist LA6: the front page is Play.
        view: View::Launch,
        size: [1280.0, 720.0],
        startup: Startup::default(),
        send: None,
        send_outbox: false,
        consent: false,
        list_outbox: false,
        collector: CollectorArgs::default(),
    };
    let mut it = argv;
    while let Some(arg) = it.next() {
        let mut value = |name: &str| {
            it.next()
                .ok_or_else(|| format!("{name} needs a value\n\n{USAGE}"))
        };
        match arg.as_str() {
            "-h" | "--help" | "/?" => return Ok(None),
            "--game-dir" => args.game_dir = Some(value("--game-dir")?),
            "--app-dir" => args.app_dir = Some(PathBuf::from(value("--app-dir")?)),
            "--install" => args.startup.install_zip = Some(PathBuf::from(value("--install")?)),
            "--uninstall" => args.startup.uninstall = true,
            "--launch" => args.startup.launch = true,
            "--exit-after-launch" => args.startup.exit_after_launch = true,
            "--check-update" => update_flags.check = true,
            "--update" => update_flags.game = true,
            "--self-update" => update_flags.launcher = true,
            "--exit-after-update" => args.startup.exit_after_update = true,
            "--update-url" => args.update_url = Some(value("--update-url")?),
            "--verify-binary" => args.verify_binary = true,
            "--step" => args.step = Some(elevate::StepSpec::parse(&value("--step")?)?),
            "--result" => args.result = Some(PathBuf::from(value("--result")?)),
            "--report" => args.startup.report_to = Some(PathBuf::from(value("--report")?)),
            "--description" => args.startup.description = value("--description")?,
            "--with-minidump" => args.startup.with_minidump = true,
            "--exit-after-report" => args.startup.exit_after_report = true,
            "--send" => args.send = Some(PathBuf::from(value("--send")?)),
            "--send-outbox" => args.send_outbox = true,
            "--consent" => args.consent = true,
            "--outbox" => args.list_outbox = true,
            "--report-url" => args.collector.url = Some(value("--report-url")?),
            "--report-token-file" => {
                args.collector.token_file = Some(PathBuf::from(value("--report-token-file")?))
            }
            "--report-ca" => args.collector.ca = Some(PathBuf::from(value("--report-ca")?)),
            "--report-pin" => args.collector.pin = Some(value("--report-pin")?),
            "--view" => {
                let v = value("--view")?;
                args.view =
                    View::parse(&v).ok_or_else(|| format!("no such view: {v}\n\n{USAGE}"))?;
            }
            "--size" => {
                let v = value("--size")?;
                args.size = parse_size(&v)?;
            }
            other => return Err(format!("unknown option: {other}\n\n{USAGE}")),
        }
    }
    args.startup.requested_size = args.size;
    args.startup.update = update_flags.resolve();
    Ok(Some(args))
}

/// `1280x720`. Rejected rather than clamped when it is nonsense: a window the player cannot see is
/// worse than a message saying why there is no window.
fn parse_size(text: &str) -> Result<[f32; 2], String> {
    let (w, h) = text
        .split_once(['x', 'X'])
        .ok_or_else(|| format!("--size wants <W>x<H>, got {text:?}"))?;
    let w: f32 = w
        .trim()
        .parse()
        .map_err(|_| format!("--size width {w:?} is not a number"))?;
    let h: f32 = h
        .trim()
        .parse()
        .map_err(|_| format!("--size height {h:?} is not a number"))?;
    if !(200.0..=10000.0).contains(&w) || !(150.0..=10000.0).contains(&h) {
        return Err(format!("--size {text:?} is outside 200x150 .. 10000x10000"));
    }
    Ok([w, h])
}

fn main() {
    let args = match parse_args() {
        Ok(Some(a)) => a,
        Ok(None) => {
            print!("{USAGE}");
            return;
        }
        Err(e) => {
            fail(&e);
            return;
        }
    };

    let layout = match args.app_dir {
        Some(ref root) => Layout::rooted(root),
        None => match Layout::discover() {
            Ok(l) => l,
            Err(e) => {
                fail(&e);
                return;
            }
        },
    };
    log::init(&layout.log_file());
    log::line(format!(
        "---- mh_launcher {} starting, state in {}",
        crate::version::VERSION,
        layout.root.display()
    ));

    if args.verify_binary {
        std::process::exit(verify_binary(&layout));
    }

    // dist RP1, BEFORE the window: these three verbs print and exit, exactly as --verify-binary
    // does. An upload proven only by a person clicking is an upload proven once.
    if args.list_outbox {
        std::process::exit(list_outbox(&layout));
    }
    if args.send.is_some() || args.send_outbox {
        std::process::exit(send_from_command_line(&layout, &args));
    }

    let (mut cfg, note) = Config::load(&layout.config());
    if let Some(note) = note {
        log::line(format!("config: {note}"));
    }
    let mut changed = false;
    let mut game_dir_notice = None;
    if let Some(dir) = args.game_dir {
        // `--game-dir` overrides everything, including the search below (the harness's path).
        cfg.game_dir = dir;
        changed = true;
    } else {
        // dist LA7: find mh.exe without asking -- the launcher's own directory, the CWD, then the
        // saved directory; the first that holds it wins and is written back, so the Status field is
        // pre-filled and the Play view opens ready. Only when none matches is the player asked.
        let own_dir = std::env::current_exe()
            .ok()
            .and_then(|p| p.parent().map(Path::to_path_buf));
        let cwd = std::env::current_dir().ok();
        let resolved = paths::resolve_game_dir(own_dir.as_deref(), cwd.as_deref(), &cfg.game_dir);
        match &resolved.found {
            Some((dir, source)) => {
                log::line(format!(
                    "game dir: {} ({} holds {})",
                    dir.display(),
                    source.describe(),
                    paths::GAME_EXE
                ));
                let text = dir.display().to_string();
                if cfg.game_dir.trim() != text {
                    cfg.game_dir = text;
                    changed = true;
                }
            }
            None => log::line(format!(
                "game dir: none of the launcher's directory, the current directory or the saved \
                 setting ({:?}) holds {} -- asking",
                cfg.game_dir,
                paths::GAME_EXE
            )),
        }
        if let Some(notice) = resolved.notice() {
            log::line(format!("game dir: {notice}"));
            game_dir_notice = Some(notice);
        }
    }
    if let Some(url) = args.update_url {
        cfg.update_base_url = url;
        changed = true;
    }
    if changed {
        if let Err(e) = cfg.save(&layout.config()) {
            log::line(format!("config: {e}"));
        }
    }
    log::line(format!(
        "config: game_dir={:?} installed={:?} ({:?}) updates from {}",
        cfg.game_dir,
        cfg.installed_version,
        cfg.installed_tag,
        cfg.update_base_url_or_default()
    ));
    // dist LA13: which token this launcher runs with, on every start. An elevated launcher is the
    // measured cause of elevated games (both 09-20 players); `launch::start` compensates, and this
    // line is how a log says it happened.
    let own_elevation = launch::current_process_elevation();
    log::line(format!(
        "launcher: token {}",
        launch::describe_elevation(own_elevation)
    ));

    // dist LA13: `--step` -- the one verb an ELEVATED re-run of this launcher is given. Performed
    // here, before any window, exactly like --verify-binary: it installs/uninstalls/provisions in
    // the game directory, writes its verdict to --result, and exits. It cannot launch anything.
    if let Some(step) = args.step.as_ref() {
        let Some(result) = args.result.as_ref() else {
            fail("--step needs --result <file>");
            std::process::exit(1);
        };
        let game_dir = cfg.game_dir.trim().to_string();
        if game_dir.is_empty() {
            fail("--step needs --game-dir");
            std::process::exit(1);
        }
        let code = elevate::perform_step(&layout, Path::new(&game_dir), step, result);
        log::line(format!("---- mh_launcher exiting with code {code} (step)"));
        std::process::exit(code);
    }

    let mut startup = args.startup;
    startup.restart_args = restart_args(&std::env::args().skip(1).collect::<Vec<_>>());
    startup.game_dir_notice = game_dir_notice;
    // dist RP1: resolve the `--report-*` set ONCE, here, so the window shows the reason a bad
    // override is bad instead of discovering it when the player presses Send.
    startup.collector_override = if args.collector.any() {
        let resolved = args.collector.resolve();
        if let Err(e) = &resolved {
            log::line(format!("upload: {e}"));
        }
        Some(resolved)
    } else {
        None
    };

    // NOTHING BELOW HERE OPENS A WINDOW WHEN THERE IS NOTHING TO SHOW. `--exit-after-update` with
    // no update requested is exactly the state the replacement launcher is started in after a
    // self-update -- its arguments are this process's, minus the flag that asked for the update --
    // so it has to mean "there was no work; we are done", or every self-update would end with a
    // window nobody asked for.
    if startup.exit_after_update && startup.update.is_none() {
        log::line("update: no update work was requested and --exit-after-update was given");
        log::line("---- mh_launcher exiting");
        return;
    }

    let view = args.view;
    if let Err(e) = run_window(args.size, layout, cfg, view, startup) {
        fail(&e);
        set_exit_code(1);
    }
    log::line(format!(
        "---- mh_launcher exiting with code {}",
        EXIT_CODE.load(Ordering::SeqCst)
    ));
    let code = EXIT_CODE.load(Ordering::SeqCst);
    if code != 0 {
        std::process::exit(code);
    }
}

/// `--verify-binary`: the health gate a candidate launcher has to pass before it may replace a
/// running one.
///
/// It is deliberately more than `println!` of a version string. What the gate has to answer is "can
/// THIS machine run THIS binary", which a digest cannot: the missing VC++ runtime, the wrong
/// architecture, the antivirus quarantine and the unreadable state directory all pass a hash and
/// fail here. So by the time it prints, the process has started, its CRT has loaded, its state
/// directory has resolved, its log has opened, and the key it would verify manifests with has
/// parsed. A launcher that could never verify a manifest is not one to become the installed one.
///
/// It prints to stdout even in the release build, where there is no console: a GUI-subsystem
/// process still inherits the pipe its parent gave it, and the parent here is `update::health_gate`.
fn verify_binary(layout: &Layout) -> i32 {
    if !update::public_key_ok() {
        let msg = "mh_launcher: the public key compiled into this build does not parse";
        eprintln!("{msg}");
        log::line(msg);
        return 1;
    }
    let line = update::verify_binary_line();
    log::line(format!(
        "--verify-binary: {line} (state in {})",
        layout.root.display()
    ));
    println!("{line}");
    0
}

/// `--outbox`: what is waiting to be sent, and nothing else. Exit 0 whether or not there is any --
/// "nothing is waiting" is an answer, not a failure.
fn list_outbox(layout: &Layout) -> i32 {
    let outbox = upload::Outbox::new(&layout.reports());
    let pending = outbox.pending();
    if pending.is_empty() {
        println!(
            "no reports are waiting to be sent ({})",
            outbox.dir().display()
        );
        return 0;
    }
    println!(
        "{} report(s) waiting in {}:",
        pending.len(),
        outbox.dir().display()
    );
    for p in &pending {
        let size = std::fs::metadata(p).map(|m| m.len()).unwrap_or(0);
        println!("  {}  ({size} bytes)", p.display());
    }
    println!("send them with: mh_launcher --send-outbox --consent");
    0
}

/// `--send <zip>` / `--send-outbox`: **describe, then send only if told to.**
///
/// The description is printed for every report BEFORE any of them is sent, and without `--consent`
/// the process exits 2 having opened no socket. That is the command line's half of plan D13's
/// consent rule -- the Report view's consent panel is the other half, and both render the same
/// `upload::consent_text`, so the scripted path cannot be a quieter one.
fn send_from_command_line(layout: &Layout, args: &Args) -> i32 {
    let collector = match args.collector.resolve() {
        Ok(c) => c,
        Err(e) => {
            eprintln!("{e}");
            log::line(format!("upload: {e}"));
            return 1;
        }
    };
    let outbox = upload::Outbox::new(&layout.reports());

    let mut zips: Vec<PathBuf> = Vec::new();
    if let Some(zip) = args.send.clone() {
        zips.push(zip);
    }
    if args.send_outbox {
        for p in outbox.pending() {
            if !zips.contains(&p) {
                zips.push(p);
            }
        }
    }
    if zips.is_empty() {
        println!(
            "no reports are waiting to be sent ({})",
            outbox.dir().display()
        );
        return 0;
    }

    let mut prepared = Vec::new();
    for zip in &zips {
        match upload::prepare(zip, &collector.endpoint()) {
            Ok(p) => {
                println!("{}", upload::consent_text(&p));
                prepared.push(p);
            }
            Err(e) => {
                eprintln!("{e}");
                log::line(format!("upload: {e}"));
                return 1;
            }
        }
    }

    if !args.consent {
        println!(
            "NOTHING HAS BEEN SENT. {} report(s) are described above; pass --consent to send them.",
            prepared.len()
        );
        log::line("upload: described the report(s) and stopped -- no consent was given");
        return 2;
    }

    let mut failed = 0;
    for p in &prepared {
        match upload::send(&collector, p, &outbox) {
            Ok(a) => {
                println!("{}", a.summary());
                if let Err(e) = outbox.done(&p.zip) {
                    log::line(format!("upload: {e}"));
                }
            }
            Err(e) => {
                eprintln!("{e}");
                failed += 1;
            }
        }
    }
    if failed > 0 {
        1
    } else {
        0
    }
}

/// The arguments the REPLACEMENT launcher is started with after a self-update: this process's own,
/// minus the flags that asked for work already done or work the new process must not redo.
///
/// The strip is not tidiness. `--self-update` left in would have the replacement try to update
/// itself the moment it came up, and against a manifest that still advertises a newer launcher than
/// the one just installed that is a restart loop wearing an update's clothes. `--install` goes with
/// its argument for the same reason, and `--launch` goes because the player asked to play the
/// version they had, not to have a new launcher start a game behind their back.
///
/// `--exit-after-update` deliberately STAYS: in the replacement there is no update work left, so it
/// means "exit at once", which is what makes a self-update scriptable from end to end.
fn restart_args(argv: &[String]) -> Vec<String> {
    let mut out = Vec::new();
    let mut it = argv.iter();
    while let Some(arg) = it.next() {
        match arg.as_str() {
            "--self-update"
            | "--update"
            | "--check-update"
            | "--uninstall"
            | "--launch"
            | "--exit-after-launch"
            // dist LA4: a replacement launcher must not rebuild the report the one it replaced
            // already built -- and `--exit-after-report` without `--report` would be a flag with
            // no work, which is the shape that made `--exit-after-update` need its own guard.
            | "--with-minidump"
            | "--exit-after-report"
            // dist RP1: a send is work, not configuration. The `--report-*` overrides below are
            // configuration and deliberately STAY, the way `--app-dir` does -- a replacement
            // launcher that forgot which collector it was pointed at would go back to the baked
            // one mid-test.
            | "--send-outbox"
            | "--consent"
            | "--outbox" => {}
            "--install" | "--report" | "--description" | "--send" | "--step" | "--result" => {
                it.next();
            }
            other => out.push(other.to_string()),
        }
    }
    out
}

/// The full argv a replacement launcher is started with (dist LA11): `restart_args`'s strip, plus
/// the work the ORIGINAL request still owes.
///
/// THE STRIP ALONE WAS THE LA11 BUG. A scripted `--update --self-update --exit-after-update` on a
/// v0.1.0 launcher installed the launcher, restarted it with `["--app-dir", .., "--exit-after-update"]`
/// and the replacement said `update: no update work was requested` -- the game half of the request
/// died with the process that received it. So the caller says what is still owed: `carry_game_update`
/// appends `--update` (the game half, which the replacement runs at startup), and `view` names the
/// page the player pressed Update on so the window they get back opens where they were.
///
/// `--update` is appended, never merely left in, because the replacement must not inherit
/// `--self-update` (a restart loop against a manifest that still advertises the launcher just
/// installed) -- and a plain `--self-update` request carries nothing, so a launcher-only script still
/// ends with the replacement exiting at once.
pub fn restart_argv(
    stripped: &[String],
    carry_game_update: bool,
    view: Option<&str>,
) -> Vec<String> {
    let mut out: Vec<String> = Vec::new();
    let mut it = stripped.iter();
    while let Some(arg) = it.next() {
        if arg == "--view" && view.is_some() {
            it.next();
            continue;
        }
        out.push(arg.clone());
    }
    if carry_game_update {
        out.push("--update".to_string());
    }
    if let Some(v) = view {
        out.push("--view".to_string());
        out.push(v.to_string());
    }
    out
}

/// Open the window, trying each renderer in turn.
///
/// WGPU FIRST, GLOW SECOND. eframe's own default is the other way round, and on a desktop with a
/// real GPU driver either works -- but the machines this game is played on are not all that. A
/// Windows guest with the synthetic display adapter (Hyper-V, VirtualBox), a plain RDP session, or a
/// fresh install that has not seen its GPU driver yet exposes Microsoft's software OpenGL 1.1 and no
/// ICD, and `egui_glow` refuses that outright ("egui_glow requires opengl 2.0+"). `wgpu` reaches
/// D3D12/D3D11 instead and, failing those, the WARP software rasteriser, so it is the backend more
/// likely to produce a window at all. Glow stays as the fallback rather than being dropped because
/// the failure is not one-directional: a driver whose D3D path is broken while its GL path works is
/// rarer, but a launcher whose whole job is starting something else should not be the thing that
/// cannot start.
///
/// The `Err` returned is the LAST renderer's message with both attempts named, because "cannot open
/// a window: <one API's complaint>" sends a player hunting the wrong driver.
fn run_window(
    size: [f32; 2],
    layout: Layout,
    cfg: Config,
    view: View,
    startup: Startup,
) -> Result<(), String> {
    let mut attempts: Vec<String> = Vec::new();
    // The app takes ownership of all three, so the second attempt needs its own copy; cloning
    // before each `run_native` is what keeps the retry a retry rather than a half-configured run.
    let mut carried = Some((layout, cfg, startup));
    for renderer in [eframe::Renderer::Wgpu, eframe::Renderer::Glow] {
        let Some((layout, cfg, startup)) = carried.take() else {
            break;
        };
        let options = eframe::NativeOptions {
            renderer,
            viewport: egui::ViewportBuilder::default()
                .with_inner_size(size)
                // 800x600 is the smaller of the two sizes dist LA1 has to render at, so nothing may
                // be laid out assuming more room than that.
                .with_min_inner_size([800.0, 600.0])
                .with_title("Mission Humanity"),
            ..Default::default()
        };
        log::line(format!("window: trying the {renderer:?} renderer"));
        let spare = (layout.clone(), cfg.clone(), startup.clone());
        match eframe::run_native(
            "Mission Humanity",
            options,
            Box::new(move |_cc| Ok(Box::new(App::new(layout, cfg, view, startup)))),
        ) {
            Ok(()) => return Ok(()),
            Err(e) => {
                log::line(format!("window: {renderer:?} failed: {e}"));
                attempts.push(format!("{renderer:?}: {e}"));
                carried = Some(spare);
            }
        }
    }
    Err(format!(
        "cannot open a window -- every renderer failed:\n  {}",
        attempts.join("\n  ")
    ))
}

/// Say why nothing is going to happen, on both channels that exist.
///
/// In release there is no console (`windows_subsystem = "windows"`), so `eprintln!` reaches nobody
/// and a message box is the only thing a player can see; in debug the console is the faster read.
/// Both, always -- a duplicated line costs nothing and a swallowed one costs a bug report.
fn fail(msg: &str) {
    eprintln!("{msg}");
    log::line(format!("fatal: {msg}"));
    rfd::MessageDialog::new()
        .set_title("Mission Humanity launcher")
        .set_description(msg)
        .set_level(rfd::MessageLevel::Error)
        .show();
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sizes_parse_and_nonsense_is_refused() {
        assert_eq!(parse_size("1280x720").unwrap(), [1280.0, 720.0]);
        assert_eq!(parse_size("800X600").unwrap(), [800.0, 600.0]);
        for bad in ["1280", "x720", "1280x", "0x0", "1280x99999", "wide x tall"] {
            assert!(parse_size(bad).is_err(), "{bad} should be refused");
        }
    }

    /// The usage text is the only documentation a player gets, so every flag the parser accepts has
    /// to be in it. A flag added without a line here is a feature nobody can find.
    #[test]
    fn usage_names_every_flag() {
        for flag in [
            "--game-dir",
            "--install",
            "--uninstall",
            "--launch",
            "--exit-after-launch",
            "--view",
            "--size",
            "--app-dir",
            "--check-update",
            "--update",
            "--self-update",
            "--exit-after-update",
            "--update-url",
            "--verify-binary",
            "--step",
            "--result",
            "--report",
            "--description",
            "--with-minidump",
            "--exit-after-report",
            "--send",
            "--send-outbox",
            "--consent",
            "--outbox",
            "--report-url",
            "--report-token-file",
            "--report-ca",
            "--report-pin",
        ] {
            assert!(USAGE.contains(flag), "{flag} is missing from --help");
        }
    }

    /// The restart argument strip, which is the difference between a self-update and a restart loop.
    #[test]
    fn the_replacement_launcher_is_not_asked_to_update_again() {
        let argv: Vec<String> = [
            "--app-dir",
            "C:/state",
            "--self-update",
            "--exit-after-update",
            "--install",
            "C:/some.zip",
            "--view",
            "launch",
        ]
        .iter()
        .map(|s| s.to_string())
        .collect();
        let out = restart_args(&argv);
        assert_eq!(
            out,
            vec![
                "--app-dir",
                "C:/state",
                "--exit-after-update",
                "--view",
                "launch"
            ]
        );
        assert!(!out.iter().any(|a| a == "C:/some.zip"), "{out:?}");
        // Idempotent: feeding the result back in changes nothing, so a second self-update from the
        // replacement inherits the same arguments rather than eroding them.
        assert_eq!(restart_args(&out), out);
    }

    fn argv(list: &[&str]) -> Vec<String> {
        list.iter().map(|s| s.to_string()).collect()
    }

    /// dist LA11, the parse: `--update --self-update` used to collapse to `--self-update` (the
    /// last flag won). Both together are the one-button request -- the launcher first, then the
    /// game -- and each alone still means what it always did.
    #[test]
    fn update_and_self_update_together_are_one_request_launcher_first() {
        let parsed = |list: &[&str]| {
            parse_args_from(argv(list).into_iter())
                .unwrap()
                .unwrap()
                .startup
                .update
        };
        assert_eq!(
            parsed(&["--update", "--self-update", "--exit-after-update"]),
            Some(UpdateAction::Update)
        );
        assert_eq!(
            parsed(&["--self-update", "--update"]),
            Some(UpdateAction::Update),
            "order does not matter"
        );
        assert_eq!(parsed(&["--update"]), Some(UpdateAction::Apply));
        assert_eq!(parsed(&["--self-update"]), Some(UpdateAction::SelfUpdate));
        assert_eq!(parsed(&["--check-update"]), Some(UpdateAction::Check));
        assert_eq!(
            parsed(&["--check-update", "--update"]),
            Some(UpdateAction::Apply)
        );
        assert_eq!(parsed(&["--view", "status"]), None);
    }

    /// dist LA11's done_when: the restart after a self-update CARRIES the original request. The
    /// measured failure, replayed: `--update --self-update --exit-after-update` restarted as
    /// `["--app-dir", .., "--exit-after-update"]` and the replacement had no work. Now the
    /// replacement gets `--update` -- and never `--self-update`.
    #[test]
    fn the_replacement_launcher_is_told_to_finish_the_game_update() {
        let original = argv(&[
            "--app-dir",
            "C:/state",
            "--game-dir",
            "C:/game",
            "--update",
            "--self-update",
            "--exit-after-update",
        ]);
        let stripped = restart_args(&original);
        assert_eq!(
            stripped,
            argv(&[
                "--app-dir",
                "C:/state",
                "--game-dir",
                "C:/game",
                "--exit-after-update"
            ])
        );
        let restart = restart_argv(&stripped, true, None);
        assert_eq!(
            restart,
            argv(&[
                "--app-dir",
                "C:/state",
                "--game-dir",
                "C:/game",
                "--exit-after-update",
                "--update"
            ])
        );
        assert!(!restart.iter().any(|a| a == "--self-update"));
        // And the replacement parses that as the game half, scripted to exit when done.
        let parsed = parse_args_from(restart.into_iter()).unwrap().unwrap();
        assert_eq!(parsed.startup.update, Some(UpdateAction::Apply));
        assert!(parsed.startup.exit_after_update);

        // A launcher-only request carries nothing: the replacement exits at once, as before.
        let only_self = restart_args(&argv(&["--self-update", "--exit-after-update"]));
        assert_eq!(
            restart_argv(&only_self, false, None),
            argv(&["--exit-after-update"])
        );
    }

    /// dist LA12: the one Update button pressed in the window. No flags to carry, so the restart
    /// is `--update` plus the page the player was on; an inherited `--view` is replaced, not
    /// doubled.
    #[test]
    fn the_button_restart_reopens_on_the_same_page_with_the_game_update_owed() {
        assert_eq!(
            restart_argv(&[], true, Some("status")),
            argv(&["--update", "--view", "status"])
        );
        let inherited = argv(&["--view", "report", "--app-dir", "C:/state"]);
        assert_eq!(
            restart_argv(&inherited, true, Some("play")),
            argv(&["--app-dir", "C:/state", "--update", "--view", "play"])
        );
        // Without a view to set, an inherited one is kept as it was.
        assert_eq!(
            restart_argv(&inherited, false, None),
            argv(&["--view", "report", "--app-dir", "C:/state"])
        );
    }
}
