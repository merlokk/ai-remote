//! The status line payload, read with `serde_json`.
//!
//! Claude Code hands the status line one JSON object on stdin (`CLAUDE.md §9`)
//! and we pull a handful of scalars out of it. `serde_json` came in with
//! `async-nats` anyway, so parsing it by hand would be code we maintain for
//! nothing; what is left here is the dotted-path lookup on top.
//!
//! `Json::pointer("/a/b")` is the built-in equivalent, but the paths in this
//! crate are written as `rate_limits.five_hour.used_percentage` — the way the
//! docs and the Claude Code payload reference talks about them — so `path()`
//! keeps the code reading like the contract it implements.

pub use serde_json::Value as Json;

/// Parse the payload. Anything unparseable is the caller's problem to degrade —
/// see `main`, which turns it into `Json::Null`.
pub fn parse(input: &str) -> Result<Json, serde_json::Error> {
    serde_json::from_str(input)
}

/// Dotted-path reads that never panic and never distinguish "missing" from
/// "wrong type" — for a status line the two degrade identically.
pub trait Lookup {
    /// `a.b.c` — every missing step short-circuits to `None`. Object keys only;
    /// the payload holds no arrays we read into.
    fn path(&self, path: &str) -> Option<&Json>;

    fn str_at(&self, path: &str) -> Option<&str> {
        self.path(path).and_then(Json::as_str)
    }

    fn num_at(&self, path: &str) -> Option<f64> {
        self.path(path).and_then(Json::as_f64)
    }
}

impl Lookup for Json {
    fn path(&self, path: &str) -> Option<&Json> {
        path.split('.').try_fold(self, |node, key| node.get(key))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reads_scalars_by_dotted_path() {
        let json = parse(r#"{"model": {"display_name": "Opus 5", "n": 1.5}}"#).unwrap();
        assert_eq!(json.str_at("model.display_name"), Some("Opus 5"));
        assert_eq!(json.num_at("model.n"), Some(1.5));
    }

    #[test]
    fn missing_path_is_none_not_panic() {
        let json = parse(r#"{"a": {"b": 1}}"#).unwrap();
        assert_eq!(json.num_at("a.b.c"), None, "walking past a scalar");
        assert_eq!(json.str_at("nope.at.all"), None);
        assert_eq!(json.num_at("a"), None, "an object read as a number");
        assert_eq!(json.str_at("a.b"), None, "a number read as a string");
        assert_eq!(Json::Null.path("anything"), None);
    }

    #[test]
    fn walking_into_an_array_stops_rather_than_indexing_it() {
        // Object keys only — `a.0` does not mean "first element", and the status
        // line payload never asks it to (`CLAUDE.md §9.1` is all scalars).
        let json = parse(r#"{"a": [{"b": 7}]}"#).unwrap();
        assert_eq!(json.num_at("a.0.b"), None);
    }

    #[test]
    fn parses_windows_paths_and_rejects_garbage() {
        let json = parse(r#"{"p": "E:\\projects\\ai-remote"}"#).unwrap();
        assert_eq!(json.str_at("p"), Some(r"E:\projects\ai-remote"));
        assert!(parse("").is_err());
        assert!(parse(r#"{"a": 1} trailing"#).is_err());
    }
}
