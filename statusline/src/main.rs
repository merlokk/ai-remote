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

use std::io::{Read, Write};
use std::time::{SystemTime, UNIX_EPOCH};

use statusline::config::Config;
use statusline::link::{Cache, Link};
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
    let settings = config.settings();
    let cache_path = statusline::link::default_path();
    // A file read, not a connection: the dot shows what the last render found,
    // because finding out now would mean printing after the network (§9.8).
    let cache = Cache::load(&cache_path, &settings.url);
    let link = if settings.enabled { cache.link() } else { Link::Off };

    // Print first, and flush by hand: stdout is a pipe here, so it is block
    // buffered, and the publish below is allowed to take its whole budget.
    let line = render::render(&data, now, link);
    println!("{line}");
    let _ = std::io::stdout().flush();

    // Nothing below this point can affect what the user already sees.
    if !settings.enabled || !cache.due(now, config.retry_after()) {
        return;
    }

    let result = nats::publish_blocking(&settings, &Status::from_payload(&data, now, &line));
    let _ = Cache::recorded(result.is_ok(), now, &settings.url).save(&cache_path);

    if let Err(e) = result
        && config.debug
    {
        eprintln!("statusline: {} ({})", e, settings.url);
    }
}
