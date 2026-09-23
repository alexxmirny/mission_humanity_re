// Everything the launcher bakes with `option_env!` must be listed here, or a rebuild after changing
// it silently reuses the old object and ships the PREVIOUS value under the new file name.
//
// Cargo does not track environment variables read through `option_env!` on its own. That was already
// known for MH_LAUNCHER_VERSION -- src/version.rs says why, and this file carried that one line --
// but the same reasoning covers the four `upload.rs`/`update.rs` bake, and they were missing. CI
// never noticed because every Actions run is a cold build in a fresh checkout; it bites only where
// builds are incremental, which is every maintainer's box and, on 2026-09-23, the hand-cut
// v0.2.0-rc2 release that GitHub's billing block forced to be built locally. The failure is silent:
// cargo prints `Finished`, the exe is the old one, and nothing in the artifact says so -- the same
// shape as docs/dead-ends.md G285, where a restored source with an older timestamp left MSBuild
// holding a mutant binary under a clean `git status`.
//
// A baked report token or CA certificate that is one release out of date is not a cosmetic drift:
// the launcher pins TLS against BAKED_CA_PEM and authenticates with BAKED_TOKEN, so a stale pair
// means bug reports silently stop uploading.
fn main() {
    for var in [
        "MH_LAUNCHER_VERSION", // src/version.rs   -- the shipped version string
        "MH_UPDATE_BASE_URL",  // src/update.rs    -- where self-update looks for manifest.json
        "MH_REPORT_URL",       // src/upload.rs    -- the collector's base URL
        "MH_REPORT_TOKEN",     // src/upload.rs    -- the write-only report token
        "MH_REPORT_CA_PEM",    // src/upload.rs    -- the collector's root CA, pinned
        "MH_REPORT_SPKI_PIN",  // src/upload.rs    -- SHA-256 of that root's SubjectPublicKeyInfo
    ] {
        println!("cargo:rerun-if-env-changed={var}");
    }
}
