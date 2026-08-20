//! NATS access — `CLAUDE.md §9.7`.
//!
//! The Rust counterpart of `lib/bus.py`: JSON in, JSON out, over the same
//! server the approval flow uses (`nats://127.0.0.1:4222`, §3). Deliberately
//! thin — `async-nats` is the client, this module is the repo's conventions on
//! top of it: where the server is, what the subject is, and the one rule that
//! matters here.
//!
//! **That rule: the status line comes first.** This code runs on every render,
//! so a NATS server that is down, slow, or simply not running must cost the
//! user nothing. Every call is bounded by a timeout, every failure is a
//! `Result` the caller is free to drop on the floor, and nothing here panics —
//! `main` prints the line *before* it publishes, so even a hang that eats the
//! whole budget leaves the status bar already drawn.

use std::time::Duration;

use serde::Serialize;

/// The server `nats/docker-compose.yml` brings up, same default as
/// `lib/bus.py::DEFAULT_SERVERS`.
pub const DEFAULT_URL: &str = "nats://127.0.0.1:4222";
/// The subject the status line publishes to.
pub const DEFAULT_SUBJECT: &str = "status";
/// The whole publish — connect, write, flush — gets this long. A status line
/// that blocks is worse than a status line that skips a sample.
///
/// Kept short because it is a *worst case the user feels*: Claude Code reads
/// the command's output until it exits, so printing the line first (§9.7) wins
/// nothing if the process then sits on a socket. On localhost the real cost is
/// single-digit milliseconds; this budget only ever applies to a server that is
/// somewhere else and not answering, and §9.8 makes sure that is paid rarely.
pub const DEFAULT_TIMEOUT: Duration = Duration::from_millis(500);

#[derive(Debug)]
pub enum Error {
    Connect(String),
    Publish(String),
    Encode(String),
    /// The budget ran out. Carries it, so a log line can say which one.
    Timeout(Duration),
    /// The tokio runtime could not be built — no threads, no handles.
    Runtime(String),
}

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Error::Connect(e) => write!(f, "cannot connect to NATS: {e}"),
            Error::Publish(e) => write!(f, "cannot publish: {e}"),
            Error::Encode(e) => write!(f, "cannot encode the payload: {e}"),
            Error::Timeout(d) => write!(f, "gave up after {}ms", d.as_millis()),
            Error::Runtime(e) => write!(f, "cannot start the async runtime: {e}"),
        }
    }
}

impl std::error::Error for Error {}

/// A connected client. Mirrors the surface of `lib/bus.py::Bus` that this side
/// needs — publish and flush; nothing subscribes from the status line.
pub struct Bus {
    client: async_nats::Client,
}

impl Bus {
    pub async fn connect(url: &str) -> Result<Bus, Error> {
        async_nats::connect(url)
            .await
            .map(|client| Bus { client })
            .map_err(|e| Error::Connect(e.to_string()))
    }

    /// Serialise `value` as compact JSON and publish it. Fire-and-forget at the
    /// protocol level: NATS acknowledges nothing, so a successful return means
    /// "handed to the client", and only [`flush`](Bus::flush) means "on the
    /// wire". That is exactly the `nats pub` caveat in §4.
    pub async fn publish_json<T: Serialize>(&self, subject: &str, value: &T) -> Result<(), Error> {
        let payload = serde_json::to_vec(value).map_err(|e| Error::Encode(e.to_string()))?;
        self.client
            .publish(subject.to_string(), payload.into())
            .await
            .map_err(|e| Error::Publish(e.to_string()))
    }

    /// Push buffered messages to the server and wait for it to confirm receipt.
    /// Without this a short-lived process exits before its publish leaves the
    /// buffer — the same reason `registration_handler.py --once` flushes (§6).
    pub async fn flush(&self) -> Result<(), Error> {
        self.client
            .flush()
            .await
            .map_err(|e| Error::Publish(e.to_string()))
    }

    /// The `async-nats` client, for anything this wrapper does not cover.
    pub fn client(&self) -> &async_nats::Client {
        &self.client
    }
}

/// Where to publish and whether to bother. Built from `statusline-config.json`
/// (§9.9) — this struct is the runtime shape, `config::Config` is the file.
#[derive(Debug, Clone, PartialEq)]
pub struct Settings {
    pub url: String,
    pub subject: String,
    pub timeout: Duration,
    pub enabled: bool,
}

impl Default for Settings {
    fn default() -> Self {
        Settings {
            url: DEFAULT_URL.to_string(),
            subject: DEFAULT_SUBJECT.to_string(),
            timeout: DEFAULT_TIMEOUT,
            enabled: true,
        }
    }
}

/// Connect, publish, flush — the whole thing inside `settings.timeout`, from a
/// synchronous caller.
///
/// The status line is a one-shot process with no runtime of its own, so this
/// builds a single-threaded one, uses it, and drops it. Returns `Ok(())` when
/// the server confirmed the flush, and `Ok(())` *without publishing* when
/// `settings.enabled` is false; every other outcome is an `Err` the caller may
/// ignore.
pub fn publish_blocking<T: Serialize>(settings: &Settings, value: &T) -> Result<(), Error> {
    if !settings.enabled {
        return Ok(());
    }

    let runtime = tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
        .map_err(|e| Error::Runtime(e.to_string()))?;

    runtime.block_on(async {
        // One budget for connect + publish + flush, not one each: the caller
        // cares how long the whole thing can hold up the process.
        tokio::time::timeout(settings.timeout, async {
            let bus = Bus::connect(&settings.url).await?;
            bus.publish_json(&settings.subject, value).await?;
            bus.flush().await
        })
        .await
        .unwrap_or(Err(Error::Timeout(settings.timeout)))
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn defaults_to_the_local_server_and_the_status_subject() {
        let s = Settings::default();
        assert_eq!(
            s.url, "nats://127.0.0.1:4222",
            "the same server as lib/bus.py"
        );
        assert_eq!(s.subject, "status");
        assert_eq!(s.timeout, DEFAULT_TIMEOUT);
        assert!(s.enabled);
    }

    #[test]
    fn disabled_publishing_is_a_no_op_and_never_touches_the_network() {
        // No server needed, and no runtime built: it returns before either.
        let off = Settings {
            enabled: false,
            ..Settings::default()
        };
        assert!(publish_blocking(&off, &serde_json::json!({"a": 1})).is_ok());
    }

    #[test]
    fn an_unreachable_server_is_an_error_not_a_hang_and_not_a_panic() {
        // Port 1 is reserved and never listening, so this exercises the failure
        // path the status line lives with whenever docker is down.
        let nowhere = Settings {
            url: "nats://127.0.0.1:1".to_string(),
            timeout: Duration::from_millis(300),
            ..Settings::default()
        };
        let result = publish_blocking(&nowhere, &serde_json::json!({"a": 1}));
        assert!(
            matches!(result, Err(Error::Connect(_) | Error::Timeout(_))),
            "{result:?}"
        );
    }
}
