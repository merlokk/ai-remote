//! The status document published to NATS — `CLAUDE.md §9.7`.
//!
//! The same numbers the line shows, as JSON, so something other than a terminal
//! can watch them. It is a *projection* of the Claude Code payload, not a copy:
//! the fields the status line actually reads, plus the rendered line itself and
//! the countdowns already resolved against a clock the subscriber does not have.
//!
//! Typed on purpose. A `serde_json::Value` assembled by hand would let a rename
//! in `render.rs` drift away from what subscribers parse; a struct makes the
//! wire format something the compiler checks, and `Deserialize` means the tests
//! (and any Rust consumer) read it back without restating the shape.

use serde::{Deserialize, Serialize};

use crate::json::{Json, Lookup};
use crate::render;

/// One message on the `status` subject.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Status {
    /// When it was published, Unix epoch seconds — the clock the countdowns below
    /// were resolved against.
    pub ts: u64,
    /// The rendered line, colours stripped.
    pub line: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub session_id: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub cwd: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub model: Option<Model>,
    /// The reasoning effort the session is running at. Kept in the payload's own
    /// shape rather than folded into `model` — this document is a projection of
    /// the payload, and there `effort` is a sibling of `model`, not part of it.
    /// The line puts them together anyway (§9.2), because that is a presentation
    /// decision and this is the wire.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub effort: Option<Effort>,
    /// Absent exactly when the line prints `limits n/a`.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub rate_limits: Option<RateLimits>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub context_window: Option<ContextWindow>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Model {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub id: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub display_name: Option<String>,
}

/// `low` … `max`, as the payload spells it — never interpreted here, because the
/// set of levels is Claude Code's to grow and a name we do not know is still a
/// name worth showing.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Effort {
    pub level: String,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct RateLimits {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub five_hour: Option<Window>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub seven_day: Option<Window>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Window {
    /// What is spent, 0–100 — clamped the same way the bar is, so the number on
    /// the bus and the number on screen can never disagree.
    pub used_percentage: f64,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub resets_at: Option<u64>,
    /// Seconds until `resets_at`, saturating at 0. Redundant with `resets_at`
    /// and worth it: a subscriber that trusts its own clock less than ours (a
    /// dashboard in a container, say) gets the countdown for free.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub resets_in: Option<u64>,
    /// The countdown as the line spells it: `2h14m`, `4d8h`, `<1m`, `now`.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub resets_in_text: Option<String>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct ContextWindow {
    pub used_percentage: f64,
}

impl Status {
    /// Project the Claude Code payload onto the wire format. Total: a payload
    /// missing everything yields a `Status` with just `ts` and `line`, matching
    /// the placeholders the line itself falls back to (§9.3).
    ///
    /// `line` is what was printed — passed in rather than re-rendered, so the
    /// bus cannot show a different line than the terminal did. Colours are
    /// stripped here, so the caller may hand over the raw one.
    pub fn from_payload(data: &Json, now: u64, line: &str) -> Status {
        let model = Model {
            id: data.str_at("model.id").map(str::to_string),
            display_name: data.str_at("model.display_name").map(str::to_string),
        };
        let limits = RateLimits {
            five_hour: Window::from_payload(data.path("rate_limits.five_hour"), now),
            seven_day: Window::from_payload(data.path("rate_limits.seven_day"), now),
        };

        Status {
            ts: now,
            line: render::strip_ansi(line),
            session_id: data.str_at("session_id").map(str::to_string),
            cwd: data.str_at("cwd").map(str::to_string),
            model: (model != Model::default()).then_some(model),
            effort: data.str_at("effort.level").map(|level| Effort { level: level.to_string() }),
            rate_limits: (limits != RateLimits::default()).then_some(limits),
            context_window: data
                .num_at("context_window.used_percentage")
                .map(|used| ContextWindow { used_percentage: used.clamp(0.0, 100.0) }),
        }
    }
}

impl Window {
    fn from_payload(window: Option<&Json>, now: u64) -> Option<Window> {
        let window = window?;
        // No `used_percentage` means no window: the bar is the point, and there
        // is nothing to draw. The line drops the segment for the same reason.
        let used = window.num_at("used_percentage")?.clamp(0.0, 100.0);
        let resets_at = window.num_at("resets_at");

        Some(Window {
            used_percentage: used,
            resets_at: resets_at.map(|at| at.max(0.0) as u64),
            resets_in: resets_at.map(|at| (at.max(0.0) as u64).saturating_sub(now)),
            resets_in_text: resets_at.map(|at| render::countdown(at, now)),
        })
    }
}

impl Default for Model {
    fn default() -> Self {
        Model { id: None, display_name: None }
    }
}

impl Default for RateLimits {
    fn default() -> Self {
        RateLimits { five_hour: None, seven_day: None }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::json::parse;

    const PAYLOAD: &str = r#"{
        "session_id": "0000-1111",
        "cwd": "E:\\projects\\ai-remote",
        "model": {"id": "claude-opus-5[1m]", "display_name": "Opus 5 (1M context)"},
        "effort": {"level": "high"},
        "context_window": {"used_percentage": 6, "remaining_percentage": 94},
        "rate_limits": {
            "five_hour": {"used_percentage": 44, "resets_at": 1786141200},
            "seven_day": {"used_percentage": 24, "resets_at": 1786510800}
        }
    }"#;

    const NOW: u64 = 1786141200 - (2 * 3600 + 14 * 60);

    /// Exactly what `main` does: render once, publish that line.
    fn status(payload: &str, now: u64) -> Status {
        let data = parse(payload).unwrap();
        let line = render::render(&data, now, crate::link::Link::Up);
        Status::from_payload(&data, now, &line)
    }

    #[test]
    fn carries_every_value_the_line_shows() {
        let status = status(PAYLOAD, NOW);
        assert_eq!(status.ts, NOW);
        assert_eq!(
            status.line,
            "● Opus 5 (1M context) · high │ 5h ████░░░░ 44% · 2h14m │ 7d ██░░░░░░ 24% · 4d8h │ ctx ░░░░░░░░ 6%",
            "the line travels with the numbers, and without escape codes"
        );
        assert_eq!(status.session_id.as_deref(), Some("0000-1111"));
        assert_eq!(status.cwd.as_deref(), Some(r"E:\projects\ai-remote"));

        let model = status.model.unwrap();
        assert_eq!(model.id.as_deref(), Some("claude-opus-5[1m]"));
        assert_eq!(model.display_name.as_deref(), Some("Opus 5 (1M context)"));

        // Structured as well as inside `line`, so a subscriber shows it beside the
        // model name without parsing a terminal line back apart.
        assert_eq!(status.effort.unwrap().level, "high");

        let five = status.rate_limits.unwrap().five_hour.unwrap();
        assert_eq!(five.used_percentage, 44.0);
        assert_eq!(five.resets_at, Some(1786141200));
        assert_eq!(five.resets_in, Some(2 * 3600 + 14 * 60));
        assert_eq!(five.resets_in_text.as_deref(), Some("2h14m"));

        assert_eq!(status.context_window.unwrap().used_percentage, 6.0);
    }

    #[test]
    fn serialises_to_the_documented_json_and_reads_back() {
        let status = status(PAYLOAD, NOW);
        let text = serde_json::to_string(&status).unwrap();
        let back: Status = serde_json::from_str(&text).unwrap();
        assert_eq!(back, status, "the wire format round-trips");

        let raw: Json = parse(&text).unwrap();
        assert_eq!(raw.str_at("effort.level"), Some("high"));
        assert_eq!(raw.num_at("rate_limits.seven_day.used_percentage"), Some(24.0));
        assert_eq!(raw.str_at("rate_limits.seven_day.resets_in_text"), Some("4d8h"));
        assert_eq!(raw.str_at("model.display_name"), Some("Opus 5 (1M context)"));
        assert_eq!(raw.num_at("context_window.used_percentage"), Some(6.0));
        assert_eq!(raw.num_at("ts"), Some(NOW as f64));
    }

    #[test]
    fn absent_sections_are_absent_from_the_json_not_null() {
        // An API key instead of a subscription: no `rate_limits` at all.
        let status = status(r#"{"model": {"display_name": "Opus 5"}}"#, NOW);
        assert_eq!(status.rate_limits, None);
        assert_eq!(status.context_window, None);
        assert_eq!(status.effort, None);
        assert_eq!(status.line, "● Opus 5 │ limits n/a");

        let text = serde_json::to_string(&status).unwrap();
        assert!(!text.contains("rate_limits"), "{text}");
        assert!(!text.contains("effort"), "{text}");
        assert!(!text.contains("null"), "an absent field is omitted, never null: {text}");
        assert!(!text.contains("session_id"));
    }

    #[test]
    fn one_window_arriving_alone_leaves_the_other_out() {
        let one = r#"{"rate_limits": {"seven_day": {"used_percentage": 24}}}"#;
        let limits = status(one, NOW).rate_limits.unwrap();
        assert_eq!(limits.five_hour, None);
        let seven = limits.seven_day.unwrap();
        assert_eq!(seven.used_percentage, 24.0);
        assert_eq!(seven.resets_at, None, "no reset time, no countdown fields");
        assert_eq!(seven.resets_in, None);
        assert_eq!(seven.resets_in_text, None);
    }

    #[test]
    fn an_empty_payload_still_produces_a_publishable_document() {
        let line = render::render(&Json::Null, 42, crate::link::Link::Off);
        let status = Status::from_payload(&Json::Null, 42, &line);
        assert_eq!(status.ts, 42);
        assert_eq!(status.line, "? │ limits n/a");
        assert_eq!(status.model, None);
        assert_eq!(serde_json::to_string(&status).unwrap(), r#"{"ts":42,"line":"? │ limits n/a"}"#);
    }

    #[test]
    fn percentages_are_clamped_and_past_resets_never_go_negative() {
        let odd = r#"{"rate_limits": {"five_hour": {"used_percentage": 130, "resets_at": 10}},
                      "context_window": {"used_percentage": -5}}"#;
        let status = status(odd, 99_999);
        let five = status.rate_limits.unwrap().five_hour.unwrap();
        assert_eq!(five.used_percentage, 100.0);
        assert_eq!(five.resets_in, Some(0), "a reset in the past is 0s away, not underflow");
        assert_eq!(five.resets_in_text.as_deref(), Some("now"));
        assert_eq!(status.context_window.unwrap().used_percentage, 0.0);
    }
}
