//! `mh_launcher` dist **LA2** + **RL8**: finding an update, refusing a bad one, and applying a good one.
//!
//! The whole of this module is one sentence: **fetch a signed manifest, refuse it several ways, and
//! only then touch the disk.** Everything below is either one of those refusals or the
//! stage-verify-swap that follows them.
//!
//! Since RL8 there are TWO signed documents per release channel instead of one root manifest, so a
//! game-only release never has to re-sign (or even look at) the launcher's:
//!
//! ```text
//!   GET <base>/channels/<channel>/launcher.json   (+ .minisig)     kind "launcher"
//!   GET <base>/channels/<channel>/game.json       (+ .minisig)     kind "game"
//!        |
//!        +-- 1. SIGNATURE   minisign, against the ONE key compiled in below
//!        +-- 2. SCHEMA      schema 2, and the kind AND channel INSIDE the signed body must be the
//!        |                  ones asked for -- a genuine stable file served as latest is refused
//!        |                  ("refuse CHANNEL"), not trusted for its signature
//!        +-- 3. CLOCK       more than a day in the future -> no
//!        +-- 4. REPLAY      older than the last copy this launcher accepted for the SAME channel and
//!        |                  kind -> no (the floor is kept on disk and re-verified on every load)
//!        +-- 5. URL         https only (or the configured origin), never api.github.com
//!        |
//!   at INSTALL (not at fetch):  6. NOT NEWER   the launcher never goes down; the game goes down only
//!        |                                     when the player switched channel
//!        |
//!   plan(launcher, game, ...)  ->  launcher first, game deferred or blocked by min_launcher
//!        |
//!   GET <asset url>  ->  versions\<ver>.staging\  ->  sha256  ->  rename to versions\<ver>\
//!        |
//!   copy beside mh.exe (the LA1 install), and KEEP versions\<old>\ until the new one has run once
//! ```
//!
//! There is NO age limit any more (the 30-day STALE check of schema 1 is gone): a genuine manifest
//! does not expire, so a channel that is quiet for two months keeps working. What replaces it is
//! REPLAY (an old genuine file cannot walk a launcher backwards) plus two visible mitigations for
//! the freeze risk that was accepted in exchange -- the log warns once a manifest is older than
//! `STALE_WARN_DAYS`, and the Status page says "manifest issued N days ago".
//!
//! ---- WHY A SIGNED FILE ON PAGES AND NOT THE GITHUB API (plan decision D11) ----
//!
//! Measured rather than preferred. The unauthenticated GitHub REST limit is **60 requests an hour
//! per IP** and a conditional request answered `304 Not Modified` **still spends one**, so a
//! launcher that polls the Releases API rate-limits every player behind a single NAT together and
//! the polite ETag dance does not save it. `releases/latest` additionally sorts by the tagged
//! COMMIT's date rather than by publication date, so it can name a release that is not the newest.
//! A static signed file on Pages has neither property. **The launcher therefore issues zero requests
//! to `api.github.com`** -- a clause dist LA2 is accepted on, asserted by `check_url` below, by
//! `no_url_may_reach_the_github_api` in the tests, and by a packet capture in the acceptance run.
//!
//! ---- WHY A SIGNATURE AND NOT JUST HTTPS ----
//!
//! TLS proves who served the file; it does not prove who wrote it. The threat the signature answers
//! is the one that does not need a network attacker at all: a Pages deploy from a compromised CI
//! token, or a repository takeover. So the manifest is verified against a key that exists only in
//! this binary and in the maintainer's hands, and the asset digests it carries are what make the
//! zips tamper-evident in turn. **Signature first, parse second** -- `accept_v2` never hands
//! `serde_json` a byte that has not already verified, because a JSON parser is a bigger attack
//! surface than an Ed25519 check.
//!
//! The refusals that are NOT about forgery are about a signature that is genuine and misplaced.
//! Both are TUF's named attacks: a **rollback** (serving an old, genuinely-signed manifest to walk
//! a player back onto a version with a known bug) is what REPLAY and NOT NEWER refuse, and
//! **cross-serving** (a genuine latest file under the stable URL) is what the in-body kind and
//! channel refuse. Neither can be caught by verifying the signature, because in both the
//! signature is perfectly valid.

use std::collections::BTreeMap;
use std::io::Read;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::{Duration, Instant};

use chrono::{DateTime, Utc};
use serde::Deserialize;
use sha2::{Digest, Sha256};

use crate::config::DEFAULT_TAG;
use crate::elevate;
use crate::install::{self, PackageName};
use crate::log;
use crate::paths::{Layout, FIRST_RUN_MARKER, GAME_EXE};
use crate::relay::Relay;

/// **The one key this launcher trusts.** A minisign Ed25519 public key, as the bare base64 line of
/// a `.pub` file (the `untrusted comment:` line is not part of it and is not trusted -- the name is
/// literal).
///
/// HOW THIS VALUE WAS MADE, so it can be remade:
///
/// ```text
/// rsign generate --unencrypted -p mh_launcher_minisign.pub -s mh_launcher_minisign.key
/// ```
///
/// (`cargo install rsign2` if `rsign` is not on PATH; `minisign -G -W` and
/// `python tools/gen_update_manifest.py --genkey` produce an interchangeable pair -- **use
/// `--unencrypted`, not `-W`/`--passwordless`**: rsign2's `-W` still writes a `scrypt`-tagged
/// secret key with an empty password, which `tools/gen_update_manifest.py`'s reader refuses BY
/// NAME as an encrypted key -- see that file's header for why an untested decryption path is
/// worse than a refusal). The SECRET half never enters this repository: it lives outside the tree
/// (a password manager and, for CI, ONE GitHub Actions repository secret,
/// `MH_LAUNCHER_MINISIGN_KEY`) and `tools/gen_update_manifest.py` is the only thing that reads it,
/// inside `.github/workflows/release.yml`'s `publish` job.
///
/// **THIS IS THE RELEASE KEY (dist LA3, cut 2026-09-17).** It replaces LA2's development key, whose
/// secret half was never the release key either -- a manifest signed under the OLD development key
/// is now refused here, which is the correct direction for that mistake to fail in. Rotating this
/// key means: generate a new pair the same way, replace this line, replace the
/// `MH_LAUNCHER_MINISIGN_KEY` repository secret, and ship the new PUBLIC_KEY in a launcher release
/// BEFORE retiring the old secret -- an already-installed launcher only ever trusts the key it was
/// built with.
pub const PUBLIC_KEY: &str = "RWQ6YzK1wVy0Emhh7SnKY/k05ne4CYJonz8ZYqmXRMKCu6KF27yxMvZO";

/// Where `channels/<channel>/{launcher,game}.json` live when `launcher.toml` does not say
/// otherwise: the public repository's
/// GitHub Pages site, stamped in at BUILD time by the release workflow (`MH_UPDATE_BASE_URL`, LA3)
/// rather than written here -- the tree carries no publisher identity, by the same rule that keeps
/// hosts out of `tools/` (lint_machine_paths). A build without it has NO default: the update check
/// then says so and does nothing until `launcher.toml` names a source. Pages rather than the API for
/// the reasons in the module header; a plain CDN GET that no rate limit counts.
pub const DEFAULT_BASE_URL: &str = match option_env!("MH_UPDATE_BASE_URL") {
    Some(u) => u,
    None => "",
};

/// The schema this build understands. A manifest claiming anything else is refused rather than
/// read optimistically: the fields a future schema adds are exactly the ones an old launcher would
/// not know to honour. Schema 1 was the root `manifest.json`; this launcher no longer fetches it.
pub const SCHEMA: u32 = 2;

/// `kind` of a launcher manifest / a game manifest -- the file name stem and the in-body tag.
pub const KIND_LAUNCHER: &str = "launcher";
pub const KIND_GAME: &str = "game";

/// How far into the future a manifest may claim to be. A day, because the machine's clock is the
/// thing more likely to be wrong than the publisher's.
pub const MAX_SKEW_DAYS: i64 = 1;

/// A manifest older than this is still ACCEPTED (RL8 removed the age limit) but the log says so:
/// the cost of dropping STALE is that a frozen genuine file looks fine, and this is the tripwire.
pub const STALE_WARN_DAYS: i64 = 60;

/// How many version directories survive a prune, INCLUDING the current one. Two: the running
/// version and the last-known-good one behind it.
pub const KEEP_VERSIONS: usize = 2;

/// Caps. A manifest is a few hundred bytes and a release zip is a few megabytes; both limits are
/// there so a server that answers with an endless body fills a socket rather than the disk.
const MAX_MANIFEST_BYTES: u64 = 256 * 1024;
const MAX_ASSET_BYTES: u64 = 512 * 1024 * 1024;

/// How long the launcher's own replacement gets to answer `--verify-binary`. A binary that cannot
/// say its own version in five seconds is not one to hand the future to.
const HEALTH_GATE_TIMEOUT: Duration = Duration::from_secs(5);

/// A game run this short does not count as "it started", so a crash inside it leaves the previous
/// version in place. Twenty seconds is past the menu, past the DirectDraw probe and past the module
/// bind; a fault after that is a game bug, not a broken install.
const STARTED_SECONDS: f64 = 20.0;

// Every refusal carries one of these prefixes, so the log, the tests and the acceptance script all
// name the same thing. They are a vocabulary, not decoration: `grep 'refuse NOT NEWER' launcher.log`
// has to answer a support question without anyone reading this file.
pub const R_FETCH: &str = "refuse FETCH";
/// The marker `Http` puts in a `refuse FETCH` error for an HTTP 404 answer, and the only thing
/// `is_not_found` accepts. A transport failure, a 500 or a 403 never carries it.
pub const NOT_FOUND_MARK: &str = "HTTP 404";
pub const R_SIGNATURE: &str = "refuse SIGNATURE";
pub const R_MALFORMED: &str = "refuse MALFORMED";
pub const R_SCHEMA: &str = "refuse SCHEMA";
pub const R_CHANNEL: &str = "refuse CHANNEL";
pub const R_CLOCK: &str = "refuse CLOCK";
pub const R_REPLAY: &str = "refuse REPLAY";
pub const R_NOT_NEWER: &str = "refuse NOT NEWER";
pub const R_URL: &str = "refuse URL";
pub const R_DIGEST: &str = "refuse DIGEST";
pub const R_HEALTH: &str = "refuse HEALTH GATE";
pub const R_LAUNCHER_OLD: &str = "refuse LAUNCHER TOO OLD";

// --------------------------------------------------------------------------- the manifests

/// One downloadable file.
#[derive(Clone, Debug, Deserialize)]
pub struct Asset {
    pub url: String,
    pub sha256: String,
    /// REQUIRED and non-zero (RL8): the size is what bounds the download before a byte is hashed.
    pub size: u64,
}

/// `channels/<channel>/launcher.json`, schema 2. `deny_unknown_fields` is deliberately NOT set: a
/// schema-2 manifest with an extra field is still a schema-2 manifest, and the schema gate is what
/// guards a shape change. What IS strict is that every field here is required -- a manifest
/// missing a digest must fail to parse rather than default to the empty string and then fail to
/// match.
#[derive(Clone, Debug, Deserialize)]
pub struct LauncherManifest {
    pub schema: u32,
    pub kind: String,
    pub channel: String,
    pub version: String,
    pub issued_at: String,
    pub url: String,
    pub sha256: String,
    pub size: u64,
    /// The launcher's release notes page (optional). Part of the signed document; shown by the
    /// Settings / About UI (RL14), not yet read by anything.
    #[serde(default)]
    #[allow(dead_code)]
    pub notes_url: String,
}

/// `channels/<channel>/game.json`, schema 2.
#[derive(Clone, Debug, Deserialize)]
pub struct GameManifest {
    pub schema: u32,
    pub kind: String,
    pub channel: String,
    pub version: String,
    pub issued_at: String,
    /// The oldest launcher that may install this game (semver). A launcher below it does not
    /// install the game (`plan`).
    pub min_launcher: String,
    /// configuration tag (`net`, ...) -> the zip that carries it.
    pub game: BTreeMap<String, Asset>,
    /// dist LA6: the relay the launcher provisions into the game's `mh_net.ini` + `mh_key.txt`
    /// (`relay.rs`). ABSENT means "touch neither file"; it is never defaulted to a blank pair.
    /// Inside the signed body like every other field, so the one thing that can point every
    /// launcher at a relay is the release key.
    #[serde(default)]
    pub relay: Option<Relay>,
    #[serde(default)]
    pub notes_url: String,
}

impl GameManifest {
    pub fn asset(&self, tag: &str) -> Result<&Asset, String> {
        self.game.get(tag).ok_or_else(|| {
            format!(
                "{R_MALFORMED}: the manifest has no {tag:?} configuration (it offers {})",
                self.game.keys().cloned().collect::<Vec<_>>().join(", ")
            )
        })
    }
}

/// What one check yields: both channel manifests, each of which passed every gate.
#[derive(Clone, Debug)]
pub struct Releases {
    /// `None` when the channel publishes no `launcher.json` (an HTTP 404 on it): "this channel
    /// offers no launcher". Never set by any other failure -- those are errors.
    pub launcher: Option<LauncherManifest>,
    pub game: GameManifest,
}

impl Releases {
    /// How many days ago the OLDER of the two manifests was issued -- the freeze tripwire's number
    /// (`STALE_WARN_DAYS`, the Status page's "manifest issued N days ago").
    pub fn age_days(&self, now: DateTime<Utc>) -> Option<i64> {
        let b = issued_age_days(&self.game.issued_at, now)?;
        match &self.launcher {
            Some(l) => Some(issued_age_days(&l.issued_at, now)?.max(b)),
            None => Some(b),
        }
    }
}

/// Whole days between `issued_at` and `now` (negative for the future); `None` if unparseable.
pub fn issued_age_days(issued_at: &str, now: DateTime<Utc>) -> Option<i64> {
    let issued = DateTime::parse_from_rfc3339(issued_at).ok()?;
    Some(now.signed_duration_since(issued).num_days())
}

/// A manifest document that passed every gate (`accept_v2`).
#[derive(Clone, Debug)]
pub enum Accepted {
    Launcher(LauncherManifest),
    Game(GameManifest),
}

impl Accepted {
    pub fn into_launcher(self) -> Result<LauncherManifest, String> {
        match self {
            Accepted::Launcher(m) => Ok(m),
            Accepted::Game(_) => Err(format!("{R_SCHEMA}: expected a launcher manifest")),
        }
    }

    pub fn into_game(self) -> Result<GameManifest, String> {
        match self {
            Accepted::Game(m) => Ok(m),
            Accepted::Launcher(_) => Err(format!("{R_SCHEMA}: expected a game manifest")),
        }
    }
}

/// Just enough of any schema's manifest to decide whether to read the rest: what the SCHEMA and
/// CHANNEL gates look at. `kind` and `channel` default to empty so a schema-1 document (which has
/// neither) reaches the schema refusal rather than a parse error.
#[derive(Deserialize)]
struct Header {
    schema: u32,
    #[serde(default)]
    kind: String,
    #[serde(default)]
    channel: String,
}

// --------------------------------------------------------------------------- the refusals

/// 1. The signature, against `PUBLIC_KEY`, before anything else looks at the bytes.
///
/// `allow_legacy` is **false**: minisign's legacy algorithm signs the file itself rather than its
/// BLAKE2b hash, and accepting both would mean accepting a signature made under weaker assumptions
/// than the ones this project's own signer works under. Nothing we publish needs it.
///
/// Production reaches this through `accept_v2(.., PUBLIC_KEY)`; the bare form is the tests' way of
/// asking "would the RELEASE key accept this".
#[cfg(test)]
pub fn verify_signature(bytes: &[u8], signature: &str) -> Result<(), String> {
    verify_signature_with(bytes, signature, PUBLIC_KEY)
}

/// `verify_signature` against an explicit key -- for the tests that carry a fixture signed by the
/// committed TEST-ONLY key (`tests/data/schema2_test.key`). Production never calls this with
/// anything but `PUBLIC_KEY`.
pub fn verify_signature_with(
    bytes: &[u8],
    signature: &str,
    public_key: &str,
) -> Result<(), String> {
    let key = minisign_verify::PublicKey::from_base64(public_key)
        .map_err(|e| format!("{R_SIGNATURE}: this build's public key does not parse ({e})"))?;
    let sig = minisign_verify::Signature::decode(signature)
        .map_err(|e| format!("{R_SIGNATURE}: the .minisig is not a minisign signature ({e})"))?;
    key.verify(bytes, &sig, false).map_err(|e| {
        format!(
            "{R_SIGNATURE}: the manifest is not signed by this launcher's key ({e}). \
             The file was not written by whoever holds the release key -- nothing was downloaded."
        )
    })
}

fn parse_issued(issued_at: &str) -> Result<DateTime<Utc>, String> {
    DateTime::parse_from_rfc3339(issued_at)
        .map(|d| d.with_timezone(&Utc))
        .map_err(|e| {
            format!("{R_MALFORMED}: issued_at {issued_at:?} is not an RFC 3339 timestamp ({e})")
        })
}

fn check_hex64(what: &str, value: &str) -> Result<(), String> {
    if value.len() == 64 && value.bytes().all(|b| b.is_ascii_hexdigit()) {
        Ok(())
    } else {
        Err(format!(
            "{R_MALFORMED}: {what} sha256 {value:?} is not 64 hex digits"
        ))
    }
}

fn check_semver(what: &str, value: &str) -> Result<(), String> {
    semver::Version::parse(value)
        .map(|_| ())
        .map_err(|e| format!("{R_MALFORMED}: {what} {value:?} is not semver ({e})"))
}

/// A configuration tag is used in file names (`package_file_name`) and directory names, so it is
/// held to a conservative shape before it is ever joined to a path.
fn check_tag(tag: &str) -> Result<(), String> {
    if !tag.is_empty()
        && tag
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || b == b'-' || b == b'_' || b == b'.')
        && !tag.starts_with('.')
    {
        Ok(())
    } else {
        Err(format!(
            "{R_MALFORMED}: {tag:?} is not a usable configuration name"
        ))
    }
}

fn check_asset(what: &str, a: &Asset) -> Result<(), String> {
    check_hex64(what, &a.sha256)?;
    if a.size == 0 {
        return Err(format!(
            "{R_MALFORMED}: {what} has size 0 (size is required and > 0)"
        ));
    }
    Ok(())
}

/// Parse a launcher manifest's bytes and check its shape. Does NOT check the signature, the
/// channel or the clock -- `accept_v2` does, in order; this is the "is it well-formed" half.
pub fn parse_launcher(bytes: &[u8]) -> Result<LauncherManifest, String> {
    let m: LauncherManifest = serde_json::from_slice(bytes)
        .map_err(|e| format!("{R_MALFORMED}: launcher.json does not parse ({e})"))?;
    // Belt and braces after `check_header`: the typed fields agree with what the gate read.
    if m.schema != SCHEMA || m.kind != KIND_LAUNCHER {
        return Err(format!(
            "{R_SCHEMA}: not a schema-{SCHEMA} {KIND_LAUNCHER} manifest (schema {}, kind {:?})",
            m.schema, m.kind
        ));
    }
    check_semver("the launcher version", &m.version)?;
    parse_issued(&m.issued_at)?;
    check_hex64("the launcher", &m.sha256)?;
    if m.size == 0 {
        return Err(format!(
            "{R_MALFORMED}: the launcher has size 0 (size is required and > 0)"
        ));
    }
    Ok(m)
}

/// Parse a game manifest's bytes and check its shape (see `parse_launcher`).
pub fn parse_game(bytes: &[u8]) -> Result<GameManifest, String> {
    let m: GameManifest = serde_json::from_slice(bytes)
        .map_err(|e| format!("{R_MALFORMED}: game.json does not parse ({e})"))?;
    if m.schema != SCHEMA || m.kind != KIND_GAME {
        return Err(format!(
            "{R_SCHEMA}: not a schema-{SCHEMA} {KIND_GAME} manifest (schema {}, kind {:?})",
            m.schema, m.kind
        ));
    }
    check_semver("the game version", &m.version)?;
    check_semver("min_launcher", &m.min_launcher)?;
    parse_issued(&m.issued_at)?;
    if m.game.is_empty() {
        return Err(format!("{R_MALFORMED}: the manifest offers no game assets"));
    }
    for (tag, asset) in &m.game {
        check_tag(tag)?;
        check_asset(&format!("the {tag} zip"), asset)?;
    }
    // dist LA6: a relay that would not survive being written into an ini is refused HERE, so a
    // manifest is either applied whole or not at all -- never a game update installed and then
    // a half-provisioned relay.
    if let Some(relay) = &m.relay {
        relay
            .validate()
            .map_err(|e| format!("{R_MALFORMED}: the manifest's relay field is unusable ({e})"))?;
    }
    Ok(m)
}

/// The SCHEMA / CHANNEL gate, over a document whose signature has ALREADY verified: schema 2, and the
/// kind and channel written inside the signed body are the ones that were asked for.
fn check_header(bytes: &[u8], kind: &str, channel: &str) -> Result<(), String> {
    let h: Header = serde_json::from_slice(bytes)
        .map_err(|e| format!("{R_MALFORMED}: the manifest does not parse ({e})"))?;
    if h.schema != SCHEMA {
        return Err(format!(
            "{R_SCHEMA}: the manifest is schema {} and this launcher understands {SCHEMA}. \
             (Schema 1 was the root manifest.json; update the launcher first.)",
            h.schema
        ));
    }
    if h.kind != kind {
        return Err(format!(
            "{R_SCHEMA}: this is a {:?} manifest and a {kind:?} one was asked for",
            h.kind
        ));
    }
    if h.channel != channel {
        return Err(format!(
            "{R_CHANNEL}: the manifest is signed as channel {:?} but was served as {channel:?}. \
             A genuine file under the wrong URL is cross-serving, not an update.",
            h.channel
        ));
    }
    Ok(())
}

/// The CLOCK gate: a manifest dated more than `MAX_SKEW_DAYS` into the future is refused. (There is
/// no age limit -- see the module header.)
pub fn check_clock(issued_at: &str, now: DateTime<Utc>) -> Result<(), String> {
    let issued = parse_issued(issued_at)?;
    let ahead = issued.signed_duration_since(now);
    if ahead.num_days() > MAX_SKEW_DAYS {
        return Err(format!(
            "{R_CLOCK}: the manifest is dated {issued_at}, which is {} days in the future. \
             Either this machine's clock is wrong or the file is not what it claims.",
            ahead.num_days()
        ));
    }
    Ok(())
}

/// The REPLAY gate: older than the last accepted copy for this channel+kind is a rollback of the
/// MANIFEST itself. Equal is fine (the same file fetched twice).
pub fn check_replay(issued_at: &str, floor: Option<DateTime<Utc>>) -> Result<(), String> {
    let Some(floor) = floor else { return Ok(()) };
    let issued = parse_issued(issued_at)?;
    if issued < floor {
        return Err(format!(
            "{R_REPLAY}: the manifest was issued {issued_at}, before the {} copy this launcher \
             already accepted. An older genuine file is a rollback, whoever signed it.",
            floor.to_rfc3339_opts(chrono::SecondsFormat::Secs, true)
        ));
    }
    Ok(())
}

/// Rollback: the offered version must be strictly newer than what is installed. The shared
/// comparison behind `check_launcher_install` and `check_game_install`.
///
/// Semver, so `0.2.0` beats `0.10.0` nowhere and a prerelease sorts below its release --
/// `0.2.0-rc1 < 0.2.0`, which is the whole reason this is not a string compare. An EMPTY installed
/// version means nothing is installed yet and anything parseable is an upgrade.
pub fn check_newer(offered: &str, installed: &str) -> Result<(), String> {
    let offered_v = semver::Version::parse(offered).map_err(|e| {
        format!("{R_MALFORMED}: the manifest's version {offered:?} is not semver ({e})")
    })?;
    if installed.trim().is_empty() {
        return Ok(());
    }
    let installed_v = match semver::Version::parse(installed) {
        Ok(v) => v,
        Err(e) => {
            // Not a refusal. An unreadable local record is this machine's problem, and refusing
            // every update because of it is the failure mode that leaves a player stuck for good.
            log::line(format!(
                "update: the installed version {installed:?} is not semver ({e}); treating the \
                 game directory as having nothing installed"
            ));
            return Ok(());
        }
    };
    if offered_v > installed_v {
        Ok(())
    } else {
        Err(format!(
            "{R_NOT_NEWER}: the manifest offers {offered} and {installed} is installed. \
             An update that is not newer is a rollback, whoever signed it."
        ))
    }
}

/// 5. Where a byte may be fetched from.
///
/// Two rules, and the second is the one dist LA2 is accepted on:
///
/// * **TLS, or the configured origin.** Every URL must be `https`, with one exception: a URL on
///   exactly the scheme+host+port the update base URL already names. That exception is what lets a
///   `http://127.0.0.1:<port>/` stand-in be tested end to end without a second code path; it cannot
///   widen the production case, because there the base URL is itself `https`.
/// * **Never `api.github.com`.** Stated as a host check rather than left implicit in "we only fetch
///   what the manifest says", because the manifest is the thing an attacker would edit, and because
///   a clause this specific deserves an assertion this specific. A URL with userinfo
///   (`https://api.github.com@evil.example/`) is refused outright for the same reason -- it is the
///   classic way to make a host check read the wrong half of an authority.
pub fn check_url(url: &str, base: &str) -> Result<(), String> {
    let (scheme, authority) = split_origin(url)
        .ok_or_else(|| format!("{R_URL}: {url:?} is not a <scheme>://<host>/<path> URL"))?;
    if authority.contains('@') {
        return Err(format!(
            "{R_URL}: {url:?} carries userinfo before its host, which is how a host check is made \
             to read the wrong half of an authority"
        ));
    }
    let host = authority
        .split(':')
        .next()
        .unwrap_or_default()
        .to_ascii_lowercase();
    if host.is_empty() {
        return Err(format!("{R_URL}: {url:?} has no host"));
    }
    if host == "api.github.com" {
        return Err(format!(
            "{R_URL}: {url:?} addresses api.github.com. This launcher never calls the GitHub API \
             (plan decision D11): the rate limit is 60 requests an hour per IP and a 304 spends \
             one, so a whole household behind one address would lock itself out."
        ));
    }
    if scheme == "https" {
        return Ok(());
    }
    match split_origin(base) {
        Some((bs, ba)) if bs == scheme && ba.eq_ignore_ascii_case(authority) => Ok(()),
        _ => Err(format!(
            "{R_URL}: {url:?} is not https and is not on the configured update origin ({base})"
        )),
    }
}

/// `("https", "host:port")` from a URL, without a URL crate. Everything this launcher fetches is a
/// plain absolute `http(s)` URL, and the one question asked of it is its origin.
fn split_origin(url: &str) -> Option<(&str, &str)> {
    let (scheme, rest) = url.split_once("://")?;
    if scheme.is_empty() || rest.is_empty() {
        return None;
    }
    let authority = rest.split(['/', '?', '#']).next()?;
    if authority.is_empty() {
        return None;
    }
    Some((scheme, authority))
}

/// NOT NEWER, for the GAME, at install time: the offered version must be strictly newer than
/// what is installed -- EXCEPT while an explicit channel switch is in progress (`switching`, see
/// `Env::switching`), when any version other than the installed one may be installed, older
/// included: a player who moved from `latest` to `stable` asked for the stable game, and the stable
/// game is usually the older one. The exception ends with the install (the caller records the new
/// `installed_channel`), so it cannot be used to walk a launcher back by merely being served an old
/// file -- that is what REPLAY and the signature are for.
pub fn check_game_install(offered: &str, installed: &str, switching: bool) -> Result<(), String> {
    if !switching {
        return check_newer(offered, installed);
    }
    let offered_v = semver::Version::parse(offered).map_err(|e| {
        format!("{R_MALFORMED}: the manifest's version {offered:?} is not semver ({e})")
    })?;
    if let Ok(installed_v) = semver::Version::parse(installed.trim()) {
        if offered_v == installed_v {
            return Err(format!(
                "{R_NOT_NEWER}: the channel offers {offered} and {installed} is already installed"
            ));
        }
    }
    Ok(())
}

/// The launcher NEVER goes down, switch or no switch: its own version is the one thing a channel
/// choice has no business changing, and a launcher built for a newer manifest shape is not safe to
/// replace with an older one.
pub fn check_launcher_install(offered: &str, mine: &str) -> Result<(), String> {
    check_newer(offered, mine)
}

/// All the gates, in order, over bytes that have just come off the network, for one document.
///
/// `floor` is the issued_at of the last copy accepted for this channel and kind (`load_floor`), or
/// `None` when there is none. `public_key` is `PUBLIC_KEY` in production; the tests pass the
/// committed test-only key -- never the other way round.
///
/// There is deliberately no installed-version argument: NOT NEWER is a property of an INSTALL, not
/// of a fetched file (`check_game_install`, `check_launcher_install`).
// Eight arguments because that is what the gates consist of; bundling them in a struct would only
// move the same eight names one line away from where the tests read them.
#[allow(clippy::too_many_arguments)]
pub fn accept_v2(
    bytes: &[u8],
    signature: &str,
    base_url: &str,
    kind: &str,
    channel: &str,
    floor: Option<DateTime<Utc>>,
    now: DateTime<Utc>,
    public_key: &str,
) -> Result<Accepted, String> {
    verify_signature_with(bytes, signature, public_key)?;
    check_header(bytes, kind, channel)?;
    match kind {
        KIND_LAUNCHER => {
            let m = parse_launcher(bytes)?;
            check_clock(&m.issued_at, now)?;
            check_replay(&m.issued_at, floor)?;
            check_url(&m.url, base_url).map_err(|e| format!("{e} (the launcher)"))?;
            Ok(Accepted::Launcher(m))
        }
        KIND_GAME => {
            let m = parse_game(bytes)?;
            check_clock(&m.issued_at, now)?;
            check_replay(&m.issued_at, floor)?;
            for (tag, asset) in &m.game {
                check_url(&asset.url, base_url).map_err(|e| format!("{e} (the {tag} zip)"))?;
            }
            Ok(Accepted::Game(m))
        }
        other => Err(format!("{R_SCHEMA}: {other:?} is not a manifest kind")),
    }
}

// --------------------------------------------------------------------------- the environment

/// Everything a check or an apply decision depends on besides the network and the disk: which
/// channel is followed, which one the game on disk came from, who is asking, and which key to
/// trust. A struct so the production call sites read `Env::release(..)` and the tests can pass the
/// test-only key and a chosen launcher version without a second code path.
#[derive(Clone, Copy, Debug)]
pub struct Env<'a> {
    pub channel: &'a str,
    pub installed_channel: &'a str,
    /// This launcher's version (semver).
    pub mine: &'a str,
    pub key: &'a str,
}

impl<'a> Env<'a> {
    /// The production environment: this build's version, the RELEASE key.
    pub fn release(channel: &'a str, installed_channel: &'a str) -> Self {
        Self {
            channel,
            installed_channel,
            mine: crate::version::VERSION,
            key: PUBLIC_KEY,
        }
    }

    /// Is an explicit channel switch in progress? The game on disk came from a KNOWN other
    /// channel. Empty `installed_channel` (a bridge arrival, a fresh directory) is not a switch.
    pub fn switching(&self) -> bool {
        is_switch(self.installed_channel, self.channel)
    }
}

/// The switch rule on its own (`Env::switching`, `plan`): the game on disk came from a KNOWN
/// channel that is not the one now followed.
pub fn is_switch(installed_channel: &str, channel: &str) -> bool {
    let installed = installed_channel.trim();
    !installed.is_empty() && installed != channel
}

// --------------------------------------------------------------------------- the plan

/// What a check means for this machine, decided WITHOUT touching anything (`plan`).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Plan {
    /// The launcher version to self-update to, when the channel's is newer than this launcher.
    /// Always done FIRST: a newer launcher restarts and the game half runs there.
    pub launcher: Option<String>,
    /// The game version the channel offers that this machine should install (a missing
    /// configuration counts as installable at any version), or `None` when the game is current.
    pub game: Option<String>,
    /// `game` is set but THIS launcher must not install it: it is older than the game's
    /// `min_launcher`, and the channel's launcher satisfies it. The launcher updates first and the
    /// game follows in the replacement.
    pub deferred: bool,
    /// `refuse LAUNCHER TOO OLD`: a game is wanted but this launcher is below `min_launcher` and the
    /// channel's launcher does not fix that. The current game is kept; the message is for the
    /// status line.
    pub blocked: Option<String>,
}

/// The sequencing rule (dist RL8 section 3), as a pure function of the two manifests and the
/// machine's state.
///
/// * the launcher is offered when the channel's is newer than `mine` (never otherwise);
/// * the game is offered when `check_game_install` allows it against `installed` (the receipt's
///   version of the chosen configuration, empty when it is not installed);
/// * a channel with no launcher manifest (`None`) never plans a launcher step; `min_launcher` is then
///   checked against `mine` alone (blocked, "download it by hand", when too old);
/// * a game-only release therefore yields `launcher: None` -- the launcher manifest is not acted on;
/// * `mine` below the game's `min_launcher`: if the channel's launcher is newer AND reaches
///   `min_launcher`, the game is `deferred` until after the restart; otherwise it is `blocked`.
pub fn plan(
    launcher_m: Option<&LauncherManifest>,
    game_m: &GameManifest,
    mine: &str,
    installed: &str,
    installed_channel: &str,
    channel: &str,
) -> Plan {
    let mut p = Plan::default();
    // A channel with no launcher manifest never plans a launcher step.
    let launcher_newer =
        launcher_m.is_some_and(|l| check_launcher_install(&l.version, mine).is_ok());
    if let (true, Some(l)) = (launcher_newer, launcher_m) {
        p.launcher = Some(l.version.clone());
    }
    if check_game_install(
        &game_m.version,
        installed,
        is_switch(installed_channel, channel),
    )
    .is_err()
    {
        return p;
    }
    let too_old = match (
        semver::Version::parse(mine),
        semver::Version::parse(&game_m.min_launcher),
    ) {
        (Ok(m), Ok(min)) => m < min,
        // An unreadable version on either side is not a reason to refuse: the manifest's own
        // min_launcher was validated at parse time, so this is `mine`, and a launcher that cannot
        // read its own version is a build problem the health gate already reports.
        _ => false,
    };
    if !too_old {
        p.game = Some(game_m.version.clone());
        return p;
    }
    let fixes_it = launcher_newer
        && launcher_m
            .and_then(|l| semver::Version::parse(&l.version).ok())
            .zip(semver::Version::parse(&game_m.min_launcher).ok())
            .is_some_and(|(l, min)| l >= min);
    if fixes_it {
        p.game = Some(game_m.version.clone());
        p.deferred = true;
    } else {
        p.blocked = Some(match launcher_m {
            Some(l) => format!(
                "{R_LAUNCHER_OLD}: game {} needs launcher {} or newer, this is {mine} and the \
                 {channel} channel offers launcher {}. The current game is kept.",
                game_m.version, game_m.min_launcher, l.version
            ),
            None => format!(
                "{R_LAUNCHER_OLD}: game {} needs launcher {} or newer, this is {mine} and the \
                 {channel} channel offers no launcher. Download the launcher by hand. The \
                 current game is kept.",
                game_m.version, game_m.min_launcher
            ),
        });
    }
    p
}

/// The configuration to install from `game`: the requested one if the manifest offers it, else
/// `net` (decision D1: the published game map is `net` only) with a log line, else a refusal. A
/// pick the channel does not offer falls back rather than failing, so a player who once chose
/// `net-debug` is not stranded by a channel that never shipped it.
pub fn resolve_tag(game: &GameManifest, tag: &str) -> Result<String, String> {
    if game.game.contains_key(tag) {
        return Ok(tag.to_string());
    }
    if game.game.contains_key(DEFAULT_TAG) {
        log::line(format!(
            "update: the {} channel does not offer the {tag:?} configuration; using {DEFAULT_TAG}",
            game.channel
        ));
        return Ok(DEFAULT_TAG.to_string());
    }
    game.asset(tag).map(|_| tag.to_string())
}

// --------------------------------------------------------------------------- fetching

/// Where bytes come from. A trait so the rules above can be tested without a server, and so the one
/// implementation that opens a socket is a single named type that is easy to audit.
pub trait Fetch: Send {
    fn get(&self, url: &str, limit: u64) -> Result<Vec<u8>, String>;
}

/// The real one: `ureq` over rustls.
///
/// THE TRUST STORE IS COMPILED IN (`rustls-webpki-roots`) rather than read from Windows. That is a
/// deliberate narrowing: the launcher talks to two hosts it chose, and a fixed root set means an
/// enterprise MITM root or a hand-installed CA on the player's machine cannot silently re-point
/// them. It also means the roots age with the binary, which is a cost the signature absorbs -- a
/// launcher too old to trust the CDN's chain fails to fetch rather than fetching something wrong.
/// The `win-system-proxy` feature is likewise off: nothing here should be redirected by an
/// environment variable or a system setting.
pub struct Http {
    agent: ureq::Agent,
}

impl Http {
    pub fn new() -> Self {
        let config = ureq::Agent::config_builder()
            .timeout_global(Some(Duration::from_secs(120)))
            .user_agent(format!("mh_launcher/{}", crate::version::VERSION))
            // NO PROXY, EVER. `ureq`'s default is to read `HTTP_PROXY`/`HTTPS_PROXY` from the
            // environment, and an updater that can be re-pointed by an environment variable is an
            // updater whose destination is not the one this file states. The signature would still
            // catch a substitution, but a fetch that silently went somewhere else is not a thing to
            // leave discoverable only by a packet capture.
            .proxy(None)
            // NO CONNECTION POOL. Measured during LA2's acceptance run: against a stand-in that
            // closes the connection after each response, the SECOND request -- `manifest.json` then
            // `manifest.json.minisig` -- failed with "Peer disconnected" on the pooled socket. A
            // launcher makes two or three requests in a session and a pool buys it nothing, so the
            // whole class of stale-socket failure is simply not entered.
            .max_idle_connections(0)
            // Release asset URLs redirect to an object store, so redirects must be followed; a
            // small bound keeps a redirect loop from being a hang. Where a redirect ENDS is not
            // checked, and does not need to be: every byte fetched is hashed against the digest the
            // signed manifest carries before anything is done with it.
            .max_redirects(5)
            .build();
        Self {
            agent: config.into(),
        }
    }
}

impl Default for Http {
    fn default() -> Self {
        Self::new()
    }
}

impl Fetch for Http {
    fn get(&self, url: &str, limit: u64) -> Result<Vec<u8>, String> {
        log::line(format!("update: GET {url}"));
        let mut response = self.agent.get(url).call().map_err(|e| match e {
            ureq::Error::StatusCode(404) => {
                format!("{R_FETCH}: {url} -- {NOT_FOUND_MARK} (not found)")
            }
            e => format!("{R_FETCH}: {url} -- {e}"),
        })?;
        let bytes = response
            .body_mut()
            .with_config()
            .limit(limit)
            .read_to_vec()
            .map_err(|e| format!("{R_FETCH}: {url} -- reading the body failed ({e})"))?;
        log::line(format!("update: got {} B from {url}", bytes.len()));
        Ok(bytes)
    }
}

fn join_url(base: &str, name: &str) -> String {
    format!("{}/{}", base.trim_end_matches('/'), name)
}

/// `<base>/channels/<channel>/<kind>.json` -- where a channel manifest lives. The `.minisig` is the
/// same URL plus `.minisig`.
pub fn manifest_url(base: &str, channel: &str, kind: &str) -> String {
    join_url(base, &format!("channels/{channel}/{kind}.json"))
}

/// Is `err` the `Fetch` error for an HTTP 404 answer -- and nothing else?
pub fn is_not_found(err: &str) -> bool {
    err.starts_with(R_FETCH) && err.contains(NOT_FOUND_MARK)
}

/// Fetch one channel manifest and run it through every gate. -> the accepted document plus the
/// exact bytes and signature it was accepted from (what `remember_accepted` keeps).
///
/// `Ok(None)` only for the OPTIONAL kind (the launcher) when its manifest answers HTTP 404. A
/// present manifest whose `.minisig` answers 404 is `refuse SIGNATURE`, not "absent".
fn fetch_accepted(
    fetch: &dyn Fetch,
    base_url: &str,
    env: &Env,
    kind: &str,
    layout: Option<&Layout>,
) -> Result<Option<(Accepted, Vec<u8>, String)>, String> {
    let url = manifest_url(base_url, env.channel, kind);
    let bytes = match fetch.get(&url, MAX_MANIFEST_BYTES) {
        Ok(b) => b,
        Err(e) if kind == KIND_LAUNCHER && is_not_found(&e) => return Ok(None),
        Err(e) => return Err(e),
    };
    let sig = match fetch.get(&format!("{url}.minisig"), MAX_MANIFEST_BYTES) {
        Ok(s) => s,
        Err(e) if kind == KIND_LAUNCHER && is_not_found(&e) => {
            return Err(format!(
                "{R_SIGNATURE}: {url} exists but {url}.minisig does not (HTTP 404); an unsigned \
                 manifest is refused, not treated as absent"
            ));
        }
        Err(e) => return Err(e),
    };
    let sig =
        String::from_utf8(sig).map_err(|_| format!("{R_SIGNATURE}: {url}.minisig is not text"))?;
    let floor = layout.and_then(|l| load_floor(l, env.channel, kind, env.key));
    let accepted = accept_v2(
        &bytes,
        &sig,
        base_url,
        kind,
        env.channel,
        floor,
        Utc::now(),
        env.key,
    )
    .map_err(|e| format!("{e} ({url})"))?;
    Ok(Some((accepted, bytes, sig)))
}

/// Fetch and accept the channel's two manifests. The only function the UI and the command line
/// both call to answer "is there an update?".
///
/// `installed_gate` is the game version to run the strict NOT NEWER gate against (`--check-update`
/// says "not newer" as a refusal, exit 1); empty for every caller that decides from the manifests
/// itself (`plan`). The gate honours a switch in progress (`Env::switching`).
///
/// `remember` (dist LA6 + RL8): where to keep the accepted bytes + signatures so `load_accepted`
/// can hand the relay back to a later launch and `load_floor` can refuse a replay. `None` only in
/// tests. Both documents are kept or neither: a launcher.json that passed next to a game.json that
/// did not leaves the floors where they were.
pub fn check(
    fetch: &dyn Fetch,
    base_url: &str,
    env: &Env,
    installed_gate: &str,
    remember: Option<&Layout>,
) -> Result<Releases, String> {
    if base_url.trim().is_empty() {
        return Err(
            "NO UPDATE SOURCE: this build carries no default update URL and launcher.toml \
                    names none (set update_base_url, or build with MH_UPDATE_BASE_URL)"
                .to_string(),
        );
    }
    let launcher = fetch_accepted(fetch, base_url, env, KIND_LAUNCHER, remember)?;
    // game.json stays mandatory: a 404 here is an error like any other.
    let (game, gbytes, gsig) = fetch_accepted(fetch, base_url, env, KIND_GAME, remember)?
        .ok_or_else(|| format!("{R_FETCH}: the {} channel has no game.json", env.channel))?;
    let (launcher, launcher_raw) = match launcher {
        Some((a, b, s)) => (Some(a.into_launcher()?), Some((b, s))),
        None => {
            log::line(format!(
                "update: the {} channel has no launcher.json (HTTP 404) -- it offers no launcher; \
                 no launcher update will be planned",
                env.channel
            ));
            (None, None)
        }
    };
    let releases = Releases {
        launcher,
        game: game.into_game()?,
    };
    log::line(format!(
        "update: {} channel accepted -- game {} (issued {}, min launcher {}, {} configuration(s), \
         relay {}), launcher {}",
        env.channel,
        releases.game.version,
        releases.game.issued_at,
        releases.game.min_launcher,
        releases.game.game.len(),
        if releases.game.relay.is_some() {
            "named"
        } else {
            "none"
        },
        match &releases.launcher {
            Some(l) => format!("{} (issued {})", l.version, l.issued_at),
            None => "none offered".to_string(),
        }
    ));
    debug_assert!(releases
        .launcher
        .as_ref()
        .is_none_or(|l| l.channel == releases.game.channel));
    if let Some(days) = releases.age_days(Utc::now()) {
        if days > STALE_WARN_DAYS {
            log::line(format!(
                "update: WARNING the {} channel's manifest was issued {days} days ago (over \
                 {STALE_WARN_DAYS}). Genuine manifests do not expire, so this is either a quiet \
                 channel or somebody pinning an old file.",
                env.channel
            ));
        }
    }
    if let Some(layout) = remember {
        let mut keep = vec![(KIND_GAME, &gbytes, &gsig)];
        if let Some((b, s)) = &launcher_raw {
            keep.insert(0, (KIND_LAUNCHER, b, s));
        }
        for (kind, bytes, sig) in keep {
            if let Err(e) = remember_accepted(layout, env.channel, kind, bytes, sig) {
                log::line(format!("update: {e}"));
            }
        }
    }
    if !installed_gate.is_empty() {
        check_game_install(&releases.game.version, installed_gate, env.switching())?;
    }
    Ok(releases)
}

// --------------------------------------------------------------------------- the accepted copy

/// Keep a manifest that just passed every gate, WITH its signature, at
/// `accepted\<channel>\<kind>.json`. Bytes, not fields: what is kept is exactly what was signed,
/// and the loaders trust nothing about the file beyond what the key still says.
pub fn remember_accepted(
    layout: &Layout,
    channel: &str,
    kind: &str,
    bytes: &[u8],
    sig: &str,
) -> Result<(), String> {
    let m = layout.accepted_manifest(channel, kind);
    let s = layout.accepted_signature(channel, kind);
    if let Some(dir) = m.parent() {
        std::fs::create_dir_all(dir)
            .map_err(|e| format!("cannot create {}: {e}", dir.display()))?;
    }
    std::fs::write(&m, bytes).map_err(|e| format!("cannot write {}: {e}", m.display()))?;
    std::fs::write(&s, sig).map_err(|e| format!("cannot write {}: {e}", s.display()))?;
    Ok(())
}

/// The last accepted copy of one manifest, re-verified (signature, schema, kind, channel) and
/// re-parsed. `None` when there is none, or when the copy no longer verifies (an edited file is a
/// file this launcher never accepted); the reason goes to the log.
///
/// Only the SIGNATURE and SCHEMA gates run here, deliberately. The clock and replay gates decide
/// whether to ACCEPT a new fetch; this decides whether something already accepted may still be
/// used -- for its relay, and as the replay floor.
fn load_accepted_doc(layout: &Layout, channel: &str, kind: &str, key: &str) -> Option<Accepted> {
    let path = layout.accepted_manifest(channel, kind);
    let bytes = std::fs::read(&path).ok()?;
    let sig = std::fs::read_to_string(layout.accepted_signature(channel, kind)).ok()?;
    let checked = verify_signature_with(&bytes, &sig, key)
        .and_then(|()| check_header(&bytes, kind, channel))
        .and_then(|()| match kind {
            KIND_LAUNCHER => parse_launcher(&bytes).map(Accepted::Launcher),
            _ => parse_game(&bytes).map(Accepted::Game),
        });
    match checked {
        Ok(a) => Some(a),
        Err(e) => {
            log::line(format!(
                "update: the accepted manifest at {} is not usable and is ignored -- {e}",
                path.display()
            ));
            None
        }
    }
}

/// The REPLAY floor for `channel`+`kind`: the issued_at of the last accepted copy, which has just
/// been re-verified. `None` when there is no usable copy (nothing to compare against).
pub fn load_floor(layout: &Layout, channel: &str, kind: &str, key: &str) -> Option<DateTime<Utc>> {
    let issued = match load_accepted_doc(layout, channel, kind, key)? {
        Accepted::Launcher(m) => m.issued_at,
        Accepted::Game(m) => m.issued_at,
    };
    parse_issued(&issued).ok()
}

/// The last accepted GAME manifest of the channel `launcher.toml` names, re-verified. This is what
/// the launch path reads the relay from (dist LA6), and what the elevated `--step` reads.
pub fn load_accepted(layout: &Layout) -> Option<GameManifest> {
    let (cfg, _) = crate::config::Config::load(&layout.config());
    load_accepted_with_key(layout, cfg.channel(), PUBLIC_KEY)
}

pub fn load_accepted_with_key(
    layout: &Layout,
    channel: &str,
    public_key: &str,
) -> Option<GameManifest> {
    load_accepted_doc(layout, channel, KIND_GAME, public_key)?
        .into_game()
        .ok()
}

// --------------------------------------------------------------------------- applying

pub fn sha256_bytes(bytes: &[u8]) -> String {
    let mut h = Sha256::new();
    h.update(bytes);
    h.finalize().iter().map(|b| format!("{b:02x}")).collect()
}

fn check_digest(what: &str, bytes: &[u8], expected: &str) -> Result<(), String> {
    let got = sha256_bytes(bytes);
    if got.eq_ignore_ascii_case(expected.trim()) {
        Ok(())
    } else {
        Err(format!(
            "{R_DIGEST}: {what} hashes to {got}, the signed manifest says {expected}. \
             The file served is not the file that was signed for."
        ))
    }
}

#[derive(Clone, Debug)]
pub struct Applied {
    pub version: String,
    pub tag: String,
    /// The channel the installed game came from (dist RL8): the caller records it as
    /// `installed_channel`, which ends a channel switch.
    pub channel: String,
    pub summary: String,
    /// The version directories still on disk after the apply, newest first.
    pub kept: Vec<String>,
}

/// A progress line for the Play page (dist LA8): "downloading …", "installing …". The update job
/// runs on its own thread and the page repaints ten times a second, so the closure is how the
/// thread tells the page what it is doing without the page having to guess from the clock.
pub type Progress<'a> = &'a (dyn Fn(&str) + Sync);

#[cfg(test)]
fn no_progress(_: &str) {}

/// A version set downloaded, verified and swapped in, not yet copied beside `mh.exe`.
pub struct Staged {
    pub version_dir: PathBuf,
    pub pkg: PackageName,
    pub package: String,
}

/// Download one configuration, verify it, swap it in, and install it beside `mh.exe`.
///
/// THE ORDER IS THE POINT. The download lands in `versions\<ver>.staging\`, every byte is hashed
/// against the signed manifest BEFORE anything is unpacked, and the directory only becomes
/// `versions\<ver>\` by a rename -- an operation that either happened or did not. A launcher that
/// unpacked in place would have a window in which the version directory holds half of two releases,
/// and the machine that finds that window is the one that lost power during an update.
///
/// Two halves since dist LA8 (`fetch_and_stage`, `install_staged_set`), because a switch of
/// configuration has to UNINSTALL the old set between them -- after the new zip is verified and
/// on disk, so a failed download never leaves a game directory with nothing in it. `progress`
/// is what the Play page shows meanwhile.
///
/// dist LA13: the second half is the ONE step that may run elevated. When the game directory is
/// not writable by this token (Program Files), it is performed by an elevated re-run of this
/// launcher (`elevate::run_step_elevated`, `--step install:<version>:<tag>`) -- the download and
/// the staging above stay per-user, and so does everything after, the game's launch included.
///
/// dist RL4: install writes NO ini and NO key; the relay is provisioned by the caller into the
/// config directory (`relay::provision_for_game`), which never needs elevation.
pub fn apply(
    layout: &Layout,
    fetch: &dyn Fetch,
    manifest: &GameManifest,
    tag: &str,
    game_dir: &Path,
    base_url: &str,
    progress: Progress,
) -> Result<Applied, String> {
    match apply_guarded(
        layout,
        fetch,
        manifest,
        tag,
        game_dir,
        base_url,
        progress,
        &|| false,
    )? {
        Guarded::Applied(a) => Ok(a),
        Guarded::Deferred(why) => Err(why),
    }
}

/// What `apply_guarded` did.
#[derive(Clone, Debug)]
pub enum Guarded {
    Applied(Applied),
    /// The guard said the game is running once the download was in: nothing was installed. The
    /// string says why, for the log and the status line.
    Deferred(String),
}

/// `apply` with the RL9 guard: after the download is verified and staged -- the slow part, during
/// which a player can start the game -- and IMMEDIATELY before the first byte is copied beside
/// `mh.exe`, `running()` is asked again. True means the game is up: nothing is installed and the
/// staged set simply waits (the next attempt re-stages it, which is a download's worth of cost
/// paid only in that narrow race).
#[allow(clippy::too_many_arguments)]
pub fn apply_guarded(
    layout: &Layout,
    fetch: &dyn Fetch,
    manifest: &GameManifest,
    tag: &str,
    game_dir: &Path,
    base_url: &str,
    progress: Progress,
    running: &dyn Fn() -> bool,
) -> Result<Guarded, String> {
    let staged = fetch_and_stage(layout, fetch, manifest, tag, base_url, progress)?;
    if running() {
        let why = format!(
            "the game started while {} was being downloaded -- it stays staged and installs \
             after the game has exited",
            staged.package
        );
        log::line(format!("update: {why}"));
        return Ok(Guarded::Deferred(why));
    }
    let summary = install_set(layout, game_dir, &staged, progress)?;
    let kept = version_dirs(layout);
    log::line(format!(
        "update: {} installed; version directories now {:?}",
        manifest.version, kept
    ));
    Ok(Guarded::Applied(Applied {
        version: manifest.version.clone(),
        tag: staged.pkg.tag.clone(),
        channel: manifest.channel.clone(),
        summary,
        kept,
    }))
}

/// Copy a staged set beside `mh.exe`, elevating that one step when the folder needs it (dist
/// LA13). Shared by `apply`, `reverify` and `rollback`.
pub fn install_set(
    layout: &Layout,
    game_dir: &Path,
    staged: &Staged,
    progress: Progress,
) -> Result<String, String> {
    progress(&format!(
        "installing {} beside {GAME_EXE}...",
        staged.package
    ));
    if elevate::needs_elevation(game_dir) {
        return elevated_install(layout, game_dir, staged, progress);
    }
    match install_staged_set(staged, game_dir) {
        Ok(report) => Ok(report.summary()),
        Err(e) if elevate::is_access_denied(&e) => {
            log::line(format!("update: {e} -- retrying that step elevated"));
            elevated_install(layout, game_dir, staged, progress)
        }
        Err(e) => Err(e),
    }
}

fn elevated_install(
    layout: &Layout,
    game_dir: &Path,
    staged: &Staged,
    progress: Progress,
) -> Result<String, String> {
    progress(&format!(
        "installing {} beside {GAME_EXE} -- this needs administrator rights once (the game \
         directory is not writable); answer the prompt...",
        staged.package
    ));
    elevate::run_step_elevated(
        layout,
        game_dir,
        &elevate::StepSpec::Install {
            version: staged.pkg.version.clone(),
            tag: staged.pkg.tag.clone(),
        },
    )
}

/// The second half of `apply`, and the whole of an elevated `--step install` (dist LA13): remove a
/// DIFFERENT configuration's set first (receipt-driven, so retail's own `mh.dll` -- parked as
/// `mh.dll.mhbak`, the one FOREIGN file every install displaces, dist LA12 -- is put back and then
/// parked again), copy the staged set beside `mh.exe`, write the receipt.
///
/// dist RL4: the relay is NOT provisioned here any more -- it goes into the config directory, from
/// the caller, so this step has nothing user-side to write and an elevated run has no reason to.
pub fn install_staged_set(
    staged: &Staged,
    game_dir: &Path,
) -> Result<install::InstallReport, String> {
    if let Some(receipt) = install::read_manifest(game_dir) {
        if receipt.tag != staged.pkg.tag {
            let un = install::uninstall(game_dir)?;
            log::line(format!(
                "update: switched away from {} -- {}",
                receipt.tag,
                un.summary()
            ));
        }
    }
    install::install_from_version_dir(&staged.version_dir, &staged.pkg, &staged.package, game_dir)
}

/// The first half of `apply`: download, digest, unpack into staging, rename into place.
pub fn fetch_and_stage(
    layout: &Layout,
    fetch: &dyn Fetch,
    manifest: &GameManifest,
    tag: &str,
    base_url: &str,
    progress: Progress,
) -> Result<Staged, String> {
    let asset = manifest.asset(tag)?;
    check_url(&asset.url, base_url)?;
    let package = install::package_file_name(&manifest.version, tag);

    let limit = if asset.size > 0 {
        asset.size.saturating_add(1)
    } else {
        MAX_ASSET_BYTES
    };
    progress(&format!(
        "downloading {package}{}...",
        if asset.size > 0 {
            format!(" ({:.1} MB)", asset.size as f64 / 1_048_576.0)
        } else {
            String::new()
        }
    ));
    let bytes = fetch.get(&asset.url, limit.min(MAX_ASSET_BYTES))?;
    if asset.size > 0 && bytes.len() as u64 != asset.size {
        return Err(format!(
            "{R_DIGEST}: {package} is {} B and the signed manifest says {} B",
            bytes.len(),
            asset.size
        ));
    }
    progress(&format!("verifying {package}..."));
    check_digest(&package, &bytes, &asset.sha256)?;
    log::line(format!(
        "update: {package} verified against the signed manifest ({} B, sha256 {})",
        bytes.len(),
        asset.sha256
    ));

    let downloads = layout.root.join("download");
    std::fs::create_dir_all(&downloads)
        .map_err(|e| format!("cannot create {}: {e}", downloads.display()))?;
    let zip_path = downloads.join(&package);
    std::fs::write(&zip_path, &bytes)
        .map_err(|e| format!("cannot write {}: {e}", zip_path.display()))?;

    let pkg = PackageName {
        version: manifest.version.clone(),
        tag: tag.to_string(),
    };
    let version_dir = install::stage_zip(layout, &zip_path, &pkg.version)?;
    // The zip has done its job; keeping it would double the disk cost of every update for nothing.
    let _ = std::fs::remove_file(&zip_path);
    Ok(Staged {
        version_dir,
        pkg,
        package,
    })
}

// --------------------------------------------------------------------------- dist LA8: Play

/// What a game directory needs before the chosen configuration can be launched. Read off the
/// receipt (`mh_launcher_installed.txt`), not off `launcher.toml`: the receipt is what an
/// uninstall reads, so it is the one record that cannot say "installed" about a directory that
/// is not.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Readiness {
    /// The chosen configuration is installed: provision the relay and start.
    Ready,
    /// Nothing this launcher installed is there: download and install the chosen one first.
    NeedsInstall,
    /// A different configuration is installed: uninstall it, install the chosen one, then start.
    NeedsSwitch { from: String },
}

pub fn readiness(game_dir: &Path, tag: &str) -> Readiness {
    match install::read_manifest(game_dir) {
        None => Readiness::NeedsInstall,
        Some(r) if r.tag == tag => Readiness::Ready,
        Some(r) => Readiness::NeedsSwitch { from: r.tag },
    }
}

/// The installed version of `tag` per the game directory's receipt, empty when that configuration
/// is not what is installed there (nothing, or a different one -- a switch of configuration, not a
/// rollback, so no version gate applies to it).
pub fn installed_version_of(game_dir: &Path, tag: &str) -> String {
    match install::read_manifest(game_dir) {
        Some(r) if r.tag == tag => r.version,
        _ => String::new(),
    }
}

/// Refuse to install a game this launcher is too old for: the two non-proceeding verdicts of
/// `plan` as one error, for the paths that install in THIS process (`make_ready`,
/// `apply_if_needed`). `Ok(Some(version))` is a game to install now; `Ok(None)` is "current".
fn game_to_install_now(p: &Plan) -> Result<Option<String>, String> {
    if let Some(b) = &p.blocked {
        return Err(b.clone());
    }
    if p.deferred {
        return Err(format!(
            "{R_LAUNCHER_OLD}: game {} needs a newer launcher than this one, and the channel has \
             one. Press Update (or run --update --self-update) so the launcher goes first.",
            p.game.as_deref().unwrap_or("?")
        ));
    }
    Ok(p.game.clone())
}

/// Make a game directory ready to play the chosen configuration (dist LA8): nothing to do when
/// it is installed; otherwise the LA2 update path -- fetch the signed manifests, download, verify,
/// swap in, install (uninstalling a different configuration first) and provision the relay (LA6).
/// `Ok(None)` means nothing was installed because nothing had to be.
///
/// The manifest is checked with NOTHING installed, whatever `launcher.toml` remembers: on this
/// path the receipt says the chosen configuration is absent, and an "is it newer" gate against a
/// version that is not there (or is a different configuration about to be removed) would refuse
/// the very install the player asked for. A channel that does not offer the chosen tag installs
/// `net` instead (`resolve_tag`); one that offers neither fails with `refuse MALFORMED` before
/// anything is uninstalled, so it installs and removes nothing. A launcher older than the game's
/// `min_launcher` installs nothing (`refuse LAUNCHER TOO OLD`).
pub fn make_ready(
    layout: &Layout,
    fetch: &dyn Fetch,
    base_url: &str,
    game_dir: &Path,
    tag: &str,
    env: &Env,
    progress: Progress,
) -> Result<Option<Applied>, String> {
    let need = readiness(game_dir, tag);
    log::line(format!(
        "play: {} in {} -> {need:?}",
        tag,
        game_dir.display()
    ));
    if need == Readiness::Ready {
        return Ok(None);
    }
    progress(&format!("fetching the signed manifests for {tag}..."));
    let releases = check(fetch, base_url, env, "", Some(layout))?;
    let tag = resolve_tag(&releases.game, tag)?;
    if readiness(game_dir, &tag) == Readiness::Ready {
        return Ok(None);
    }
    let p = plan(
        releases.launcher.as_ref(),
        &releases.game,
        env.mine,
        "",
        env.installed_channel,
        env.channel,
    );
    game_to_install_now(&p)?;
    let applied = apply(
        layout,
        fetch,
        &releases.game,
        &tag,
        game_dir,
        base_url,
        progress,
    )?;
    Ok(Some(applied))
}

// --------------------------------------------------------------------------- dist LA12: the offer

/// What the accepted manifests offer THIS machine, relative to what it has (dist LA12, `plan`).
///
/// Computed once per check and shown on the Play page ("0.1.2 is available -- Update"), so the
/// automatic check every launcher start makes can SAY what it found without installing anything.
/// `None` in both fields is "up to date".
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Offer {
    /// The game version, when `plan` would install it -- newer than the receipt's for the chosen
    /// configuration, an older one while a channel switch is in progress, or the configuration
    /// is not installed at all (an install, not an update, but the button does the same thing).
    pub game: Option<String>,
    /// The channel's launcher version, when it is newer than this binary.
    pub launcher: Option<String>,
    /// `plan` refused the game (`refuse LAUNCHER TOO OLD`): the full sentence, for the log.
    pub blocked: Option<String>,
    /// How old the older of the two manifests is, in days (the Status page's "manifest issued N
    /// days ago"). Filled by the caller that has a clock; `offer_for` leaves it `None`.
    pub age_days: Option<i64>,
    /// The channel publishes no launcher manifest: the UI says "no launcher offered on this
    /// channel" instead of a launcher version.
    pub no_launcher: bool,
}

impl Offer {
    /// The one line the Play page shows, or `None` when nothing is offered.
    pub fn line(&self) -> Option<String> {
        match (&self.game, &self.launcher) {
            (None, None) => self
                .blocked
                .as_ref()
                .map(|_| "a newer launcher is needed".to_string()),
            (Some(g), None) => Some(format!("{g} is available")),
            (None, Some(l)) => Some(format!("launcher {l} is available")),
            (Some(g), Some(l)) => Some(format!("{g} is available (with launcher {l})")),
        }
    }

    pub fn any(&self) -> bool {
        self.game.is_some() || self.launcher.is_some()
    }
}

/// Decide what `releases` offers over `installed` (the receipt's version for the chosen
/// configuration, empty when that configuration is not installed) and over `env.mine`.
pub fn offer_for(releases: &Releases, installed: &str, env: &Env) -> Offer {
    let p = plan(
        releases.launcher.as_ref(),
        &releases.game,
        env.mine,
        installed,
        env.installed_channel,
        env.channel,
    );
    Offer {
        game: p.game,
        launcher: p.launcher,
        blocked: p.blocked,
        age_days: None,
        no_launcher: releases.launcher.is_none(),
    }
}

/// The game half of the one Update button (dist LA12), and what `--update` does: install the
/// chosen configuration when it is missing or a different one is there (the LA8 switch), update it
/// when the channel's is newer than the receipt -- or merely DIFFERENT while a channel switch is in
/// progress -- and do NOTHING (`Ok(None)`) when it is current.
///
/// The rollback refusal lives on: an offer that is not newer than the receipt is never installed
/// over it (outside a switch). It is simply not an ERROR here, because this is also what the
/// replacement launcher runs after a self-update (LA11), and a player who pressed Update for a
/// launcher whose game was already current must not be told the manifest was refused. A launcher
/// too old for the game (`min_launcher`) IS an error: installing anyway is the thing the field
/// exists to prevent.
#[allow(clippy::too_many_arguments)]
pub fn apply_if_needed(
    layout: &Layout,
    fetch: &dyn Fetch,
    releases: &Releases,
    tag: &str,
    game_dir: &Path,
    base_url: &str,
    env: &Env,
    progress: Progress,
) -> Result<Option<Applied>, String> {
    let tag = resolve_tag(&releases.game, tag)?;
    let installed = installed_version_of(game_dir, &tag);
    let p = plan(
        releases.launcher.as_ref(),
        &releases.game,
        env.mine,
        &installed,
        env.installed_channel,
        env.channel,
    );
    if game_to_install_now(&p)?.is_none() {
        log::line(format!(
            "update: the game stays at {installed:?} -- the {} channel offers {}",
            env.channel, releases.game.version
        ));
        return Ok(None);
    }
    apply(
        layout,
        fetch,
        &releases.game,
        &tag,
        game_dir,
        base_url,
        progress,
    )
    .map(Some)
}

// --------------------------------------------------------------------------- last known good

/// Every real version directory, newest first. `*.staging` is skipped: a staging directory is a
/// download in flight, not a version.
pub fn version_dirs(layout: &Layout) -> Vec<String> {
    let mut found: Vec<String> = Vec::new();
    if let Ok(entries) = std::fs::read_dir(layout.versions()) {
        for entry in entries.flatten() {
            if !entry.file_type().map(|t| t.is_dir()).unwrap_or(false) {
                continue;
            }
            let name = entry.file_name().to_string_lossy().to_string();
            if name.ends_with(crate::paths::VERSION_STAGING_SUFFIX) {
                continue;
            }
            found.push(name);
        }
    }
    // Semver order where possible, name order where not -- an unparseable directory name sorts
    // last rather than being invisible, because a directory nothing can order is still a directory
    // taking up disk.
    found.sort_by(|a, b| {
        let (va, vb) = (
            semver::Version::parse(a).ok(),
            semver::Version::parse(b).ok(),
        );
        match (va, vb) {
            (Some(x), Some(y)) => y.cmp(&x),
            (Some(_), None) => std::cmp::Ordering::Less,
            (None, Some(_)) => std::cmp::Ordering::Greater,
            (None, None) => b.cmp(a),
        }
    });
    found
}

pub fn first_run_marker(layout: &Layout, version: &str) -> PathBuf {
    layout.version_dir(version).join(FIRST_RUN_MARKER)
}

pub fn has_started_once(layout: &Layout, version: &str) -> bool {
    first_run_marker(layout, version).is_file()
}

/// Record that this version has been played. Called from the launch path, not from the update path:
/// an update that installed perfectly and then could not start is exactly the case the previous
/// version directory is kept for.
pub fn mark_started(layout: &Layout, version: &str) -> Result<(), String> {
    let path = first_run_marker(layout, version);
    if path.is_file() {
        return Ok(());
    }
    if let Some(dir) = path.parent() {
        if !dir.is_dir() {
            return Ok(());
        }
    }
    std::fs::write(
        &path,
        format!(
            "{}\nmh_launcher {}\n",
            log::stamp(),
            crate::version::VERSION
        ),
    )
    .map_err(|e| format!("cannot write {}: {e}", path.display()))?;
    log::line(format!(
        "update: {version} has started once -- {FIRST_RUN_MARKER} written, the previous version is \
         no longer needed as a fallback"
    ));
    Ok(())
}

/// Did this run count as "the new version started"?
///
/// A clean quit always does. A crash counts only if the game really ran first: a fault twenty
/// seconds in is a game bug, while a fault in the first second is what a broken install looks like,
/// and the whole value of keeping the old directory is being wrong in that direction.
pub fn run_counts_as_started(is_crash: bool, seconds: f64) -> bool {
    !is_crash || seconds >= STARTED_SECONDS
}

/// Delete all but the newest `KEEP_VERSIONS` version directories -- but only once `current` has
/// actually started.
///
/// Returns the directories removed. Nothing is removed while the marker is missing, which is the
/// clause dist LA2 is accepted on: *the old version directory survives until the new one has
/// started once*.
pub fn prune(layout: &Layout, current: &str) -> Vec<String> {
    let all = version_dirs(layout);
    if !has_started_once(layout, current) {
        log::line(format!(
            "update: keeping all {} version director(ies) -- {current} has not started yet, so the \
             one behind it is still the way back",
            all.len()
        ));
        return Vec::new();
    }
    let mut keep: Vec<String> = Vec::new();
    if all.iter().any(|v| v == current) {
        keep.push(current.to_string());
    }
    for name in &all {
        if keep.len() >= KEEP_VERSIONS {
            break;
        }
        if !keep.contains(name) {
            keep.push(name.clone());
        }
    }
    let mut removed = Vec::new();
    for name in &all {
        if keep.contains(name) {
            continue;
        }
        let dir = layout.version_dir(name);
        match std::fs::remove_dir_all(&dir) {
            Ok(()) => {
                log::line(format!("update: pruned {}", dir.display()));
                removed.push(name.clone());
            }
            Err(e) => log::line(format!("update: cannot prune {}: {e}", dir.display())),
        }
    }
    if removed.is_empty() {
        log::line(format!("update: nothing to prune (keeping {keep:?})"));
    }
    removed
}

// --------------------------------------------------------------------------- the launcher itself

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum SelfUpdate {
    /// The manifest's launcher is this one, or older.
    NotNeeded(String),
    /// The exe was replaced and a new process was started from it.
    Restarted(String),
    /// dist RL9, "Later": the exe was replaced and nothing was started (the window is closing).
    Replaced(String),
}

/// What `--verify-binary` prints. The health gate compares against it, so it is one place.
pub fn verify_binary_line() -> String {
    format!("mh_launcher {} ok", crate::version::VERSION)
}

/// Does the key compiled into this build parse? Part of `--verify-binary`, so a launcher that could
/// never verify a manifest fails its own health gate rather than becoming the installed one.
pub fn public_key_ok() -> bool {
    minisign_verify::PublicKey::from_base64(PUBLIC_KEY).is_ok()
}

/// Run a candidate launcher's own `--verify-binary` and require it to pass.
///
/// THIS IS THE WHOLE POINT OF THE SELF-UPDATE. Replacing a running executable is a one-way door:
/// if the replacement cannot start, the machine has no launcher and the player has no way to get
/// one except by hand. So the new binary is made to prove it runs, and to prove it is the version
/// the manifest promised, BEFORE it takes the old one's place. A digest proves the bytes are the
/// bytes that were signed; only executing them proves they are a program that works on THIS machine
/// -- the missing VC++ runtime, the wrong architecture, the antivirus quarantine.
fn health_gate(exe: &Path, expect_version: &str) -> Result<String, String> {
    let mut child = Command::new(exe)
        .arg("--verify-binary")
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .stdin(Stdio::null())
        .spawn()
        .map_err(|e| format!("{R_HEALTH}: {} will not start at all ({e})", exe.display()))?;
    let deadline = Instant::now() + HEALTH_GATE_TIMEOUT;
    let status = loop {
        match child.try_wait() {
            Ok(Some(s)) => break s,
            Ok(None) => {
                if Instant::now() >= deadline {
                    let _ = child.kill();
                    return Err(format!(
                        "{R_HEALTH}: {} did not answer --verify-binary within {}s",
                        exe.display(),
                        HEALTH_GATE_TIMEOUT.as_secs()
                    ));
                }
                std::thread::sleep(Duration::from_millis(25));
            }
            Err(e) => {
                return Err(format!(
                    "{R_HEALTH}: cannot wait on {} ({e})",
                    exe.display()
                ))
            }
        }
    };
    let mut out = String::new();
    if let Some(mut pipe) = child.stdout.take() {
        let _ = pipe.read_to_string(&mut out);
    }
    let out = out.trim().to_string();
    if !status.success() {
        return Err(format!(
            "{R_HEALTH}: {} answered --verify-binary with exit code {} (said {out:?}). \
             The old launcher is untouched.",
            exe.display(),
            status.code().unwrap_or(-1)
        ));
    }
    if !out.contains(expect_version) {
        return Err(format!(
            "{R_HEALTH}: {} reports {out:?} but the manifest promised version {expect_version}. \
             The old launcher is untouched.",
            exe.display()
        ));
    }
    Ok(out)
}

/// Replace this executable with the manifest's launcher and start the replacement.
///
/// `restart_args` are what the new process is given; the caller strips the flags that asked for the
/// update so the replacement does not immediately try to update again -- and, since dist LA11,
/// APPENDS the work that is still owed (`--update` when the request was launcher-then-game), so the
/// original request survives the restart. `crate::restart_argv` composes it and its test is the
/// LA11 evidence.
/// The three names `self_replace` (1.5, Windows) gives its scratch copies of the launcher:
/// `.<stem>.<32 random a-z>.__selfdelete__.exe` (the helper that deletes the old binary, then itself),
/// `__relocated__` (the old binary, moved aside) and `__temp__` (the new one mid-copy). They land next
/// to the launcher or in %TEMP% (the crate tries a rename there first).
const SELF_REPLACE_SUFFIXES: [&str; 3] =
    [".__selfdelete__.exe", ".__relocated__.exe", ".__temp__.exe"];

/// True for a file name `self_replace` made for the executable whose stem is `stem`.
fn is_self_replace_leftover(name: &str, stem: &str) -> bool {
    let Some(rest) = name.strip_prefix('.').and_then(|r| r.strip_prefix(stem)) else {
        return false;
    };
    let Some(rest) = rest.strip_prefix('.') else {
        return false;
    };
    SELF_REPLACE_SUFFIXES.iter().any(|sfx| {
        rest.strip_suffix(sfx)
            .is_some_and(|r| r.len() == 32 && r.bytes().all(|b| b.is_ascii_lowercase()))
    })
}

/// Deletes what a previous self-update left behind in `dirs` (2026-09-26: players found
/// `*.__selfdelete__.exe` beside the launcher after an update). The crate's helper deletes the old
/// binary and then ITSELF through a DELETE_ON_CLOSE handle handed to a `cmd.exe /c exit` it spawns;
/// if any step of that fails it simply exits, and nothing reports it. So every start sweeps. A helper
/// that is still running cannot be deleted (Windows refuses on a mapped image), so the sweep can never
/// pull a file out from under an update in progress -- it just finds it again next start.
/// Returns the paths removed.
pub fn sweep_self_replace_leftovers(dirs: &[PathBuf], stem: &str) -> Vec<PathBuf> {
    // HELPERS FIRST, and a live one freezes its directory. The restarted launcher runs this while the
    // helper may still be waiting to delete `__relocated__`; taking that file first would make the
    // helper's own DeleteFileW fail, and a failing helper exits WITHOUT deleting itself -- the very
    // leftover this sweep exists for. A helper we cannot delete is a helper still running.
    let mut removed = Vec::new();
    for dir in dirs {
        let Ok(entries) = std::fs::read_dir(dir) else {
            continue;
        };
        let mut rest = Vec::new();
        let mut helper_alive = false;
        for e in entries.flatten() {
            let name = e.file_name();
            let Some(name) = name.to_str() else { continue };
            if !is_self_replace_leftover(name, stem) {
                continue;
            }
            if name.ends_with(SELF_REPLACE_SUFFIXES[0]) {
                if std::fs::remove_file(e.path()).is_ok() {
                    removed.push(e.path());
                } else {
                    helper_alive = true;
                }
            } else {
                rest.push(e.path());
            }
        }
        if helper_alive {
            continue;
        }
        for p in rest {
            if std::fs::remove_file(&p).is_ok() {
                removed.push(p);
            }
        }
    }
    removed
}

/// The startup call: the launcher's own directory and %TEMP%, for the running executable's stem.
pub fn sweep_self_replace_leftovers_at_start() {
    let Ok(exe) = std::env::current_exe() else {
        return;
    };
    let Some(stem) = exe.file_stem().and_then(|s| s.to_str()).map(str::to_owned) else {
        return;
    };
    let mut dirs = vec![std::env::temp_dir()];
    if let Some(d) = exe.parent() {
        dirs.push(d.to_path_buf());
    }
    for p in sweep_self_replace_leftovers(&dirs, &stem) {
        log::line(format!(
            "update: removed a self-update leftover {}",
            p.display()
        ));
    }
}

/// A launcher candidate that has been downloaded, digest-checked and has PASSED ITS HEALTH GATE,
/// and is waiting to take this executable's place (dist RL9). Nothing has been replaced yet.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct StagedLauncher {
    pub version: String,
    pub candidate: PathBuf,
}

/// `stage_self_update`'s verdict.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum SelfStage {
    /// The manifest's launcher is this one, or older.
    NotNeeded(String),
    Staged(StagedLauncher),
}

/// The first half of a launcher self-update (dist RL9): refuse a launcher that is not newer,
/// download, check the digest, and make the candidate prove itself with `--verify-binary` -- and
/// STOP. The running executable is not touched, so this is safe to do silently in the background;
/// `commit_self_update` is the one-way door and is only walked through when the player says so
/// (Restart now) or the launcher is closing anyway (Later).
pub fn stage_self_update(
    layout: &Layout,
    fetch: &dyn Fetch,
    manifest: &LauncherManifest,
    mine: &str,
    base_url: &str,
) -> Result<SelfStage, String> {
    stage_self_update_with(layout, fetch, manifest, mine, base_url, &health_gate)
}

/// `stage_self_update` with the health gate injectable, so a test can stage a byte string that is
/// not a program.
pub fn stage_self_update_with(
    layout: &Layout,
    fetch: &dyn Fetch,
    manifest: &LauncherManifest,
    mine: &str,
    base_url: &str,
    gate: &dyn Fn(&Path, &str) -> Result<String, String>,
) -> Result<SelfStage, String> {
    let theirs = &manifest.version;
    // The launcher NEVER goes down (`check_launcher_install`), whatever channel is followed.
    if let Err(e) = check_launcher_install(theirs, mine) {
        log::line(format!("update: the launcher stays at {mine} -- {e}"));
        return Ok(SelfStage::NotNeeded(format!(
            "the launcher is {mine}; the manifest offers {theirs}"
        )));
    }
    check_url(&manifest.url, base_url)?;

    let dir = layout.root.join("update");
    std::fs::create_dir_all(&dir).map_err(|e| format!("cannot create {}: {e}", dir.display()))?;
    let candidate = dir.join(format!("mh_launcher-{theirs}.exe"));
    let bytes = fetch.get(
        &manifest.url,
        manifest.size.saturating_add(1).min(MAX_ASSET_BYTES),
    )?;
    if bytes.len() as u64 != manifest.size {
        return Err(format!(
            "{R_DIGEST}: the launcher executable is {} B and the signed manifest says {} B",
            bytes.len(),
            manifest.size
        ));
    }
    check_digest("the launcher executable", &bytes, &manifest.sha256)?;
    std::fs::write(&candidate, &bytes)
        .map_err(|e| format!("cannot write {}: {e}", candidate.display()))?;
    log::line(format!(
        "update: launcher {theirs} downloaded to {} ({} B, digest matches)",
        candidate.display(),
        bytes.len()
    ));

    match gate(&candidate, theirs) {
        Ok(said) => log::line(format!(
            "update: health gate passed -- {} said {said:?}",
            candidate.display()
        )),
        Err(e) => {
            // The one-way door was not opened. Take the candidate away so a later run does not find
            // a binary that has already failed and wonder about it.
            let _ = std::fs::remove_file(&candidate);
            log::line(format!("update: {e}"));
            return Err(e);
        }
    }
    Ok(SelfStage::Staged(StagedLauncher {
        version: theirs.clone(),
        candidate,
    }))
}

/// The second half (dist RL9): replace this executable with the staged candidate. With
/// `restart_args` the replacement is started with them (Restart now -- `crate::restart_argv`
/// composes the `--view <current>` list); with `None` nothing is started (Later: the launcher is
/// already closing, and the next start simply runs the new version).
pub fn commit_self_update(
    staged: &StagedLauncher,
    restart_args: Option<&[String]>,
) -> Result<SelfUpdate, String> {
    let running = std::env::current_exe()
        .map_err(|e| format!("cannot find this executable's own path ({e})"))?;
    commit_self_update_with(
        staged,
        restart_args,
        &running,
        &|candidate| {
            self_replace::self_replace(candidate)
                .map_err(|e| format!("cannot replace {} ({e})", running.display()))
        },
        &|exe, args| {
            Command::new(exe)
                .args(args)
                .spawn()
                .map(|_| ())
                .map_err(|e| format!("replaced the launcher but cannot start it ({e})"))
        },
    )
}

/// The spawn half of a restart: start `exe` with `args`.
pub type SpawnFn<'a> = &'a dyn Fn(&Path, &[String]) -> Result<(), String>;

/// `commit_self_update` with the replace and the spawn injectable (a test cannot replace the test
/// binary it is running in).
pub fn commit_self_update_with(
    staged: &StagedLauncher,
    restart_args: Option<&[String]>,
    running: &Path,
    replace: &dyn Fn(&Path) -> Result<(), String>,
    spawn: SpawnFn,
) -> Result<SelfUpdate, String> {
    if !staged.candidate.is_file() {
        return Err(format!(
            "the staged launcher {} is gone -- nothing was replaced",
            staged.candidate.display()
        ));
    }
    replace(&staged.candidate)?;
    let _ = std::fs::remove_file(&staged.candidate);
    match restart_args {
        Some(args) => {
            log::line(format!(
                "update: {} replaced with launcher {}; restarting with {args:?}",
                running.display(),
                staged.version
            ));
            spawn(running, args)?;
            Ok(SelfUpdate::Restarted(staged.version.clone()))
        }
        None => {
            log::line(format!(
                "update: {} replaced with launcher {}; not restarted (the window is closing)",
                running.display(),
                staged.version
            ));
            Ok(SelfUpdate::Replaced(staged.version.clone()))
        }
    }
}

/// Replace this executable with the manifest's launcher and start the replacement: stage, then
/// commit with a restart (the Update button and `--self-update`). dist RL9 split the two halves so
/// the silent auto-update can stage now and commit at the player's word.
pub fn self_update(
    layout: &Layout,
    fetch: &dyn Fetch,
    releases: &Releases,
    base_url: &str,
    restart_args: &[String],
) -> Result<SelfUpdate, String> {
    let Some(launcher) = &releases.launcher else {
        return Ok(SelfUpdate::NotNeeded(
            "this channel offers no launcher".to_string(),
        ));
    };
    match stage_self_update(layout, fetch, launcher, crate::version::VERSION, base_url)? {
        SelfStage::NotNeeded(msg) => Ok(SelfUpdate::NotNeeded(msg)),
        SelfStage::Staged(staged) => commit_self_update(&staged, Some(restart_args)),
    }
}

// --------------------------------------------------------------------------- dist RL9: silent auto-update

/// What the silent auto-update needs besides a `Fetch`: where, which configuration, and the few
/// facts that decide whether to act (`auto_update`).
pub struct AutoCtx<'a> {
    pub layout: &'a Layout,
    pub base_url: &'a str,
    pub game_dir: &'a Path,
    pub tag: &'a str,
    pub env: &'a Env<'a>,
    /// The version the player ROLLED BACK from (`Config::rollback_skip`), or empty. The auto-update
    /// never installs this version or anything not newer than it -- a rollback that the next tick
    /// undid would be no rollback. A release newer than it is offered as usual, and the manual
    /// Update button ignores the pin.
    pub skip_version: &'a str,
    /// A launcher is already staged and waiting for its restart: do not download another.
    pub launcher_pending: bool,
}

/// What one silent run found and did.
#[derive(Clone, Debug, Default)]
pub struct AutoRun {
    /// What the accepted manifests offer (the Play page's line).
    pub offer: Offer,
    /// A launcher candidate that passed its health gate and awaits "Restart now / Later".
    pub launcher: Option<StagedLauncher>,
    /// The game update that was installed, if one was.
    pub applied: Option<Applied>,
    /// A game update was wanted and held back, and why (the game is running; a launcher restart is
    /// owed first). Not an error: the next tick tries again.
    pub deferred: Option<String>,
    /// Failures that did not stop the other half (a launcher download that failed while the game
    /// update succeeded). The current versions are untouched by every one of them.
    pub errors: Vec<String>,
}

/// The silent update (dist RL9): fetch the signed manifests, `plan`, and carry out what is owed
/// WITHOUT a click -- with three limits.
///
/// 1. **Launcher: stage only.** A newer launcher is downloaded, digest-checked and health-gated
///    (`stage_self_update`) and handed back; the restart is the player's call.
/// 2. **Game: only an UPDATE of what is installed.** Installing a configuration that is not there
///    (or switching one) is Play's job (`make_ready`) and the player's decision; a silent download
///    of the whole game onto a folder that merely holds retail would be a surprise.
/// 3. **Never while `mh.exe` runs from the folder.** Asked before the download starts and again
///    right before the first file is copied (`apply_guarded`) -- the download takes long enough
///    for a player to press Play.
///
/// An `Err` is a failure before anything was decided (the fetch, a refused manifest); the caller
/// shows it on the non-blocking update line and the NEXT tick tries again. Whatever fails, the
/// installed versions are exactly what they were.
pub fn auto_update(
    fetch: &dyn Fetch,
    ctx: &AutoCtx,
    running: &dyn Fn() -> bool,
    progress: Progress,
) -> Result<AutoRun, String> {
    let releases = check(fetch, ctx.base_url, ctx.env, "", Some(ctx.layout))?;
    let tag = resolve_tag(&releases.game, ctx.tag)?;
    let installed = installed_version_of(ctx.game_dir, &tag);
    let p = plan(
        releases.launcher.as_ref(),
        &releases.game,
        ctx.env.mine,
        &installed,
        ctx.env.installed_channel,
        ctx.env.channel,
    );
    let mut run = AutoRun {
        offer: offer_for(&releases, &installed, ctx.env),
        ..Default::default()
    };
    run.offer.age_days = releases.age_days(Utc::now());

    if let (Some(launcher), false) = (&releases.launcher, ctx.launcher_pending) {
        if p.launcher.is_some() {
            match stage_self_update(ctx.layout, fetch, launcher, ctx.env.mine, ctx.base_url) {
                Ok(SelfStage::Staged(s)) => run.launcher = Some(s),
                Ok(SelfStage::NotNeeded(_)) => {}
                Err(e) => {
                    log::line(format!("update: launcher not staged -- {e}"));
                    run.errors.push(e);
                }
            }
        }
    }

    let Some(game_version) = p.game.clone() else {
        return Ok(run);
    };
    if p.deferred {
        let why = format!(
            "game {game_version} needs the newer launcher first -- it follows after the restart"
        );
        log::line(format!("update: {why}"));
        run.deferred = Some(why);
        return Ok(run);
    }
    if installed.is_empty() {
        log::line(format!(
            "update: {game_version} is offered but {tag} is not installed here -- Play installs it, \
             the silent update does not"
        ));
        return Ok(run);
    }
    if !ctx.skip_version.trim().is_empty()
        && check_newer(&game_version, ctx.skip_version.trim()).is_err()
    {
        log::line(format!(
            "update: {game_version} is not newer than {} (rolled back from it) -- left alone",
            ctx.skip_version.trim()
        ));
        return Ok(run);
    }
    if running() {
        let why = format!("{game_version} is waiting for the game to exit");
        log::line(format!("update: {why}"));
        run.deferred = Some(why);
        return Ok(run);
    }
    match apply_guarded(
        ctx.layout,
        fetch,
        &releases.game,
        &tag,
        ctx.game_dir,
        ctx.base_url,
        progress,
        running,
    ) {
        Ok(Guarded::Applied(a)) => run.applied = Some(a),
        Ok(Guarded::Deferred(why)) => run.deferred = Some(why),
        Err(e) => {
            log::line(format!("update: the game update failed -- {e}"));
            run.errors.push(e);
        }
    }
    Ok(run)
}

// --------------------------------------------------------------------------- dist RL9: re-verify and roll back

/// Receipt rows that are not the install's business any more (dist RL4): the player's ini and key
/// live in the config directory, and a receipt written by an older launcher may still list them.
fn is_player_file(name: &str) -> bool {
    name.eq_ignore_ascii_case(crate::relay::INI_NAME)
        || name.eq_ignore_ascii_case(crate::relay::KEY_NAME)
}

/// The receipt's rows whose file is missing or no longer hashes to what was installed.
pub fn receipt_mismatches(game_dir: &Path, receipt: &install::Manifest) -> (usize, Vec<String>) {
    let mut checked = 0;
    let mut bad = Vec::new();
    for (_, sha, file) in &receipt.files {
        if is_player_file(file) {
            continue;
        }
        checked += 1;
        let path = game_dir.join(file);
        if install::sha256_file(&path).ok().as_deref() != Some(sha.as_str()) {
            bad.push(file.clone());
        }
    }
    (checked, bad)
}

fn staged_from_receipt(layout: &Layout, version: &str, tag: &str) -> Staged {
    Staged {
        version_dir: layout.version_dir(version),
        pkg: PackageName {
            version: version.to_string(),
            tag: tag.to_string(),
        },
        package: install::package_file_name(version, tag),
    }
}

/// Diagnostics "Re-verify" (dist RL9): re-hash every file the install record lists and, on a
/// mismatch, put the set back from the kept copy of that version under `versions\`. Returns the
/// status line. The caller has already refused to run while the game is.
pub fn reverify(layout: &Layout, game_dir: &Path, progress: Progress) -> Result<String, String> {
    let receipt = install::read_manifest(game_dir).ok_or_else(|| {
        format!(
            "nothing in {} was installed by this launcher, so there is nothing to verify",
            game_dir.display()
        )
    })?;
    let (checked, bad) = receipt_mismatches(game_dir, &receipt);
    if bad.is_empty() {
        let line = format!(
            "all {checked} installed file(s) match the install record ({} {})",
            receipt.version, receipt.tag
        );
        log::line(format!("reverify: {line}"));
        return Ok(line);
    }
    log::line(format!("reverify: {} file(s) differ: {bad:?}", bad.len()));
    let staged = staged_from_receipt(layout, &receipt.version, &receipt.tag);
    if !staged.version_dir.is_dir() {
        return Err(format!(
            "{} of {checked} file(s) differ ({}) and the kept copy of {} is gone -- press Update \
             to download it again",
            bad.len(),
            bad.join(", "),
            receipt.version
        ));
    }
    // The kept copy must itself be what was installed, or "restoring" it restores a different file.
    let (_, kept_bad) = staged_mismatches(&staged.version_dir, &receipt);
    if !kept_bad.is_empty() {
        return Err(format!(
            "{} file(s) differ ({}) and the kept copy of {} does not match the install record either \
             ({}) -- press Update to download it again",
            bad.len(),
            bad.join(", "),
            receipt.version,
            kept_bad.join(", ")
        ));
    }
    install_set(layout, game_dir, &staged, progress)?;
    let again = install::read_manifest(game_dir)
        .map(|r| receipt_mismatches(game_dir, &r).1)
        .unwrap_or_default();
    if !again.is_empty() {
        return Err(format!(
            "reinstalled {} from the kept copy but {} file(s) still differ: {}",
            receipt.version,
            again.len(),
            again.join(", ")
        ));
    }
    let line = format!(
        "{} of {checked} file(s) differed ({}) -- reinstalled {} from the kept copy",
        bad.len(),
        bad.join(", "),
        receipt.version
    );
    log::line(format!("reverify: {line}"));
    Ok(line)
}

/// The rows of `receipt` whose file in the kept version directory is missing or hashes differently.
fn staged_mismatches(version_dir: &Path, receipt: &install::Manifest) -> (usize, Vec<String>) {
    let mut checked = 0;
    let mut bad = Vec::new();
    for (_, sha, file) in &receipt.files {
        if is_player_file(file) {
            continue;
        }
        checked += 1;
        if install::sha256_file(&version_dir.join(file))
            .ok()
            .as_deref()
            != Some(sha.as_str())
        {
            bad.push(file.clone());
        }
    }
    (checked, bad)
}

/// What a rollback did.
#[derive(Clone, Debug)]
pub struct RolledBack {
    pub from: String,
    pub to: String,
    pub tag: String,
    pub summary: String,
}

/// The version `rollback` would go to: the newest kept version directory that is OLDER than
/// `current` (semver; an unparseable name is never a target).
pub fn rollback_target(layout: &Layout, current: &str) -> Option<String> {
    let cur = semver::Version::parse(current).ok()?;
    version_dirs(layout).into_iter().find(|name| {
        semver::Version::parse(name).is_ok_and(|v| v < cur) && layout.version_dir(name).is_dir()
    })
}

/// Diagnostics "Roll back" (dist RL9): switch the game folder to the kept previous version using
/// the same two-versions-on-disk mechanism the updater maintains (`prune`). The caller records
/// `from` as the auto-update pin (`Config::rollback_skip`) and has already refused to run while the
/// game is.
pub fn rollback(
    layout: &Layout,
    game_dir: &Path,
    progress: Progress,
) -> Result<RolledBack, String> {
    let receipt = install::read_manifest(game_dir).ok_or_else(|| {
        format!(
            "nothing in {} was installed by this launcher, so there is nothing to roll back",
            game_dir.display()
        )
    })?;
    let to = rollback_target(layout, &receipt.version).ok_or_else(|| {
        format!(
            "no version older than {} is kept (the launcher keeps the previous one until the new \
             one has started once)",
            receipt.version
        )
    })?;
    let staged = staged_from_receipt(layout, &to, &receipt.tag);
    let have = std::fs::read_dir(&staged.version_dir)
        .map(|d| d.flatten().filter(|e| e.path().is_file()).count())
        .unwrap_or(0);
    if have == 0 {
        return Err(format!(
            "the kept copy of {to} ({}) is empty -- nothing was changed",
            staged.version_dir.display()
        ));
    }
    log::line(format!(
        "rollback: {} -> {to} ({}) from {}",
        receipt.version,
        receipt.tag,
        staged.version_dir.display()
    ));
    let summary = install_set(layout, game_dir, &staged, progress)?;
    Ok(RolledBack {
        from: receipt.version,
        to,
        tag: receipt.tag,
        summary,
    })
}

// --------------------------------------------------------------------------- dist RL9: the tick's state

/// How often the silent auto-update looks (dist RL9): on start, then every 30 minutes.
pub const AUTO_INTERVAL: Duration = Duration::from_secs(30 * 60);
/// ... and how often it retries while an update is WAITING (the game is running): a player who
/// quits the game should not wait half an hour for the update that was held back.
pub const AUTO_DEFERRED_INTERVAL: Duration = Duration::from_secs(10);

/// Everything the window remembers for the silent auto-update, in ONE struct so `App` carries one
/// field for it (the view code is being rewritten in parallel; one line is all the merge sees).
/// The scheduling is pure (`due`, `schedule`) and tested without a window.
#[derive(Debug, Default)]
pub struct AutoState {
    /// The silent update runs at all: an interactive start with an update source. Scripted runs
    /// (`--launch`, `--update`, `--report`, ...) never start background work of their own.
    pub enabled: bool,
    /// When the next look is due; `None` = now (the first tick after start).
    pub next: Option<Instant>,
    /// A launcher candidate that passed its health gate and awaits the player's "Restart now /
    /// Later" -- and, failing both, the window closing (`commit_self_update(.., None)`).
    pub pending_restart: Option<StagedLauncher>,
    /// The modal was answered ("Later"): do not ask again this run.
    pub restart_dismissed: bool,
    /// "waiting for the game to exit" has been logged for this wait (once, not every 10 s).
    pub waiting_logged: bool,
    /// A migration attempt failed this run (a declined UAC prompt): do not nag again until restart.
    pub migrate_failed: bool,
    /// The Apply job in flight was started by Play (a pending game update installing first): if it
    /// fails, the current version is launched anyway.
    pub play_update: bool,
}

impl AutoState {
    /// Is a look due at `now`?
    pub fn due(&self, now: Instant) -> bool {
        self.enabled && self.next.is_none_or(|t| now >= t)
    }

    /// Schedule the next look: soon while something is waiting, the usual interval otherwise.
    pub fn schedule(&mut self, now: Instant, waiting: bool) {
        self.next = Some(
            now + if waiting {
                AUTO_DEFERRED_INTERVAL
            } else {
                AUTO_INTERVAL
            },
        );
    }

    /// The version to show in the restart modal, until it has been answered.
    pub fn restart_prompt(&self) -> Option<&str> {
        if self.restart_dismissed {
            return None;
        }
        self.pending_restart.as_ref().map(|s| s.version.as_str())
    }
}

// --------------------------------------------------------------------------- tests

#[cfg(test)]
mod tests {
    use super::*;

    // ---- fixtures ------------------------------------------------------------------------------
    //
    // Every `s2_*` fixture is a schema-2 channel manifest signed by the COMMITTED TEST-ONLY key
    // (`tests/data/schema2_test.key`, made by `tests/data/gen_schema2_fixtures.py`). That key is
    // injected through `accept_v2`'s key parameter and nowhere else -- `PUBLIC_KEY` stays the
    // release key, and `a_test_key_signature_is_not_the_release_keys` pins that a manifest signed
    // by the test key is REFUSED by it. `manifest.json` (+ `.minisig`) is the one fixture signed by
    // the RELEASE key: a genuine schema-1 root manifest, kept to prove it is refused as schema.

    macro_rules! fixture {
        ($name:literal) => {
            (
                include_bytes!(concat!("../tests/data/", $name, ".json")).as_slice(),
                include_str!(concat!("../tests/data/", $name, ".json.minisig")),
            )
        };
    }

    const TEST_PUB: &str = include_str!("../tests/data/schema2_test.pub");
    const TEST_BASE: &str = "http://127.0.0.1:8099/";
    const PLAY_ZIP_NET: &[u8] = include_bytes!("../tests/data/mission_humanity_re-0.3.0-net.zip");
    const RELAY_ADDR: &str = "192.0.2.10:7100";
    const RELAY_KEY: &str = "4d487465737474656b65794d487465737474656b65794d487465737474656b65";

    fn test_key() -> String {
        TEST_PUB
            .lines()
            .find(|l| !l.starts_with("untrusted comment"))
            .unwrap()
            .trim()
            .to_string()
    }

    fn at(rfc3339: &str) -> DateTime<Utc> {
        DateTime::parse_from_rfc3339(rfc3339)
            .unwrap()
            .with_timezone(&Utc)
    }

    /// After every fixture was issued (the latest game is 2026-10-02), before any real date.
    fn now() -> DateTime<Utc> {
        at("2026-10-08T12:00:00Z")
    }

    fn accept_game(
        (bytes, sig): (&[u8], &str),
        channel: &str,
        floor: Option<DateTime<Utc>>,
        now: DateTime<Utc>,
    ) -> Result<GameManifest, String> {
        accept_v2(
            bytes,
            sig,
            TEST_BASE,
            KIND_GAME,
            channel,
            floor,
            now,
            &test_key(),
        )
        .and_then(Accepted::into_game)
    }

    fn accept_launcher(
        (bytes, sig): (&[u8], &str),
        channel: &str,
        floor: Option<DateTime<Utc>>,
        now: DateTime<Utc>,
    ) -> Result<LauncherManifest, String> {
        accept_v2(
            bytes,
            sig,
            TEST_BASE,
            KIND_LAUNCHER,
            channel,
            floor,
            now,
            &test_key(),
        )
        .and_then(Accepted::into_launcher)
    }

    fn game_latest() -> GameManifest {
        accept_game(fixture!("s2_game_latest"), "latest", None, now()).unwrap()
    }
    fn game_stable() -> GameManifest {
        accept_game(fixture!("s2_game_stable"), "stable", None, now()).unwrap()
    }
    fn launcher_latest() -> LauncherManifest {
        accept_launcher(fixture!("s2_launcher_latest"), "latest", None, now()).unwrap()
    }
    fn launcher_stable() -> LauncherManifest {
        accept_launcher(fixture!("s2_launcher_stable"), "stable", None, now()).unwrap()
    }

    // ---- the gates -----------------------------------------------------------------------------

    #[test]
    fn the_baked_public_key_parses() {
        minisign_verify::PublicKey::from_base64(PUBLIC_KEY)
            .expect("the compiled-in public key must be a minisign key");
    }

    /// The happy path, and the two documents' shapes (dist RL8 section 1): every field the
    /// design names reads back, the relay is the signed one, the optional notes_url is optional.
    #[test]
    fn schema2_manifests_signed_by_the_test_key_are_accepted_and_read_back() {
        let g = game_latest();
        assert_eq!(
            (g.schema, g.kind.as_str(), g.channel.as_str()),
            (2, "game", "latest")
        );
        assert_eq!(
            (g.version.as_str(), g.min_launcher.as_str()),
            ("0.3.0", "0.2.0")
        );
        assert_eq!(g.issued_at, "2026-10-02T00:00:00Z");
        assert_eq!(g.game.len(), 3);
        assert_eq!(g.asset("net").unwrap().size, PLAY_ZIP_NET.len() as u64);
        assert_eq!(g.notes_url, "https://example.invalid/notes/0.3.0");
        let relay = g.relay.as_ref().expect("the fixture names a relay");
        assert_eq!(
            (relay.addr.as_str(), relay.key.as_str()),
            (RELAY_ADDR, RELAY_KEY)
        );
        assert_eq!(
            game_stable().game.keys().collect::<Vec<_>>(),
            vec!["net"],
            "decision D1: the published map is net only"
        );

        let l = launcher_latest();
        assert_eq!(
            (l.kind.as_str(), l.channel.as_str()),
            ("launcher", "latest")
        );
        assert_eq!((l.version.as_str(), l.size), ("0.2.0", 1234));
        assert!(l.url.ends_with("mh_launcher-0.2.0.exe"));
        assert_eq!(l.notes_url, "", "notes_url is optional");
    }

    /// A signature made by the test key is the TEST key's: the release key refuses it, so a
    /// committed test key cannot be used to feed a real launcher anything.
    #[test]
    fn a_test_key_signature_is_not_the_release_keys() {
        let (bytes, sig) = fixture!("s2_game_latest");
        let e = verify_signature(bytes, sig).unwrap_err();
        assert!(e.starts_with(R_SIGNATURE), "{e}");
        let e = accept_v2(
            bytes,
            sig,
            TEST_BASE,
            KIND_GAME,
            "latest",
            None,
            now(),
            PUBLIC_KEY,
        )
        .unwrap_err();
        assert!(e.starts_with(R_SIGNATURE), "{e}");
    }

    /// done_when: a tampered signature is refused. A flipped body byte, a body that is somebody
    /// else's (the launcher manifest under the game's signature), and a damaged signature.
    #[test]
    fn a_tampered_manifest_or_signature_is_refused_before_anything_is_read() {
        let (bytes, sig) = fixture!("s2_game_latest");
        let mut flipped = bytes.to_vec();
        let last = flipped.len() - 2;
        flipped[last] ^= 0x20;
        let e = accept_game((&flipped, sig), "latest", None, now()).unwrap_err();
        assert!(e.starts_with(R_SIGNATURE), "{e}");

        // Re-pointing the relay is exactly the attack the signature exists to stop.
        let repointed = String::from_utf8(bytes.to_vec())
            .unwrap()
            .replace(RELAY_ADDR, "198.51.100.7:7100");
        let e = accept_game((repointed.as_bytes(), sig), "latest", None, now()).unwrap_err();
        assert!(e.starts_with(R_SIGNATURE), "{e}");

        let (lbytes, _) = fixture!("s2_launcher_latest");
        let e = accept_game((lbytes, sig), "latest", None, now()).unwrap_err();
        assert!(e.starts_with(R_SIGNATURE), "{e}");

        let damaged = sig.replacen("RUS", "RUT", 1);
        let e = accept_game((bytes, &damaged), "latest", None, now()).unwrap_err();
        assert!(e.starts_with(R_SIGNATURE), "{e}");
        let e = accept_game((bytes, "not a signature"), "latest", None, now()).unwrap_err();
        assert!(e.starts_with(R_SIGNATURE), "{e}");
    }

    /// done_when: a non-https URL is refused -- unless it is on the configured origin (the local
    /// stand-in), which is how the other tests run end to end.
    #[test]
    fn a_non_https_url_off_the_configured_origin_is_refused() {
        let (bytes, sig) = fixture!("s2_game_latest_http");
        let e = accept_game((bytes, sig), "latest", None, now()).unwrap_err();
        assert!(e.starts_with(R_URL), "{e}");
        assert!(e.contains("evil.example"), "{e}");
        // The same fixtures against a production (https) base: their stand-in URLs are refused too.
        for (fx, kind) in [
            (fixture!("s2_game_latest"), KIND_GAME),
            (fixture!("s2_launcher_latest"), KIND_LAUNCHER),
        ] {
            let e = accept_v2(
                fx.0,
                fx.1,
                "https://example.invalid/mh",
                kind,
                "latest",
                None,
                now(),
                &test_key(),
            )
            .unwrap_err();
            assert!(e.starts_with(R_URL), "{kind}: {e}");
        }
    }

    /// Cross-serving: a GENUINE manifest under the wrong name is refused for what is written
    /// inside it, not trusted for its signature.
    #[test]
    fn a_genuine_manifest_served_as_the_wrong_channel_or_kind_is_refused() {
        // stable's game manifest, fetched as latest (and vice versa)
        let e = accept_game(fixture!("s2_game_stable"), "latest", None, now()).unwrap_err();
        assert!(e.starts_with(R_CHANNEL), "{e}");
        let e = accept_game(fixture!("s2_game_latest"), "stable", None, now()).unwrap_err();
        assert!(e.starts_with(R_CHANNEL), "{e}");
        let e = accept_launcher(fixture!("s2_launcher_stable"), "latest", None, now()).unwrap_err();
        assert!(e.starts_with(R_CHANNEL), "{e}");
        // a launcher manifest served where the game's belongs (and the reverse)
        let e = accept_game(fixture!("s2_launcher_latest"), "latest", None, now()).unwrap_err();
        assert!(e.starts_with(R_SCHEMA), "{e}");
        let e = accept_launcher(fixture!("s2_game_latest"), "latest", None, now()).unwrap_err();
        assert!(e.starts_with(R_SCHEMA), "{e}");
        // a kind that is not a kind
        let (b, s) = fixture!("s2_game_latest");
        let e =
            accept_v2(b, s, TEST_BASE, "relay", "latest", None, now(), &test_key()).unwrap_err();
        assert!(e.starts_with(R_SCHEMA), "{e}");
    }

    /// The genuine, RELEASE-key-signed schema-1 root manifest: its signature verifies, and it is
    /// refused anyway, as a schema -- this launcher no longer reads the root manifest.json.
    #[test]
    fn the_schema1_root_manifest_is_refused_as_a_schema_even_with_a_valid_release_signature() {
        let bytes: &[u8] = include_bytes!("../tests/data/manifest.json");
        let sig = include_str!("../tests/data/manifest.json.minisig");
        verify_signature(bytes, sig).expect("the fixture really is signed by the release key");
        for kind in [KIND_GAME, KIND_LAUNCHER] {
            let e = accept_v2(
                bytes,
                sig,
                TEST_BASE,
                kind,
                "stable",
                None,
                now(),
                PUBLIC_KEY,
            )
            .unwrap_err();
            assert!(e.starts_with(R_SCHEMA), "{kind}: {e}");
            assert!(e.contains("schema 1"), "{e}");
        }
    }

    /// done_when: an expired issued_at is ACCEPTED -- the 30-day STALE refusal is gone. Nine months
    /// after issue the manifest still passes; only the clock-ahead direction is refused.
    #[test]
    fn an_old_issued_at_is_accepted_but_a_future_one_is_refused() {
        let nine_months_on = at("2027-07-08T12:00:00Z");
        accept_game(fixture!("s2_game_latest"), "latest", None, nine_months_on)
            .expect("no STALE gate any more");
        accept_launcher(
            fixture!("s2_launcher_latest"),
            "latest",
            None,
            nine_months_on,
        )
        .expect("no STALE gate any more");
        // ...and the log tripwire is what notices (Releases::age_days over STALE_WARN_DAYS).
        let rel = Releases {
            launcher: Some(launcher_latest()),
            game: game_latest(),
        };
        assert_eq!(rel.age_days(at("2026-10-12T00:00:00Z")), Some(11));
        assert!(rel.age_days(nine_months_on).unwrap() > STALE_WARN_DAYS);

        // CLOCK: two days ahead refused, an hour ahead fine.
        let e = accept_game(
            fixture!("s2_game_latest"),
            "latest",
            None,
            at("2026-09-29T23:00:00Z"),
        )
        .unwrap_err();
        assert!(e.starts_with(R_CLOCK), "{e}");
        accept_game(
            fixture!("s2_game_latest"),
            "latest",
            None,
            at("2026-10-01T23:00:00Z"),
        )
        .expect("an hour of skew is not a conspiracy");
    }

    /// REPLAY: older than the last accepted copy is refused, equal is fine, and the floor is read
    /// back from disk -- re-verified, per channel and per kind.
    #[test]
    fn a_manifest_older_than_the_last_accepted_one_is_a_replay() {
        let newer = fixture!("s2_game_latest");
        let older = fixture!("s2_game_latest_old");
        let floor = at("2026-10-02T00:00:00Z");
        let e = accept_game(older, "latest", Some(floor), now()).unwrap_err();
        assert!(e.starts_with(R_REPLAY), "{e}");
        accept_game(newer, "latest", Some(floor), now()).expect("equal is the same file again");
        accept_game(older, "latest", None, now()).expect("with no floor it is just a manifest");

        // The floor on disk: written when accepted, re-verified when read.
        let root = std::env::temp_dir().join("mh_launcher_test_replay_floor");
        let _ = std::fs::remove_dir_all(&root);
        let layout = Layout::rooted(&root);
        let key = test_key();
        assert_eq!(load_floor(&layout, "latest", KIND_GAME, &key), None);
        remember_accepted(&layout, "latest", KIND_GAME, newer.0, newer.1).unwrap();
        assert_eq!(load_floor(&layout, "latest", KIND_GAME, &key), Some(floor));
        assert_eq!(
            load_floor(&layout, "stable", KIND_GAME, &key),
            None,
            "the floor is per channel"
        );
        assert_eq!(
            load_floor(&layout, "latest", KIND_LAUNCHER, &key),
            None,
            "and per kind"
        );
        assert_eq!(
            load_floor(&layout, "latest", KIND_GAME, PUBLIC_KEY),
            None,
            "a copy the key does not verify is no floor"
        );
        // An edited copy is a file this launcher never accepted: ignored, not trusted as a floor.
        let edited = String::from_utf8(newer.0.to_vec())
            .unwrap()
            .replace("2026-10-02T00:00:00Z", "2030-01-01T00:00:00Z");
        std::fs::write(layout.accepted_manifest("latest", KIND_GAME), edited).unwrap();
        assert_eq!(load_floor(&layout, "latest", KIND_GAME, &key), None);
        let _ = std::fs::remove_dir_all(&root);
    }

    /// The relay survives the accepted-copy round trip, per channel, and only under the right key.
    #[test]
    fn the_accepted_game_manifest_round_trips_for_its_relay() {
        let root = std::env::temp_dir().join("mh_launcher_test_accepted_relay");
        let _ = std::fs::remove_dir_all(&root);
        let layout = Layout::rooted(&root);
        let key = test_key();
        assert!(load_accepted_with_key(&layout, "latest", &key).is_none());
        let (b, s) = fixture!("s2_game_latest");
        remember_accepted(&layout, "latest", KIND_GAME, b, s).unwrap();
        let m = load_accepted_with_key(&layout, "latest", &key).expect("the copy verifies");
        assert_eq!(m.relay.as_ref().map(|r| r.addr.as_str()), Some(RELAY_ADDR));
        assert!(
            load_accepted_with_key(&layout, "stable", &key).is_none(),
            "another channel's slot is empty"
        );
        assert!(
            load_accepted(&layout).is_none(),
            "the release key refuses it"
        );
        // A genuine manifest dropped into the wrong channel's slot fails the header gate on load.
        let (sb, ss) = fixture!("s2_game_stable");
        remember_accepted(&layout, "latest", KIND_GAME, sb, ss).unwrap();
        assert!(load_accepted_with_key(&layout, "latest", &key).is_none());
        let _ = std::fs::remove_dir_all(&root);
    }

    /// Malformed bodies: size is required and > 0, digests are 64 hex, versions are semver, the
    /// map is not empty, an unusable relay poisons the whole manifest.
    #[test]
    fn a_malformed_manifest_body_is_refused() {
        let game = String::from_utf8(fixture!("s2_game_latest").0.to_vec()).unwrap();
        let size = PLAY_ZIP_NET.len();
        let no_size = game.replacen(&format!("\"size\": {size}"), "\"size_\": 1", 1);
        assert!(parse_game(no_size.as_bytes())
            .unwrap_err()
            .starts_with(R_MALFORMED));
        let zero = game.replacen(&format!("\"size\": {size}"), "\"size\": 0", 1);
        let e = parse_game(zero.as_bytes()).unwrap_err();
        assert!(e.starts_with(R_MALFORMED) && e.contains("size"), "{e}");
        let short_sha = game.replacen(&fixture_sha(PLAY_ZIP_NET), "00ff", 1);
        assert!(parse_game(short_sha.as_bytes())
            .unwrap_err()
            .starts_with(R_MALFORMED));
        let bad_ver = game.replacen("\"version\": \"0.3.0\"", "\"version\": \"three\"", 1);
        assert!(parse_game(bad_ver.as_bytes())
            .unwrap_err()
            .starts_with(R_MALFORMED));
        let bad_min = game.replacen("\"min_launcher\": \"0.2.0\"", "\"min_launcher\": \"\"", 1);
        assert!(parse_game(bad_min.as_bytes())
            .unwrap_err()
            .starts_with(R_MALFORMED));
        let bad_time = game.replacen("2026-10-02T00:00:00Z", "yesterday", 1);
        assert!(parse_game(bad_time.as_bytes())
            .unwrap_err()
            .starts_with(R_MALFORMED));
        let bad_tag = game.replacen("\"net-debug\"", "\"../net\"", 1);
        assert!(parse_game(bad_tag.as_bytes())
            .unwrap_err()
            .starts_with(R_MALFORMED));
        let bad_relay = game.replace(RELAY_ADDR, "192.0.2.10:7100 ; comment");
        let e = parse_game(bad_relay.as_bytes()).unwrap_err();
        assert!(e.starts_with(R_MALFORMED) && e.contains("relay"), "{e}");
        let no_relay = game.replace(
            &format!(
                "\"relay\": {{\n    \"addr\": \"{RELAY_ADDR}\",\n    \"key\": \"{RELAY_KEY}\"\n  }},\n  "
            ),
            "",
        );
        assert!(!no_relay.contains("relay"), "the replace must have matched");
        assert!(parse_game(no_relay.as_bytes()).unwrap().relay.is_none());

        let launcher = String::from_utf8(fixture!("s2_launcher_latest").0.to_vec()).unwrap();
        let zero = launcher.replacen("\"size\": 1234", "\"size\": 0", 1);
        assert!(parse_launcher(zero.as_bytes())
            .unwrap_err()
            .starts_with(R_MALFORMED));
        assert!(parse_launcher(b"{\"schema\":2}")
            .unwrap_err()
            .starts_with(R_MALFORMED));
    }

    fn fixture_sha(bytes: &[u8]) -> String {
        sha256_bytes(bytes)
    }

    // ---- NOT NEWER at install, and the switch rule ---------------------------------------------

    /// dist RL8 section 2: the game is strictly-newer-only, EXCEPT while a channel switch is in
    /// progress (any version other than the installed one); an empty `installed_channel` (a bridge
    /// arrival) is not a switch; the launcher never downgrades, switch or no switch.
    #[test]
    fn not_newer_is_strict_except_during_a_channel_switch() {
        // strict
        assert!(check_game_install("0.3.0", "0.2.9", false).is_ok());
        for installed in ["0.3.0", "0.3.1", "1.0.0"] {
            let e = check_game_install("0.3.0", installed, false).unwrap_err();
            assert!(e.starts_with(R_NOT_NEWER), "{installed}: {e}");
        }
        // nothing installed: any parseable version is an install
        assert!(check_game_install("0.0.1", "", false).is_ok());
        // switching: older installs, newer installs, the very same version does not
        assert!(check_game_install("0.2.9", "0.3.0", true).is_ok());
        assert!(check_game_install("0.4.0", "0.3.0", true).is_ok());
        assert!(check_game_install("0.3.0-rc1", "0.3.0", true).is_ok());
        let e = check_game_install("0.3.0", "0.3.0", true).unwrap_err();
        assert!(e.starts_with(R_NOT_NEWER), "{e}");
        assert!(check_game_install("not semver", "0.3.0", true).is_err());

        // what counts as a switch: a KNOWN other channel, never an empty one
        for (installed_channel, channel, switching) in [
            ("latest", "stable", true),
            ("stable", "latest", true),
            ("stable", "stable", false),
            ("", "stable", false),
            ("", "latest", false),
            ("  ", "latest", false),
        ] {
            assert_eq!(
                is_switch(installed_channel, channel),
                switching,
                "{installed_channel:?} -> {channel:?}"
            );
            let env = Env {
                channel,
                installed_channel,
                mine: "0.2.0",
                key: "",
            };
            assert_eq!(env.switching(), switching);
        }
        // the bridge arrival wanting an older game: strict, so refused
        let env = Env::release("stable", "");
        assert!(check_game_install("0.2.9", "0.3.0", env.switching()).is_err());

        // the launcher never goes down, whatever channel is followed
        assert!(check_launcher_install("0.2.1", "0.2.0").is_ok());
        for mine in ["0.2.0", "0.2.1", "0.3.0"] {
            assert!(
                check_launcher_install("0.2.0", mine)
                    .unwrap_err()
                    .starts_with(R_NOT_NEWER),
                "{mine}"
            );
        }
        assert!(check_launcher_install("0.2.0", "0.2.0-rc1").is_ok());
    }

    // ---- the plan (dist RL8 section 3) -----------------------------------------------------------

    fn plan_of(mine: &str, installed: &str, installed_channel: &str, channel: &str) -> Plan {
        let (launcher, game) = if channel == "stable" {
            (launcher_stable(), game_stable())
        } else {
            (launcher_latest(), game_latest())
        };
        plan(
            Some(&launcher),
            &game,
            mine,
            installed,
            installed_channel,
            channel,
        )
    }

    /// done_when: a game-only release does not touch the launcher manifest -- the launcher is
    /// already at the channel's version, so the plan has a game step and no launcher step.
    #[test]
    fn a_game_only_release_plans_no_launcher_action() {
        let p = plan_of("0.2.0", "0.2.5", "latest", "latest");
        assert_eq!(
            p,
            Plan {
                launcher: None,
                game: Some("0.3.0".into()),
                deferred: false,
                blocked: None
            }
        );
        // ...and a launcher AHEAD of the channel's (a dev build, a newer promote) is not touched.
        assert_eq!(plan_of("0.9.0", "0.2.5", "latest", "latest").launcher, None);
        // a current game, a current launcher: nothing at all
        assert_eq!(
            plan_of("0.2.0", "0.3.0", "latest", "latest"),
            Plan::default()
        );
        // an installed game NEWER than the channel's, no switch: a rollback, so nothing
        assert_eq!(plan_of("0.2.0", "0.4.0", "latest", "latest").game, None);
        // not installed at all: an install at any version
        assert_eq!(
            plan_of("0.2.0", "", "", "latest").game.as_deref(),
            Some("0.3.0")
        );
    }

    #[test]
    fn a_newer_launcher_goes_first_and_the_game_follows() {
        let p = plan_of("0.1.9", "0.2.9", "stable", "stable");
        // stable: launcher 0.1.5 is OLDER than 0.1.9 -> no launcher step; the game 0.2.9 is current
        assert_eq!(p, Plan::default());
        let p = plan_of("0.1.0", "0.2.0", "stable", "stable");
        assert_eq!(p.launcher.as_deref(), Some("0.1.5"), "launcher first");
        assert_eq!(p.game.as_deref(), Some("0.2.9"));
        assert!(!p.deferred, "stable's min_launcher 0.1.0 is already met");
        assert!(p.blocked.is_none());
    }

    #[test]
    fn a_launcher_below_min_launcher_defers_or_blocks_the_game() {
        // latest: game 0.3.0 needs launcher 0.2.0; the channel's launcher IS 0.2.0 -> deferred
        let p = plan_of("0.1.0", "0.2.5", "latest", "latest");
        assert_eq!(p.launcher.as_deref(), Some("0.2.0"));
        assert_eq!(p.game.as_deref(), Some("0.3.0"));
        assert!(p.deferred && p.blocked.is_none(), "{p:?}");
        // the channel's launcher does NOT reach min_launcher -> blocked, game kept, launcher still
        // offered (it is newer than mine)
        let p = plan(
            Some(&launcher_stable()),
            &game_latest(),
            "0.1.0",
            "0.2.5",
            "latest",
            "latest",
        );
        assert_eq!(p.game, None);
        assert!(!p.deferred);
        assert_eq!(p.launcher.as_deref(), Some("0.1.5"));
        let b = p.blocked.expect("blocked");
        assert!(b.starts_with(R_LAUNCHER_OLD), "{b}");
        assert!(b.contains("0.2.0") && b.contains("0.1.0"), "{b}");
        // ...but only when a game install is actually wanted: a current game blocks nothing
        let p = plan(
            Some(&launcher_stable()),
            &game_latest(),
            "0.1.0",
            "0.3.0",
            "latest",
            "latest",
        );
        assert!(p.blocked.is_none() && p.game.is_none());
        // and an unreadable own version is not a reason to refuse
        let p = plan(
            Some(&launcher_latest()),
            &game_latest(),
            "garbage",
            "0.2.5",
            "latest",
            "latest",
        );
        assert!(p.blocked.is_none() && !p.deferred);
    }

    /// done_when: switching channel DOWN installs the older game. The plan offers it only while
    /// the switch is in progress.
    #[test]
    fn a_channel_switch_down_plans_the_older_game() {
        // installed 0.3.0 from latest; now following stable (0.2.9)
        let p = plan_of("0.2.0", "0.3.0", "latest", "stable");
        assert_eq!(p.game.as_deref(), Some("0.2.9"));
        assert!(p.blocked.is_none() && !p.deferred);
        // the same state WITHOUT a switch (bridge arrival: installed_channel empty): a rollback
        assert_eq!(plan_of("0.2.0", "0.3.0", "", "stable").game, None);
        // and once the switch is over (installed from stable), still nothing
        assert_eq!(plan_of("0.2.0", "0.3.0", "stable", "stable").game, None);
        // switching to a channel whose game is the version already installed: nothing to do
        assert_eq!(plan_of("0.2.0", "0.2.9", "latest", "stable").game, None);
    }

    #[test]
    fn the_offer_names_what_the_plan_would_do_and_says_nothing_when_current() {
        let rel = Releases {
            launcher: Some(launcher_latest()),
            game: game_latest(),
        };
        let env = |mine, installed_channel| Env {
            channel: "latest",
            installed_channel,
            mine,
            key: "",
        };
        // nothing installed, an older launcher: both halves are offered
        let o = offer_for(&rel, "", &env("0.1.0", ""));
        assert_eq!(o.game.as_deref(), Some("0.3.0"));
        assert_eq!(o.launcher.as_deref(), Some("0.2.0"));
        assert_eq!(
            o.line().as_deref(),
            Some("0.3.0 is available (with launcher 0.2.0)")
        );
        // the game is current, the launcher is behind: only the launcher
        let o = offer_for(&rel, "0.3.0", &env("0.1.9", ""));
        assert_eq!(o.game, None);
        assert_eq!(o.line().as_deref(), Some("launcher 0.2.0 is available"));
        // an older game behind a current launcher: only the game
        let o = offer_for(&rel, "0.2.5", &env("0.2.0", ""));
        assert_eq!(o.line().as_deref(), Some("0.3.0 is available"));
        // both current -- or NEWER than the channel (a rollback is never offered): nothing
        let o = offer_for(&rel, "0.3.0", &env("0.2.0", ""));
        assert_eq!(o, Offer::default());
        assert!(!o.any());
        assert_eq!(o.line(), None);
        assert!(!offer_for(&rel, "0.9.0", &env("0.9.0", "")).any());
        // a launcher too old for a game its channel's launcher cannot fix: the offer says so
        let stuck = Releases {
            launcher: Some(launcher_stable()),
            game: game_latest(),
        };
        let o = offer_for(&stuck, "0.2.5", &env("0.1.0", "latest"));
        assert!(o.blocked.is_some());
        assert_eq!(o.game, None);
        assert_eq!(o.launcher.as_deref(), Some("0.1.5"));
    }

    // ---- the stand-in server -------------------------------------------------------------------

    /// A `Fetch` that answers from a table, so the whole check path can be tested with no socket.
    struct Canned(Vec<(String, Vec<u8>)>);
    impl Fetch for Canned {
        fn get(&self, url: &str, _limit: u64) -> Result<Vec<u8>, String> {
            self.0
                .iter()
                .find(|(u, _)| u == url)
                .map(|(_, b)| b.clone())
                .ok_or_else(|| format!("{R_FETCH}: nothing canned for {url}"))
        }
    }

    fn put(rows: &mut Vec<(String, Vec<u8>)>, channel: &str, kind: &str, fx: (&[u8], &str)) {
        let url = manifest_url(TEST_BASE, channel, kind);
        rows.push((format!("{url}.minisig"), fx.1.as_bytes().to_vec()));
        rows.push((url, fx.0.to_vec()));
    }

    /// Both channels' manifests plus every zip they point at (0.2.9 is the net zip's bytes under
    /// another name: the stable fixture carries that digest).
    fn server() -> Canned {
        let mut rows = Vec::new();
        put(
            &mut rows,
            "latest",
            KIND_LAUNCHER,
            fixture!("s2_launcher_latest"),
        );
        put(&mut rows, "latest", KIND_GAME, fixture!("s2_game_latest"));
        put(
            &mut rows,
            "stable",
            KIND_LAUNCHER,
            fixture!("s2_launcher_stable"),
        );
        put(&mut rows, "stable", KIND_GAME, fixture!("s2_game_stable"));
        for (name, bytes) in [
            ("mission_humanity_re-0.3.0-net.zip", PLAY_ZIP_NET),
            ("mission_humanity_re-0.2.9-net.zip", PLAY_ZIP_NET),
        ] {
            rows.push((format!("{TEST_BASE}{name}"), bytes.to_vec()));
        }
        Canned(rows)
    }

    /// The test environment: the test key, a launcher version that satisfies both channels'
    /// `min_launcher`, and no switch unless `installed_channel` says otherwise.
    fn env<'a>(key: &'a str, channel: &'a str, installed_channel: &'a str) -> Env<'a> {
        Env {
            channel,
            installed_channel,
            mine: "0.2.0",
            key,
        }
    }

    #[test]
    fn check_fetches_both_manifests_and_runs_every_gate() {
        let key = test_key();
        let base = "https://example.invalid/mh";
        let canned = Canned(vec![
            (
                "https://example.invalid/mh/channels/latest/launcher.json".into(),
                fixture!("s2_launcher_latest").0.to_vec(),
            ),
            (
                "https://example.invalid/mh/channels/latest/launcher.json.minisig".into(),
                fixture!("s2_launcher_latest").1.as_bytes().to_vec(),
            ),
        ]);
        let e = check(&canned, base, &env(&key, "latest", ""), "", None).unwrap_err();
        assert!(
            e.starts_with(R_URL),
            "the stand-in's http URLs are off a https origin: {e}"
        );
        // a missing signature is a fetch failure, not a silent pass
        let only_manifest = Canned(vec![(
            "https://example.invalid/mh/channels/latest/launcher.json".into(),
            fixture!("s2_launcher_latest").0.to_vec(),
        )]);
        let e = check(&only_manifest, base, &env(&key, "latest", ""), "", None).unwrap_err();
        assert!(e.starts_with(R_FETCH), "{e}");
        // no update source at all
        let e = check(&server(), "  ", &env(&key, "latest", ""), "", None).unwrap_err();
        assert!(e.starts_with("NO UPDATE SOURCE"), "{e}");
        // the real thing, over the stand-in origin: the channel's own two files, both kinds
        let rel = check(&server(), TEST_BASE, &env(&key, "latest", ""), "", None).unwrap();
        assert_eq!(
            (
                rel.game.version.as_str(),
                rel.launcher.as_ref().unwrap().version.as_str()
            ),
            ("0.3.0", "0.2.0")
        );
        // the channel decides which files are read: stable's are a different pair
        let rel = check(&server(), TEST_BASE, &env(&key, "stable", ""), "", None).unwrap();
        assert_eq!(
            (
                rel.game.version.as_str(),
                rel.launcher.as_ref().unwrap().version.as_str()
            ),
            ("0.2.9", "0.1.5")
        );
        // the release key trusts neither
        let e = check(&server(), TEST_BASE, &Env::release("latest", ""), "", None).unwrap_err();
        assert!(e.starts_with(R_SIGNATURE), "{e}");
    }

    /// `server()` with chosen URLs answering a chosen error instead of their bytes.
    struct Faulty(Canned, Vec<(String, String)>);
    impl Fetch for Faulty {
        fn get(&self, url: &str, limit: u64) -> Result<Vec<u8>, String> {
            match self.1.iter().find(|(u, _)| u == url) {
                Some((_, e)) => Err(e.clone()),
                None => self.0.get(url, limit),
            }
        }
    }

    fn err404(url: &str) -> String {
        format!("{R_FETCH}: {url} -- {NOT_FOUND_MARK} (not found)")
    }

    /// A channel's launcher.json is OPTIONAL: only an HTTP 404 on it means "no launcher offered".
    #[test]
    fn a_missing_launcher_manifest_is_an_absent_launcher_and_nothing_else_is() {
        let key = test_key();
        let url = manifest_url(TEST_BASE, "latest", KIND_LAUNCHER);
        let sig_url = format!("{url}.minisig");
        let faulty = |rows: Vec<(String, String)>| Faulty(server(), rows);
        let run = |rows: Vec<(String, String)>, layout: Option<&Layout>| {
            check(
                &faulty(rows),
                TEST_BASE,
                &env(&key, "latest", ""),
                "",
                layout,
            )
        };
        // launcher.json 404: Ok, no launcher, the game accepted
        let rel = run(vec![(url.clone(), err404(&url))], None).unwrap();
        assert!(rel.launcher.is_none());
        assert_eq!(rel.game.version, "0.3.0");
        // both missing is the same thing
        let rel = run(
            vec![
                (url.clone(), err404(&url)),
                (sig_url.clone(), err404(&sig_url)),
            ],
            None,
        )
        .unwrap();
        assert!(rel.launcher.is_none());
        // the age tripwire still works off the game alone
        assert!(rel.age_days(Utc::now()).is_some());
        // launcher.json present, .minisig 404: a SIGNATURE refusal, not "absent"
        let e = run(vec![(sig_url.clone(), err404(&sig_url))], None).unwrap_err();
        assert!(e.starts_with(R_SIGNATURE), "{e}");
        // a 500 on launcher.json: an error
        let e500 = format!("{R_FETCH}: {url} -- http status: 500");
        let e = run(vec![(url.clone(), e500)], None).unwrap_err();
        assert!(e.starts_with(R_FETCH) && e.contains("500"), "{e}");
        // so is a transport failure, and a 500 on the .minisig
        let e = run(
            vec![(url.clone(), format!("{R_FETCH}: {url} -- timed out"))],
            None,
        )
        .unwrap_err();
        assert!(!is_not_found(&e), "{e}");
        let e = run(
            vec![(sig_url.clone(), format!("{R_FETCH}: {sig_url} -- 500"))],
            None,
        )
        .unwrap_err();
        assert!(e.starts_with(R_FETCH), "{e}");
        // a bad signature on a present launcher.json stays a refusal
        let e = check(&server(), TEST_BASE, &Env::release("latest", ""), "", None).unwrap_err();
        assert!(e.starts_with(R_SIGNATURE), "{e}");
        // game.json stays mandatory
        let gurl = manifest_url(TEST_BASE, "latest", KIND_GAME);
        let e = run(vec![(gurl.clone(), err404(&gurl))], None).unwrap_err();
        assert!(e.starts_with(R_FETCH), "{e}");
        // nothing is remembered for the absent kind
        let root = std::env::temp_dir().join("mh_launcher_test_no_launcher_remember");
        let _ = std::fs::remove_dir_all(&root);
        let layout = Layout::rooted(&root);
        run(vec![(url.clone(), err404(&url))], Some(&layout)).unwrap();
        assert!(layout.accepted_manifest("latest", KIND_GAME).exists());
        assert!(!layout.accepted_manifest("latest", KIND_LAUNCHER).exists());
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn plan_without_a_launcher_manifest_plans_the_game_only_and_blocks_by_hand() {
        let g = game_latest(); // 0.3.0, min_launcher 0.2.0
        let p = plan(None, &g, "0.2.0", "0.2.5", "latest", "latest");
        assert_eq!(
            p,
            Plan {
                launcher: None,
                game: Some("0.3.0".into()),
                deferred: false,
                blocked: None
            }
        );
        // mine below min_launcher: blocked, with the hand-download message, never deferred
        let p = plan(None, &g, "0.1.0", "0.2.5", "latest", "latest");
        assert!(p.launcher.is_none() && p.game.is_none() && !p.deferred);
        let b = p.blocked.expect("blocked");
        assert!(b.starts_with(R_LAUNCHER_OLD), "{b}");
        assert!(
            b.contains("offers no launcher") && b.contains("by hand"),
            "{b}"
        );
        // the offer says so too
        let rel = Releases {
            launcher: None,
            game: g,
        };
        let key = test_key();
        let o = offer_for(&rel, "0.2.5", &env(&key, "latest", "latest"));
        assert!(o.no_launcher && o.launcher.is_none());
        assert_eq!(o.game.as_deref(), Some("0.3.0"));
    }

    /// `--check-update`'s strict gate, with the switch exception; and the replay floor written by
    /// `check` is what the next `check` reads.
    #[test]
    fn check_applies_the_install_gate_and_remembers_what_it_accepted() {
        let root = std::env::temp_dir().join("mh_launcher_test_check_gate");
        let _ = std::fs::remove_dir_all(&root);
        let layout = Layout::rooted(&root);
        let key = test_key();
        let srv = server();
        let e = check(
            &srv,
            TEST_BASE,
            &env(&key, "latest", ""),
            "0.3.0",
            Some(&layout),
        )
        .unwrap_err();
        assert!(e.starts_with(R_NOT_NEWER), "{e}");
        // ...even though the manifests themselves were accepted and kept:
        assert_eq!(
            load_floor(&layout, "latest", KIND_GAME, &key),
            Some(at("2026-10-02T00:00:00Z"))
        );
        assert_eq!(
            load_floor(&layout, "latest", KIND_LAUNCHER, &key),
            Some(at("2026-10-01T00:00:00Z"))
        );
        check(
            &srv,
            TEST_BASE,
            &env(&key, "latest", ""),
            "0.2.5",
            Some(&layout),
        )
        .unwrap();
        // stable (0.2.9) against an installed 0.3.0: a rollback unless the channel was switched
        let e = check(
            &srv,
            TEST_BASE,
            &env(&key, "stable", ""),
            "0.3.0",
            Some(&layout),
        )
        .unwrap_err();
        assert!(e.starts_with(R_NOT_NEWER), "{e}");
        check(
            &srv,
            TEST_BASE,
            &env(&key, "stable", "latest"),
            "0.3.0",
            Some(&layout),
        )
        .unwrap();

        // a server replaying an OLDER genuine latest game manifest is refused by the floor
        let mut rows = Vec::new();
        put(
            &mut rows,
            "latest",
            KIND_LAUNCHER,
            fixture!("s2_launcher_latest"),
        );
        put(
            &mut rows,
            "latest",
            KIND_GAME,
            fixture!("s2_game_latest_old"),
        );
        let e = check(
            &Canned(rows),
            TEST_BASE,
            &env(&key, "latest", ""),
            "",
            Some(&layout),
        )
        .unwrap_err();
        assert!(e.starts_with(R_REPLAY), "{e}");
        let _ = std::fs::remove_dir_all(&root);
    }

    // ---- installing: Play, the configuration switch, the channel switch ----------------------------

    /// A scratch game directory: a stock `mh.exe` and the game's OWN `mh.dll`, nothing installed.
    fn play_game_dir(root: &Path) -> PathBuf {
        let game = root.join("game");
        std::fs::create_dir_all(&game).unwrap();
        std::fs::write(game.join(GAME_EXE), b"exe").unwrap();
        std::fs::write(game.join("mh.dll"), b"the game's own dll").unwrap();
        game
    }

    fn read(p: PathBuf) -> String {
        String::from_utf8_lossy(&std::fs::read(p).unwrap_or_default()).to_string()
    }

    fn scratch(name: &str) -> PathBuf {
        let root = std::env::temp_dir().join(name);
        let _ = std::fs::remove_dir_all(&root);
        root
    }

    /// dist LA8 done_when (1), over schema 2 and RL4/RL7: *on a game directory with nothing installed,
    /// Play ends with the `net` zip's files installed (receipt names the tag), NO ini or key beside
    /// the exe, and the relay provisioned into the CONFIG directory*.
    #[test]
    fn play_on_an_empty_directory_installs_net_and_provisions_the_relay_in_the_config_dir() {
        let root = scratch("mh_launcher_test_play_install");
        let layout = Layout::rooted(root.join("state"));
        let game = play_game_dir(&root);
        let server = server();
        let key = test_key();
        let steps = std::sync::Mutex::new(Vec::<String>::new());
        let progress = |m: &str| steps.lock().unwrap().push(m.to_string());

        assert_eq!(readiness(&game, "net"), Readiness::NeedsInstall);
        let applied = make_ready(
            &layout,
            &server,
            TEST_BASE,
            &game,
            "net",
            &env(&key, "latest", ""),
            &progress,
        )
        .expect("the install path succeeds")
        .expect("something was installed");
        assert_eq!(
            (
                applied.version.as_str(),
                applied.tag.as_str(),
                applied.channel.as_str()
            ),
            ("0.3.0", "net", "latest")
        );

        let receipt = install::read_manifest(&game).expect("a receipt was written");
        assert_eq!(receipt.tag, "net");
        assert_eq!(receipt.version, "0.3.0");
        assert_eq!(read(game.join("mh.dll")), "fixture mh.dll (net)");
        assert_eq!(
            read(game.join(format!("mh.dll{}", crate::paths::BACKUP_SUFFIX))),
            "the game's own dll",
            "the game's dll is parked, not lost"
        );
        // RL4: install writes nothing the player owns beside the exe, even though the fixture zip
        // still carries an ini.
        assert!(!game.join(crate::relay::INI_NAME).exists());
        assert!(!game.join(crate::relay::KEY_NAME).exists());
        assert!(!receipt
            .files
            .iter()
            .any(|(_, _, f)| f == crate::relay::INI_NAME));
        // The accepted copy is there for the launch path to re-read the relay from ...
        let accepted = load_accepted_with_key(&layout, "latest", &key).expect("accepted copy");
        // ... and the relay goes into the config directory (user storage), per `relay_mode`.
        let plan = crate::relay::plan_for("auto", "", accepted.relay.as_ref());
        let done = crate::relay::provision_for_game(&layout, &game, &plan).unwrap();
        assert_ne!(done, crate::relay::Provisioned::NONE);
        let cfg = layout
            .root
            .join("games")
            .join(crate::paths::game_dir_hash(&game));
        let ini = read(cfg.join(crate::relay::INI_NAME));
        assert!(ini.contains("transport=udp"), "{ini}");
        assert!(ini.contains(&format!("relay={RELAY_ADDR}")), "{ini}");
        assert!(read(cfg.join(crate::relay::KEY_NAME)).starts_with(RELAY_KEY));
        assert!(
            !game.join(crate::relay::INI_NAME).exists(),
            "still nothing beside the exe"
        );
        let steps = steps.lock().unwrap().clone();
        assert!(steps.iter().any(|s| s.starts_with("fetching")), "{steps:?}");
        assert!(
            steps.iter().any(|s| s.starts_with("downloading")),
            "{steps:?}"
        );
        assert!(
            steps.iter().any(|s| s.starts_with("installing")),
            "{steps:?}"
        );
        // A second Play is a plain launch: the receipt says it is there.
        assert_eq!(readiness(&game, "net"), Readiness::Ready);
        assert!(make_ready(
            &layout,
            &server,
            TEST_BASE,
            &game,
            "net",
            &env(&key, "latest", ""),
            &no_progress
        )
        .unwrap()
        .is_none());
        let _ = std::fs::remove_dir_all(&root);
    }

    /// dist RL7: a folder installed by an older launcher as `net-debug` (receipt tag, a
    /// `mh_harness.dll` row) is SWITCHED to `net` by Play -- the debug-only file leaves with the old
    /// set, retail's dll is still the one parked as `.mhbak`. Built by hand, so the test needs no
    /// retired zip.
    #[test]
    fn a_legacy_net_debug_install_is_switched_to_net() {
        let root = scratch("mh_launcher_test_play_switch");
        let layout = Layout::rooted(root.join("state"));
        let game = play_game_dir(&root);
        let server = server();
        let key = test_key();
        let e = env(&key, "latest", "");

        // the old install: our debug dll over retail's (parked), a harness dll, the receipt
        std::fs::rename(game.join("mh.dll"), game.join("mh.dll.mhbak")).unwrap();
        std::fs::write(game.join("mh.dll"), b"old debug dll").unwrap();
        std::fs::write(game.join("mh_harness.dll"), b"old harness").unwrap();
        let rows = format!(
            "replaced\t{}\tmh.dll\ncreated\t{}\tmh_harness.dll\n",
            sha256_bytes(b"old debug dll"),
            sha256_bytes(b"old harness")
        );
        std::fs::write(
            game.join(crate::paths::INSTALL_MANIFEST),
            format!(
                "version\t0.2.0-rc7.1\ntag\tnet-debug\npackage\tp.zip\ninstalled_at\tx\n{rows}"
            ),
        )
        .unwrap();

        assert_eq!(
            readiness(&game, "net"),
            Readiness::NeedsSwitch {
                from: "net-debug".into()
            }
        );
        let applied = make_ready(&layout, &server, TEST_BASE, &game, "net", &e, &no_progress)
            .unwrap()
            .expect("a switch installs");
        assert_eq!(applied.tag, "net");
        let receipt = install::read_manifest(&game).unwrap();
        assert_eq!(receipt.tag, "net");
        assert!(
            !game.join("mh_harness.dll").exists(),
            "the debug-only file left with the debug set"
        );
        assert!(!receipt.files.iter().any(|(_, _, f)| f == "mh_harness.dll"));
        assert_eq!(read(game.join("mh.dll")), "fixture mh.dll (net)");
        assert_eq!(
            read(game.join(format!("mh.dll{}", crate::paths::BACKUP_SUFFIX))),
            "the game's own dll",
            "the backup is the GAME's dll, not our previous one"
        );
        assert_eq!(readiness(&game, "net"), Readiness::Ready);
        let un = install::uninstall(&game).unwrap();
        assert!(un.restored.contains(&"mh.dll".to_string()), "{un:?}");
        assert_eq!(read(game.join("mh.dll")), "the game's own dll");
        let _ = std::fs::remove_dir_all(&root);
    }

    /// dist RL8 section 1: a chosen tag the channel does not offer falls back to `net` (and
    /// installs it); a manifest that offers neither installs -- and uninstalls -- nothing.
    #[test]
    fn a_tag_the_channel_does_not_offer_falls_back_to_net_and_no_net_touches_nothing() {
        let root = scratch("mh_launcher_test_play_fallback");
        let layout = Layout::rooted(root.join("state"));
        let game = play_game_dir(&root);
        let server = server();
        let key = test_key();
        // stable offers `net` only; the pick was net-debug
        let stable = game_stable();
        assert_eq!(resolve_tag(&stable, "net-debug").unwrap(), "net");
        assert_eq!(resolve_tag(&stable, "net").unwrap(), "net");
        assert_eq!(
            resolve_tag(&game_latest(), "net-debug").unwrap(),
            "net-debug"
        );
        let applied = make_ready(
            &layout,
            &server,
            TEST_BASE,
            &game,
            "net-debug",
            &env(&key, "stable", ""),
            &no_progress,
        )
        .unwrap()
        .expect("net was installed in its place");
        assert_eq!(
            (applied.tag.as_str(), applied.version.as_str()),
            ("net", "0.2.9")
        );
        assert_eq!(install::read_manifest(&game).unwrap().tag, "net");
        // the pick is still unserved, but net is installed: nothing further to do
        let none = make_ready(
            &layout,
            &server,
            TEST_BASE,
            &game,
            "net-debug",
            &env(&key, "stable", "stable"),
            &no_progress,
        )
        .unwrap();
        assert!(
            none.is_none(),
            "net is what the fallback resolves to, and it is there"
        );

        // a manifest with neither the pick nor `net`: MALFORMED, and nothing is touched
        let mut odd = game_stable();
        odd.game.clear();
        let asset = odd_asset();
        odd.game.insert("brokered-debug".into(), asset);
        assert!(resolve_tag(&odd, "net-debug")
            .unwrap_err()
            .starts_with(R_MALFORMED));
        let releases = Releases {
            launcher: Some(launcher_stable()),
            game: odd,
        };
        let before = read(game.join("mh.dll"));
        let e = apply_if_needed(
            &layout,
            &server,
            &releases,
            "net-debug",
            &game,
            TEST_BASE,
            &env(&key, "stable", "stable"),
            &no_progress,
        )
        .unwrap_err();
        assert!(e.starts_with(R_MALFORMED), "{e}");
        assert_eq!(
            install::read_manifest(&game).unwrap().tag,
            "net",
            "still installed"
        );
        assert_eq!(read(game.join("mh.dll")), before);
        let _ = std::fs::remove_dir_all(&root);
    }

    fn odd_asset() -> Asset {
        Asset {
            url: format!("{TEST_BASE}mission_humanity_re-0.2.9-net.zip"),
            sha256: sha256_bytes(PLAY_ZIP_NET),
            size: PLAY_ZIP_NET.len() as u64,
        }
    }

    /// The game half of Update / what `--update` does after the LA11 restart: install when the
    /// configuration is missing, update when the channel is newer, and touch NOTHING -- `None`,
    /// no error -- when the receipt is current or newer (the rollback refusal, kept).
    #[test]
    fn apply_if_needed_installs_updates_or_leaves_a_current_game_alone() {
        let root = scratch("mh_launcher_test_apply_if_needed");
        let layout = Layout::rooted(root.join("state"));
        let game = play_game_dir(&root);
        let server = server();
        let key = test_key();
        let e = env(&key, "latest", "");
        let releases = check(&server, TEST_BASE, &e, "", Some(&layout)).unwrap();
        assert_eq!(releases.game.version, "0.3.0");
        let go = || {
            apply_if_needed(
                &layout,
                &server,
                &releases,
                "net",
                &game,
                TEST_BASE,
                &e,
                &no_progress,
            )
        };

        let done = go().unwrap().expect("nothing was installed, so 0.3.0 is");
        assert_eq!(
            (done.version.as_str(), done.channel.as_str()),
            ("0.3.0", "latest")
        );
        assert_eq!(install::read_manifest(&game).unwrap().version, "0.3.0");
        assert!(go().unwrap().is_none(), "0.3.0 over 0.3.0 is nothing to do");

        let receipt = game.join(crate::paths::INSTALL_MANIFEST);
        let set = |from: &str, to: &str| {
            let text = std::fs::read_to_string(&receipt)
                .unwrap()
                .replace(&format!("version\t{from}"), &format!("version\t{to}"));
            std::fs::write(&receipt, text).unwrap();
        };
        set("0.3.0", "0.2.9");
        assert_eq!(go().unwrap().expect("0.3.0 over 0.2.9").version, "0.3.0");
        set("0.3.0", "0.9.0");
        assert!(
            go().unwrap().is_none(),
            "a newer receipt is a rollback: refused silently"
        );
        assert_eq!(install::read_manifest(&game).unwrap().version, "0.9.0");
        let _ = std::fs::remove_dir_all(&root);
    }

    /// done_when: switching channel DOWN installs the older game -- and ONLY when the switch is
    /// in progress, and the install ends the switch. The same offer without a switch is a rollback.
    #[test]
    fn switching_channel_down_installs_the_older_game_and_ends_the_switch() {
        let root = scratch("mh_launcher_test_channel_switch");
        let layout = Layout::rooted(root.join("state"));
        let game = play_game_dir(&root);
        let server = server();
        let key = test_key();

        // on latest: 0.3.0
        let on_latest = env(&key, "latest", "");
        let latest = check(&server, TEST_BASE, &on_latest, "", Some(&layout)).unwrap();
        let a = apply_if_needed(
            &layout,
            &server,
            &latest,
            "net",
            &game,
            TEST_BASE,
            &on_latest,
            &no_progress,
        )
        .unwrap()
        .unwrap();
        assert_eq!(
            (a.version.as_str(), a.channel.as_str()),
            ("0.3.0", "latest")
        );

        // the player picks stable. WITHOUT knowing the game came from latest it is a rollback...
        let blind = env(&key, "stable", "");
        let stable = check(&server, TEST_BASE, &blind, "", Some(&layout)).unwrap();
        assert_eq!(stable.game.version, "0.2.9");
        assert!(apply_if_needed(
            &layout,
            &server,
            &stable,
            "net",
            &game,
            TEST_BASE,
            &blind,
            &no_progress
        )
        .unwrap()
        .is_none());
        assert_eq!(install::read_manifest(&game).unwrap().version, "0.3.0");

        // ...but installed_channel = latest makes it the explicit switch it is.
        let switching = env(&key, "stable", "latest");
        assert!(switching.switching());
        let offer = offer_for(&stable, "0.3.0", &switching);
        assert_eq!(
            offer.game.as_deref(),
            Some("0.2.9"),
            "the older game is the offer"
        );
        let a = apply_if_needed(
            &layout,
            &server,
            &stable,
            "net",
            &game,
            TEST_BASE,
            &switching,
            &no_progress,
        )
        .unwrap()
        .expect("the older game installs");
        assert_eq!(
            (a.version.as_str(), a.channel.as_str()),
            ("0.2.9", "stable")
        );
        assert_eq!(install::read_manifest(&game).unwrap().version, "0.2.9");

        // the caller records a.channel; the switch is over and the strict rule is back
        let settled = env(&key, "stable", &a.channel);
        assert!(!settled.switching());
        assert!(apply_if_needed(
            &layout,
            &server,
            &stable,
            "net",
            &game,
            TEST_BASE,
            &settled,
            &no_progress
        )
        .unwrap()
        .is_none());
        // switching back UP installs the newer game as well
        let up = env(&key, "latest", "stable");
        let a = apply_if_needed(
            &layout,
            &server,
            &latest,
            "net",
            &game,
            TEST_BASE,
            &up,
            &no_progress,
        )
        .unwrap()
        .expect("0.3.0 over 0.2.9");
        assert_eq!(a.version, "0.3.0");
        let _ = std::fs::remove_dir_all(&root);
    }

    /// A launcher below the game's min_launcher installs nothing -- not through the Update path,
    /// not through Play -- and says why. The same game installs once the launcher is current.
    #[test]
    fn a_launcher_below_min_launcher_installs_no_game() {
        let root = scratch("mh_launcher_test_min_launcher");
        let layout = Layout::rooted(root.join("state"));
        let game = play_game_dir(&root);
        let server = server();
        let key = test_key();
        let old = Env {
            mine: "0.1.0",
            ..env(&key, "latest", "")
        };
        let e = make_ready(
            &layout,
            &server,
            TEST_BASE,
            &game,
            "net",
            &old,
            &no_progress,
        )
        .unwrap_err();
        assert!(
            e.starts_with(R_LAUNCHER_OLD),
            "deferred to the launcher update: {e}"
        );
        assert!(install::read_manifest(&game).is_none(), "nothing installed");
        assert_eq!(read(game.join("mh.dll")), "the game's own dll");

        let releases = check(&server, TEST_BASE, &old, "", Some(&layout)).unwrap();
        let e = apply_if_needed(
            &layout,
            &server,
            &releases,
            "net",
            &game,
            TEST_BASE,
            &old,
            &no_progress,
        )
        .unwrap_err();
        assert!(e.starts_with(R_LAUNCHER_OLD), "{e}");
        assert!(install::read_manifest(&game).is_none());

        // blocked outright: the channel's launcher does not reach min_launcher either
        let stuck = Releases {
            launcher: Some(launcher_stable()),
            game: game_latest(),
        };
        let e = apply_if_needed(
            &layout,
            &server,
            &stuck,
            "net",
            &game,
            TEST_BASE,
            &old,
            &no_progress,
        )
        .unwrap_err();
        assert!(e.starts_with(R_LAUNCHER_OLD) && e.contains("0.2.0"), "{e}");

        // the current launcher installs it
        let ok = env(&key, "latest", "");
        assert!(apply_if_needed(
            &layout,
            &server,
            &releases,
            "net",
            &game,
            TEST_BASE,
            &ok,
            &no_progress
        )
        .unwrap()
        .is_some());
        let _ = std::fs::remove_dir_all(&root);
    }

    // ---- unchanged by RL8: semver ordering, URL rules, last-known-good, self-update leftovers --------

    /// Prerelease ordering, which is the reason this is semver and not a string compare.
    #[test]
    fn a_prerelease_sorts_below_its_release() {
        assert!(check_newer("0.2.0", "0.2.0-rc1").is_ok());
        assert!(check_newer("0.2.0-rc1", "0.2.0").is_err());
        assert!(check_newer("0.10.0", "0.9.0").is_ok());
        assert!(check_newer("0.9.0", "0.10.0").is_err());
        // Nothing installed: anything parseable is an upgrade, nothing unparseable is a version.
        assert!(check_newer("0.0.1", "").is_ok());
        assert!(check_newer("not a version", "").is_err());
    }

    /// dist LA2's clause: *zero requests to api.github.com*. The host check, stated as a test so a
    /// future edit that widened it would have to delete this.
    #[test]
    fn no_url_may_reach_the_github_api() {
        for bad in [
            "https://api.github.com/repos/x/y/releases/latest",
            "https://API.GitHub.COM/repos/x/y/releases",
            "http://api.github.com/x",
            "https://api.github.com:443/x",
        ] {
            let e = check_url(bad, "https://example.invalid/").unwrap_err();
            assert!(e.starts_with(R_URL), "{bad}: {e}");
            assert!(e.contains("api.github.com"), "{bad}: {e}");
        }
        // The release asset CDN is a different host and is fine.
        assert!(check_url(
            "https://github.com/o/r/releases/download/v1/x.zip",
            "https://example.invalid/"
        )
        .is_ok());
        // ...including through the objects host the CDN redirects to.
        assert!(check_url(
            "https://objects.githubusercontent.com/x",
            "https://e.invalid/"
        )
        .is_ok());
    }

    #[test]
    fn plain_http_is_refused_unless_it_is_the_configured_origin() {
        assert!(check_url("http://evil.example/x.zip", TEST_BASE).is_err());
        assert!(check_url("http://127.0.0.1:8099/x.zip", TEST_BASE).is_ok());
        // A different port is a different origin.
        assert!(check_url("http://127.0.0.1:9/x.zip", TEST_BASE).is_err());
        // And the exception cannot be widened by a production base URL.
        assert!(check_url(
            "http://127.0.0.1:8099/x.zip",
            "https://example.invalid/pages/"
        )
        .is_err());
    }

    #[test]
    fn userinfo_cannot_smuggle_a_host_past_the_check() {
        for bad in [
            "https://api.github.com@evil.example/x",
            "https://evil.example@api.github.com/x",
        ] {
            let e = check_url(bad, TEST_BASE).unwrap_err();
            assert!(e.starts_with(R_URL), "{bad}: {e}");
        }
    }

    #[test]
    fn nonsense_urls_are_refused_rather_than_fetched() {
        for bad in ["", "not a url", "https://", "://host/x", "/relative/path"] {
            assert!(check_url(bad, TEST_BASE).is_err(), "{bad:?}");
        }
    }

    #[test]
    fn a_short_crashing_run_does_not_count_as_a_first_run() {
        assert!(
            run_counts_as_started(false, 0.2),
            "a clean quit always counts"
        );
        assert!(
            !run_counts_as_started(true, 0.2),
            "an instant crash does not"
        );
        assert!(
            run_counts_as_started(true, 60.0),
            "a crash an hour in is a game bug"
        );
    }

    #[test]
    fn the_verify_binary_line_carries_this_builds_version() {
        assert!(verify_binary_line().contains(crate::version::VERSION));
        assert!(verify_binary_line().starts_with("mh_launcher "));
    }

    /// The last-known-good rule, over a temporary layout: nothing is pruned before the marker, and
    /// exactly the oldest goes after it.
    #[test]
    fn nothing_is_pruned_until_the_new_version_has_started() {
        let root = std::env::temp_dir().join("mh_launcher_test_prune");
        let _ = std::fs::remove_dir_all(&root);
        let layout = Layout::rooted(&root);
        for v in ["0.1.0", "0.2.0", "0.3.0"] {
            std::fs::create_dir_all(layout.version_dir(v)).unwrap();
        }
        assert_eq!(version_dirs(&layout), vec!["0.3.0", "0.2.0", "0.1.0"]);
        assert!(prune(&layout, "0.3.0").is_empty(), "no marker, no pruning");
        assert_eq!(version_dirs(&layout).len(), 3);

        mark_started(&layout, "0.3.0").unwrap();
        assert!(has_started_once(&layout, "0.3.0"));
        assert_eq!(prune(&layout, "0.3.0"), vec!["0.1.0".to_string()]);
        assert_eq!(version_dirs(&layout), vec!["0.3.0", "0.2.0"]);
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn a_staging_directory_is_not_a_version() {
        let root = std::env::temp_dir().join("mh_launcher_test_staging_not_a_version");
        let _ = std::fs::remove_dir_all(&root);
        let layout = Layout::rooted(&root);
        std::fs::create_dir_all(layout.version_dir("0.1.0")).unwrap();
        std::fs::create_dir_all(
            layout
                .versions()
                .join(format!("0.2.0{}", crate::paths::VERSION_STAGING_SUFFIX)),
        )
        .unwrap();
        assert_eq!(version_dirs(&layout), vec!["0.1.0"]);
        let _ = std::fs::remove_dir_all(&root);
    }

    const R32: &str = "abcdefghijklmnopqrstuvwxyzabcdef";

    #[test]
    fn self_replace_leftover_names() {
        let stem = "mh_launcher";
        for sfx in SELF_REPLACE_SUFFIXES {
            assert!(is_self_replace_leftover(
                &format!(".{stem}.{R32}{sfx}"),
                stem
            ));
        }
        // The launcher itself, another program's leftovers, a short or non-lowercase tag.
        assert!(!is_self_replace_leftover("mh_launcher.exe", stem));
        assert!(!is_self_replace_leftover(
            &format!(".other.{R32}.__selfdelete__.exe"),
            stem
        ));
        assert!(!is_self_replace_leftover(
            ".mh_launcher.abc.__selfdelete__.exe",
            stem
        ));
        assert!(!is_self_replace_leftover(
            &format!(".mh_launcher.{}.__temp__.exe", R32.to_uppercase()),
            stem
        ));
    }

    fn sweep_dir(name: &str) -> PathBuf {
        let d = std::env::temp_dir().join(name);
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        d
    }

    #[test]
    fn sweep_removes_leftovers_and_keeps_the_rest() {
        let d = sweep_dir("mh_launcher_test_sweep");
        let names = [
            format!(".mh_launcher.{R32}.__selfdelete__.exe"),
            format!(".mh_launcher.{R32}.__relocated__.exe"),
            format!(".mh_launcher.{R32}.__temp__.exe"),
        ];
        for n in &names {
            std::fs::write(d.join(n), b"x").unwrap();
        }
        std::fs::write(d.join("mh_launcher.exe"), b"x").unwrap();
        std::fs::write(d.join(format!(".other.{R32}.__selfdelete__.exe")), b"x").unwrap();
        let removed = sweep_self_replace_leftovers(std::slice::from_ref(&d), "mh_launcher");
        assert_eq!(removed.len(), 3, "{removed:?}");
        for n in &names {
            assert!(!d.join(n).exists(), "{n} survived");
        }
        assert!(d.join("mh_launcher.exe").exists());
        assert!(d.join(format!(".other.{R32}.__selfdelete__.exe")).exists());
        let _ = std::fs::remove_dir_all(&d);
    }

    #[cfg(windows)]
    #[test]
    fn a_live_helper_freezes_its_directory() {
        use std::os::windows::fs::OpenOptionsExt;
        let d = sweep_dir("mh_launcher_test_sweep_live");
        let helper = d.join(format!(".mh_launcher.{R32}.__selfdelete__.exe"));
        let relocated = d.join(format!(".mh_launcher.{R32}.__relocated__.exe"));
        std::fs::write(&helper, b"x").unwrap();
        std::fs::write(&relocated, b"x").unwrap();
        // Share mode 0 = no FILE_SHARE_DELETE: stands in for the running helper's mapped image.
        let hold = std::fs::OpenOptions::new()
            .read(true)
            .share_mode(0)
            .open(&helper)
            .unwrap();
        let removed = sweep_self_replace_leftovers(std::slice::from_ref(&d), "mh_launcher");
        assert!(removed.is_empty(), "{removed:?}");
        assert!(
            relocated.exists(),
            "the helper's file to delete was taken from under it"
        );
        drop(hold);
        let removed = sweep_self_replace_leftovers(std::slice::from_ref(&d), "mh_launcher");
        assert_eq!(removed.len(), 2, "{removed:?}");
        let _ = std::fs::remove_dir_all(&d);
    }

    // ---- dist RL9: the silent auto-update, the launcher stage/commit split, re-verify, rollback -------

    /// A game directory with 0.2.9 (stable) installed, ready for a silent update to 0.3.0 (latest).
    fn installed_at_029(name: &str) -> (PathBuf, Layout, PathBuf, Canned, String) {
        let root = scratch(name);
        let layout = Layout::rooted(root.join("state"));
        let game = play_game_dir(&root);
        let server = server();
        let key = test_key();
        make_ready(
            &layout,
            &server,
            TEST_BASE,
            &game,
            "net",
            &env(&key, "stable", ""),
            &no_progress,
        )
        .unwrap()
        .unwrap();
        assert_eq!(install::read_manifest(&game).unwrap().version, "0.2.9");
        (root, layout, game, server, key)
    }

    fn ctx<'a>(
        layout: &'a Layout,
        game: &'a Path,
        env: &'a Env<'a>,
        skip: &'a str,
        pending: bool,
    ) -> AutoCtx<'a> {
        AutoCtx {
            layout,
            base_url: TEST_BASE,
            game_dir: game,
            tag: "net",
            env,
            skip_version: skip,
            launcher_pending: pending,
        }
    }

    /// RL9 done_when: an update published while the game runs is DEFERRED (nothing on disk changes,
    /// the download does not even start), and applied after the game exits with no click.
    #[test]
    fn a_silent_update_waits_for_the_game_and_applies_after_it_exits() {
        let (root, layout, game, server, key) = installed_at_029("mh_launcher_test_auto_defer");
        let e = env(&key, "latest", "");
        let before = read(game.join("mh.dll"));
        let downloads = std::sync::atomic::AtomicUsize::new(0);
        struct Counting<'a>(&'a Canned, &'a std::sync::atomic::AtomicUsize);
        impl Fetch for Counting<'_> {
            fn get(&self, url: &str, limit: u64) -> Result<Vec<u8>, String> {
                if url.ends_with(".zip") {
                    self.1.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
                }
                self.0.get(url, limit)
            }
        }
        let counted = Counting(&server, &downloads);

        // game running (hand-launched or ours): deferred, nothing downloaded, nothing changed
        let run = auto_update(
            &counted,
            &ctx(&layout, &game, &e, "", false),
            &|| true,
            &no_progress,
        )
        .unwrap();
        assert!(run.applied.is_none());
        assert!(
            run.deferred.as_deref().is_some_and(|d| d.contains("0.3.0")),
            "{run:?}"
        );
        assert_eq!(
            run.offer.game.as_deref(),
            Some("0.3.0"),
            "the offer is still shown"
        );
        assert_eq!(downloads.load(std::sync::atomic::Ordering::SeqCst), 0);
        assert_eq!(install::read_manifest(&game).unwrap().version, "0.2.9");
        assert_eq!(read(game.join("mh.dll")), before);

        // the game exits: the next look applies it, no click
        let run = auto_update(
            &counted,
            &ctx(&layout, &game, &e, "", false),
            &|| false,
            &no_progress,
        )
        .unwrap();
        let a = run
            .applied
            .clone()
            .expect("applied once the game had exited");
        assert_eq!(
            (a.version.as_str(), a.channel.as_str()),
            ("0.3.0", "latest")
        );
        assert_eq!(install::read_manifest(&game).unwrap().version, "0.3.0");
        assert!(run.errors.is_empty() && run.deferred.is_none(), "{run:?}");
        // and a third look has nothing to do
        let run = auto_update(
            &counted,
            &ctx(&layout, &game, &env(&key, "latest", "latest"), "", false),
            &|| false,
            &no_progress,
        )
        .unwrap();
        assert!(run.applied.is_none() && run.deferred.is_none());
        assert_eq!(run.offer.game, None);
        let _ = std::fs::remove_dir_all(&root);
    }

    /// The race the guard exists for: the game was not running when the look started, and IS when
    /// the download finishes -- immediately before the first file is copied. Nothing is installed.
    #[test]
    fn the_guard_rechecks_right_before_the_install() {
        let (root, layout, game, server, key) = installed_at_029("mh_launcher_test_auto_guard");
        let e = env(&key, "latest", "");
        let calls = std::sync::atomic::AtomicUsize::new(0);
        // first ask (before the download): not running; second (right before install): running
        let running = || calls.fetch_add(1, std::sync::atomic::Ordering::SeqCst) >= 1;
        let before = read(game.join("mh.dll"));
        let run = auto_update(
            &server,
            &ctx(&layout, &game, &e, "", false),
            &running,
            &no_progress,
        )
        .unwrap();
        assert!(run.applied.is_none(), "{run:?}");
        assert!(
            run.deferred
                .as_deref()
                .is_some_and(|d| d.contains("started while")),
            "{run:?}"
        );
        assert_eq!(calls.load(std::sync::atomic::Ordering::SeqCst), 2);
        assert_eq!(install::read_manifest(&game).unwrap().version, "0.2.9");
        assert_eq!(read(game.join("mh.dll")), before);
        let _ = std::fs::remove_dir_all(&root);
    }

    /// RL9 done_when: a forced download failure keeps the old version -- and Play still works
    /// (readiness is Ready, the game files are untouched, a plain `apply_if_needed` retry works once
    /// the server is back).
    #[test]
    fn a_failed_download_keeps_the_old_version_and_does_not_block_play() {
        let (root, layout, game, mut server, key) = installed_at_029("mh_launcher_test_auto_fail");
        let e = env(&key, "latest", "");
        let zip = server
            .0
            .iter()
            .position(|(u, _)| u.ends_with("0.3.0-net.zip"))
            .unwrap();
        let removed = server.0.remove(zip);
        let before = read(game.join("mh.dll"));
        let run = auto_update(
            &server,
            &ctx(&layout, &game, &e, "", false),
            &|| false,
            &no_progress,
        )
        .unwrap();
        assert!(run.applied.is_none());
        assert_eq!(run.errors.len(), 1, "{run:?}");
        assert!(run.errors[0].contains(R_FETCH), "{:?}", run.errors);
        assert_eq!(install::read_manifest(&game).unwrap().version, "0.2.9");
        assert_eq!(read(game.join("mh.dll")), before);
        assert_eq!(
            readiness(&game, "net"),
            Readiness::Ready,
            "Play is not blocked"
        );
        // a manifest fetch failure is an Err for the caller to retry next interval
        let empty = Canned(Vec::new());
        assert!(auto_update(
            &empty,
            &ctx(&layout, &game, &e, "", false),
            &|| false,
            &no_progress
        )
        .is_err());
        // the server comes back: the same look succeeds
        server.0.push(removed);
        let run = auto_update(
            &server,
            &ctx(&layout, &game, &e, "", false),
            &|| false,
            &no_progress,
        )
        .unwrap();
        assert_eq!(run.applied.expect("applied").version, "0.3.0");
        let _ = std::fs::remove_dir_all(&root);
    }

    /// The silent update only UPDATES: it never installs onto a folder that holds no install of this
    /// configuration, and it honours the rollback pin.
    #[test]
    fn a_silent_update_never_installs_from_nothing_and_honours_the_rollback_pin() {
        let root = scratch("mh_launcher_test_auto_scope");
        let layout = Layout::rooted(root.join("state"));
        let game = play_game_dir(&root);
        let server = server();
        let key = test_key();
        let e = env(&key, "latest", "");
        let run = auto_update(
            &server,
            &ctx(&layout, &game, &e, "", false),
            &|| false,
            &no_progress,
        )
        .unwrap();
        assert!(run.applied.is_none() && run.errors.is_empty(), "{run:?}");
        assert!(
            install::read_manifest(&game).is_none(),
            "Play installs, not the tick"
        );
        assert_eq!(run.offer.game.as_deref(), Some("0.3.0"));

        // installed 0.2.9, pinned at 0.3.0 (rolled back from it): left alone
        let (root2, layout2, game2, server2, key2) = installed_at_029("mh_launcher_test_auto_pin");
        let e2 = env(&key2, "latest", "");
        let run = auto_update(
            &server2,
            &ctx(&layout2, &game2, &e2, "0.3.0", false),
            &|| false,
            &no_progress,
        )
        .unwrap();
        assert!(run.applied.is_none() && run.errors.is_empty(), "{run:?}");
        assert_eq!(install::read_manifest(&game2).unwrap().version, "0.2.9");
        let _ = std::fs::remove_dir_all(&root);
        let _ = std::fs::remove_dir_all(&root2);
    }

    /// A launcher below the game's `min_launcher` stages (or fails to stage) the launcher and defers
    /// the game to after the restart; nothing is replaced and the game stays.
    #[test]
    fn a_newer_launcher_is_staged_first_and_the_game_follows_the_restart() {
        let (root, layout, game, server, key) = installed_at_029("mh_launcher_test_auto_launcher");
        // 0.1.0 is below both channels' launcher and the game's min_launcher (0.2.0)
        let old = Env {
            mine: "0.1.0",
            ..env(&key, "latest", "")
        };
        let run = auto_update(
            &server,
            &ctx(&layout, &game, &old, "", false),
            &|| false,
            &no_progress,
        )
        .unwrap();
        // the fixture's launcher entry advertises 1234 bytes but serves none: a staging FAILURE,
        // reported without touching anything
        assert!(run.launcher.is_none());
        assert_eq!(run.errors.len(), 1, "{run:?}");
        assert!(
            run.deferred
                .as_deref()
                .is_some_and(|d| d.contains("newer launcher")),
            "{run:?}"
        );
        assert!(run.applied.is_none());
        assert_eq!(install::read_manifest(&game).unwrap().version, "0.2.9");
        // with a launcher already staged, no second download is attempted
        let run = auto_update(
            &server,
            &ctx(&layout, &game, &old, "", true),
            &|| false,
            &no_progress,
        )
        .unwrap();
        assert!(run.errors.is_empty() && run.launcher.is_none(), "{run:?}");
        let _ = std::fs::remove_dir_all(&root);
    }

    fn launcher_entry(version: &str, bytes: &[u8]) -> (LauncherManifest, Canned) {
        let url = format!("{TEST_BASE}mh_launcher-{version}.exe");
        let m = LauncherManifest {
            schema: SCHEMA,
            kind: KIND_LAUNCHER.to_string(),
            version: version.to_string(),
            channel: "latest".into(),
            issued_at: "2026-10-08T00:00:00Z".into(),
            url: url.clone(),
            sha256: sha256_bytes(bytes),
            size: bytes.len() as u64,
            notes_url: String::new(),
        };
        (m, Canned(vec![(url, bytes.to_vec())]))
    }

    /// RL9 done_when (launcher): staging downloads, digest-checks and health-gates -- and replaces
    /// NOTHING; the commit swaps it in and starts it with the restart args (Restart now) or starts
    /// nothing (Later); a failing gate or a wrong digest removes/never writes the candidate.
    #[test]
    fn a_launcher_is_staged_without_replacing_anything_and_committed_on_request() {
        let root = scratch("mh_launcher_test_stage_launcher");
        let layout = Layout::rooted(root.join("state"));
        let (m, fetch) = launcher_entry("0.2.0", b"a launcher executable");
        let passed = std::sync::Mutex::new(Vec::<String>::new());
        let gate = |p: &Path, v: &str| -> Result<String, String> {
            passed.lock().unwrap().push(format!("{}:{v}", p.display()));
            Ok(format!("mh_launcher {v} ok"))
        };

        // not newer: nothing staged
        assert!(matches!(
            stage_self_update_with(&layout, &fetch, &m, "0.2.0", TEST_BASE, &gate).unwrap(),
            SelfStage::NotNeeded(_)
        ));
        assert!(passed.lock().unwrap().is_empty());

        let SelfStage::Staged(staged) =
            stage_self_update_with(&layout, &fetch, &m, "0.1.0", TEST_BASE, &gate).unwrap()
        else {
            panic!("a newer launcher must stage");
        };
        assert_eq!(staged.version, "0.2.0");
        assert_eq!(
            std::fs::read(&staged.candidate).unwrap(),
            b"a launcher executable"
        );
        assert_eq!(passed.lock().unwrap().len(), 1, "the health gate ran");

        // Restart now: replace + spawn with the args
        let replaced = std::sync::Mutex::new(Vec::<PathBuf>::new());
        let spawned = std::sync::Mutex::new(Vec::<(PathBuf, Vec<String>)>::new());
        let args = vec!["--view".to_string(), "play".to_string()];
        let running = PathBuf::from(r"C:\fake\mh_launcher.exe");
        let done = commit_self_update_with(
            &staged,
            Some(&args),
            &running,
            &|c| {
                replaced.lock().unwrap().push(c.to_path_buf());
                Ok(())
            },
            &|exe, a| {
                spawned
                    .lock()
                    .unwrap()
                    .push((exe.to_path_buf(), a.to_vec()));
                Ok(())
            },
        )
        .unwrap();
        assert_eq!(done, SelfUpdate::Restarted("0.2.0".into()));
        assert_eq!(
            replaced.lock().unwrap().as_slice(),
            std::slice::from_ref(&staged.candidate)
        );
        assert_eq!(
            spawned.lock().unwrap().as_slice(),
            &[(running.clone(), args)]
        );
        assert!(!staged.candidate.exists(), "the candidate is consumed");

        // Later: stage again, commit with no restart -> replaced, nothing spawned
        let SelfStage::Staged(again) =
            stage_self_update_with(&layout, &fetch, &m, "0.1.0", TEST_BASE, &gate).unwrap()
        else {
            panic!()
        };
        let later = commit_self_update_with(&again, None, &running, &|_| Ok(()), &|_, _| {
            panic!("Later must not start a process")
        })
        .unwrap();
        assert_eq!(later, SelfUpdate::Replaced("0.2.0".into()));
        // a commit whose candidate vanished replaces nothing
        assert!(commit_self_update_with(
            &again,
            None,
            &running,
            &|_| panic!("replaced"),
            &|_, _| Ok(())
        )
        .is_err());

        // a failing gate: error, candidate removed, nothing to commit
        let bad_gate =
            |_: &Path, _: &str| -> Result<String, String> { Err(format!("{R_HEALTH}: no")) };
        let e =
            stage_self_update_with(&layout, &fetch, &m, "0.1.0", TEST_BASE, &bad_gate).unwrap_err();
        assert!(e.starts_with(R_HEALTH), "{e}");
        assert!(!layout
            .root
            .join("update")
            .join("mh_launcher-0.2.0.exe")
            .exists());
        // a wrong digest is refused before anything is written
        let mut tampered = m.clone();
        tampered.sha256 = sha256_bytes(b"something else");
        let e = stage_self_update_with(&layout, &fetch, &tampered, "0.1.0", TEST_BASE, &gate)
            .unwrap_err();
        assert!(e.starts_with(R_DIGEST), "{e}");
        assert!(!layout
            .root
            .join("update")
            .join("mh_launcher-0.2.0.exe")
            .exists());
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn the_auto_schedule_is_30_minutes_and_10_seconds_while_waiting() {
        let mut a = AutoState {
            enabled: true,
            ..Default::default()
        };
        let t0 = Instant::now();
        assert!(a.due(t0), "the first tick after start is due at once");
        a.schedule(t0, false);
        assert!(!a.due(t0 + Duration::from_secs(29 * 60)));
        assert!(a.due(t0 + Duration::from_secs(30 * 60)));
        a.schedule(t0, true);
        assert!(!a.due(t0 + Duration::from_secs(9)));
        assert!(a.due(t0 + Duration::from_secs(10)));
        a.enabled = false;
        assert!(
            !a.due(t0 + Duration::from_secs(3600)),
            "scripted runs never tick"
        );
        // the modal is shown until answered
        a.pending_restart = Some(StagedLauncher {
            version: "0.2.0".into(),
            candidate: PathBuf::from("x"),
        });
        assert_eq!(a.restart_prompt(), Some("0.2.0"));
        a.restart_dismissed = true;
        assert_eq!(a.restart_prompt(), None);
        assert!(
            a.pending_restart.is_some(),
            "Later keeps it staged for the window close"
        );
    }

    /// Diagnostics: re-verify says all-match, then restores a damaged and a deleted file from the
    /// kept copy; with the kept copy gone it says so instead of guessing.
    #[test]
    fn reverify_restores_damaged_files_from_the_kept_copy() {
        let (root, layout, game, _server, _key) = installed_at_029("mh_launcher_test_reverify");
        let ok = reverify(&layout, &game, &no_progress).unwrap();
        assert!(ok.starts_with("all ") && ok.contains("0.2.9"), "{ok}");

        let good = read(game.join("mh.dll"));
        std::fs::write(game.join("mh.dll"), b"corrupted").unwrap();
        let names = install::read_manifest(&game).unwrap().files;
        let other = names
            .iter()
            .map(|(_, _, f)| f.clone())
            .find(|f| f != "mh.dll" && game.join(f).is_file())
            .unwrap();
        let other_good = read(game.join(&other));
        std::fs::remove_file(game.join(&other)).unwrap();
        let fixed = reverify(&layout, &game, &no_progress).unwrap();
        assert!(fixed.contains("reinstalled 0.2.9"), "{fixed}");
        assert_eq!(read(game.join("mh.dll")), good);
        assert_eq!(read(game.join(&other)), other_good);
        assert!(reverify(&layout, &game, &no_progress)
            .unwrap()
            .starts_with("all "));

        // the kept copy is gone: an honest error, nothing changed
        std::fs::write(game.join("mh.dll"), b"corrupted again").unwrap();
        std::fs::remove_dir_all(layout.version_dir("0.2.9")).unwrap();
        let e = reverify(&layout, &game, &no_progress).unwrap_err();
        assert!(e.contains("kept copy of 0.2.9 is gone"), "{e}");
        assert_eq!(read(game.join("mh.dll")), "corrupted again");
        // a folder this launcher never installed into
        let bare = root.join("bare");
        std::fs::create_dir_all(&bare).unwrap();
        assert!(reverify(&layout, &bare, &no_progress).is_err());
        let _ = std::fs::remove_dir_all(&root);
    }

    /// Diagnostics: roll back switches to the kept older version via the last-two-versions mechanism;
    /// with nothing older kept it refuses.
    #[test]
    fn rollback_switches_to_the_kept_previous_version() {
        let (root, layout, game, server, key) = installed_at_029("mh_launcher_test_rollback");
        // nothing older than 0.2.9
        let e = rollback(&layout, &game, &no_progress).unwrap_err();
        assert!(e.contains("no version older than 0.2.9"), "{e}");
        assert_eq!(rollback_target(&layout, "0.2.9"), None);

        let run = auto_update(
            &server,
            &ctx(&layout, &game, &env(&key, "latest", ""), "", false),
            &|| false,
            &no_progress,
        )
        .unwrap();
        assert_eq!(run.applied.unwrap().version, "0.3.0");
        assert_eq!(rollback_target(&layout, "0.3.0").as_deref(), Some("0.2.9"));
        let done = rollback(&layout, &game, &no_progress).unwrap();
        assert_eq!(
            (done.from.as_str(), done.to.as_str(), done.tag.as_str()),
            ("0.3.0", "0.2.9", "net")
        );
        let r = install::read_manifest(&game).unwrap();
        assert_eq!(r.version, "0.2.9");
        assert_eq!(receipt_mismatches(&game, &r).1, Vec::<String>::new());
        // and the pin keeps the silent update from undoing it
        let run = auto_update(
            &server,
            &ctx(&layout, &game, &env(&key, "latest", ""), &done.from, false),
            &|| false,
            &no_progress,
        )
        .unwrap();
        assert!(run.applied.is_none(), "{run:?}");
        assert_eq!(install::read_manifest(&game).unwrap().version, "0.2.9");
        let _ = std::fs::remove_dir_all(&root);
    }

    // ---- RL9 end-to-end over REAL HTTP (the rehearsal stand-in, in-process) -------------------------

    /// A tiny HTTP/1.1 server on 127.0.0.1:8099 (the origin the signed fixtures name) serving
    /// `server()`'s table; `fail_zips` makes every `.zip` answer 500 -- "the CDN is down".
    struct StandIn {
        stop: std::sync::Arc<std::sync::atomic::AtomicBool>,
        fail_zips: std::sync::Arc<std::sync::atomic::AtomicBool>,
        thread: Option<std::thread::JoinHandle<()>>,
    }

    impl StandIn {
        /// `None` when port 8099 is taken (another lane's rehearsal): the caller skips, loudly.
        fn start() -> Option<StandIn> {
            use std::io::{Read, Write};
            use std::sync::atomic::{AtomicBool, Ordering};
            use std::sync::Arc;
            let listener = std::net::TcpListener::bind("127.0.0.1:8099").ok()?;
            listener.set_nonblocking(true).unwrap();
            let table: Vec<(String, Vec<u8>)> = server()
                .0
                .into_iter()
                .map(|(u, b)| {
                    (
                        u.strip_prefix("http://127.0.0.1:8099").unwrap().to_string(),
                        b,
                    )
                })
                .collect();
            let stop = Arc::new(AtomicBool::new(false));
            let fail_zips = Arc::new(AtomicBool::new(false));
            let (stop2, fail2) = (stop.clone(), fail_zips.clone());
            let thread = std::thread::spawn(move || {
                while !stop2.load(Ordering::SeqCst) {
                    let Ok((mut sock, _)) = listener.accept() else {
                        std::thread::sleep(Duration::from_millis(5));
                        continue;
                    };
                    sock.set_nonblocking(false).unwrap();
                    let _ = sock.set_read_timeout(Some(Duration::from_secs(5)));
                    let mut buf = [0u8; 4096];
                    let n = sock.read(&mut buf).unwrap_or(0);
                    let req = String::from_utf8_lossy(&buf[..n]).to_string();
                    let path = req.split_whitespace().nth(1).unwrap_or("/").to_string();
                    let reply = if path.ends_with(".zip") && fail2.load(Ordering::SeqCst) {
                        b"HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n".to_vec()
                    } else if let Some((_, body)) = table.iter().find(|(p, _)| *p == path) {
                        let mut r = format!(
                            "HTTP/1.1 200 OK\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
                            body.len()
                        )
                        .into_bytes();
                        r.extend_from_slice(body);
                        r
                    } else {
                        b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
                            .to_vec()
                    };
                    let _ = sock.write_all(&reply);
                }
            });
            Some(StandIn {
                stop,
                fail_zips,
                thread: Some(thread),
            })
        }
    }

    impl Drop for StandIn {
        fn drop(&mut self) {
            self.stop.store(true, std::sync::atomic::Ordering::SeqCst);
            if let Some(t) = self.thread.take() {
                let _ = t.join();
            }
        }
    }

    /// The RL9 scenarios over real sockets and the real `Http` fetcher: the game is running (held
    /// back, no zip is even requested), a 500 on the zip (old version kept, Play ready), the CDN
    /// comes back and the same look applies the update with no click.
    #[test]
    fn the_silent_update_over_real_http_defers_survives_a_500_and_then_applies() {
        use std::sync::atomic::Ordering;
        let Some(stand_in) = StandIn::start() else {
            eprintln!("SKIPPED: 127.0.0.1:8099 is in use (another rehearsal is running)");
            return;
        };
        let root = scratch("mh_launcher_test_auto_http");
        let layout = Layout::rooted(root.join("state"));
        let game = play_game_dir(&root);
        let key = test_key();
        let http = Http::new();
        // 0.2.9 first (stable), then follow latest
        make_ready(
            &layout,
            &http,
            TEST_BASE,
            &game,
            "net",
            &env(&key, "stable", ""),
            &no_progress,
        )
        .unwrap()
        .unwrap();
        let e = env(&key, "latest", "");

        // 1. the game is running: held back
        let run = auto_update(
            &http,
            &ctx(&layout, &game, &e, "", false),
            &|| true,
            &no_progress,
        )
        .unwrap();
        assert!(run.applied.is_none() && run.deferred.is_some(), "{run:?}");
        assert_eq!(install::read_manifest(&game).unwrap().version, "0.2.9");

        // 2. the CDN answers 500 for the zip: the old version stays, Play is ready
        stand_in.fail_zips.store(true, Ordering::SeqCst);
        let run = auto_update(
            &http,
            &ctx(&layout, &game, &e, "", false),
            &|| false,
            &no_progress,
        )
        .unwrap();
        assert!(run.applied.is_none());
        assert!(run.errors[0].contains("500"), "{:?}", run.errors);
        assert_eq!(install::read_manifest(&game).unwrap().version, "0.2.9");
        assert_eq!(readiness(&game, "net"), Readiness::Ready);

        // 3. the CDN is back; the next look applies it
        stand_in.fail_zips.store(false, Ordering::SeqCst);
        let run = auto_update(
            &http,
            &ctx(&layout, &game, &e, "", false),
            &|| false,
            &no_progress,
        )
        .unwrap();
        assert_eq!(run.applied.expect("applied").version, "0.3.0");
        assert_eq!(install::read_manifest(&game).unwrap().version, "0.3.0");
        drop(stand_in);
        let _ = std::fs::remove_dir_all(&root);
    }
}
