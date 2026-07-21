"""Versioned, atomic JSON config store (stdlib only).

Backs ``handler-config.json`` / ``responder-config.json`` (see CLAUDE.md §6).
Every config carries a top-level schema version ``v``; loading rejects a
mismatch and saving stamps the current version. Saves are atomic (write to a
temp file in the same directory, fsync, then ``os.replace``) so a crash mid-write
never leaves a half-written config that ``hook.py`` would choke on.
"""
from __future__ import annotations

import copy
import json
import os
import tempfile
from collections.abc import Iterator
from pathlib import Path
from typing import Any

SCHEMA_VERSION = 1


class ConfigError(Exception):
    """Base class for config problems."""


class ConfigVersionError(ConfigError):
    """Config is missing ``v`` or its version does not match SCHEMA_VERSION."""


class Config:
    """A JSON config bound to a path, with dict-like access and atomic ``save()``."""

    def __init__(self, path: str | os.PathLike[str], data: dict[str, Any]):
        self.path = Path(path)
        self.data = data

    @classmethod
    def load(
        cls,
        path: str | os.PathLike[str],
        *,
        default: dict[str, Any] | None = None,
    ) -> "Config":
        """Load config from ``path``.

        If the file is missing and ``default`` is given, return a Config holding a
        deep copy of ``default`` (not yet written to disk). If it is missing and no
        default is given, raise ConfigError. A present file with a missing or
        mismatched ``v`` raises ConfigVersionError.
        """
        p = Path(path)
        if not p.exists():
            if default is None:
                raise ConfigError(f"config not found: {p}")
            return cls(p, copy.deepcopy(default))

        try:
            raw = json.loads(p.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as e:
            raise ConfigError(f"cannot read config {p}: {e}") from e

        if not isinstance(raw, dict):
            raise ConfigError(f"config {p} must be a JSON object")

        version = raw.get("v")
        if version != SCHEMA_VERSION:
            raise ConfigVersionError(
                f"config {p} has version {version!r}, expected {SCHEMA_VERSION}"
            )
        return cls(p, raw)

    def save(self) -> None:
        """Atomically persist the config, stamping the current schema version."""
        self.data["v"] = SCHEMA_VERSION
        self.path.parent.mkdir(parents=True, exist_ok=True)

        payload = json.dumps(self.data, ensure_ascii=False, indent=2, sort_keys=True)
        fd, tmp_name = tempfile.mkstemp(
            dir=self.path.parent, prefix=f".{self.path.name}.", suffix=".tmp"
        )
        try:
            with os.fdopen(fd, "w", encoding="utf-8") as f:
                f.write(payload)
                f.flush()
                os.fsync(f.fileno())
            os.replace(tmp_name, self.path)
        except BaseException:
            # Leave no half-written temp file behind on failure.
            try:
                os.unlink(tmp_name)
            except OSError:
                pass
            raise

    # --- dict-like conveniences -------------------------------------------------
    def __getitem__(self, key: str) -> Any:
        return self.data[key]

    def __setitem__(self, key: str, value: Any) -> None:
        self.data[key] = value

    def __contains__(self, key: object) -> bool:
        return key in self.data

    def __iter__(self) -> Iterator[str]:
        return iter(self.data)

    def get(self, key: str, default: Any = None) -> Any:
        return self.data.get(key, default)

    def setdefault(self, key: str, default: Any) -> Any:
        return self.data.setdefault(key, default)
