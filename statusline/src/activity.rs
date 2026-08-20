//! What Claude is doing, as a document on the bus — `CLAUDE.md §9.10`.
//!
//! The sibling of `status.rs`. That one answers "what is this session
//! spending"; this one answers "what is it doing right now", and it arrives on
//! the events the numbers cannot see: a tool is about to run, a tool has run,
//! the turn is over. Same binary, same config, same server — Claude Code hands
//! the very same executable a hook payload instead of a status payload, and
//! [`is_hook_payload`] is how `main` tells which one it got.
//!
//! Three events, no more. `PreToolUse` and `PostToolUse` bracket every tool
//! call, and `Stop` is the turn ending; between them a subscriber can always say
//! whether the session is working or waiting for a human. Anything else Claude
//! Code can fire is deliberately not handled here — see [`Activity::from_payload`].
//!
//! **What goes on the wire is a summary, not the arguments.** A tool's input is
//! command text, file paths, sometimes a secret; a readout on a phone or a
//! 2.16" panel wants one short line. So exactly one value is lifted out of
//! `tool_input` ([`summarise`]), flattened to a single line and cut to
//! [`SUMMARY_MAX_CHARS`]. `last_assistant_message` — the model's own prose,
//! which `Stop` carries in full — is never published at all.

use serde::{Deserialize, Serialize};

use crate::json::{Json, Lookup};

/// Bumped when a field below changes meaning.
///
/// `status.rs` has no such field and does not need one: `ts` + `line` are
/// always there, which is enough for a subscriber to recognise the document on
/// an open subject. This one has no such pair — every field but `ts`, `event`
/// and `state` may be absent — so `v` is both the version and the "this is
/// ours" marker, the way `lib/bus.py`'s protocol documents use it.
pub const SCHEMA_VERSION: u32 = 1;

/// The subject these documents go to, next to `status` rather than on it: two
/// shapes on one subject would make every subscriber tell them apart, and §9.7
/// says `status` is a projection of the status line's payload.
pub const DEFAULT_SUBJECT: &str = "activity";

/// How much of a summary is worth publishing.
///
/// A number picked for the narrowest reader, not the widest: the ESP32 panel
/// (§10.8) shows a line of this in a font where ~40 characters fit, and a
/// terminal or a web card can always show less of a longer string but cannot
/// invent a shorter one. Long enough for a real `py -m pytest tests/x.py -q`,
/// short enough that a heredoc full of a file does not travel.
pub const SUMMARY_MAX_CHARS: usize = 80;

/// The hook events this document is published for: the payload's
/// `hook_event_name`, the `event` we publish, and the `state` it implies.
///
/// `PostToolUse` is `thinking`, not `idle`: the tool is done but the turn is
/// not, and the only event that means "waiting for a human" is `Stop`.
const EVENTS: [(&str, &str, &str); 3] = [
    ("PreToolUse", "pre_tool", "running"),
    ("PostToolUse", "post_tool", "thinking"),
    ("Stop", "stop", "idle"),
];

/// `hook_event_name` values that are *not* a hook — see [`is_hook_payload`].
const NOT_HOOK_EVENTS: [&str; 2] = ["Status", "StatusLine"];

/// Keys lifted out of `tool_input` for the summary, in order of preference.
///
/// Names, not tools. A table keyed by `tool_name` would have to grow with every
/// tool Claude Code adds and would say nothing at all about an MCP tool; a
/// preference order over the keys that actually appear covers both, and an
/// unknown tool that happens to take a `command` or a `path` is summarised
/// correctly by accident rather than skipped by omission.
const SUMMARY_KEYS: [&str; 10] = [
    "command",       // Bash
    "file_path",     // Read / Write / Edit
    "notebook_path", // NotebookEdit
    // Before `path`: for Grep and Glob the pattern is the question and the path
    // is only where it was asked.
    "pattern",       // Grep / Glob
    "path",          // an MCP tool, or a search root with no pattern
    "url",           // WebFetch
    "query",         // WebSearch
    "skill",         // Skill
    "description",   // Agent — the 3-5 word label, not the whole prompt
    "prompt",        // last resort: something is better than a bare tool name
];

/// One message on the `activity` subject.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Activity {
    pub v: u32,
    /// When it was published, Unix epoch seconds. The only clock in the
    /// document — a hook fires when it fires, and there is nothing to count down.
    pub ts: u64,
    /// `pre_tool` / `post_tool` / `stop` — snake_case, because this is our wire
    /// format and not an echo of Claude Code's event names.
    pub event: String,
    /// What the session is doing as of this event: `running`, `thinking`, `idle`.
    /// Derivable from `event`, and sent anyway: it is what a readout actually
    /// shows, and a subscriber should not have to hard-code the mapping to draw
    /// a dot.
    pub state: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub session_id: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub cwd: Option<String>,
    /// Absent on `stop`, which is not about a tool.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub tool_name: Option<String>,
    /// One value out of `tool_input`, flattened and cut — see [`summarise`].
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub summary: Option<String>,
    /// Claude Code's id for this tool call, so `post_tool` can be matched to the
    /// `pre_tool` it closes rather than guessed at by tool name.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub tool_use_id: Option<String>,
    /// Present when the tool call is inside a subagent, so a readout can say
    /// *whose* work this is instead of showing the main loop doing everything.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub agent_type: Option<String>,
}

/// Did Claude Code hand us a *hook* payload rather than a status-line one?
///
/// The whole dispatch, and it has to be answered before anything is printed:
/// stdout means something on a hook. On `Stop` in particular, plain text on
/// stdout is fed back to Claude as context — a status line printed there would
/// become something the model reads.
///
/// So the test is not "is this an event I handle" but "is this an event at all":
/// every hook payload carries `hook_event_name` and the status line's does not.
/// An event we do not publish (`SessionStart`, `PermissionRequest` — that one is
/// `approver/hook.py`'s job, §7) therefore produces *nothing*, not a line.
///
/// [`NOT_HOOK_EVENTS`] is the insurance for the other direction: if some later
/// Claude Code starts stamping the status payload with a name of its own, the
/// line keeps printing instead of being silently replaced by a publish.
pub fn is_hook_payload(data: &Json) -> bool {
    match data.str_at("hook_event_name") {
        Some(name) => !name.is_empty() && !NOT_HOOK_EVENTS.contains(&name),
        None => false,
    }
}

impl Activity {
    /// Project a hook payload onto the wire format, or `None` if this is not one
    /// of the three events (see [`is_hook_payload`] for what that means).
    pub fn from_payload(data: &Json, now: u64) -> Option<Activity> {
        let name = data.str_at("hook_event_name")?;
        let (_, event, state) = EVENTS.iter().find(|(hook, _, _)| *hook == name)?;

        Some(Activity {
            v: SCHEMA_VERSION,
            ts: now,
            event: event.to_string(),
            state: state.to_string(),
            session_id: data.str_at("session_id").map(str::to_string),
            cwd: data.str_at("cwd").map(str::to_string),
            tool_name: data.str_at("tool_name").map(str::to_string),
            summary: summarise(data.path("tool_input")),
            tool_use_id: data.str_at("tool_use_id").map(str::to_string),
            agent_type: data.str_at("agent_type").map(str::to_string),
        })
    }
}

/// The one line lifted out of a tool's input.
///
/// First of [`SUMMARY_KEYS`] that holds a string; whitespace — newlines
/// included, since a `Bash` heredoc is one input value with many lines —
/// collapsed to single spaces, then cut to [`SUMMARY_MAX_CHARS`] with an
/// ellipsis. `None` when nothing matched, which is a tool whose input is worth
/// nothing at a glance (`TodoWrite`) or has no input at all.
pub fn summarise(tool_input: Option<&Json>) -> Option<String> {
    let input = tool_input?;
    let raw = SUMMARY_KEYS
        .iter()
        .find_map(|key| input.get(key).and_then(Json::as_str))?;

    // Control characters go too: this ends up in a terminal line and on an LVGL
    // label, and neither wants an escape sequence it did not print itself.
    let flat = raw
        .split_whitespace()
        .collect::<Vec<_>>()
        .join(" ")
        .replace(|c: char| c.is_control(), " ");
    let flat = flat.trim();
    if flat.is_empty() {
        return None;
    }

    Some(truncate(flat, SUMMARY_MAX_CHARS))
}

/// Cut to `max` **characters**, not bytes: a path with a Cyrillic folder in it
/// must not be split mid-codepoint, and `String` indexing would do exactly that.
fn truncate(text: &str, max: usize) -> String {
    if text.chars().count() <= max {
        return text.to_string();
    }
    let kept: String = text.chars().take(max.saturating_sub(1)).collect();
    format!("{}…", kept.trim_end())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::json::parse;

    const NOW: u64 = 1786136782;

    /// A real `PreToolUse` payload, fields as
    /// <https://code.claude.com/docs/en/hooks> spells them.
    const PRE_TOOL: &str = r#"{
        "session_id": "abc123",
        "transcript_path": "/home/user/.claude/projects/x/transcript.jsonl",
        "cwd": "E:\\projects\\ai-remote",
        "permission_mode": "default",
        "hook_event_name": "PreToolUse",
        "tool_name": "Bash",
        "tool_input": {"command": "py -m pytest -q", "description": "Run the suite"},
        "tool_use_id": "toolu_01ABC123"
    }"#;

    fn activity(payload: &str) -> Activity {
        Activity::from_payload(&parse(payload).unwrap(), NOW).expect("a handled hook event")
    }

    #[test]
    fn a_tool_about_to_run_is_running_with_its_command() {
        let a = activity(PRE_TOOL);
        assert_eq!(a.v, SCHEMA_VERSION);
        assert_eq!(a.ts, NOW);
        assert_eq!(a.event, "pre_tool");
        assert_eq!(a.state, "running");
        assert_eq!(a.session_id.as_deref(), Some("abc123"));
        assert_eq!(a.cwd.as_deref(), Some(r"E:\projects\ai-remote"));
        assert_eq!(a.tool_name.as_deref(), Some("Bash"));
        assert_eq!(
            a.summary.as_deref(),
            Some("py -m pytest -q"),
            "`command` beats `description`"
        );
        assert_eq!(a.tool_use_id.as_deref(), Some("toolu_01ABC123"));
        assert_eq!(a.agent_type, None);
    }

    #[test]
    fn a_tool_that_has_run_is_thinking_not_idle() {
        // The tool is done; the turn is not. Only `Stop` means idle.
        let post = r#"{"hook_event_name": "PostToolUse", "tool_name": "Edit",
                       "tool_input": {"file_path": "statusline/src/main.rs",
                                      "old_string": "a", "new_string": "b"},
                       "tool_use_id": "toolu_01ABC123"}"#;
        let a = activity(post);
        assert_eq!(a.event, "post_tool");
        assert_eq!(a.state, "thinking");
        assert_eq!(a.tool_name.as_deref(), Some("Edit"));
        assert_eq!(a.summary.as_deref(), Some("statusline/src/main.rs"));
        assert_eq!(
            a.tool_use_id.as_deref(),
            Some("toolu_01ABC123"),
            "matches its pre_tool"
        );
    }

    #[test]
    fn the_turn_ending_is_idle_and_says_nothing_about_a_tool() {
        let stop = r#"{"hook_event_name": "Stop", "session_id": "abc123",
                       "last_assistant_message": "Done — the suite is green."}"#;
        let a = activity(stop);
        assert_eq!(a.event, "stop");
        assert_eq!(a.state, "idle");
        assert_eq!(a.tool_name, None);
        assert_eq!(a.summary, None);

        let text = serde_json::to_string(&a).unwrap();
        assert!(
            !text.contains("Done") && !text.contains("last_assistant_message"),
            "the model's own prose is not ours to publish: {text}"
        );
    }

    #[test]
    fn a_subagents_work_says_whose_it_is() {
        let payload = r#"{"hook_event_name": "PreToolUse", "tool_name": "Grep",
                          "tool_input": {"pattern": "TODO", "path": "src"},
                          "agent_id": "ag_1", "agent_type": "Explore"}"#;
        let a = activity(payload);
        assert_eq!(a.agent_type.as_deref(), Some("Explore"));
        assert_eq!(a.summary.as_deref(), Some("TODO"), "`pattern` beats `path`");
    }

    #[test]
    fn a_status_line_payload_is_not_a_hook_and_produces_no_document() {
        let payload = parse(include_str!("../payload.example.json")).unwrap();
        assert!(!is_hook_payload(&payload), "no hook_event_name in it");
        assert_eq!(Activity::from_payload(&payload, NOW), None);
        assert!(!is_hook_payload(&Json::Null));
    }

    #[test]
    fn an_event_we_do_not_publish_is_still_a_hook_and_still_silent() {
        // The dispatch has two answers, and this is the case that needs both:
        // nothing to publish, and nothing to print either — stdout on a hook is
        // not a status bar. `PermissionRequest` is `approver/hook.py`'s (§7).
        for name in [
            "SessionStart",
            "UserPromptSubmit",
            "PermissionRequest",
            "SubagentStop",
        ] {
            let payload = parse(&format!(r#"{{"hook_event_name": "{name}"}}"#)).unwrap();
            assert!(is_hook_payload(&payload), "{name} is a hook payload");
            assert_eq!(
                Activity::from_payload(&payload, NOW),
                None,
                "{name} publishes nothing"
            );
        }
    }

    #[test]
    fn a_status_payload_stamped_with_an_event_name_still_prints_a_line() {
        // Insurance against a later Claude Code labelling the status payload:
        // being wrong here would replace the line with a silent publish.
        for name in NOT_HOOK_EVENTS {
            let payload = parse(&format!(r#"{{"hook_event_name": "{name}"}}"#)).unwrap();
            assert!(
                !is_hook_payload(&payload),
                "{name} must not be read as a hook"
            );
        }
        let empty = parse(r#"{"hook_event_name": ""}"#).unwrap();
        assert!(!is_hook_payload(&empty), "an empty name is no name");
    }

    #[test]
    fn the_summary_is_one_short_line_whatever_the_input_was() {
        let heredoc = format!(
            r#"{{"hook_event_name": "PreToolUse", "tool_name": "Bash",
                 "tool_input": {{"command": "cat <<'EOF'\n{}\nEOF"}}}}"#,
            "x".repeat(200)
        );
        let summary = activity(&heredoc).summary.unwrap();
        assert_eq!(
            summary.chars().count(),
            SUMMARY_MAX_CHARS,
            "cut to the limit"
        );
        assert!(summary.ends_with('…'), "and it says it was cut: {summary}");
        assert!(!summary.contains('\n'), "one line: {summary}");
        assert!(
            summary.starts_with("cat <<'EOF' xxx"),
            "newlines became spaces: {summary}"
        );
    }

    #[test]
    fn cutting_a_summary_never_splits_a_character() {
        // Bytes would: each of these is two bytes, so a byte-indexed cut panics.
        let long = "щ".repeat(200);
        let input = parse(&format!(r#"{{"command": "{long}"}}"#)).unwrap();
        let summary = summarise(Some(&input)).unwrap();
        assert_eq!(summary.chars().count(), SUMMARY_MAX_CHARS);
        assert!(summary.starts_with("щщщ"));
    }

    #[test]
    fn nothing_worth_showing_means_no_summary_rather_than_an_empty_one() {
        let cases = [
            r#"{"todos": [{"content": "a"}]}"#, // TodoWrite: no key we lift
            r#"{"command": "   "}"#,            // whitespace only
            r#"{"command": 42}"#,               // not a string
            r#"{}"#,
        ];
        for case in cases {
            let input = parse(case).unwrap();
            assert_eq!(summarise(Some(&input)), None, "{case}");
        }
        assert_eq!(summarise(None), None, "a payload with no tool_input at all");
    }

    #[test]
    fn every_kind_of_tool_input_gets_the_field_worth_reading() {
        let cases = [
            (
                r#"{"url": "https://code.claude.com/docs/en/hooks", "prompt": "long…"}"#,
                "https://code.claude.com/docs/en/hooks",
            ),
            (
                r#"{"query": "esp32 amoled", "allowed_domains": []}"#,
                "esp32 amoled",
            ),
            (
                r#"{"description": "Check the bus", "prompt": "a very long prompt"}"#,
                "Check the bus",
            ),
            (
                r#"{"skill": "code-review", "args": "--fix"}"#,
                "code-review",
            ),
            (
                r#"{"notebook_path": "notes.ipynb", "new_source": "x"}"#,
                "notes.ipynb",
            ),
            (r#"{"prompt": "only a prompt"}"#, "only a prompt"),
        ];
        for (input, expected) in cases {
            let parsed = parse(input).unwrap();
            assert_eq!(
                summarise(Some(&parsed)).as_deref(),
                Some(expected),
                "{input}"
            );
        }
    }

    #[test]
    fn absent_is_absent_on_the_wire_and_it_reads_back() {
        let bare = parse(r#"{"hook_event_name": "Stop"}"#).unwrap();
        let a = Activity::from_payload(&bare, 42).unwrap();
        let text = serde_json::to_string(&a).unwrap();
        assert_eq!(text, r#"{"v":1,"ts":42,"event":"stop","state":"idle"}"#);
        assert!(!text.contains("null"), "omitted, never null: {text}");

        let back: Activity = serde_json::from_str(&text).unwrap();
        assert_eq!(back, a, "the wire format round-trips");
    }

    #[test]
    fn the_documented_json_is_what_a_subscriber_sees() {
        // The shape §9.10 shows, read the way a non-Rust subscriber reads it.
        let text = serde_json::to_string(&activity(PRE_TOOL)).unwrap();
        let raw = parse(&text).unwrap();
        assert_eq!(raw.num_at("v"), Some(1.0));
        assert_eq!(raw.str_at("event"), Some("pre_tool"));
        assert_eq!(raw.str_at("state"), Some("running"));
        assert_eq!(raw.str_at("tool_name"), Some("Bash"));
        assert_eq!(raw.str_at("summary"), Some("py -m pytest -q"));
        assert_eq!(raw.str_at("session_id"), Some("abc123"));
        assert!(
            !text.contains("transcript_path"),
            "a projection, not a copy: {text}"
        );
        assert!(!text.contains("permission_mode"), "{text}");
    }
}
