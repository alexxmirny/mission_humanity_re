//! Sending a report to the collector (dist **RP1**, plan decisions D13 + D14).
//!
//! LA4 builds a zip and stops. This module is the other half: one `POST /v1/reports`, signed,
//! over a TLS connection whose trust anchor is pinned into the binary, with one retry and an
//! outbox for what still did not go.
//!
//! ```text
//!   prepare(zip)        read the zip back -- its bytes, its sha256, its entry list,
//!        |              its OWN description.txt and report.json
//!   consent_text()      the screen/lines the player sees before anything leaves the machine
//!        |
//!   send()              POST multipart {description, meta, report}  --  attempt 1
//!        |                  X  retryable (socket, 5xx, 429, 408)  ->  attempt 2 after 2s
//!        |                  X  again      ->  Outbox::keep()  ->  re-offered next launch
//!        +-- 201 {"status":"ok","ulid":...,"sha256":...}   /   200 {"status":"duplicate",...}
//! ```
//!
//! ## What is posted is read back OUT OF THE ZIP, not carried alongside it
//!
//! `description` and `meta` are not passed in from the caller: `prepare` unzips `description.txt`
//! and `report.json` from the archive it is about to upload and posts those. Two reasons, and the
//! second is the one that matters. (1) `report.json` IS the `meta` object by construction -- LA4's
//! header already says so -- and reading it back means the stored `meta.json` and the copy inside
//! the zip cannot disagree even in principle. (2) **An outbox resend happens in a LATER process**,
//! days later, with no App state and no `report::Built` in memory. A sidecar file next to the zip
//! would be a second thing to keep, to corrupt and to leave behind; the zip already carries both
//! fields, so there is nothing to keep in step.
//!
//! ## TLS: the trust anchor is baked, and the pin is what makes a wrong one fail closed
//!
//! The collector has no domain name yet (plan section 6 answer 4), so Caddy serves its own
//! `tls internal` certificate and this launcher cannot use a public CA. It therefore trusts
//! EXACTLY ONE root -- the one compiled into it -- and nothing else: not the Windows store, not
//! webpki's roots (which `update.rs` uses for the CDN, correctly, because that host does have a
//! public chain).
//!
//! **THE ANCHOR IS THE ROOT, NOT THE LEAF, AND THAT IS MEASURED.** Caddy's internal CA re-issues
//! the leaf about every twelve hours and the intermediate every seven days, each with a fresh key
//! (read off the live edge on 2026-09-18: leaf `notAfter` 12h out, intermediate 7 days, and the
//! served chain is leaf + intermediate -- the root is NOT sent). A leaf SPKI pin would have broken
//! within the day and an intermediate pin within the week; only the root, valid ten years, stays
//! put. Since the root is not in the presented chain it cannot be recognised from the wire either,
//! which is why the certificate itself is baked in rather than merely hashed.
//!
//! So there are two build-time values and they do different jobs:
//!
//! * `MH_REPORT_CA_PEM` -- the root certificate, the actual trust anchor. CI fetches it from the
//!   box (`docker exec mh-caddy cat /data/caddy/pki/authorities/local/root.crt`).
//! * `MH_REPORT_SPKI_PIN` -- the SHA-256 of that root's `SubjectPublicKeyInfo`, hex. The REVIEWED
//!   constant. `Collector::new` hashes the baked certificate's SPKI and refuses to build an agent
//!   at all unless it equals this. A fetch that picked up the wrong file, a re-keyed CA nobody
//!   announced, a workflow variable edited in one place and not the other: all of them fail closed
//!   here, before a socket is opened, instead of silently trusting whatever was baked.
//!
//! Compute the pin the same way the workflow does:
//!
//! ```text
//! openssl x509 -in root.crt -pubkey -noout | openssl pkey -pubin -outform der \
//!   | openssl dgst -sha256
//! ```
//!
//! ## Plain HTTP is refused outright
//!
//! `update.rs` allows one `http://` exception so a stand-in server on `127.0.0.1` can be tested
//! without a second code path. This module does NOT, because it does not need to: a local uvicorn
//! serves TLS with `--ssl-certfile`, so the test path and the production path are the same path.
//! The report is the one thing this launcher sends that is about the player rather than about the
//! game, and a scheme check that has an exception is a scheme check with a way around it.

use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::Duration;

use hmac::{Hmac, Mac};
use sha2::{Digest, Sha256};

use crate::log;

// ---- the baked configuration -------------------------------------------------------------------

/// The collector's base URL, stamped in at BUILD time (`MH_REPORT_URL`), never written here: the
/// tree carries no host, by the same rule that keeps the Pages URL out of `update.rs`
/// (`lint_machine_paths`). A build without it simply has no upload feature -- the Report view says
/// so and the command line refuses, rather than defaulting somewhere.
pub const BAKED_URL: &str = match option_env!("MH_REPORT_URL") {
    Some(v) => v,
    None => "",
};

/// The WRITE-ONLY shared token (`MH_REPORT_TOKEN`), in the spirit of a Sentry DSN (plan D13): it
/// can submit a report and it can do nothing else -- it cannot list, read or delete one. It is
/// baked rather than configured because a player cannot be asked to hold a secret, and it is
/// rotatable because it is only ever compared server-side (`collector/auth.py`), so a new token
/// plus a new launcher release retires the old one.
pub const BAKED_TOKEN: &str = match option_env!("MH_REPORT_TOKEN") {
    Some(v) => v,
    None => "",
};

/// The collector's ROOT CA certificate, PEM (`MH_REPORT_CA_PEM`). See the module header.
pub const BAKED_CA_PEM: &str = match option_env!("MH_REPORT_CA_PEM") {
    Some(v) => v,
    None => "",
};

/// SHA-256 of that root's SubjectPublicKeyInfo, hex (`MH_REPORT_SPKI_PIN`). See the module header.
pub const BAKED_SPKI_PIN: &str = match option_env!("MH_REPORT_SPKI_PIN") {
    Some(v) => v,
    None => "",
};

/// The path the collector serves (`src/collector/collector/app.py`).
pub const ENDPOINT_PATH: &str = "/v1/reports";

/// How many times a report is offered to the collector before it goes to the outbox. TWO: the
/// first try and the one retry RP1's scope names. More would be a client deciding on its own that
/// a service which has refused it twice wants a third connection.
pub const ATTEMPTS: usize = 2;

/// The pause between them. Short enough that a player watching a spinner does not think it hung,
/// long enough to clear a momentary reconnect.
pub const RETRY_DELAY: Duration = Duration::from_secs(2);

/// Whole-request timeout. A 48 MB report on a domestic uplink is minutes, not seconds.
const TIMEOUT: Duration = Duration::from_secs(300);

/// Refusal prefixes, one vocabulary shared by the log, the UI and the tests -- the same discipline
/// `update.rs` uses, and for the same reason: `grep 'refuse PIN' launcher.log` has to answer a
/// support question without anyone reading this file.
pub const R_CONFIG: &str = "refuse CONFIG";
pub const R_SCHEME: &str = "refuse SCHEME";
pub const R_PIN: &str = "refuse PIN";
pub const R_ZIP: &str = "refuse ZIP";
pub const R_SEND: &str = "refuse SEND";

/// A configured destination. Constructing one is the whole of the validation: by the time this
/// exists the URL is https, the token is non-empty, the certificate parses, and its SPKI hash is
/// the pinned one.
#[derive(Clone)]
pub struct Collector {
    base_url: String,
    token: String,
    ca_pem: String,
    pin: String,
}

/// HAND-WRITTEN, NOT DERIVED, and that is the whole reason it exists: a `#[derive(Debug)]` here
/// would print the report token into any log line, panic message or test failure that formatted a
/// `Collector`. The token is the one secret this binary carries.
impl std::fmt::Debug for Collector {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Collector")
            .field("base_url", &self.base_url)
            .field("token", &"<redacted>")
            .field("pin", &self.pin)
            .finish()
    }
}

impl Collector {
    /// The build-time configuration. `Err` when this build was not given one -- which is the
    /// ordinary state of a local `cargo build`, and is why the Report view asks before it offers.
    pub fn baked() -> Result<Self, String> {
        Self::new(BAKED_URL, BAKED_TOKEN, BAKED_CA_PEM, BAKED_SPKI_PIN)
    }

    pub fn new(base_url: &str, token: &str, ca_pem: &str, pin: &str) -> Result<Self, String> {
        let base_url = base_url.trim().trim_end_matches('/').to_string();
        if base_url.is_empty() {
            return Err(format!(
                "{R_CONFIG}: this build has no collector address (MH_REPORT_URL was not set when \
                 it was compiled), so it can build a report but not send one"
            ));
        }
        check_https(&base_url)?;
        if token.trim().is_empty() {
            return Err(format!(
                "{R_CONFIG}: this build has no report token (MH_REPORT_TOKEN)"
            ));
        }
        if ca_pem.trim().is_empty() {
            return Err(format!(
                "{R_CONFIG}: this build has no collector root certificate (MH_REPORT_CA_PEM)"
            ));
        }
        let pin = pin.trim().to_ascii_lowercase();
        if pin.len() != 64 || !pin.bytes().all(|b| b.is_ascii_hexdigit()) {
            return Err(format!(
                "{R_CONFIG}: the pinned SPKI hash must be 64 hex digits (sha256), got {} \
                 character(s)",
                pin.len()
            ));
        }
        let ca_pem = normalize_pem(ca_pem);
        let cert = ureq::tls::Certificate::from_pem(ca_pem.as_bytes())
            .map_err(|e| format!("{R_CONFIG}: the baked collector certificate is not PEM: {e}"))?;
        let actual = spki_sha256_hex(cert.der())?;
        if actual != pin {
            return Err(format!(
                "{R_PIN}: the certificate compiled into this build hashes to {actual}, and the \
                 pin says {pin}. One of the two build variables is wrong -- nothing is sent."
            ));
        }
        Ok(Self {
            base_url,
            token: token.trim().to_string(),
            ca_pem,
            pin,
        })
    }

    pub fn endpoint(&self) -> String {
        format!("{}{ENDPOINT_PATH}", self.base_url)
    }

    /// The SPKI hash this build will accept, for the log line that makes a trust failure
    /// diagnosable from a player's `launcher.log` alone.
    pub fn pin(&self) -> &str {
        &self.pin
    }

    /// The agent that opens the socket. One trust anchor, no proxy, no pool -- the reasoning for
    /// the last two is `update::Http::new`'s and is not repeated here beyond naming it: a
    /// destination an environment variable can move is not the destination this file states, and a
    /// pooled socket is a stale-socket failure class a program making one request cannot benefit
    /// from entering.
    pub fn agent(&self) -> Result<ureq::Agent, String> {
        let cert = ureq::tls::Certificate::from_pem(self.ca_pem.as_bytes())
            .map_err(|e| format!("{R_CONFIG}: the baked collector certificate is not PEM: {e}"))?;
        let tls = ureq::tls::TlsConfig::builder()
            .root_certs(ureq::tls::RootCerts::Specific(Arc::new(vec![cert])))
            .build();
        let config = ureq::Agent::config_builder()
            .tls_config(tls)
            .timeout_global(Some(TIMEOUT))
            .user_agent(format!("mh_launcher/{}", crate::version::VERSION))
            .proxy(None)
            .max_idle_connections(0)
            // NO REDIRECTS. The update path follows them because a release asset URL legitimately
            // redirects to an object store; a report POST has one destination, and a 30x here is
            // either a misconfiguration or something trying to move the upload.
            .max_redirects(0)
            .build();
        Ok(config.into())
    }
}

/// `https://` and nothing else, with a host, and no userinfo before it (the classic way a host
/// check is made to read the wrong half of an authority -- `update::check_url` refuses it too).
pub fn check_https(url: &str) -> Result<(), String> {
    let Some((scheme, rest)) = url.split_once("://") else {
        return Err(format!(
            "{R_SCHEME}: {url:?} is not a <scheme>://<host>/<path> URL"
        ));
    };
    if !scheme.eq_ignore_ascii_case("https") {
        return Err(format!(
            "{R_SCHEME}: {url:?} is not https. A report is the one thing this launcher sends that \
             is about the player rather than about the game; it does not travel in the clear, and \
             there is no local exception because a local collector can serve TLS too."
        ));
    }
    let authority = rest.split(['/', '?', '#']).next().unwrap_or_default();
    if authority.is_empty() {
        return Err(format!("{R_SCHEME}: {url:?} has no host"));
    }
    if authority.contains('@') {
        return Err(format!(
            "{R_SCHEME}: {url:?} carries userinfo before its host"
        ));
    }
    Ok(())
}

/// A PEM whose newlines survived being carried through a CI environment variable as `\n`.
///
/// Not cosmetic: `MH_REPORT_CA_PEM` reaches the compiler through a workflow `env:` value, and both
/// shapes turn up in practice -- a YAML block scalar keeps real newlines, a value pasted into a
/// repository variable usually does not. A PEM without line breaks parses as nothing at all, and
/// the failure would read as "the certificate is wrong" rather than "the variable lost its shape".
fn normalize_pem(pem: &str) -> String {
    if pem.contains('\n') {
        pem.to_string()
    } else {
        pem.replace("\\n", "\n")
    }
}

// ---- the SPKI hash -----------------------------------------------------------------------------

/// SHA-256 over a certificate's `SubjectPublicKeyInfo`, DER, as hex.
///
/// The same bytes `openssl x509 -pubkey -noout | openssl pkey -pubin -outform der` writes, which
/// is what makes the pin computable with tools anyone has. Hand-walked rather than parsed with an
/// X.509 crate because the walk is six `skip`s and a slice, and the alternative is a new
/// dependency in the one place in this program that decides whether to trust a server.
///
/// ```text
/// Certificate  ::= SEQUENCE { tbsCertificate, signatureAlgorithm, signatureValue }
/// TBSCertificate ::= SEQUENCE {
///     [0] version OPTIONAL,      <- skipped when present
///     serialNumber,  signature,  issuer,  validity,  subject,
///     subjectPublicKeyInfo       <- these bytes, tag and all
/// }
/// ```
pub fn spki_sha256_hex(cert_der: &[u8]) -> Result<String, String> {
    let (tag, body) = der_element(cert_der, 0).map_err(|e| format!("certificate: {e}"))?;
    if tag != 0x30 {
        return Err("certificate: the outer element is not a SEQUENCE".to_string());
    }
    let (tag, tbs) = der_element(body, 0).map_err(|e| format!("tbsCertificate: {e}"))?;
    if tag != 0x30 {
        return Err("tbsCertificate: not a SEQUENCE".to_string());
    }
    let mut at = 0usize;
    // [0] EXPLICIT version, present on every v3 certificate and absent on a v1 one.
    let (first_tag, _) = der_element(tbs, at).map_err(|e| format!("version: {e}"))?;
    if first_tag == 0xA0 {
        at = der_end(tbs, at).map_err(|e| format!("version: {e}"))?;
    }
    for field in ["serialNumber", "signature", "issuer", "validity", "subject"] {
        at = der_end(tbs, at).map_err(|e| format!("{field}: {e}"))?;
    }
    let end = der_end(tbs, at).map_err(|e| format!("subjectPublicKeyInfo: {e}"))?;
    let spki = &tbs[at..end];
    if spki.first() != Some(&0x30) {
        return Err("subjectPublicKeyInfo: not a SEQUENCE".to_string());
    }
    Ok(hex(&Sha256::digest(spki)))
}

/// `(tag, content)` of the DER element starting at `at`.
fn der_element(buf: &[u8], at: usize) -> Result<(u8, &[u8]), String> {
    let (tag, start, len) = der_header(buf, at)?;
    Ok((tag, &buf[start..start + len]))
}

/// Where the DER element starting at `at` ends.
fn der_end(buf: &[u8], at: usize) -> Result<usize, String> {
    let (_, start, len) = der_header(buf, at)?;
    Ok(start + len)
}

/// `(tag, content start, content length)`. Long-form lengths up to four bytes, which is every
/// certificate that exists; a longer one is refused rather than guessed at.
fn der_header(buf: &[u8], at: usize) -> Result<(u8, usize, usize), String> {
    if at + 2 > buf.len() {
        return Err("truncated".to_string());
    }
    let tag = buf[at];
    let first = buf[at + 1];
    if first < 0x80 {
        let len = first as usize;
        let start = at + 2;
        if start + len > buf.len() {
            return Err("length runs past the end".to_string());
        }
        return Ok((tag, start, len));
    }
    let count = (first & 0x7f) as usize;
    if count == 0 || count > 4 {
        return Err(format!("unsupported DER length form ({count} byte(s))"));
    }
    if at + 2 + count > buf.len() {
        return Err("truncated length".to_string());
    }
    let mut len = 0usize;
    for b in &buf[at + 2..at + 2 + count] {
        len = (len << 8) | *b as usize;
    }
    let start = at + 2 + count;
    if start + len > buf.len() {
        return Err("length runs past the end".to_string());
    }
    Ok((tag, start, len))
}

// ---- the signature -----------------------------------------------------------------------------

type HmacSha256 = Hmac<Sha256>;

/// The scheme `src/collector/collector/signing.py` defines, spelled the same way on this side:
/// hex HMAC-SHA256, keyed by the shared token, over `"{timestamp}\n{sha256_hex(body)}"` where
/// `body` is the WHOLE multipart payload. The unit tests below carry a vector generated by that
/// module, which is the only thing that keeps two implementations of one scheme honest.
pub fn sign(token: &str, timestamp: i64, body: &[u8]) -> String {
    let body_hash = hex(&Sha256::digest(body));
    let message = format!("{timestamp}\n{body_hash}");
    let mut mac = HmacSha256::new_from_slice(token.as_bytes())
        .expect("HMAC accepts a key of any length, including an empty one");
    mac.update(message.as_bytes());
    hex(&mac.finalize().into_bytes())
}

pub fn hex(bytes: &[u8]) -> String {
    let mut s = String::with_capacity(bytes.len() * 2);
    for b in bytes {
        s.push_str(&format!("{b:02x}"));
    }
    s
}

// ---- what is about to be sent ------------------------------------------------------------------

/// A report, read back off the disk, with everything the consent screen has to be able to name.
#[derive(Clone, Debug)]
pub struct Prepared {
    pub zip: PathBuf,
    pub bytes: Vec<u8>,
    pub sha256: String,
    /// `(entry name, uncompressed size)`, in the order the zip holds them.
    pub entries: Vec<(String, u64)>,
    pub description: String,
    /// `report.json`, verbatim -- the `meta` field of the POST.
    pub meta: String,
    pub destination: String,
}

impl Prepared {
    pub fn size(&self) -> usize {
        self.bytes.len()
    }
}

/// Read a built report back and describe it. The zip is the source of every field: nothing here
/// is carried over from the process that built it, so an outbox resend days later produces the
/// same POST as an immediate one.
pub fn prepare(zip: &Path, destination: &str) -> Result<Prepared, String> {
    let bytes =
        std::fs::read(zip).map_err(|e| format!("{R_ZIP}: cannot read {}: {e}", zip.display()))?;
    let sha256 = hex(&Sha256::digest(&bytes));

    let cursor = std::io::Cursor::new(&bytes);
    let mut archive = zip::ZipArchive::new(cursor)
        .map_err(|e| format!("{R_ZIP}: {} is not a readable zip: {e}", zip.display()))?;
    let mut entries = Vec::new();
    for i in 0..archive.len() {
        let e = archive
            .by_index(i)
            .map_err(|e| format!("{R_ZIP}: entry {i} of {}: {e}", zip.display()))?;
        entries.push((e.name().to_string(), e.size()));
    }
    let description = read_entry(&mut archive, "description.txt")?;
    let meta = read_entry(&mut archive, "report.json")?;
    if description.trim().is_empty() {
        return Err(format!(
            "{R_ZIP}: {} carries an empty description, and the collector refuses one (422)",
            zip.display()
        ));
    }
    serde_json::from_str::<serde_json::Value>(&meta)
        .map_err(|e| format!("{R_ZIP}: report.json in {} is not JSON: {e}", zip.display()))?;

    Ok(Prepared {
        zip: zip.to_path_buf(),
        bytes,
        sha256,
        entries,
        description,
        meta,
        destination: destination.to_string(),
    })
}

fn read_entry(
    archive: &mut zip::ZipArchive<std::io::Cursor<&Vec<u8>>>,
    name: &str,
) -> Result<String, String> {
    use std::io::Read;
    let mut e = archive.by_name(name).map_err(|_| {
        format!("{R_ZIP}: there is no {name} in this zip -- it was not built by this launcher")
    })?;
    let mut s = String::new();
    e.read_to_string(&mut s)
        .map_err(|err| format!("{R_ZIP}: cannot read {name}: {err}"))?;
    Ok(s)
}

/// **The consent screen's text, and the one the command line prints.** One function so the two
/// paths cannot say different things about the same zip -- and so what the player agrees to is
/// generated from the bytes that are about to leave rather than from a description of them.
///
/// Every entry is named, with its size, because "the session log directory" is not informed
/// consent and a file list is. `report.json` is included whole for the same reason it is shown in
/// the Report view: it is the part of a report that is about the player's machine.
pub fn consent_text(p: &Prepared) -> String {
    let mut s = String::new();
    s.push_str("This sends the following file, and nothing else, to:\n");
    s.push_str(&format!("    {}\n\n", p.destination));
    s.push_str(&format!("{}\n", p.zip.display()));
    s.push_str(&format!(
        "    {} bytes, sha256 {}\n\n",
        p.bytes.len(),
        p.sha256
    ));
    s.push_str(&format!("It contains {} file(s):\n", p.entries.len()));
    for (name, size) in &p.entries {
        s.push_str(&format!("    {name}  ({size} bytes)\n"));
    }
    s.push_str("\nYour description, sent as written:\n");
    for line in p.description.trim().lines() {
        s.push_str(&format!("    {line}\n"));
    }
    s.push_str("\nreport.json -- what this says about your machine:\n");
    for line in p.meta.lines() {
        s.push_str(&format!("    {line}\n"));
    }
    s.push_str(
        "\nThe multiplayer key is not in it: mh_key.txt is excluded outright and any 64-digit key\n\
         printed into a log was blanked before the log was added. Nothing else on your machine is\n\
         read, and nothing is sent until you say so.\n",
    );
    s
}

// ---- the POST ----------------------------------------------------------------------------------

/// What the collector said when it took the report.
#[derive(Clone, Debug)]
pub struct Accepted {
    pub status: u16,
    pub ulid: String,
    /// The sha256 the COLLECTOR computed. Compared against ours by `send`, because a matching
    /// digest at the receiver is the only proof the bytes that arrived are the bytes that left.
    pub sha256: String,
    pub duplicate: bool,
}

impl Accepted {
    pub fn summary(&self) -> String {
        if self.duplicate {
            format!(
                "the collector already had this report ({}), sha256 {}",
                self.ulid, self.sha256
            )
        } else {
            format!(
                "sent ({}) -- the collector stored {} ({})",
                self.status, self.ulid, self.sha256
            )
        }
    }
}

/// Why an attempt failed, and -- the part that decides everything after it -- whether trying again
/// could ever help.
#[derive(Clone, Debug)]
pub enum Failed {
    /// A socket, a TLS refusal, a timeout, a 5xx, a 429 or a 408. Another launch might work.
    Retryable(String),
    /// A 4xx that is about this request rather than about the moment: a 413 (too big), a 422
    /// (malformed), a 401 (the token this build carries is not the one the collector wants). A
    /// retry would send the identical bytes to the identical refusal, so it is not kept.
    Refused(String),
}

impl Failed {
    pub fn message(&self) -> &str {
        match self {
            Failed::Retryable(m) | Failed::Refused(m) => m,
        }
    }
    pub fn is_retryable(&self) -> bool {
        matches!(self, Failed::Retryable(_))
    }
}

/// One attempt. Exposed so a test can drive a single POST without the retry and the outbox.
pub fn post_once(agent: &ureq::Agent, c: &Collector, p: &Prepared) -> Result<Accepted, Failed> {
    let boundary = boundary_for(&p.bytes);
    let body = multipart(&boundary, p);
    let timestamp = chrono::Utc::now().timestamp();
    let signature = sign(&c.token, timestamp, &body);
    let url = c.endpoint();

    log::line(format!(
        "upload: POST {url} -- {} bytes, sha256 {}",
        body.len(),
        p.sha256
    ));
    let sent = agent
        .post(&url)
        .content_type(format!("multipart/form-data; boundary={boundary}"))
        .header("X-Report-Token", &c.token)
        .header("X-Report-Timestamp", timestamp.to_string())
        .header("X-Report-Signature", &signature)
        .send(&body[..]);

    let mut response = match sent {
        Ok(r) => r,
        Err(ureq::Error::StatusCode(code)) => {
            return Err(classify(code, "(no body)"));
        }
        Err(e) => {
            // Everything that is not an HTTP status: DNS, connect, the TLS handshake (a collector
            // presenting a certificate this build does not pin lands HERE), a timeout.
            let text = e.to_string();
            // The hint is attached only to the error it explains. Appending "the certificate is
            // not the pinned one" to a connection-refused is how a support reader is sent to
            // chase a trust problem that is really a service that is not running.
            let hint = if text.to_ascii_lowercase().contains("certificate") {
                " -- this collector is not the one this build pins (see `refuse PIN` and the \
                 launcher page of the maintainer docs)"
            } else {
                ""
            };
            return Err(Failed::Retryable(format!(
                "{R_SEND}: {url} -- {text}{hint}"
            )));
        }
    };
    let code = response.status().as_u16();
    let text = response
        .body_mut()
        .read_to_string()
        .unwrap_or_else(|e| format!("(the body could not be read: {e})"));
    if !(200..300).contains(&code) {
        return Err(classify(code, &text));
    }
    let v: serde_json::Value = serde_json::from_str(&text).map_err(|e| {
        Failed::Retryable(format!("{R_SEND}: {code} but the body is not JSON: {e}"))
    })?;
    let accepted = Accepted {
        status: code,
        ulid: v["ulid"].as_str().unwrap_or_default().to_string(),
        sha256: v["sha256"].as_str().unwrap_or_default().to_string(),
        duplicate: v["status"].as_str() == Some("duplicate"),
    };
    if accepted.sha256 != p.sha256 {
        return Err(Failed::Refused(format!(
            "{R_SEND}: the collector stored sha256 {} and we sent {} -- the bytes that arrived are \
             not the bytes that left",
            accepted.sha256, p.sha256
        )));
    }
    Ok(accepted)
}

/// Which HTTP statuses are worth another launch. 429 and 408 are the moment, not the request; 5xx
/// is the service; everything else in the 4xx range is this request and will be refused again.
fn classify(code: u16, body: &str) -> Failed {
    let body = body.trim();
    let msg = format!("{R_SEND}: the collector answered {code} -- {body}");
    if code >= 500 || code == 429 || code == 408 {
        Failed::Retryable(msg)
    } else {
        Failed::Refused(msg)
    }
}

/// The multipart body, byte for byte what `src/collector/scripts/send_report.py::build_multipart`
/// produces: CRLF line endings, the three fields in the order `description`, `meta`, `report`, and
/// the file part carrying a filename and `application/zip`. The HMAC is over exactly these bytes,
/// so the body is assembled once and both signed and sent -- never rebuilt between the two.
fn multipart(boundary: &str, p: &Prepared) -> Vec<u8> {
    let mut out: Vec<u8> = Vec::with_capacity(p.bytes.len() + 512);
    let field = |name: &str, value: &str, out: &mut Vec<u8>| {
        out.extend_from_slice(format!("--{boundary}\r\n").as_bytes());
        out.extend_from_slice(
            format!("Content-Disposition: form-data; name=\"{name}\"\r\n\r\n").as_bytes(),
        );
        out.extend_from_slice(value.as_bytes());
        out.extend_from_slice(b"\r\n");
    };
    field("description", p.description.trim(), &mut out);
    field("meta", &p.meta, &mut out);

    let filename = p
        .zip
        .file_name()
        .map(|n| n.to_string_lossy().to_string())
        .unwrap_or_else(|| "report.zip".to_string());
    out.extend_from_slice(format!("--{boundary}\r\n").as_bytes());
    out.extend_from_slice(
        format!("Content-Disposition: form-data; name=\"report\"; filename=\"{filename}\"\r\n")
            .as_bytes(),
    );
    out.extend_from_slice(b"Content-Type: application/zip\r\n\r\n");
    out.extend_from_slice(&p.bytes);
    out.extend_from_slice(b"\r\n");
    out.extend_from_slice(format!("--{boundary}--\r\n").as_bytes());
    out
}

/// A boundary that cannot occur inside the body. Derived from the content's own digest rather than
/// from a random source (this crate has none, and does not need one): the only property required
/// is that it does not appear in the payload, and the loop is what guarantees that rather than
/// hopes for it.
fn boundary_for(zip_bytes: &[u8]) -> String {
    let base = hex(&Sha256::digest(zip_bytes));
    for n in 0..16u32 {
        let candidate = format!("mhreport{}{n:x}", &base[..24]);
        if !contains(zip_bytes, candidate.as_bytes()) {
            return candidate;
        }
    }
    format!("mhreport{}", &base[..32])
}

fn contains(haystack: &[u8], needle: &[u8]) -> bool {
    haystack.windows(needle.len()).any(|w| w == needle)
}

/// The whole policy: try, retry once, and keep what still did not go.
///
/// The outbox is the last step rather than the first because a report that went is a report that
/// should not be on the player's disk being re-offered for ever.
pub fn send(c: &Collector, p: &Prepared, outbox: &Outbox) -> Result<Accepted, String> {
    let agent = c.agent()?;
    log::line(format!(
        "upload: {} -- trusting only the root whose SPKI is {}",
        c.endpoint(),
        c.pin()
    ));
    let mut last = String::new();
    for attempt in 1..=ATTEMPTS {
        match post_once(&agent, c, p) {
            Ok(a) => {
                log::line(format!("upload: {} (attempt {attempt})", a.summary()));
                return Ok(a);
            }
            Err(e) => {
                last = e.message().to_string();
                log::line(format!(
                    "upload: attempt {attempt} of {ATTEMPTS} failed -- {last}"
                ));
                if !e.is_retryable() {
                    log::line("upload: that refusal will not change on a retry; not kept");
                    return Err(last);
                }
                if attempt < ATTEMPTS {
                    std::thread::sleep(RETRY_DELAY);
                }
            }
        }
    }
    match outbox.keep(&p.zip) {
        Ok(kept) => Err(format!(
            "{last}\nKept for later: {} -- this launcher will offer it again next time it starts.",
            kept.display()
        )),
        Err(e) => Err(format!("{last}\nand it could not even be kept: {e}")),
    }
}

// ---- the outbox --------------------------------------------------------------------------------

/// `%LOCALAPPDATA%\MissionHumanity\reports\outbox\` -- reports that were built and not accepted.
///
/// A directory of zips and nothing else: no index, no state file, no sidecars. What has to survive
/// a reboot is "this zip has not been sent", and a file's existence says that with no second
/// source of truth to fall out of step (and no way for a half-written index to lose a report that
/// is sitting right there). The zips are self-describing -- see the module header.
pub struct Outbox {
    dir: PathBuf,
}

impl Outbox {
    /// `reports_dir/outbox`.
    pub fn new(reports_dir: &Path) -> Self {
        Self {
            dir: reports_dir.join("outbox"),
        }
    }

    pub fn dir(&self) -> &Path {
        &self.dir
    }

    /// Put a report in. A rename when the zip is already under the reports directory (the ordinary
    /// case, and atomic), a copy when it is not -- a zip the player put somewhere of their own
    /// choosing must not be moved out from under them.
    pub fn keep(&self, zip: &Path) -> Result<PathBuf, String> {
        std::fs::create_dir_all(&self.dir)
            .map_err(|e| format!("cannot create {}: {e}", self.dir.display()))?;
        let leaf = zip
            .file_name()
            .ok_or_else(|| format!("{} has no file name", zip.display()))?;
        let dest = self.dir.join(leaf);
        if dest == zip {
            return Ok(dest);
        }
        // A report this launcher built lives under `reports\` and is MOVED (a rename, atomic, and
        // it does not leave the same zip on disk twice). A path the player named themselves --
        // `--send <a zip of their own>` -- is COPIED: their file must not disappear because the
        // collector happened to be down.
        let ours = self
            .dir
            .parent()
            .map(|reports| zip.starts_with(reports))
            .unwrap_or(false);
        if ours && std::fs::rename(zip, &dest).is_ok() {
            log::line(format!(
                "upload: kept {} for a later launch",
                dest.display()
            ));
            return Ok(dest);
        }
        std::fs::copy(zip, &dest)
            .map_err(|e| format!("cannot copy {} to {}: {e}", zip.display(), dest.display()))?;
        log::line(format!(
            "upload: kept a copy of {} for a later launch",
            dest.display()
        ));
        Ok(dest)
    }

    /// What is waiting, oldest name first. The report file names are UTC stamps, so that is
    /// chronological -- the same property `paths::newest_session_dir` leans on.
    pub fn pending(&self) -> Vec<PathBuf> {
        let Ok(read) = std::fs::read_dir(&self.dir) else {
            return Vec::new();
        };
        let mut out: Vec<PathBuf> = read
            .flatten()
            .map(|e| e.path())
            .filter(|p| p.is_file() && p.extension().is_some_and(|e| e.eq_ignore_ascii_case("zip")))
            .collect();
        out.sort();
        out
    }

    /// Drop one, once the collector has it.
    pub fn done(&self, zip: &Path) -> Result<(), String> {
        if zip.starts_with(&self.dir) {
            std::fs::remove_file(zip)
                .map_err(|e| format!("cannot remove {}: {e}", zip.display()))?;
            log::line(format!(
                "upload: {} has been accepted; removed",
                zip.display()
            ));
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A self-signed P-256 certificate, made once with
    /// `openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes ...`, whose SPKI hash
    /// was computed with the openssl pipeline this module's header documents. It is the same shape
    /// as the certificate the pin is really taken from (Caddy's local root is ECC too) and it
    /// carries v3 extensions, so the `[0] version` branch of the walk is exercised.
    const FIXTURE_PEM: &str = include_str!("../tests/data/spki_fixture.pem");
    const FIXTURE_SPKI_SHA256: &str =
        "fda955fdd39e1941ad6026e465b6f68f3fa07f71b7741d06a1e4bf10f69e3879";

    fn fixture_collector(url: &str) -> Result<Collector, String> {
        Collector::new(url, "a-token", FIXTURE_PEM, FIXTURE_SPKI_SHA256)
    }

    /// The walk, against a value openssl produced. This is the whole trust decision: a hash that
    /// is right for the wrong bytes would pin nothing.
    #[test]
    fn the_spki_hash_is_the_one_openssl_computes() {
        let cert = ureq::tls::Certificate::from_pem(FIXTURE_PEM.as_bytes()).unwrap();
        assert_eq!(spki_sha256_hex(cert.der()).unwrap(), FIXTURE_SPKI_SHA256);
    }

    /// Garbage in must not become a hash. A DER walk that ran off the end and returned SOMETHING
    /// would be a pin that accepts anything.
    #[test]
    fn a_thing_that_is_not_a_certificate_has_no_spki_hash() {
        for bad in [
            vec![],
            vec![0x30],
            vec![0x30, 0x03, 0x02, 0x01, 0x00],
            vec![0x02, 0x01, 0x00],
            vec![0x30, 0x82, 0xff, 0xff, 0x30, 0x00],
        ] {
            assert!(spki_sha256_hex(&bad).is_err(), "{bad:?}");
        }
    }

    /// **The vector, generated by the server's own module** -- the only thing that keeps two
    /// implementations of one scheme from drifting:
    ///
    /// ```text
    /// python -c "import sys; sys.path.insert(0,'src/collector');
    ///            from collector.signing import sign;
    ///            print(sign('a-token', '1758153600', b'the body'))"
    /// ```
    #[test]
    fn the_signature_matches_the_collectors_own_signing_py() {
        assert_eq!(
            sign("a-token", 1_758_153_600, b"the body"),
            "0430c8d5fadce24e20bd0d507a2c009c6fec13881abc8167f4a7336616b15a88"
        );
        // The empty body, which is the case a length shortcut gets wrong.
        assert_eq!(
            sign("a-token", 0, b""),
            "f09da0473f5658cdf888440d2d57ea2b9ca555c1cdb747b578de58f9fb667aa3"
        );
    }

    /// The scheme check, which has no local exception -- that is the point of it.
    #[test]
    fn only_https_is_a_destination() {
        assert!(check_https("https://collector.invalid").is_ok());
        assert!(check_https("https://127.0.0.1:8443").is_ok());
        for bad in [
            "http://collector.invalid",
            "http://127.0.0.1:8000",
            "https://user@collector.invalid",
            "https://",
            "collector.invalid",
            "ftp://collector.invalid",
            "",
        ] {
            assert!(check_https(bad).is_err(), "{bad:?} should be refused");
        }
    }

    /// A build with no collector says so instead of defaulting somewhere, and every half-set
    /// configuration is refused rather than half-honoured.
    #[test]
    fn an_unconfigured_build_refuses_to_send() {
        assert!(Collector::new("", "t", FIXTURE_PEM, FIXTURE_SPKI_SHA256).is_err());
        assert!(Collector::new("https://x.invalid", "", FIXTURE_PEM, FIXTURE_SPKI_SHA256).is_err());
        assert!(Collector::new("https://x.invalid", "t", "", FIXTURE_SPKI_SHA256).is_err());
        assert!(Collector::new("https://x.invalid", "t", FIXTURE_PEM, "").is_err());
        assert!(Collector::new("https://x.invalid", "t", FIXTURE_PEM, "not-hex").is_err());
        assert!(fixture_collector("https://x.invalid").is_ok());
    }

    /// THE PIN IS LOAD-BEARING: a certificate that is not the pinned one is refused before a
    /// socket is opened, and the message names both hashes so the mismatch is diagnosable without
    /// a debugger.
    #[test]
    fn a_certificate_that_is_not_the_pinned_one_is_refused() {
        let wrong = "0".repeat(64);
        let err = Collector::new("https://x.invalid", "t", FIXTURE_PEM, &wrong).unwrap_err();
        assert!(err.starts_with(R_PIN), "{err}");
        assert!(err.contains(FIXTURE_SPKI_SHA256), "{err}");
        // Upper case is the same pin; a hex digest's case is not a fact about the key.
        assert!(Collector::new(
            "https://x.invalid",
            "t",
            FIXTURE_PEM,
            &FIXTURE_SPKI_SHA256.to_ascii_uppercase()
        )
        .is_ok());
    }

    /// A PEM that lost its newlines to a CI variable still parses. The failure this prevents reads
    /// as "the certificate is wrong", which sends the reader to the wrong file entirely.
    #[test]
    fn a_pem_flattened_by_a_ci_variable_is_restored() {
        let flat = FIXTURE_PEM.replace('\n', "\\n");
        assert!(!flat.contains('\n'));
        let c = Collector::new("https://x.invalid", "t", &flat, FIXTURE_SPKI_SHA256).unwrap();
        assert!(c.agent().is_ok());
    }

    #[test]
    fn the_endpoint_is_the_base_plus_the_collectors_path() {
        let c = fixture_collector("https://x.invalid/").unwrap();
        assert_eq!(c.endpoint(), "https://x.invalid/v1/reports");
        let c = fixture_collector("https://x.invalid:8443").unwrap();
        assert_eq!(c.endpoint(), "https://x.invalid:8443/v1/reports");
    }

    // ---- a real zip, read back the way the uploader reads it -------------------------------

    fn temp_dir(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("mh_launcher_test_{name}"));
        std::fs::remove_dir_all(&dir).ok();
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    fn a_report(dir: &Path, name: &str, description: &str) -> PathBuf {
        let zip_path = dir.join(name);
        let input = crate::report::Input {
            game_dir: None,
            session_dir: None,
            launcher_started_utc: None,
            description,
            last_run: None,
            crash: None,
            minidump: None,
            launcher_log: None,
        };
        crate::report::build(&zip_path, &input).unwrap();
        zip_path
    }

    /// `prepare` takes the description and the meta OUT OF THE ZIP. This is what makes an outbox
    /// resend in a later process identical to an immediate send.
    #[test]
    fn what_is_posted_comes_out_of_the_zip_itself() {
        let dir = temp_dir("upload_prepare");
        let zip = a_report(
            &dir,
            "r.zip",
            "the lobby froze when the second player joined",
        );
        let p = prepare(&zip, "https://x.invalid/v1/reports").unwrap();

        assert_eq!(
            p.description,
            "the lobby froze when the second player joined"
        );
        let meta: serde_json::Value = serde_json::from_str(&p.meta).unwrap();
        assert!(meta.get("launcher_version").is_some(), "{}", p.meta);
        assert_eq!(p.sha256, hex(&Sha256::digest(std::fs::read(&zip).unwrap())));
        assert!(p.entries.iter().any(|(n, _)| n == "report.json"));
        assert!(p.entries.iter().any(|(n, _)| n == "description.txt"));

        // The consent text names the destination, the digest and every entry -- that is what makes
        // it consent rather than a notification.
        let text = consent_text(&p);
        assert!(text.contains("https://x.invalid/v1/reports"), "{text}");
        assert!(text.contains(&p.sha256), "{text}");
        for (name, _) in &p.entries {
            assert!(text.contains(name), "{name} missing from:\n{text}");
        }
        assert!(text.contains("the lobby froze when the second player joined"));
        assert!(text.contains("nothing is sent until you say so"));
        std::fs::remove_dir_all(&dir).ok();
    }

    /// Something that is not one of our reports is refused here rather than by the collector's
    /// 422 -- the player should not need a round trip to be told the file is wrong.
    #[test]
    fn a_zip_that_is_not_a_report_is_refused_before_the_socket() {
        let dir = temp_dir("upload_not_a_report");
        let path = dir.join("random.zip");
        std::fs::write(&path, b"PK\x03\x04 not really").unwrap();
        let err = prepare(&path, "https://x.invalid").unwrap_err();
        assert!(err.starts_with(R_ZIP), "{err}");

        let missing = dir.join("nope.zip");
        assert!(prepare(&missing, "https://x.invalid").is_err());
        std::fs::remove_dir_all(&dir).ok();
    }

    /// The body the HMAC is computed over, in the exact shape `send_report.py` builds -- field
    /// order, CRLFs, the file part's headers, the closing boundary.
    #[test]
    fn the_multipart_body_is_the_shape_the_collector_parses() {
        let dir = temp_dir("upload_multipart");
        let zip = a_report(&dir, "mh_report_20260918T120000Z.zip", "a description");
        let p = prepare(&zip, "https://x.invalid/v1/reports").unwrap();
        let boundary = boundary_for(&p.bytes);
        let body = multipart(&boundary, &p);
        // Over the WHOLE body, not a prefix: report.json is several hundred bytes, so the file
        // part begins well past any fixed head slice (the first spelling of this test asserted
        // against 600 bytes and missed the very part it is about).
        let has = |needle: &str| contains(&body, needle.as_bytes());

        assert!(body.starts_with(format!("--{boundary}\r\n").as_bytes()));
        assert!(has(
            "Content-Disposition: form-data; name=\"description\"\r\n\r\na description\r\n"
        ));
        assert!(has("name=\"meta\""));
        assert!(has(
            "name=\"report\"; filename=\"mh_report_20260918T120000Z.zip\"\r\nContent-Type: application/zip\r\n\r\n"
        ));
        let tail = String::from_utf8_lossy(&body[body.len() - 100..]).to_string();
        assert!(tail.ends_with(&format!("\r\n--{boundary}--\r\n")), "{tail}");
        // The zip is in there whole and unmodified.
        assert!(contains(&body, &p.bytes));
        // And the boundary genuinely does not occur inside the payload.
        assert!(!contains(&p.bytes, boundary.as_bytes()));
        std::fs::remove_dir_all(&dir).ok();
    }

    /// The outbox: what goes in comes back out, in stamp order, and a sent one leaves.
    #[test]
    fn the_outbox_keeps_what_did_not_go_and_offers_it_back() {
        let dir = temp_dir("upload_outbox");
        let reports = dir.join("reports");
        let outbox = Outbox::new(&reports);
        assert!(
            outbox.pending().is_empty(),
            "an empty outbox offers nothing"
        );

        std::fs::create_dir_all(&reports).unwrap();
        let newer = a_report(&reports, "mh_report_20260918T120000Z.zip", "later");
        let older = a_report(&reports, "mh_report_20260917T120000Z.zip", "earlier");
        let kept_newer = outbox.keep(&newer).unwrap();
        let kept_older = outbox.keep(&older).unwrap();
        // A rename, not a copy: the report does not exist twice.
        assert!(!newer.exists());
        assert!(kept_newer.exists() && kept_older.exists());

        let pending = outbox.pending();
        assert_eq!(pending.len(), 2, "{pending:?}");
        assert_eq!(pending[0], kept_older, "oldest stamp first: {pending:?}");

        // A zip from outside the reports directory is COPIED, so the player's own file stays put.
        let elsewhere = a_report(&dir, "elsewhere.zip", "somewhere else");
        outbox.keep(&elsewhere).unwrap();
        assert!(elsewhere.exists(), "the file the player chose was moved");
        assert_eq!(outbox.pending().len(), 3);

        outbox.done(&kept_older).unwrap();
        assert!(!kept_older.exists());
        assert_eq!(outbox.pending().len(), 2);
        // `done` only ever deletes inside the outbox.
        outbox.done(&elsewhere).unwrap();
        assert!(elsewhere.exists());
        std::fs::remove_dir_all(&dir).ok();
    }

    /// A refusal that a retry cannot change must not fill the outbox with reports that will be
    /// refused for ever; a momentary one must.
    #[test]
    fn only_a_retryable_failure_is_worth_keeping() {
        for code in [500u16, 502, 503, 429, 408] {
            assert!(classify(code, "x").is_retryable(), "{code}");
        }
        for code in [400u16, 401, 403, 404, 413, 422] {
            assert!(!classify(code, "x").is_retryable(), "{code}");
        }
        assert!(classify(413, "too big").message().contains("413"));
    }
}
