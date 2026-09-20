//! The one version string this binary reports about itself.
//!
//! `CARGO_PKG_VERSION` is `0.0.0` from the workspace default and stays that way on purpose: the
//! real version is the RELEASE TAG, stamped in at build time by launcher-release.yml through the
//! `MH_LAUNCHER_VERSION` environment variable, exactly the way release.yml stamps `MhVersion` into
//! the C++ side rather than trusting a source-controlled number. Measured why this has to be one
//! place (v0.1.0-rc2, 2026-09-18): the shipped `mh_launcher-0.1.0-rc2.exe` answered
//! `--verify-binary` with `mh_launcher 0.0.0 ok` while the signed manifest advertised
//! `launcher.version = "0.1.0-rc2"`, so `self_update` would have judged the manifest newer on every
//! start, downloaded its own bytes again, and then failed its own health gate on the mismatch.
//!
//! A build without the variable reports the Cargo default suffixed `-dev`, so a local build is
//! never mistaken for a release and never compares equal to one. `build.rs` declares the variable
//! to Cargo so a stale unstamped object is rebuilt when it is set.

/// `0.1.0-rc2`-shaped when stamped, `0.0.0-dev` otherwise. Read by every line that names the
/// launcher's version: the log banner, `--verify-binary`, the report's `launcher_version`, the
/// User-Agent, and the self-update comparison.
pub const VERSION: &str = match option_env!("MH_LAUNCHER_VERSION") {
    Some(v) => v,
    None => concat!(env!("CARGO_PKG_VERSION"), "-dev"),
};

#[cfg(test)]
mod tests {
    use super::VERSION;

    #[test]
    fn version_is_never_the_bare_cargo_default() {
        // A release is stamped; a dev build says so. Neither is the raw "0.0.0" a manifest could
        // be compared against by accident.
        assert_ne!(VERSION, "0.0.0");
        assert!(VERSION == option_env!("MH_LAUNCHER_VERSION").unwrap_or("0.0.0-dev"));
    }
}
