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
//! `i18n/*.toml`; no code in this module changes. `app/pages/{settings,diagnostics}.rs` draw `Model`
//! through `render`, and `store::ConfigStore` is the `LauncherStore` over the App's `Config`.
//! (`dead_code` is allowed because a schema field is read only by the rows that use it.)
#![allow(dead_code)]

pub mod ini_io;
pub mod model;
pub mod providers;
pub mod render;
pub mod schema;
#[cfg(test)]
mod shots;
pub mod store;
