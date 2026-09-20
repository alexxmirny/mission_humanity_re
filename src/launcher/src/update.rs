//! `mh_launcher` dist **LA2**: finding an update, refusing a bad one, and applying a good one.
//!
//! The whole of this module is one sentence: **fetch a signed manifest, refuse it four ways, and
//! only then touch the disk.** Everything below is either one of those four refusals or the
//! stage-verify-swap that follows them.
//!
//! ```text
//!   GET <update_base_url>/manifest.json          (+ .minisig)
//!        |
//!        +-- 1. SIGNATURE   minisign, against the ONE key compiled in below
//!        +-- 2. SCHEMA      a manifest from a future shape is not guessed at
//!        +-- 3. FRESHNESS   older than 30 days, or more than a day in the future -> no
//!        +-- 4. NOT NEWER   version <= what is installed -> no
//!        +-- 5. URL         https only (or the configured origin), never api.github.com
//!        |
//!   GET <asset url>  ->  versions\<ver>.staging\  ->  sha256  ->  rename to versions\<ver>\
//!        |
//!   copy beside mh.exe (the LA1 install), and KEEP versions\<old>\ until the new one has run once
//! ```
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
//! zips tamper-evident in turn. **Signature first, parse second** -- `accept` never hands
//! `serde_json` a byte that has not already verified, because a JSON parser is a bigger attack
//! surface than an Ed25519 check.
//!
//! The two refusals that are NOT about forgery are about a signature that is genuine and stale.
//! Both are TUF's named attacks: a **rollback** (serving an old, genuinely-signed manifest to walk
//! a player back onto a version with a known bug) is what `check_newer` refuses, and a **freeze**
//! (serving yesterday's genuine manifest forever so a player never learns there is a fix) is what
//! `check_freshness` refuses. Neither can be caught by verifying the signature, because in both the
//! signature is perfectly valid.

use std::collections::BTreeMap;
use std::io::Read;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::{Duration, Instant};

use chrono::{DateTime, Utc};
use serde::Deserialize;
use sha2::{Digest, Sha256};

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

/// Where `manifest.json` lives when `launcher.toml` does not say otherwise: the public repository's
/// GitHub Pages site, stamped in at BUILD time by the release workflow (`MH_UPDATE_BASE_URL`, LA3)
/// rather than written here -- the tree carries no publisher identity, by the same rule that keeps
/// hosts out of `tools/` (lint_machine_paths). A build without it has NO default: the update check
/// then says so and does nothing until `launcher.toml` names a source. Pages rather than the API for
/// the reasons in the module header; a plain CDN GET that no rate limit counts.
pub const DEFAULT_BASE_URL: &str = match option_env!("MH_UPDATE_BASE_URL") {
    Some(u) => u,
    None => "",
};

pub const MANIFEST_NAME: &str = "manifest.json";
pub const SIGNATURE_NAME: &str = "manifest.json.minisig";

/// The schema this build understands. A manifest claiming anything else is refused rather than
/// read optimistically: the fields a future schema adds are exactly the ones an old launcher would
/// not know to honour.
pub const SCHEMA: u32 = 1;

/// How old a manifest may be before it reads as a freeze attack. Thirty days is the plan's figure
/// and it is a trade: shorter and a quiet month breaks every launcher, longer and an attacker who
/// can pin the file has a wider window.
pub const MAX_AGE_DAYS: i64 = 30;

/// How far into the future a manifest may claim to be. A day, because the machine's clock is the
/// thing more likely to be wrong than the publisher's.
pub const MAX_SKEW_DAYS: i64 = 1;

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
pub const R_SIGNATURE: &str = "refuse SIGNATURE";
pub const R_MALFORMED: &str = "refuse MALFORMED";
pub const R_SCHEMA: &str = "refuse SCHEMA";
pub const R_STALE: &str = "refuse STALE";
pub const R_CLOCK: &str = "refuse CLOCK";
pub const R_NOT_NEWER: &str = "refuse NOT NEWER";
pub const R_URL: &str = "refuse URL";
pub const R_DIGEST: &str = "refuse DIGEST";
pub const R_HEALTH: &str = "refuse HEALTH GATE";

// --------------------------------------------------------------------------- the manifest

/// One downloadable file.
#[derive(Clone, Debug, Deserialize)]
pub struct Asset {
    pub url: String,
    pub sha256: String,
    #[serde(default)]
    pub size: u64,
}

#[derive(Clone, Debug, Deserialize)]
pub struct LauncherEntry {
    pub version: String,
    pub url: String,
    pub sha256: String,
}

/// `manifest.json`, schema 1. `deny_unknown_fields` is deliberately NOT set: a schema-1 manifest
/// with an extra field is still a schema-1 manifest, and the version gate above is what guards a
/// shape change. What IS strict is that every field here is required -- a manifest missing an asset
/// digest must fail to parse rather than default to the empty string and then fail to match.
#[derive(Clone, Debug, Deserialize)]
pub struct Manifest {
    pub schema: u32,
    pub version: String,
    pub issued_at: String,
    pub launcher: LauncherEntry,
    /// configuration tag (`net`, `net-debug`, `brokered-debug`) -> the zip that carries it.
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

impl Manifest {
    pub fn asset(&self, tag: &str) -> Result<&Asset, String> {
        self.game.get(tag).ok_or_else(|| {
            format!(
                "{R_MALFORMED}: the manifest has no {tag:?} configuration (it offers {})",
                self.game.keys().cloned().collect::<Vec<_>>().join(", ")
            )
        })
    }
}

// --------------------------------------------------------------------------- the five refusals

/// 1. The signature, against `PUBLIC_KEY`, before anything else looks at the bytes.
///
/// `allow_legacy` is **false**: minisign's legacy algorithm signs the file itself rather than its
/// BLAKE2b hash, and accepting both would mean accepting a signature made under weaker assumptions
/// than the ones this project's own signer works under. Nothing we publish needs it.
///
/// Production reaches this through `accept_with_key(.., PUBLIC_KEY)`; the bare form is the
/// tests' way of asking "would the RELEASE key accept this".
#[cfg(test)]
pub fn verify_signature(bytes: &[u8], signature: &str) -> Result<(), String> {
    verify_signature_with(bytes, signature, PUBLIC_KEY)
}

/// `verify_signature` against an explicit key -- for the tests that carry a fixture signed by a
/// throwaway key. Production never calls this with anything but `PUBLIC_KEY`.
pub fn verify_signature_with(
    bytes: &[u8],
    signature: &str,
    public_key: &str,
) -> Result<(), String> {
    let key = minisign_verify::PublicKey::from_base64(public_key)
        .map_err(|e| format!("{R_SIGNATURE}: this build's public key does not parse ({e})"))?;
    let sig = minisign_verify::Signature::decode(signature).map_err(|e| {
        format!("{R_SIGNATURE}: {SIGNATURE_NAME} is not a minisign signature ({e})")
    })?;
    key.verify(bytes, &sig, false).map_err(|e| {
        format!(
            "{R_SIGNATURE}: {MANIFEST_NAME} is not signed by this launcher's key ({e}). \
             The file was not written by whoever holds the release key -- nothing was downloaded."
        )
    })
}

/// Parse, then 2. the schema gate.
pub fn parse_manifest(bytes: &[u8]) -> Result<Manifest, String> {
    let manifest: Manifest = serde_json::from_slice(bytes)
        .map_err(|e| format!("{R_MALFORMED}: {MANIFEST_NAME} does not parse ({e})"))?;
    if manifest.schema != SCHEMA {
        return Err(format!(
            "{R_SCHEMA}: the manifest is schema {} and this launcher understands {SCHEMA}. \
             Update the launcher first.",
            manifest.schema
        ));
    }
    if manifest.game.is_empty() {
        return Err(format!("{R_MALFORMED}: the manifest offers no game assets"));
    }
    // dist LA6: a relay that would not survive being written into an ini is refused HERE, so a
    // manifest is either applied whole or not at all -- never a game update installed and then
    // a half-provisioned relay.
    if let Some(relay) = &manifest.relay {
        relay
            .validate()
            .map_err(|e| format!("{R_MALFORMED}: the manifest's relay field is unusable ({e})"))?;
    }
    Ok(manifest)
}

/// 3. Freshness, in both directions.
pub fn check_freshness(issued_at: &str, now: DateTime<Utc>) -> Result<(), String> {
    let issued = DateTime::parse_from_rfc3339(issued_at)
        .map_err(|e| {
            format!("{R_MALFORMED}: issued_at {issued_at:?} is not an RFC 3339 timestamp ({e})")
        })?
        .with_timezone(&Utc);
    let age = now.signed_duration_since(issued);
    if age.num_days() > MAX_AGE_DAYS {
        return Err(format!(
            "{R_STALE}: the manifest was issued {issued_at} -- {} days ago, and anything older \
             than {MAX_AGE_DAYS} days is treated as a freeze (somebody pinning an old genuine file \
             so this launcher never learns of a newer one).",
            age.num_days()
        ));
    }
    if (-age).num_days() > MAX_SKEW_DAYS {
        return Err(format!(
            "{R_CLOCK}: the manifest is dated {issued_at}, which is {} days in the future. \
             Either this machine's clock is wrong or the file is not what it claims.",
            (-age).num_days()
        ));
    }
    Ok(())
}

/// 4. Rollback: the offered version must be strictly newer than what is installed.
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

/// All five gates, in order, over bytes that have just come off the network, against the release
/// key. Production goes through `check` -> `accept_with_key`; this is the tests' entry.
#[cfg(test)]
pub fn accept(
    bytes: &[u8],
    signature: &str,
    base_url: &str,
    installed_version: &str,
    now: DateTime<Utc>,
) -> Result<Manifest, String> {
    accept_with_key(
        bytes,
        signature,
        base_url,
        installed_version,
        now,
        PUBLIC_KEY,
    )
}

/// `accept` against an explicit key. Production never passes anything but `PUBLIC_KEY`; the
/// dist LA8 tests pass a throwaway key so a whole install can run against a fixture manifest
/// whose assets are real zips in the tree (the release key's secret half is not).
pub fn accept_with_key(
    bytes: &[u8],
    signature: &str,
    base_url: &str,
    installed_version: &str,
    now: DateTime<Utc>,
    public_key: &str,
) -> Result<Manifest, String> {
    verify_signature_with(bytes, signature, public_key)?;
    let manifest = parse_manifest(bytes)?;
    check_freshness(&manifest.issued_at, now)?;
    check_newer(&manifest.version, installed_version)?;
    for (tag, asset) in &manifest.game {
        check_url(&asset.url, base_url).map_err(|e| format!("{e} (the {tag} zip)"))?;
    }
    check_url(&manifest.launcher.url, base_url).map_err(|e| format!("{e} (the launcher)"))?;
    Ok(manifest)
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
        let mut response = self
            .agent
            .get(url)
            .call()
            .map_err(|e| format!("{R_FETCH}: {url} -- {e}"))?;
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

/// Fetch and accept the manifest. The only function the UI and the command line both call to answer
/// "is there an update?".
///
/// `remember` (dist LA6): where to keep the accepted bytes + signature so `load_accepted` can hand
/// the relay back to a later launch. `None` only in tests.
pub fn check(
    fetch: &dyn Fetch,
    base_url: &str,
    installed_version: &str,
    remember: Option<&Layout>,
) -> Result<Manifest, String> {
    check_with_key(fetch, base_url, installed_version, remember, PUBLIC_KEY)
}

/// `check` against an explicit key -- see `accept_with_key`.
pub fn check_with_key(
    fetch: &dyn Fetch,
    base_url: &str,
    installed_version: &str,
    remember: Option<&Layout>,
    public_key: &str,
) -> Result<Manifest, String> {
    if base_url.trim().is_empty() {
        return Err("NO UPDATE SOURCE: this build carries no default update URL and launcher.toml                     names none (set update_base_url, or build with MH_UPDATE_BASE_URL)"
            .to_string());
    }
    let bytes = fetch.get(&join_url(base_url, MANIFEST_NAME), MAX_MANIFEST_BYTES)?;
    let sig = fetch.get(&join_url(base_url, SIGNATURE_NAME), MAX_MANIFEST_BYTES)?;
    let sig = String::from_utf8(sig)
        .map_err(|_| format!("{R_SIGNATURE}: {SIGNATURE_NAME} is not text"))?;
    let manifest = accept_with_key(
        &bytes,
        &sig,
        base_url,
        installed_version,
        Utc::now(),
        public_key,
    )?;
    log::line(format!(
        "update: manifest {} accepted (issued {}, {} configuration(s), launcher {}, relay {})",
        manifest.version,
        manifest.issued_at,
        manifest.game.len(),
        manifest.launcher.version,
        if manifest.relay.is_some() {
            "named"
        } else {
            "none"
        }
    ));
    if let Some(layout) = remember {
        if let Err(e) = remember_accepted(layout, &bytes, &sig) {
            log::line(format!("update: {e}"));
        }
    }
    Ok(manifest)
}

// --------------------------------------------------------------------------- the accepted copy

/// Keep the manifest that just passed every gate, WITH its signature, so a later launch can read
/// its relay (dist LA6) after re-verifying it. Bytes, not fields: what is kept is exactly what was
/// signed, and `load_accepted` trusts nothing about the file beyond what the key still says.
pub fn remember_accepted(layout: &Layout, bytes: &[u8], sig: &str) -> Result<(), String> {
    let dir = layout.accepted_dir();
    std::fs::create_dir_all(&dir).map_err(|e| format!("cannot create {}: {e}", dir.display()))?;
    let m = layout.accepted_manifest();
    let s = layout.accepted_signature();
    std::fs::write(&m, bytes).map_err(|e| format!("cannot write {}: {e}", m.display()))?;
    std::fs::write(&s, sig).map_err(|e| format!("cannot write {}: {e}", s.display()))?;
    Ok(())
}

/// The last accepted manifest, re-verified against `PUBLIC_KEY` and re-parsed. `None` when there
/// is none, or when the copy no longer verifies (an edited file is a file this launcher never
/// accepted); the reason goes to the log.
///
/// Only the SIGNATURE and SCHEMA gates run here, deliberately. Freshness and NOT-NEWER decide
/// whether to fetch a new version; this decides whether the relay the launcher already accepted
/// may still be written, and a relay does not go stale in thirty days -- the next successful check
/// replaces the copy either way.
pub fn load_accepted(layout: &Layout) -> Option<Manifest> {
    load_accepted_with_key(layout, PUBLIC_KEY)
}

pub fn load_accepted_with_key(layout: &Layout, public_key: &str) -> Option<Manifest> {
    let bytes = std::fs::read(layout.accepted_manifest()).ok()?;
    let sig = std::fs::read_to_string(layout.accepted_signature()).ok()?;
    let checked =
        verify_signature_with(&bytes, &sig, public_key).and_then(|()| parse_manifest(&bytes));
    match checked {
        Ok(m) => Some(m),
        Err(e) => {
            log::line(format!(
                "update: the accepted manifest at {} is not usable and is ignored -- {e}",
                layout.accepted_manifest().display()
            ));
            None
        }
    }
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
/// Two halves since dist LA8 (`fetch_and_stage`, `install_staged`), because a switch of
/// configuration has to UNINSTALL the old set between them -- after the new zip is verified and
/// on disk, so a failed download never leaves a game directory with nothing in it. `progress`
/// is what the Play page shows meanwhile.
pub fn apply(
    layout: &Layout,
    fetch: &dyn Fetch,
    manifest: &Manifest,
    tag: &str,
    game_dir: &Path,
    base_url: &str,
    progress: Progress,
) -> Result<Applied, String> {
    let staged = fetch_and_stage(layout, fetch, manifest, tag, base_url, progress)?;
    // dist LA8: a different configuration in the receipt is uninstalled first -- receipt-driven,
    // so `mh.dll.mhbak` (the game's own dll) is put back and then parked again by the install
    // rather than overwritten by OUR previous dll.
    if let Some(receipt) = install::read_manifest(game_dir) {
        if receipt.tag != tag {
            progress(&format!(
                "removing the {} configuration before installing {tag}...",
                receipt.tag
            ));
            let un = install::uninstall(game_dir)?;
            log::line(format!(
                "update: switched away from {} -- {}",
                receipt.tag,
                un.summary()
            ));
        }
    }
    progress(&format!(
        "installing {} beside {GAME_EXE}...",
        staged.package
    ));
    install_staged(layout, &staged, manifest, game_dir)
}

/// The first half of `apply`: download, digest, unpack into staging, rename into place.
pub fn fetch_and_stage(
    layout: &Layout,
    fetch: &dyn Fetch,
    manifest: &Manifest,
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

/// The second half of `apply`: copy the staged set beside `mh.exe` and write the receipt.
pub fn install_staged(
    layout: &Layout,
    staged: &Staged,
    manifest: &Manifest,
    game_dir: &Path,
) -> Result<Applied, String> {
    // dist LA6: the ini the zip just put beside mh.exe gets the manifest's relay before its digest
    // goes in the receipt, and the key file is written -- the same call every launch makes.
    let report = install::install_from_version_dir(
        &staged.version_dir,
        &staged.pkg,
        &staged.package,
        game_dir,
        manifest.relay.as_ref(),
    )?;
    let kept = version_dirs(layout);
    log::line(format!(
        "update: {} installed; version directories now {:?}",
        manifest.version, kept
    ));
    Ok(Applied {
        version: manifest.version.clone(),
        tag: staged.pkg.tag.clone(),
        summary: report.summary(),
        kept,
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

/// Make a game directory ready to play the chosen configuration (dist LA8): nothing to do when
/// it is installed; otherwise the LA2 update path -- fetch the signed manifest, download, verify,
/// swap in, install (uninstalling a different configuration first) and provision the relay (LA6).
/// `Ok(None)` means nothing was installed because nothing had to be.
///
/// The manifest is checked with NOTHING installed, whatever `launcher.toml` remembers: on this
/// path the receipt says the chosen configuration is absent, and an "is it newer" gate against a
/// version that is not there (or is a different configuration about to be removed) would refuse
/// the very install the player asked for. A manifest that lacks the chosen tag fails inside
/// `fetch_and_stage` with `refuse MALFORMED` -- before anything is uninstalled, so it installs
/// and removes nothing.
pub fn make_ready(
    layout: &Layout,
    fetch: &dyn Fetch,
    base_url: &str,
    game_dir: &Path,
    tag: &str,
    progress: Progress,
) -> Result<Option<Applied>, String> {
    make_ready_with_key(layout, fetch, base_url, game_dir, tag, progress, PUBLIC_KEY)
}

pub fn make_ready_with_key(
    layout: &Layout,
    fetch: &dyn Fetch,
    base_url: &str,
    game_dir: &Path,
    tag: &str,
    progress: Progress,
    public_key: &str,
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
    progress(&format!("fetching the signed manifest for {tag}..."));
    let manifest = check_with_key(fetch, base_url, "", Some(layout), public_key)?;
    let applied = apply(layout, fetch, &manifest, tag, game_dir, base_url, progress)?;
    Ok(Some(applied))
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
/// update so the replacement does not immediately try to update again.
pub fn self_update(
    layout: &Layout,
    fetch: &dyn Fetch,
    manifest: &Manifest,
    base_url: &str,
    restart_args: &[String],
) -> Result<SelfUpdate, String> {
    let mine = crate::version::VERSION;
    let theirs = &manifest.launcher.version;
    if let Err(e) = check_newer(theirs, mine) {
        log::line(format!("update: the launcher stays at {mine} -- {e}"));
        return Ok(SelfUpdate::NotNeeded(format!(
            "the launcher is {mine}; the manifest offers {theirs}"
        )));
    }
    check_url(&manifest.launcher.url, base_url)?;

    let dir = layout.root.join("update");
    std::fs::create_dir_all(&dir).map_err(|e| format!("cannot create {}: {e}", dir.display()))?;
    let candidate = dir.join(format!("mh_launcher-{theirs}.exe"));
    let bytes = fetch.get(&manifest.launcher.url, MAX_ASSET_BYTES)?;
    check_digest("the launcher executable", &bytes, &manifest.launcher.sha256)?;
    std::fs::write(&candidate, &bytes)
        .map_err(|e| format!("cannot write {}: {e}", candidate.display()))?;
    log::line(format!(
        "update: launcher {theirs} downloaded to {} ({} B, digest matches)",
        candidate.display(),
        bytes.len()
    ));

    match health_gate(&candidate, theirs) {
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

    let running = std::env::current_exe()
        .map_err(|e| format!("cannot find this executable's own path ({e})"))?;
    self_replace::self_replace(&candidate)
        .map_err(|e| format!("cannot replace {} ({e})", running.display()))?;
    let _ = std::fs::remove_file(&candidate);
    log::line(format!(
        "update: {} replaced with launcher {theirs}; restarting with {restart_args:?}",
        running.display()
    ));
    Command::new(&running)
        .args(restart_args)
        .spawn()
        .map_err(|e| format!("replaced the launcher but cannot start it ({e})"))?;
    Ok(SelfUpdate::Restarted(theirs.clone()))
}

// --------------------------------------------------------------------------- tests

#[cfg(test)]
mod tests {
    use super::*;

    /// A manifest and its signature, produced by `tools/gen_update_manifest.py` under the SAME key
    /// that `PUBLIC_KEY` names. This is the cross-check that matters: the Python signer and the
    /// Rust verifier are different implementations of one format, and a fixture signed by one and
    /// checked by the other is the only thing that proves they agree.
    const GOOD_MANIFEST: &[u8] = include_bytes!("../tests/data/manifest.json");
    const GOOD_SIG: &str = include_str!("../tests/data/manifest.json.minisig");
    /// The same manifest, signed by a DIFFERENT key. Byte-for-byte valid minisign; simply not ours.
    const WRONG_KEY_SIG: &str = include_str!("../tests/data/manifest.json.wrongkey.minisig");

    const TEST_BASE: &str = "http://127.0.0.1:8099/";

    fn now() -> DateTime<Utc> {
        // The fixture's issued_at, so the freshness gate is exercised deliberately rather than by
        // whatever the clock says on the day the test runs.
        DateTime::parse_from_rfc3339("2026-09-17T12:00:00Z")
            .unwrap()
            .with_timezone(&Utc)
    }

    #[test]
    fn the_baked_public_key_parses() {
        minisign_verify::PublicKey::from_base64(PUBLIC_KEY)
            .expect("the compiled-in public key must be a minisign key");
    }

    #[test]
    fn a_manifest_signed_by_our_key_is_accepted() {
        let m = accept(GOOD_MANIFEST, GOOD_SIG, TEST_BASE, "", now()).expect("should be accepted");
        assert_eq!(m.schema, 1);
        assert_eq!(m.version, "0.2.0");
        assert_eq!(m.game.len(), 3);
        assert!(m.game.contains_key("net"));
    }

    /// dist LA2's clause: *a manifest signed with a different key is refused*.
    #[test]
    fn a_manifest_signed_by_another_key_is_refused() {
        let e = accept(GOOD_MANIFEST, WRONG_KEY_SIG, TEST_BASE, "", now()).unwrap_err();
        assert!(e.starts_with(R_SIGNATURE), "{e}");
    }

    #[test]
    fn a_flipped_byte_is_refused() {
        let mut bytes = GOOD_MANIFEST.to_vec();
        let last = bytes.len() - 2;
        bytes[last] ^= 0x20;
        let e = accept(&bytes, GOOD_SIG, TEST_BASE, "", now()).unwrap_err();
        assert!(e.starts_with(R_SIGNATURE), "{e}");
    }

    /// dist LA2's clause: *a manifest with version <= installed is refused*.
    #[test]
    fn a_version_not_newer_than_the_installed_one_is_refused() {
        for installed in ["0.2.0", "0.3.0", "1.0.0"] {
            let e = accept(GOOD_MANIFEST, GOOD_SIG, TEST_BASE, installed, now()).unwrap_err();
            assert!(e.starts_with(R_NOT_NEWER), "{installed}: {e}");
        }
        accept(GOOD_MANIFEST, GOOD_SIG, TEST_BASE, "0.1.0", now()).expect("0.1.0 -> 0.2.0");
    }

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

    /// dist LA2's clause: *an issued_at 40 days old is refused*.
    #[test]
    fn a_forty_day_old_manifest_is_refused() {
        let issued = "2026-09-17T12:00:00Z";
        let forty_days_later = DateTime::parse_from_rfc3339("2026-10-27T12:00:00Z")
            .unwrap()
            .with_timezone(&Utc);
        let e = check_freshness(issued, forty_days_later).unwrap_err();
        assert!(e.starts_with(R_STALE), "{e}");
        assert!(e.contains("40 days ago"), "{e}");
        // The boundary is where it says it is, in both directions.
        let twenty_nine = DateTime::parse_from_rfc3339("2026-10-16T12:00:00Z")
            .unwrap()
            .with_timezone(&Utc);
        assert!(check_freshness(issued, twenty_nine).is_ok());
    }

    #[test]
    fn a_manifest_from_the_future_is_refused() {
        let issued = "2026-09-17T12:00:00Z";
        let two_days_before = DateTime::parse_from_rfc3339("2026-09-15T11:00:00Z")
            .unwrap()
            .with_timezone(&Utc);
        let e = check_freshness(issued, two_days_before).unwrap_err();
        assert!(e.starts_with(R_CLOCK), "{e}");
        // An hour of skew is not a conspiracy.
        let one_hour_before = DateTime::parse_from_rfc3339("2026-09-17T11:00:00Z")
            .unwrap()
            .with_timezone(&Utc);
        assert!(check_freshness(issued, one_hour_before).is_ok());
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
    fn a_schema_this_build_does_not_know_is_refused() {
        let bytes = br#"{"schema":2,"version":"9.0.0","issued_at":"2026-09-17T12:00:00Z",
            "launcher":{"version":"9.0.0","url":"https://x.invalid/l.exe","sha256":"00"},
            "game":{"net":{"url":"https://x.invalid/a.zip","sha256":"00","size":1}}}"#;
        let e = parse_manifest(bytes).unwrap_err();
        assert!(e.starts_with(R_SCHEMA), "{e}");
    }

    #[test]
    fn a_manifest_missing_a_digest_fails_to_parse_rather_than_defaulting() {
        let bytes = br#"{"schema":1,"version":"9.0.0","issued_at":"2026-09-17T12:00:00Z",
            "launcher":{"version":"9.0.0","url":"https://x.invalid/l.exe","sha256":"00"},
            "game":{"net":{"url":"https://x.invalid/a.zip","size":1}}}"#;
        let e = parse_manifest(bytes).unwrap_err();
        assert!(e.starts_with(R_MALFORMED), "{e}");
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

    #[test]
    fn check_fetches_both_files_and_runs_every_gate() {
        let base = "https://example.invalid/mh";
        let canned = Canned(vec![
            (
                "https://example.invalid/mh/manifest.json".into(),
                GOOD_MANIFEST.to_vec(),
            ),
            (
                "https://example.invalid/mh/manifest.json.minisig".into(),
                GOOD_SIG.as_bytes().to_vec(),
            ),
        ]);
        // The fixture's URLs are the local stand-in's, so a production base URL must refuse them --
        // which is precisely the http-origin rule doing its job.
        let e = check(&canned, base, "", None).unwrap_err();
        assert!(e.starts_with(R_URL), "{e}");

        // A missing signature file is a fetch failure, not a silent pass.
        let only_manifest = Canned(vec![(
            "https://example.invalid/mh/manifest.json".into(),
            GOOD_MANIFEST.to_vec(),
        )]);
        let e = check(&only_manifest, base, "", None).unwrap_err();
        assert!(e.starts_with(R_FETCH), "{e}");
    }

    // ---- dist LA6: the relay field ------------------------------------------------------------

    /// A manifest carrying `relay`, signed by a THROWAWAY key whose public half is below and
    /// whose secret half was generated in a scratch directory and discarded. It cannot be signed
    /// by the release key (that secret is not in this tree), and it does not need to be: what the
    /// fixture proves is that the Python signer and this parser agree on the field, and that the
    /// accepted-copy round trip re-verifies before it hands a relay to the launch path.
    const RELAY_MANIFEST: &[u8] = include_bytes!("../tests/data/manifest_relay.json");
    const RELAY_SIG: &str = include_str!("../tests/data/manifest_relay.json.minisig");
    const RELAY_TEST_KEY: &str = include_str!("../tests/data/manifest_relay.pub");
    const RELAY_ADDR: &str = "192.0.2.10:7100";
    const RELAY_KEY: &str = "4d487465737474656b65794d487465737474656b65794d487465737474656b65";

    fn test_key() -> String {
        RELAY_TEST_KEY
            .lines()
            .find(|l| !l.starts_with("untrusted comment"))
            .unwrap()
            .trim()
            .to_string()
    }

    #[test]
    fn the_release_fixture_has_no_relay_and_the_relay_fixture_has_one() {
        let plain = parse_manifest(GOOD_MANIFEST).unwrap();
        assert!(plain.relay.is_none(), "the LA2 fixture predates the field");
        let with = parse_manifest(RELAY_MANIFEST).unwrap();
        let relay = with.relay.expect("the LA6 fixture carries a relay");
        assert_eq!(relay.addr, RELAY_ADDR);
        assert_eq!(relay.key, RELAY_KEY);
        // Signed by the test key, and by NO other -- the release key must refuse it, or a fixture
        // would be a way to smuggle a relay past the build's own key.
        verify_signature_with(RELAY_MANIFEST, RELAY_SIG, &test_key()).unwrap();
        let e = verify_signature(RELAY_MANIFEST, RELAY_SIG).unwrap_err();
        assert!(e.starts_with(R_SIGNATURE), "{e}");
    }

    #[test]
    fn an_unusable_relay_makes_the_whole_manifest_malformed() {
        let mut text = String::from_utf8(RELAY_MANIFEST.to_vec()).unwrap();
        text = text.replace(RELAY_ADDR, "192.0.2.10:7100 ; comment");
        let e = parse_manifest(text.as_bytes()).unwrap_err();
        assert!(e.starts_with(R_MALFORMED), "{e}");
        assert!(e.contains("relay"), "{e}");
        let short = String::from_utf8(RELAY_MANIFEST.to_vec())
            .unwrap()
            .replace(RELAY_KEY, "abc");
        assert!(parse_manifest(short.as_bytes()).is_err());
        // An explicit null is "no relay", not an error: a generator may spell absence that way.
        let null = String::from_utf8(RELAY_MANIFEST.to_vec())
            .unwrap()
            .replace(
                &format!("\"relay\": {{\n    \"addr\": \"{RELAY_ADDR}\",\n    \"key\": \"{RELAY_KEY}\"\n  }}"),
                "\"relay\": null",
            );
        assert!(
            null.contains("\"relay\": null"),
            "the replace must have matched: {null}"
        );
        assert!(parse_manifest(null.as_bytes()).unwrap().relay.is_none());
    }

    // ---- dist LA8: Play installs what is missing -----------------------------------------------

    /// A manifest whose three assets are REAL zips in `tests/data/` (a dll, an ini, a README,
    /// plus `mh_harness.dll` in the debug ones and `libmh.dll` in the brokered one), carrying a
    /// relay, signed by a throwaway key (secret half discarded). Everything the Play path does --
    /// signature, digest, unpack, receipt, relay -- runs for real over these bytes.
    const PLAY_MANIFEST: &[u8] = include_bytes!("../tests/data/manifest_play.json");
    const PLAY_SIG: &str = include_str!("../tests/data/manifest_play.json.minisig");
    const PLAY_PUB: &str = include_str!("../tests/data/manifest_play.pub");
    const PLAY_ZIP_NET: &[u8] = include_bytes!("../tests/data/mission_humanity_re-0.3.0-net.zip");
    const PLAY_ZIP_NET_DEBUG: &[u8] =
        include_bytes!("../tests/data/mission_humanity_re-0.3.0-net-debug.zip");
    const PLAY_ZIP_BROKERED: &[u8] =
        include_bytes!("../tests/data/mission_humanity_re-0.3.0-brokered-debug.zip");

    fn play_key() -> String {
        PLAY_PUB
            .lines()
            .find(|l| !l.starts_with("untrusted comment"))
            .unwrap()
            .trim()
            .to_string()
    }

    /// The stand-in server: the manifest (or a variant of it) plus the three fixture zips.
    fn play_server(manifest: &[u8], sig: &str) -> Canned {
        Canned(vec![
            (format!("{TEST_BASE}manifest.json"), manifest.to_vec()),
            (
                format!("{TEST_BASE}manifest.json.minisig"),
                sig.as_bytes().to_vec(),
            ),
            (
                format!("{TEST_BASE}mission_humanity_re-0.3.0-net.zip"),
                PLAY_ZIP_NET.to_vec(),
            ),
            (
                format!("{TEST_BASE}mission_humanity_re-0.3.0-net-debug.zip"),
                PLAY_ZIP_NET_DEBUG.to_vec(),
            ),
            (
                format!("{TEST_BASE}mission_humanity_re-0.3.0-brokered-debug.zip"),
                PLAY_ZIP_BROKERED.to_vec(),
            ),
        ])
    }

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

    /// dist LA8 done_when (1): *on a game directory with nothing installed, Host with `net-debug`
    /// selected ends with that zip's files installed (receipt names the tag) and the relay
    /// provisioned* -- the launch itself is `app.rs`'s, and it only starts once this returns.
    #[test]
    fn host_on_an_empty_directory_installs_the_chosen_configuration_and_the_relay() {
        let root = std::env::temp_dir().join("mh_launcher_test_play_install");
        let _ = std::fs::remove_dir_all(&root);
        let layout = Layout::rooted(root.join("state"));
        let game = play_game_dir(&root);
        let server = play_server(PLAY_MANIFEST, PLAY_SIG);
        let steps = std::sync::Mutex::new(Vec::<String>::new());
        let progress = |m: &str| steps.lock().unwrap().push(m.to_string());

        assert_eq!(readiness(&game, "net-debug"), Readiness::NeedsInstall);
        let applied = make_ready_with_key(
            &layout,
            &server,
            TEST_BASE,
            &game,
            "net-debug",
            &progress,
            &play_key(),
        )
        .expect("the install path succeeds")
        .expect("something was installed");
        assert_eq!(
            (applied.version.as_str(), applied.tag.as_str()),
            ("0.3.0", "net-debug")
        );

        let receipt = install::read_manifest(&game).expect("a receipt was written");
        assert_eq!(receipt.tag, "net-debug");
        assert_eq!(receipt.version, "0.3.0");
        assert!(
            game.join("mh_harness.dll").is_file(),
            "the debug zip's extra file"
        );
        assert_eq!(read(game.join("mh.dll")), "fixture mh.dll (net-debug)");
        assert_eq!(
            read(game.join(format!("mh.dll{}", crate::paths::BACKUP_SUFFIX))),
            "the game's own dll",
            "the game's dll is parked, not lost"
        );
        // The relay from the signed manifest, provisioned (LA6) as part of the same step.
        let ini = read(game.join(crate::relay::INI_NAME));
        assert!(ini.contains("transport=udp"), "{ini}");
        assert!(ini.contains("relay=192.0.2.10:7100"), "{ini}");
        assert!(
            ini.contains("sp_clock_log=1"),
            "the DEBUG ini, with its logging key: {ini}"
        );
        assert!(read(game.join(crate::relay::KEY_NAME)).starts_with(RELAY_KEY));
        // And the accepted copy is there for the launch path to re-read the relay from.
        assert!(load_accepted_with_key(&layout, &play_key()).is_some());
        // The page was told what was happening, in order.
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
        // A second Host is a plain launch: the receipt says it is there.
        assert_eq!(readiness(&game, "net-debug"), Readiness::Ready);
        assert!(make_ready_with_key(
            &layout,
            &server,
            TEST_BASE,
            &game,
            "net-debug",
            &no_progress,
            &play_key()
        )
        .unwrap()
        .is_none());
        let _ = std::fs::remove_dir_all(&root);
    }

    /// dist LA8 done_when (2): *switching the picker to `net` and pressing Host again swaps the
    /// install (mh_harness.dll gone, receipt updated)* -- an uninstall of the old set, then the
    /// install of the new one, with the game's own dll still parked as `.mhbak`.
    #[test]
    fn switching_the_configuration_uninstalls_the_old_set_and_installs_the_new_one() {
        let root = std::env::temp_dir().join("mh_launcher_test_play_switch");
        let _ = std::fs::remove_dir_all(&root);
        let layout = Layout::rooted(root.join("state"));
        let game = play_game_dir(&root);
        let server = play_server(PLAY_MANIFEST, PLAY_SIG);
        let key = play_key();
        make_ready_with_key(
            &layout,
            &server,
            TEST_BASE,
            &game,
            "net-debug",
            &no_progress,
            &key,
        )
        .unwrap()
        .unwrap();
        assert!(game.join("mh_harness.dll").is_file());

        assert_eq!(
            readiness(&game, "net"),
            Readiness::NeedsSwitch {
                from: "net-debug".into()
            }
        );
        let applied = make_ready_with_key(
            &layout,
            &server,
            TEST_BASE,
            &game,
            "net",
            &no_progress,
            &key,
        )
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
        let ini = read(game.join(crate::relay::INI_NAME));
        assert!(ini.contains("relay=192.0.2.10:7100"), "{ini}");
        assert!(!ini.contains("sp_clock_log"), "the plain ini now: {ini}");
        assert_eq!(readiness(&game, "net"), Readiness::Ready);
        // And all the way back to the game's own files.
        let un = install::uninstall(&game).unwrap();
        assert!(un.restored.contains(&"mh.dll".to_string()), "{un:?}");
        assert_eq!(read(game.join("mh.dll")), "the game's own dll");
        let _ = std::fs::remove_dir_all(&root);
    }

    /// dist LA8 done_when (3): *a manifest lacking the chosen tag shows the MALFORMED notice and
    /// installs nothing* -- and, on a switch, UNINSTALLS nothing either. The fixture is the same
    /// manifest with only its `net` asset, signed by the same throwaway key (the generator refuses
    /// to build a partial set, so this one was signed by hand for exactly this test).
    #[test]
    fn a_manifest_without_the_chosen_tag_is_malformed_and_touches_nothing() {
        const NET_ONLY: &[u8] = include_bytes!("../tests/data/manifest_play_netonly.json");
        const NET_ONLY_SIG: &str = include_str!("../tests/data/manifest_play_netonly.json.minisig");
        let root = std::env::temp_dir().join("mh_launcher_test_play_missing_tag");
        let _ = std::fs::remove_dir_all(&root);
        let layout = Layout::rooted(root.join("state"));
        let game = play_game_dir(&root);
        let key = play_key();
        let only_net = parse_manifest(NET_ONLY).unwrap();
        assert_eq!(only_net.game.keys().collect::<Vec<_>>(), vec!["net"]);
        let server = play_server(NET_ONLY, NET_ONLY_SIG);

        let e = make_ready_with_key(
            &layout,
            &server,
            TEST_BASE,
            &game,
            "net-debug",
            &no_progress,
            &key,
        )
        .unwrap_err();
        assert!(e.starts_with(R_MALFORMED), "{e}");
        assert!(e.contains("net-debug"), "{e}");
        assert!(install::read_manifest(&game).is_none(), "nothing installed");
        assert_eq!(read(game.join("mh.dll")), "the game's own dll");
        assert!(
            !game.join(crate::relay::INI_NAME).exists(),
            "nothing provisioned"
        );
        assert!(!game.join(crate::relay::KEY_NAME).exists());
        assert!(version_dirs(&layout).is_empty(), "nothing staged");

        // On a directory holding `net`, the same refusal must not uninstall it on the way.
        make_ready_with_key(
            &layout,
            &server,
            TEST_BASE,
            &game,
            "net",
            &no_progress,
            &key,
        )
        .unwrap()
        .unwrap();
        let e = make_ready_with_key(
            &layout,
            &server,
            TEST_BASE,
            &game,
            "brokered-debug",
            &no_progress,
            &key,
        )
        .unwrap_err();
        assert!(e.starts_with(R_MALFORMED), "{e}");
        assert_eq!(
            install::read_manifest(&game).unwrap().tag,
            "net",
            "still installed"
        );
        assert_eq!(read(game.join("mh.dll")), "fixture mh.dll (net)");
        let _ = std::fs::remove_dir_all(&root);
    }

    /// The accepted copy: written by `check`, re-verified by `load_accepted`, and IGNORED the
    /// moment a byte of it changes -- the relay a launch writes is always one the key signed for.
    #[test]
    fn the_accepted_manifest_round_trips_and_an_edited_copy_is_ignored() {
        let root = std::env::temp_dir().join("mh_launcher_test_accepted_relay");
        let _ = std::fs::remove_dir_all(&root);
        let layout = Layout::rooted(&root);
        let key = test_key();
        assert!(
            load_accepted_with_key(&layout, &key).is_none(),
            "nothing kept yet"
        );

        remember_accepted(&layout, RELAY_MANIFEST, RELAY_SIG).unwrap();
        let m = load_accepted_with_key(&layout, &key).expect("the copy verifies");
        assert_eq!(m.relay.as_ref().map(|r| r.addr.as_str()), Some(RELAY_ADDR));
        // Under the release key the same copy is nothing.
        assert!(load_accepted(&layout).is_none());

        // Re-point the relay by hand: the signature no longer covers the bytes, so no relay.
        let edited = String::from_utf8(RELAY_MANIFEST.to_vec())
            .unwrap()
            .replace(RELAY_ADDR, "198.51.100.7:7100");
        std::fs::write(layout.accepted_manifest(), edited).unwrap();
        assert!(load_accepted_with_key(&layout, &key).is_none());
        let _ = std::fs::remove_dir_all(&root);
    }
}
