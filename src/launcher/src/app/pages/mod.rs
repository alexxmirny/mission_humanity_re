//! The five pages (dist RL14), one module each. A page is `fn show(app: &mut App, ui: &mut Ui)`
//! drawn inside the shell's game frame; it reads and changes `App` through the same private fields
//! `app.rs` owns (a child module sees them), and every visible string goes through `i18n::tr`.

pub mod about;
pub mod diagnostics;
pub mod play;
pub mod report;
pub mod settings;
#[cfg(test)]
mod shots;
