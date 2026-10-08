//! The declarative settings engine (dist RL13).
//!
//! ```text
//!   settings_schema.json (generated)  ┐
//!   launcher_schema.json (hand)       ┴─ schema.rs ──► Schema { subtabs, rows }
//!   mh_net.ini bytes + LauncherStore ──► model.rs  ──► Model { loaded, edits }  ◄── render.rs (egui)
//!                                         │  Apply
//!                                         └──► ini_io.rs (byte-preserving splice, atomic write)
//!   <game>\lang\*  ──► providers.rs ──► run-time option lists
//! ```
//!
//! Adding a setting means a registry row (`ini_keys.def`, regenerate the schema) plus strings in
//! `i18n/*.toml`; no code in this module changes. WU-C wires `Model` + `render` into the Settings and
//! Diagnostics pages and implements `LauncherStore` over `Config`; until then the engine is exercised
//! by its tests only, hence the `dead_code` allowance.
#![allow(dead_code)]

pub mod ini_io;
pub mod model;
pub mod providers;
pub mod render;
pub mod schema;
#[cfg(test)]
mod shots;
