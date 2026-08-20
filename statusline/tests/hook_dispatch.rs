//! One binary, two jobs — `CLAUDE.md §9.10`, through the real executable.
//!
//! The unit tests in `src/activity.rs` pin what a payload turns into; this pins
//! what the *process* does with it, which is the half that can go wrong in a way
//! no library test would catch: printing a status line onto a hook's stdout.
//!
//! That is not cosmetic. On `Stop`, plain text on stdout is handed back to Claude
//! as context, so a status bar printed there becomes something the model reads.
//! Hence one test that runs the binary for every kind of payload it can be given
//! and asserts, for each, whether stdout is a line or nothing at all.
//!
//! No server is needed and none is touched: the run writes a
//! `statusline-config.json` with `publish: false` next to the binary first.

use std::io::Write;
use std::path::PathBuf;
use std::process::{Command, Stdio};

const EXE: &str = env!("CARGO_BIN_EXE_statusline");

/// A `PreToolUse` payload as <https://code.claude.com/docs/en/hooks> spells it.
const PRE_TOOL: &str = r#"{"session_id": "abc123", "cwd": "E:\\projects\\ai-remote",
    "permission_mode": "default", "hook_event_name": "PreToolUse", "tool_name": "Bash",
    "tool_input": {"command": "py -m pytest -q"}, "tool_use_id": "toolu_01ABC"}"#;

const STOP: &str = r#"{"hook_event_name": "Stop", "session_id": "abc123",
    "last_assistant_message": "Done — the suite is green."}"#;

/// Enough of a status-line payload to render a recognisable line.
const STATUS_PAYLOAD: &str = r#"{"model": {"display_name": "Opus 5 (1M context)"},
    "effort": {"level": "high"}, "context_window": {"used_percentage": 6}}"#;

fn config_path() -> PathBuf {
    PathBuf::from(EXE)
        .parent()
        .expect("the binary has a directory")
        .join("statusline-config.json")
}

/// stdout of one run with `payload` on stdin. Panics if the process fails to
/// start; a non-zero exit is returned as part of the assertion below.
fn run(payload: &str) -> (String, bool) {
    let mut child = Command::new(EXE)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .expect("the binary runs");
    let mut stdin = child.stdin.take().expect("stdin is piped");
    stdin.write_all(payload.as_bytes()).expect("write stdin");
    drop(stdin); // the binary reads stdin to EOF, so the pipe has to close

    let out = child.wait_with_output().expect("the binary exits");
    (String::from_utf8_lossy(&out.stdout).to_string(), out.status.success())
}

/// One test, not six: the cases share a config file written next to the binary,
/// and `cargo test` would otherwise run them in threads and delete it underneath
/// each other.
#[test]
fn a_hook_payload_prints_nothing_and_a_status_payload_prints_a_line() {
    let config = config_path();
    let restore = std::fs::read_to_string(&config).ok();
    std::fs::write(&config, r#"{"v": 1, "publish": false}"#).expect("write the test config");

    let cases = [
        // Every hook event: nothing on stdout, whether or not we publish for it.
        (PRE_TOOL, "", "a PreToolUse payload"),
        (
            r#"{"hook_event_name": "PostToolUse", "tool_name": "Read",
                "tool_input": {"file_path": "src/main.rs"}, "tool_result": "…"}"#,
            "",
            "a PostToolUse payload",
        ),
        (
            STOP,
            "",
            "a Stop payload — stdout here would become context for Claude",
        ),
        (
            r#"{"hook_event_name": "SessionStart", "source": "startup"}"#,
            "",
            "an event §9.10 does not publish",
        ),
        (
            r#"{"hook_event_name": "PermissionRequest", "tool_name": "Bash"}"#,
            "",
            "PermissionRequest — approver/hook.py's job (§7), never ours",
        ),
        // And the status line, which must keep printing exactly as before.
        (
            STATUS_PAYLOAD,
            "Opus 5 (1M context)",
            "a status-line payload",
        ),
        // Unparseable stdin is not a hook, so it degrades to the line's own
        // placeholders (§9.3) rather than vanishing.
        ("not json at all", "limits n/a", "garbage on stdin"),
        ("", "limits n/a", "empty stdin"),
    ];

    let mut failures = Vec::new();
    for (payload, expected, what) in cases {
        let (stdout, ok) = run(payload);
        if !ok {
            failures.push(format!("{what}: exited non-zero"));
        }
        if expected.is_empty() {
            if !stdout.trim().is_empty() {
                failures.push(format!("{what}: expected silence, got {stdout:?}"));
            }
        } else if !stdout.contains(expected) {
            failures.push(format!("{what}: expected {expected:?} in {stdout:?}"));
        }
    }

    match restore {
        Some(text) => std::fs::write(&config, text).expect("restore the config"),
        None => std::fs::remove_file(&config).expect("remove the test config"),
    }

    assert!(failures.is_empty(), "{}", failures.join("\n"));
}
