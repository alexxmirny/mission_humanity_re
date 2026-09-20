// The launcher's version is stamped at build time from MH_LAUNCHER_VERSION (src/version.rs says
// why). Cargo does not track environment variables read through option_env! on its own, so without
// this line a `cargo build` after setting the variable would reuse the unstamped object and ship
// 0.0.0-dev under a release file name.
fn main() {
    println!("cargo:rerun-if-env-changed=MH_LAUNCHER_VERSION");
}
