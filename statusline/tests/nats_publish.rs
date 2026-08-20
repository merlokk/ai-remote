//! End-to-end against a real NATS server — `CLAUDE.md §9.7`, `§9.10`.
//!
//! The counterpart of pytest's `requires_nats` marker (`tests/conftest.py`):
//! the suite must stay green on a machine where `docker compose` is not up. Rust
//! has no "skip", so an unreachable server turns into a printed note and an
//! early return — visible with `cargo test -- --nocapture`, harmless otherwise.
//!
//! Bring the server up with `cd nats && docker compose up -d` (§3), then watch
//! what this publishes:
//!
//! ```text
//! docker exec -it nats-box nats sub status
//! docker exec -it nats-box nats sub activity
//! ```

use std::time::Duration;

use futures::StreamExt;
use statusline::activity::Activity;
use statusline::json::{self, Lookup};
use statusline::link::Link;
use statusline::nats::{Bus, Settings, publish_blocking};
use statusline::render;
use statusline::status::Status;

const PAYLOAD: &str = r#"{
    "session_id": "integration-test",
    "model": {"id": "claude-opus-5[1m]", "display_name": "Opus 5 (1M context)"},
    "effort": {"level": "high"},
    "context_window": {"used_percentage": 6},
    "rate_limits": {
        "five_hour": {"used_percentage": 44, "resets_at": 1786141200},
        "seven_day": {"used_percentage": 24, "resets_at": 1786510800}
    }
}"#;

const NOW: u64 = 1786141200 - (2 * 3600 + 14 * 60);

/// A subject of our own, so a live approval flow on `status` is not disturbed
/// and a stray publisher cannot make this test pass by accident.
const TEST_SUBJECT: &str = "status.test.statusline";
/// The same, for the activity documents of §9.10.
const TEST_ACTIVITY_SUBJECT: &str = "status.test.statusline.activity";

fn settings() -> Settings {
    Settings {
        subject: TEST_SUBJECT.to_string(),
        timeout: Duration::from_secs(2),
        ..Settings::default()
    }
}

/// `None` plus a printed reason when NATS is not reachable.
async fn bus_or_skip(what: &str) -> Option<Bus> {
    match Bus::connect(&settings().url).await {
        Ok(bus) => Some(bus),
        Err(e) => {
            println!("skipping {what}: {e} — start it with `cd nats && docker compose up -d`");
            None
        }
    }
}

#[tokio::test]
async fn a_subscriber_on_status_receives_the_rendered_values() {
    let Some(bus) = bus_or_skip("nats round trip").await else { return };

    let mut messages = bus.client().subscribe(TEST_SUBJECT).await.expect("subscribe");
    // The subscription only exists on the server once it has been flushed —
    // publishing before that races, and Core NATS never redelivers (§4).
    bus.flush().await.expect("flush the subscription");

    let data = json::parse(PAYLOAD).unwrap();
    // Link::Up is what a render that is about to publish successfully shows.
    let sent = Status::from_payload(&data, NOW, &render::render(&data, NOW, Link::Up));
    // The blocking path, on its own runtime: exactly what `main` calls.
    let published = tokio::task::spawn_blocking({
        let sent = sent.clone();
        move || publish_blocking(&settings(), &sent)
    })
    .await
    .expect("the publisher thread survives");
    published.expect("publish");

    let message = tokio::time::timeout(Duration::from_secs(2), messages.next())
        .await
        .expect("a message arrives within 2s")
        .expect("the subscription is still open");

    let text = String::from_utf8(message.payload.to_vec()).expect("utf-8 on the wire");

    // Read it back both ways: as the typed contract, and as the untyped JSON a
    // non-Rust subscriber (`nats sub status`, the web responder) actually sees.
    let received: Status = serde_json::from_str(&text).expect("the wire format parses");
    assert_eq!(received, sent);

    let raw = json::parse(&text).expect("valid JSON");
    assert_eq!(raw.str_at("model.display_name"), Some("Opus 5 (1M context)"));
    assert_eq!(raw.str_at("effort.level"), Some("high"));
    assert_eq!(raw.num_at("rate_limits.five_hour.used_percentage"), Some(44.0));
    assert_eq!(raw.str_at("rate_limits.five_hour.resets_in_text"), Some("2h14m"));
    assert_eq!(raw.num_at("context_window.used_percentage"), Some(6.0));
    assert_eq!(
        raw.str_at("line"),
        Some(
            "● Opus 5 (1M context) · high │ 5h ████░░░░ 44% · 2h14m │ 7d ██░░░░░░ 24% · 4d8h │ ctx ░░░░░░░░ 6%"
        )
    );
}

#[tokio::test]
async fn publishing_is_off_when_the_config_says_so() {
    let Some(bus) = bus_or_skip("the disabled-publish check").await else { return };

    let subject = format!("{TEST_SUBJECT}.disabled");
    let mut messages = bus.client().subscribe(subject.clone()).await.expect("subscribe");
    bus.flush().await.expect("flush the subscription");

    let off = Settings { subject, enabled: false, ..settings() };
    tokio::task::spawn_blocking(move || publish_blocking(&off, &serde_json::json!({"a": 1})))
        .await
        .expect("the publisher thread survives")
        .expect("a disabled publish still reports success");

    // Give a message that should not exist every chance to show up.
    let nothing = tokio::time::timeout(Duration::from_millis(300), messages.next()).await;
    assert!(nothing.is_err(), "nothing should reach the subject: {nothing:?}");
}

#[tokio::test]
async fn a_subscriber_on_activity_receives_what_claude_is_doing() {
    // §9.10 on the wire: the hook half of the binary, published the same way and
    // read back the same way, on a subject of its own.
    let Some(bus) = bus_or_skip("the activity round trip").await else { return };

    let mut messages = bus.client().subscribe(TEST_ACTIVITY_SUBJECT).await.expect("subscribe");
    bus.flush().await.expect("flush the subscription");

    let payload = json::parse(
        r#"{"hook_event_name": "PreToolUse", "session_id": "integration-test",
             "cwd": "E:\\projects\\ai-remote", "tool_name": "Bash",
             "tool_input": {"command": "docker exec -it nats-box nats sub activity"},
             "tool_use_id": "toolu_01ABC"}"#,
    )
    .unwrap();
    let sent = Activity::from_payload(&payload, NOW).expect("a PreToolUse payload");

    let activity_settings = Settings { subject: TEST_ACTIVITY_SUBJECT.to_string(), ..settings() };
    tokio::task::spawn_blocking({
        let sent = sent.clone();
        move || publish_blocking(&activity_settings, &sent)
    })
    .await
    .expect("the publisher thread survives")
    .expect("publish");

    let message = tokio::time::timeout(Duration::from_secs(2), messages.next())
        .await
        .expect("a message arrives within 2s")
        .expect("the subscription is still open");
    let text = String::from_utf8(message.payload.to_vec()).expect("utf-8 on the wire");

    let received: Activity = serde_json::from_str(&text).expect("the wire format parses");
    assert_eq!(received, sent);

    // And as the untyped JSON the web page and the ESP32 actually parse.
    let raw = json::parse(&text).expect("valid JSON");
    assert_eq!(raw.num_at("v"), Some(1.0));
    assert_eq!(raw.str_at("event"), Some("pre_tool"));
    assert_eq!(raw.str_at("state"), Some("running"));
    assert_eq!(raw.str_at("tool_name"), Some("Bash"));
    assert_eq!(raw.str_at("summary"), Some("docker exec -it nats-box nats sub activity"));
    assert_eq!(raw.str_at("session_id"), Some("integration-test"));
    assert_eq!(raw.str_at("tool_use_id"), Some("toolu_01ABC"));
}
