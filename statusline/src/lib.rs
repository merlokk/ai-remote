//! The status line, as a library — `CLAUDE.md §9`.
//!
//! `src/main.rs` is the Claude Code binary and does nothing this crate does not
//! expose: read the payload ([`json`]), turn it into a line ([`render`]) and a
//! document ([`status`]), put the document on the bus ([`nats`]) unless [`link`]
//! says it is not worth trying. Anything else in the repo that wants the same
//! numbers links the library instead of parsing a terminal line out of a pipe.

pub mod config;
pub mod json;
pub mod link;
pub mod nats;
pub mod render;
pub mod status;
