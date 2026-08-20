//! Claude Code status line: which model is answering, and how much of the rate
//! limits is spent. See `CLAUDE.md §9`.
//!
//! Claude Code pipes one JSON object in on stdin and prints whatever comes back
//! on stdout. Nothing here may fail loudly: a status line that panics leaves the
//! user staring at a stack trace instead of a status bar, so every read is
//! optional and every missing field degrades to a placeholder.
//!
//! The same numbers also go onto NATS (§9.7). That is strictly secondary — the
//! line is printed and flushed *before* the publish is attempted, the publish's
//! failures never reach the exit code, and while the bus is known to be down it
//! is not even tried (§9.8). What it all does is `statusline-config.json`, next
//! to this binary (§9.9).
//!
//! **This binary has a second job** (§9.10). Wired into `settings.json` as the
//! `PreToolUse` / `PostToolUse` / `Stop` hook, it publishes what the session is
//! *doing* to the `activity` subject. One executable rather than two because the
//! payload says which it is — `activity::is_hook_payload` — and because
//! everything that job needs is already here: the same config file, the same
//! server, and the same §9.8 cache. A hook runs on every tool call, so that last
//! part is what makes the second job affordable.

use std::io::{Read, Write};
use std::path::Path;
use std::time::{SystemTime, UNIX_EPOCH};

use serde::Serialize;
use statusline::activity::{self, Activity};
use statusline::config::Config;
use statusline::json::Json;
use statusline::link::{Cache, Link};
use statusline::nats::Settings;
use statusline::status::Status;
use statusline::{json, nats, render};

fn main() {
    let mut buf = String::new();
    let _ = std::io::stdin().read_to_string(&mut buf);

    let data = json::parse(&buf).unwrap_or(json::Json::Null);
    let now = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0);

    let config = Config::load(Config::default_path().as_deref());

    // Which of the two jobs this is, settled before anything is written: on a
    // hook, stdout is not a status bar — on `Stop` it is context handed back to
    // Claude (§9.10).
    if activity::is_hook_payload(&data) {
        hook(&config, &data, now);
    } else {
        status_line(&config, &data, now);
    }
}

/// The status line (§9.1–§9.8): render, print, then publish what was printed.
fn status_line(config: &Config, data: &Json, now: u64) {
    let settings = config.settings();
    let cache_path = statusline::link::default_path();
    // A file read, not a connection: the dot shows what the last render found,
    // because finding out now would mean printing after the network (§9.8).
    let cache = Cache::load(&cache_path, &settings.url);
    let link = if settings.enabled {
        cache.link()
    } else {
        Link::Off
    };

    // Print first, and flush by hand: stdout is a pipe here, so it is block
    // buffered, and the publish below is allowed to take its whole budget.
    let line = render::render(data, now, link);
    println!("{line}");
    let _ = std::io::stdout().flush();

    // Nothing below this point can affect what the user already sees.
    let status = Status::from_payload(data, now, &line);
    publish(config, &settings, &status, now, &cache, &cache_path);
}

/// The hook (§9.10): publish what the session is doing, and print nothing.
///
/// Silence is the contract rather than an omission. An event we do not publish
/// leaves here having done nothing at all — which is exactly right for a hook:
/// the tool call proceeds as it would with none wired.
fn hook(config: &Config, data: &Json, now: u64) {
    let Some(activity) = Activity::from_payload(data, now) else {
        return;
    };

    let settings = config.activity_settings();
    let cache_path = statusline::link::default_path();
    let cache = Cache::load(&cache_path, &settings.url);
    publish(config, &settings, &activity, now, &cache, &cache_path);
}

/// One document onto the bus, and the verdict into the cache the next dot reads.
///
/// Shared by both jobs, and the reason the second one is cheap: a hook fires on
/// every tool call, and while the server is known to be down this opens no socket
/// at all (§9.8). Failures are swallowed — `debug` in `statusline-config.json`
/// (§9.9) is how they are seen.
fn publish<T: Serialize>(
    config: &Config,
    settings: &Settings,
    value: &T,
    now: u64,
    cache: &Cache,
    cache_path: &Path,
) {
    if !settings.enabled || !cache.due(now, config.retry_after()) {
        return;
    }

    let result = nats::publish_blocking(settings, value);
    let _ = Cache::recorded(result.is_ok(), now, &settings.url).save(cache_path);

    if let Err(e) = result
        && config.debug
    {
        eprintln!(
            "statusline: {} ({} → {})",
            e, settings.url, settings.subject
        );
    }
}
