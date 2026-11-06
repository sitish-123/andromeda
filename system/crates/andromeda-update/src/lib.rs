//! Shared with andromeda-update-helper: the Arch news feed and its read token.

#[cfg(any(all(feature = "arch", feature = "apt"), not(any(feature = "arch", feature = "apt"))))]
compile_error!("enable one package backend feature: arch or apt");

pub mod news;
pub mod repo;
