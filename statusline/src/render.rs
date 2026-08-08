//! Turning the status line payload into the one line Claude Code prints.

use crate::json::{Json, Lookup};
use crate::link::Link;

const BAR_WIDTH: usize = 8;
const SEP: &str = " │ ";
/// The bus indicator. One glyph, because it is the only thing on this line that
/// is not about the model or the limits and it should not cost a word.
const DOT: &str = "●";

const RESET: &str = "\x1b[0m";
const BOLD: &str = "\x1b[1m";
// The terminal is black, so nothing here leans on the default foreground or on
// `\x1b[2m` dimming — both come out muddy. Two explicit greys instead: bright
// for the text you read, a step darker for labels and separators.
const TEXT: &str = "\x1b[38;5;252m";
const MUTED: &str = "\x1b[38;5;247m";
const GREEN: &str = "\x1b[32m";
const YELLOW: &str = "\x1b[33m";
const RED: &str = "\x1b[31m";

/// Where a gauge turns yellow and then red, in percent **spent**.
struct Thresholds {
    green: f64,
    yellow: f64,
}

impl Thresholds {
    fn color(&self, used: f64) -> &'static str {
        match used {
            u if u <= self.green => GREEN,
            u if u <= self.yellow => YELLOW,
            _ => RED,
        }
    }
}

/// A rate-limit window. Half of a five-hour window is a normal working state,
/// so the warning comes late.
const WINDOW: Thresholds = Thresholds { green: 50.0, yellow: 80.0 };

/// The context window, on a deliberately tighter scale: it is not a budget you
/// are allowed to spend down to nothing but a runway ending in a compact, and
/// the useful moment to notice is well before it is full.
const CONTEXT: Thresholds = Thresholds { green: 20.0, yellow: 45.0 };

/// The whole line. `now` is Unix epoch seconds — passed in so the countdowns
/// are testable. `link` is what the previous render found on the bus (§9.8);
/// `Link::Off` draws no dot, because with publishing switched off there is
/// nothing for one to mean.
pub fn render(data: &Json, now: u64, link: Link) -> String {
    let model = data.str_at("model.display_name").unwrap_or("?");
    let mut head = format!("{BOLD}{TEXT}{model}{RESET}");
    // The reasoning effort is not a field of its own — it is what *that* model is
    // currently doing, so it hangs off the name instead of becoming a fourth
    // `│`-separated segment. Muted, so the name is still what the eye lands on.
    if let Some(effort) = data.str_at("effort.level") {
        head.push_str(&format!("{MUTED} · {effort}{RESET}"));
    }
    let mut segments = vec![head];

    let windows: Vec<String> = [("5h", "rate_limits.five_hour"), ("7d", "rate_limits.seven_day")]
        .into_iter()
        .filter_map(|(label, path)| {
            let window = data.path(path)?;
            let used = window.num_at("used_percentage")?;
            Some(window_segment(label, used, window.num_at("resets_at"), now))
        })
        .collect();

    if windows.is_empty() {
        // No `rate_limits` yet: an API key instead of a subscription, or the
        // session has not seen its first API response.
        segments.push(format!("{MUTED}limits n/a{RESET}"));
    } else {
        segments.extend(windows);
    }

    // Spent, like the windows above — one line must not mix "left" and "used" —
    // and drawn the same way, so the eye reads the whole row with one habit.
    if let Some(used) = data.num_at("context_window.used_percentage") {
        segments.push(gauge("ctx", used, &CONTEXT));
    }

    let line = segments.join(&format!("{MUTED}{SEP}{RESET}"));
    match link {
        // Not a segment: no separator after it, so it reads as a lamp on the
        // line rather than another field competing with the model name.
        Link::Up => format!("{GREEN}{DOT}{RESET} {line}"),
        Link::Down => format!("{RED}{DOT}{RESET} {line}"),
        Link::Off => line,
    }
}

/// One labelled bar: `5h ████░░░░ 44%`. The bar and the number both show what
/// is **spent**, so the bar grows as the thing it measures fills up, and the
/// colour is a traffic light on that same number — which is why the scale is a
/// parameter: `ctx` and a rate-limit window fill up at very different costs.
fn gauge(label: &str, used_percentage: f64, thresholds: &Thresholds) -> String {
    let used = used_percentage.clamp(0.0, 100.0);
    let filled = (used / 100.0 * BAR_WIDTH as f64).round() as usize;
    let bar: String = "█".repeat(filled) + &"░".repeat(BAR_WIDTH - filled);
    let color = thresholds.color(used);

    format!("{MUTED}{label}{RESET} {color}{bar} {}%{RESET}", used.round())
}

/// A rate-limit window: a [`gauge`] plus how long until it rolls over.
fn window_segment(label: &str, used_percentage: f64, resets_at: Option<f64>, now: u64) -> String {
    let mut segment = gauge(label, used_percentage, &WINDOW);
    if let Some(resets_at) = resets_at {
        segment.push_str(&format!(" {MUTED}·{RESET} {TEXT}{}{RESET}", countdown(resets_at, now)));
    }
    segment
}

/// The same line with the colours taken out — what goes on the bus (§9.7) and
/// what the tests assert against, because nobody should read escape codes.
pub fn strip_ansi(text: &str) -> String {
    let mut out = String::new();
    let mut chars = text.chars();
    while let Some(c) = chars.next() {
        if c == '\x1b' {
            for c in chars.by_ref() {
                if c == 'm' {
                    break;
                }
            }
        } else {
            out.push(c);
        }
    }
    out
}

/// Time until the window rolls over, coarse on purpose — one unit of noise in a
/// status line is one too many.
pub fn countdown(resets_at: f64, now: u64) -> String {
    let secs = (resets_at.max(0.0) as u64).saturating_sub(now);
    match secs {
        0 => "now".to_string(),
        s if s < 60 => "<1m".to_string(),
        s if s < 3600 => format!("{}m", s / 60),
        s if s < 86400 => format!("{}h{}m", s / 3600, (s % 3600) / 60),
        s => format!("{}d{}h", s / 86400, (s % 86400) / 3600),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::json::parse;

    /// The real payload Claude Code sends, trimmed to the fields we read.
    const PAYLOAD: &str = r#"{
        "model": {"id": "claude-opus-5[1m]", "display_name": "Opus 5 (1M context)"},
        "effort": {"level": "high"},
        "context_window": {"used_percentage": 6, "remaining_percentage": 94},
        "rate_limits": {
            "five_hour": {"used_percentage": 44, "resets_at": 1786141200},
            "seven_day": {"used_percentage": 24, "resets_at": 1786510800}
        }
    }"#;

    /// 1786141200 - 2h14m; the seven-day window then sits 4d6h out.
    const NOW: u64 = 1786141200 - (2 * 3600 + 14 * 60);

    /// Everything below is about the fields, so the dot is left off; the one
    /// test that is about the dot asks for it explicitly.
    fn plain(data: &str, now: u64) -> String {
        strip_ansi(&render(&parse(data).unwrap(), now, Link::Off))
    }

    #[test]
    fn shows_the_model_and_both_windows_with_countdowns() {
        assert_eq!(
            plain(PAYLOAD, NOW),
            "Opus 5 (1M context) · high │ 5h ████░░░░ 44% · 2h14m │ 7d ██░░░░░░ 24% · 4d8h │ ctx ░░░░░░░░ 6%"
        );
    }

    #[test]
    fn the_effort_hangs_off_the_model_name() {
        // It is what that model is currently doing, so it rides with the name
        // rather than becoming a fourth `│`-separated field.
        let with = r#"{"model": {"display_name": "Opus 5"}, "effort": {"level": "xhigh"}}"#;
        assert_eq!(plain(with, NOW), "Opus 5 · xhigh │ limits n/a");

        // Absent — an older Claude Code, or a payload that simply never had it:
        // no dot and no trailing gap, the same rule as every other field (§9.3).
        let without = r#"{"model": {"display_name": "Opus 5"}}"#;
        assert_eq!(plain(without, NOW), "Opus 5 │ limits n/a");

        // No model name but an effort: the placeholder still carries it.
        assert_eq!(plain(r#"{"effort": {"level": "low"}}"#, NOW), "? · low │ limits n/a");

        // A level that is not a string is not a level.
        assert_eq!(plain(r#"{"effort": {"level": 3}}"#, NOW), "? │ limits n/a");
    }

    #[test]
    fn the_effort_is_muted_so_the_model_name_still_reads_first() {
        let line = render(&parse(PAYLOAD).unwrap(), NOW, Link::Off);
        assert!(line.contains(&format!("{BOLD}{TEXT}Opus 5 (1M context){RESET}")));
        assert!(line.contains(&format!("{MUTED} · high{RESET}")));
    }

    #[test]
    fn the_context_window_is_a_gauge_like_the_others() {
        // Same shape as a rate-limit window, minus the countdown: a context
        // window does not reset on a clock, it resets when the session does.
        let ctx = |used: i32| {
            plain(&format!(r#"{{"context_window": {{"used_percentage": {used}}}}}"#), NOW)
        };
        assert_eq!(ctx(50), "? │ limits n/a │ ctx ████░░░░ 50%");
        assert_eq!(ctx(100), "? │ limits n/a │ ctx ████████ 100%");
    }

    #[test]
    fn the_context_window_goes_red_earlier_than_a_rate_limit() {
        // A tighter scale on purpose: 45% of a rate limit is an ordinary
        // Tuesday, 45% of the context window is most of the way to a compact.
        let raw = |used: i32| {
            let payload = format!(r#"{{"context_window": {{"used_percentage": {used}}}}}"#);
            render(&parse(&payload).unwrap(), NOW, Link::Off)
        };
        assert!(raw(20).contains(GREEN), "up to 20% spent is green");
        assert!(!raw(20).contains(YELLOW) && !raw(20).contains(RED));

        assert!(raw(45).contains(YELLOW), "up to 45% spent is yellow");
        assert!(!raw(45).contains(RED));

        assert!(raw(46).contains(RED), "past 45% spent is red");

        // The same number on a rate-limit window is still green — the two
        // scales are deliberately different, so one must not follow the other.
        let window = r#"{"rate_limits": {"five_hour": {"used_percentage": 46}}}"#;
        assert!(!render(&parse(window).unwrap(), NOW, Link::Off).contains(RED));
    }

    #[test]
    fn the_dot_at_the_head_reports_the_bus() {
        let dotted = |link| strip_ansi(&render(&parse(PAYLOAD).unwrap(), NOW, link));
        assert!(dotted(Link::Up).starts_with("● Opus 5"), "connected: a dot, then the line");
        assert!(dotted(Link::Down).starts_with("● Opus 5"));
        assert!(dotted(Link::Off).starts_with("Opus 5"), "publishing off: no dot at all");

        // Colour is the whole message, so it is asserted on the raw line.
        let raw = |link| render(&parse(PAYLOAD).unwrap(), NOW, link);
        assert!(raw(Link::Up).starts_with(&format!("{GREEN}●{RESET} ")), "up is green");
        assert!(raw(Link::Down).starts_with(&format!("{RED}●{RESET} ")), "down is red");

        // The limit bars are green in this payload; a red dot must still be the
        // only red on the line, or the traffic light stops meaning anything.
        assert!(!raw(Link::Up).contains(RED));
    }

    #[test]
    fn colors_by_how_much_is_spent() {
        let line = render(&parse(PAYLOAD).unwrap(), NOW, Link::Off);
        assert!(line.contains(BOLD), "the model name is bold");
        assert!(line.contains(TEXT), "text is bright grey, never the terminal default");
        assert!(line.contains(MUTED), "labels and separators are a step darker");
        assert!(!line.contains("\x1b[2m"), "no dimming — it turns to mud on black");
        assert!(line.contains(GREEN), "44% and 24% spent are both green");
        assert!(!line.contains(RED));

        let high = r#"{"rate_limits": {"five_hour": {"used_percentage": 95, "resets_at": 100}}}"#;
        assert!(render(&parse(high).unwrap(), 0, Link::Off).contains(RED), "95% spent is red");

        let mid = r#"{"rate_limits": {"five_hour": {"used_percentage": 70, "resets_at": 100}}}"#;
        assert!(render(&parse(mid).unwrap(), 0, Link::Off).contains(YELLOW), "70% spent is yellow");
    }

    #[test]
    fn says_so_when_the_limits_are_absent() {
        // API users and the first render of a session get no `rate_limits`.
        let line = plain(r#"{"model": {"display_name": "Opus 5"}}"#, NOW);
        assert_eq!(line, "Opus 5 │ limits n/a");
    }

    #[test]
    fn renders_a_window_that_arrives_alone() {
        let one = r#"{"model": {"display_name": "Opus 5"},
                      "rate_limits": {"seven_day": {"used_percentage": 24, "resets_at": 1786510800}}}"#;
        assert_eq!(plain(one, NOW), "Opus 5 │ 7d ██░░░░░░ 24% · 4d8h");
    }

    #[test]
    fn survives_an_empty_or_broken_payload() {
        assert_eq!(plain("{}", NOW), "? │ limits n/a");
        assert_eq!(strip_ansi(&render(&Json::Null, NOW, Link::Off)), "? │ limits n/a");
    }

    #[test]
    fn a_window_without_a_reset_time_still_shows_its_share() {
        let no_reset = r#"{"rate_limits": {"five_hour": {"used_percentage": 44}}}"#;
        assert_eq!(plain(no_reset, NOW), "? │ 5h ████░░░░ 44%");
    }

    #[test]
    fn a_reset_in_the_past_never_prints_a_negative_countdown() {
        let past = r#"{"rate_limits": {"five_hour": {"used_percentage": 44, "resets_at": 10}}}"#;
        assert_eq!(plain(past, 99_999), "? │ 5h ████░░░░ 44% · now");
    }

    #[test]
    fn countdowns_round_down_to_the_unit_that_fits() {
        let at = |secs: u64| {
            let payload = format!(
                r#"{{"rate_limits": {{"five_hour": {{"used_percentage": 0, "resets_at": {}}}}}}}"#,
                1000 + secs
            );
            plain(&payload, 1000)
        };
        assert!(at(3 * 86400 + 5 * 3600).ends_with("· 3d5h"));
        assert!(at(3600 + 59 * 60).ends_with("· 1h59m"));
        assert!(at(59 * 60 + 59).ends_with("· 59m"));
        assert!(at(30).ends_with("· <1m"));
    }

    #[test]
    fn clamps_percentages_that_fall_outside_0_to_100() {
        let over = r#"{"rate_limits": {"five_hour": {"used_percentage": 130, "resets_at": 10}}}"#;
        assert_eq!(plain(over, 5), "? │ 5h ████████ 100% · <1m");
        let under = r#"{"rate_limits": {"five_hour": {"used_percentage": -5, "resets_at": 10}}}"#;
        assert_eq!(plain(under, 5), "? │ 5h ░░░░░░░░ 0% · <1m");
    }
}
