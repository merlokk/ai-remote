//! `statusline-config.json` — `CLAUDE.md §9.9`.
//!
//! Everything configurable, in one file next to the executable. Not the
//! environment: `settings.json` hands a status line command no arguments and no
//! variables of its own, so tuning it through the environment meant editing a
//! shell profile and restarting the terminal to change a subject name.
//! `statusline-config.json` sits beside the binary and is re-read every render.
//!
//! **Absent is the normal case.** The binary ships without one and every field
//! has a default, so a fresh `cargo build --release` publishes to
//! `nats://127.0.0.1:4222` on `status` with no file at all. The example at the
//! repository root is the copy-and-edit starting point.
//!
//! **Nothing here can break the line.** Unreadable, malformed, or stamped with
//! a schema version this build does not know all fall back to the defaults —
//! `lib/config.py` refuses to load in that situation and is right to, because a
//! half-understood approval config is a security question; a half-understood
//! status line config is a cosmetic one, and the line has to print regardless.

use std::path::PathBuf;
use std::time::Duration;

use serde::{Deserialize, Serialize};

use crate::activity;
use crate::link;
use crate::nats;

/// Bumped when a field changes meaning. Same idea as `lib/config.py`'s `v`,
/// with a fail-*safe* reaction instead of a fail-fast one — see the module docs.
pub const SCHEMA_VERSION: u32 = 1;

/// The file name looked for next to the executable.
pub const FILE_NAME: &str = "statusline-config.json";

/// Every field is `#[serde(default)]`, so a file may set only what it changes
/// and an older file keeps working when a field is added.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct Config {
    pub v: u32,
    /// Publish at all. `false` also removes the connection dot from the line —
    /// with nothing being published there is no link to report (§9.8).
    pub publish: bool,
    /// The NATS server, same default as `lib/bus.py`.
    pub url: String,
    pub subject: String,
    /// Publish the activity documents (§9.10) — what Claude is *doing*, as
    /// opposed to what it is spending. A switch of its own, because it is the
    /// half that puts command text and file paths on the bus: someone may want
    /// the numbers and not that. `publish: false` still switches off both.
    pub activity: bool,
    /// The subject those go to — deliberately not `subject` (§9.10).
    pub activity_subject: String,
    /// Budget for the whole publish — connect, write, flush (§9.7).
    pub timeout_ms: u64,
    /// How long a failed connection is believed before it is retried (§9.8).
    pub retry_after_s: u64,
    /// Print publish failures to stderr. Off by default: a NATS server that is
    /// not running is the normal case here, not an incident.
    pub debug: bool,
}

impl Default for Config {
    fn default() -> Self {
        Config {
            v: SCHEMA_VERSION,
            publish: true,
            url: nats::DEFAULT_URL.to_string(),
            subject: nats::DEFAULT_SUBJECT.to_string(),
            activity: true,
            activity_subject: activity::DEFAULT_SUBJECT.to_string(),
            timeout_ms: nats::DEFAULT_TIMEOUT.as_millis() as u64,
            retry_after_s: link::DEFAULT_RETRY_AFTER.as_secs(),
            debug: false,
        }
    }
}

impl Config {
    /// Read the config, or return the defaults. Never fails: see the module docs.
    pub fn load(path: Option<&std::path::Path>) -> Config {
        let Some(path) = path else { return Config::default() };
        let Ok(text) = std::fs::read_to_string(path) else { return Config::default() };

        match serde_json::from_str::<Config>(&text) {
            // A file from a newer (or older) build may mean something different
            // by the same field names; the defaults are the only safe reading.
            Ok(config) if config.v != SCHEMA_VERSION => Config::default(),
            Ok(config) => config,
            Err(_) => Config::default(),
        }
    }

    /// `statusline-config.json` beside the executable. `None` when the path of
    /// the running binary cannot be determined at all, which reads as "no file".
    pub fn default_path() -> Option<PathBuf> {
        let exe = std::env::current_exe().ok()?;
        Some(exe.parent()?.join(FILE_NAME))
    }

    pub fn settings(&self) -> nats::Settings {
        nats::Settings {
            url: self.url.clone(),
            subject: self.subject.clone(),
            timeout: Duration::from_millis(self.timeout_ms),
            enabled: self.publish,
        }
    }

    /// Where the activity documents go (§9.10). The same server and the same
    /// budget as the line's — only the subject differs, and the extra switch.
    pub fn activity_settings(&self) -> nats::Settings {
        nats::Settings {
            subject: self.activity_subject.clone(),
            enabled: self.publish && self.activity,
            ..self.settings()
        }
    }

    pub fn retry_after(&self) -> Duration {
        Duration::from_secs(self.retry_after_s)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn write(name: &str, contents: &str) -> PathBuf {
        let path = std::env::temp_dir().join(format!("statusline-config-test-{name}.json"));
        std::fs::write(&path, contents).unwrap();
        path
    }

    fn load(name: &str, contents: &str) -> Config {
        let path = write(name, contents);
        let config = Config::load(Some(&path));
        std::fs::remove_file(&path).unwrap();
        config
    }

    #[test]
    fn no_file_means_the_documented_defaults() {
        let config = Config::load(None);
        assert_eq!(config, Config::default());
        assert!(config.publish, "publishing is on out of the box");
        assert_eq!(config.url, "nats://127.0.0.1:4222", "the same server as lib/bus.py");
        assert_eq!(config.subject, "status");
        assert!(config.activity, "the hooks publish out of the box too (§9.10)");
        assert_eq!(config.activity_subject, "activity");
        assert_eq!(config.timeout_ms, 500);
        assert_eq!(config.retry_after_s, 30);
        assert!(!config.debug);

        let missing = std::env::temp_dir().join("statusline-config-test-nope.json");
        let _ = std::fs::remove_file(&missing);
        assert_eq!(Config::load(Some(&missing)), Config::default());
    }

    #[test]
    fn a_file_may_set_only_what_it_changes() {
        let config = load("partial", r#"{"v": 1, "subject": "claude.status"}"#);
        assert_eq!(config.subject, "claude.status");
        assert_eq!(config.url, Config::default().url, "the rest keeps its default");
        assert!(config.publish);
    }

    #[test]
    fn every_field_reaches_the_runtime_settings() {
        let config = load(
            "full",
            r#"{"v": 1, "publish": false, "url": "nats://box:4222",
                "subject": "s", "activity": false, "activity_subject": "a",
                "timeout_ms": 120, "retry_after_s": 5, "debug": true}"#,
        );
        let settings = config.settings();
        assert_eq!(settings.url, "nats://box:4222");
        assert_eq!(settings.subject, "s");
        assert_eq!(settings.timeout, Duration::from_millis(120));
        assert!(!settings.enabled, "`publish` is what switches it off");
        assert_eq!(config.retry_after(), Duration::from_secs(5));
        assert!(config.debug);

        let hooks = config.activity_settings();
        assert_eq!(hooks.subject, "a");
        assert_eq!(hooks.url, settings.url, "one server for both documents");
        assert_eq!(hooks.timeout, settings.timeout, "and one budget");
    }

    #[test]
    fn the_activity_half_can_be_switched_off_on_its_own() {
        // The line's numbers are one thing; the command text §9.10 puts on the
        // bus is another, and either may be wanted without the other.
        let quiet = load("activity-off", r#"{"v": 1, "activity": false}"#);
        assert!(quiet.settings().enabled, "the line still publishes");
        assert!(!quiet.activity_settings().enabled, "the hooks do not");

        let nothing = load("publish-off", r#"{"v": 1, "publish": false}"#);
        assert!(!nothing.settings().enabled);
        assert!(!nothing.activity_settings().enabled, "`publish: false` still means neither");
    }

    #[test]
    fn the_example_file_is_valid_and_matches_the_defaults() {
        // The one committed copy of this format, so a typo in it is a test
        // failure rather than something a user discovers.
        let example = concat!(env!("CARGO_MANIFEST_DIR"), "/../statusline-config.example.json");
        let text = std::fs::read_to_string(example).expect("the example file exists");
        let config: Config = serde_json::from_str(&text).expect("the example parses");
        assert_eq!(config, Config::default(), "the example documents the defaults");
    }

    #[test]
    fn a_broken_file_falls_back_instead_of_breaking_the_line() {
        assert_eq!(load("garbage", "not json at all"), Config::default());
        assert_eq!(load("empty", ""), Config::default());
        assert_eq!(load("wrong-type", r#"{"v": 1, "timeout_ms": "soon"}"#), Config::default());
        assert_eq!(
            load("unknown-field", r#"{"v": 1, "subjekt": "typo"}"#),
            Config::default(),
            "a misspelled key is a mistake worth ignoring the file over, not a silent no-op"
        );
    }

    #[test]
    fn a_schema_version_this_build_does_not_know_is_not_guessed_at() {
        assert_eq!(load("v-future", r#"{"v": 99, "subject": "elsewhere"}"#), Config::default());
        assert_eq!(load("v-missing", r#"{"subject": "elsewhere"}"#).subject, "elsewhere",
                   "an absent `v` defaults to the current version, so it is honoured");
    }
}
