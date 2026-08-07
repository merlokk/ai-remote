//! Is the bus reachable? — `CLAUDE.md §9.8`.
//!
//! The dot at the head of the status line, and the reason a dead NATS costs
//! nothing. Two problems, one answer: a file.
//!
//! **The dot has to be drawn before we know.** `main` prints the line first and
//! publishes after (§9.7) — reversing that would mean the user waits on the
//! network to see their model name. So the dot shows what the *previous* render
//! found. It lags by one render, which for a bar that redraws every turn is a
//! fraction of a second, and the alternative is a status line that blocks.
//!
//! **A server that is down must not be retried every render.** Refused on
//! localhost is instant, but a host that silently drops packets — a laptop off
//! the VPN, a firewall — costs the full timeout, every single render. So a
//! failure is remembered and not retried for [`RETRY_AFTER`]; in between,
//! publishing is skipped entirely and the render costs one small file read.

use std::path::{Path, PathBuf};
use std::time::Duration;

use serde::{Deserialize, Serialize};

/// How long a failure is believed before trying again — the default behind
/// `retry_after_s` in `statusline-config.json` (§9.9).
///
/// The one number that decides what a dead NATS costs. Every render in between
/// skips the network entirely; only the probe at the end of the window pays the
/// connect timeout. At 30s that is one hitch every half minute against a server
/// that answers nothing at all, and a `docker compose up` still turns the dot
/// green within a few turns of noticing.
pub const DEFAULT_RETRY_AFTER: Duration = Duration::from_secs(30);

/// Lives in the temp directory, not the repo: it is a cache, it is rewritten
/// several times a minute, and losing it costs one red dot.
pub const FILE_NAME: &str = "ai-remote-statusline-link.json";

/// What the dot says.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Link {
    /// Green: the last publish reached the server.
    Up,
    /// Red: it did not, or nothing has been tried yet.
    Down,
    /// No dot at all — publishing is switched off, so there is no link to report.
    Off,
}

/// What the last attempt found, as persisted between runs.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Cache {
    pub connected: bool,
    /// When that was decided, Unix epoch seconds.
    pub checked_at: u64,
    /// Which server it was decided about. A different `AI_REMOTE_NATS_URL` —
    /// another project, another machine — must not inherit this verdict.
    pub url: String,
}

impl Cache {
    /// A cache that knows nothing: red dot, retry now.
    pub fn unknown(url: &str) -> Cache {
        Cache { connected: false, checked_at: 0, url: url.to_string() }
    }

    pub fn recorded(connected: bool, now: u64, url: &str) -> Cache {
        Cache { connected, checked_at: now, url: url.to_string() }
    }

    /// Read the cache for `url`. Missing, unreadable, corrupt or about a
    /// different server all mean the same thing — we do not know — because
    /// none of them is worth a message in a status bar.
    pub fn load(path: &Path, url: &str) -> Cache {
        std::fs::read_to_string(path)
            .ok()
            .and_then(|text| serde_json::from_str::<Cache>(&text).ok())
            .filter(|cache| cache.url == url)
            .unwrap_or_else(|| Cache::unknown(url))
    }

    /// Best-effort: a temp directory we cannot write to is not worth reporting,
    /// it just means the dot never settles.
    pub fn save(&self, path: &Path) -> std::io::Result<()> {
        let text = serde_json::to_string(self)
            .map_err(|e| std::io::Error::new(std::io::ErrorKind::InvalidData, e))?;
        std::fs::write(path, text)
    }

    pub fn link(&self) -> Link {
        if self.connected { Link::Up } else { Link::Down }
    }

    /// Should this render attempt a publish?
    ///
    /// While connected, always — that is the normal path, and it is what keeps
    /// the data fresh. While disconnected, only once per `backoff`, so a server
    /// that is not there is paid for at most every 15 seconds instead of every
    /// render. A `checked_at` in the future (the clock moved, or a cache copied
    /// between machines) is treated as stale rather than trusted.
    pub fn due(&self, now: u64, backoff: Duration) -> bool {
        self.connected || self.checked_at > now || now - self.checked_at >= backoff.as_secs()
    }
}

pub fn default_path() -> PathBuf {
    std::env::temp_dir().join(FILE_NAME)
}

#[cfg(test)]
mod tests {
    use super::*;

    const URL: &str = "nats://127.0.0.1:4222";

    /// Each test gets its own file — `cargo test` runs them in threads.
    fn temp_file(name: &str) -> PathBuf {
        let path = std::env::temp_dir().join(format!("statusline-link-test-{name}.json"));
        let _ = std::fs::remove_file(&path);
        path
    }

    #[test]
    fn a_missing_cache_reads_as_down_and_due() {
        let cache = Cache::load(&temp_file("missing"), URL);
        assert_eq!(cache.link(), Link::Down, "nothing known yet is not a green light");
        assert!(cache.due(1_000_000, DEFAULT_RETRY_AFTER), "and it is tried immediately");
    }

    #[test]
    fn a_saved_verdict_survives_the_round_trip() {
        let path = temp_file("roundtrip");
        let saved = Cache::recorded(true, 1_000, URL);
        saved.save(&path).unwrap();

        let loaded = Cache::load(&path, URL);
        assert_eq!(loaded, saved);
        assert_eq!(loaded.link(), Link::Up);
        std::fs::remove_file(&path).unwrap();
    }

    #[test]
    fn a_cache_about_another_server_is_ignored() {
        let path = temp_file("other-url");
        Cache::recorded(true, 1_000, "nats://elsewhere:4222").save(&path).unwrap();

        let loaded = Cache::load(&path, URL);
        assert_eq!(loaded, Cache::unknown(URL), "another server's verdict is not ours");
        std::fs::remove_file(&path).unwrap();
    }

    #[test]
    fn corrupt_contents_read_as_unknown_rather_than_failing() {
        let path = temp_file("corrupt");
        std::fs::write(&path, "not json at all").unwrap();
        assert_eq!(Cache::load(&path, URL), Cache::unknown(URL));
        std::fs::remove_file(&path).unwrap();
    }

    #[test]
    fn a_failure_is_not_retried_until_the_backoff_expires() {
        let failed = Cache::recorded(false, 1_000, URL);
        let backoff = Duration::from_secs(15);
        assert!(!failed.due(1_000, backoff), "not immediately");
        assert!(!failed.due(1_014, backoff), "not one second early");
        assert!(failed.due(1_015, backoff), "but exactly on time");
        assert!(failed.due(9_999, backoff));
    }

    #[test]
    fn a_working_connection_is_used_on_every_render() {
        let up = Cache::recorded(true, 1_000, URL);
        assert!(up.due(1_000, DEFAULT_RETRY_AFTER), "no backoff while it works");
        assert!(up.due(1_001, DEFAULT_RETRY_AFTER));
    }

    #[test]
    fn a_timestamp_from_the_future_is_stale_not_trusted() {
        // The clock moved back, or the temp file came from another machine.
        let failed = Cache::recorded(false, 9_999, URL);
        assert!(failed.due(1_000, DEFAULT_RETRY_AFTER), "a future check cannot suppress a retry");
    }
}
