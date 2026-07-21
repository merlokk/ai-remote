"""Tests for lib.config — versioned, atomic JSON config store."""
import json
import os

import pytest

from lib.config import Config, ConfigError, ConfigVersionError, SCHEMA_VERSION


def test_load_missing_with_default_returns_copy(tmp_path):
    default = {"v": SCHEMA_VERSION, "clients": {}}
    cfg = Config.load(tmp_path / "missing.json", default=default)
    assert cfg.data == {"v": SCHEMA_VERSION, "clients": {}}
    # Mutating the loaded config must not touch the caller's default object.
    cfg["clients"]["a"] = 1
    assert default["clients"] == {}


def test_load_missing_without_default_raises(tmp_path):
    with pytest.raises(ConfigError):
        Config.load(tmp_path / "missing.json")


def test_load_existing_valid(tmp_path):
    p = tmp_path / "c.json"
    p.write_text(json.dumps({"v": SCHEMA_VERSION, "key_id": "approver-1"}), encoding="utf-8")
    cfg = Config.load(p)
    assert cfg["key_id"] == "approver-1"


def test_load_version_mismatch_raises(tmp_path):
    p = tmp_path / "c.json"
    p.write_text(json.dumps({"v": SCHEMA_VERSION + 1}), encoding="utf-8")
    with pytest.raises(ConfigVersionError):
        Config.load(p)


def test_load_missing_version_raises(tmp_path):
    p = tmp_path / "c.json"
    p.write_text(json.dumps({"key_id": "x"}), encoding="utf-8")
    with pytest.raises(ConfigVersionError):
        Config.load(p)


def test_save_then_load_roundtrip(tmp_path):
    p = tmp_path / "c.json"
    cfg = Config.load(p, default={"v": SCHEMA_VERSION, "pending_tokens": []})
    cfg["pending_tokens"].append({"key_id": "approver-1"})
    cfg.save()

    again = Config.load(p)
    assert again["pending_tokens"] == [{"key_id": "approver-1"}]


def test_save_enforces_version(tmp_path):
    p = tmp_path / "c.json"
    cfg = Config.load(p, default={"clients": {}})  # no explicit v
    cfg.save()
    on_disk = json.loads(p.read_text(encoding="utf-8"))
    assert on_disk["v"] == SCHEMA_VERSION


def test_save_is_atomic_no_temp_left_behind(tmp_path):
    p = tmp_path / "c.json"
    cfg = Config.load(p, default={"v": SCHEMA_VERSION})
    cfg.save()
    leftovers = [f for f in os.listdir(tmp_path) if f != "c.json"]
    assert leftovers == []


def test_save_creates_parent_dirs(tmp_path):
    p = tmp_path / "nested" / "deep" / "c.json"
    cfg = Config.load(p, default={"v": SCHEMA_VERSION})
    cfg.save()
    assert p.exists()


def test_dict_helpers(tmp_path):
    cfg = Config.load(tmp_path / "c.json", default={"v": SCHEMA_VERSION})
    assert cfg.get("missing") is None
    assert cfg.get("missing", 42) == 42
    cfg.setdefault("clients", {})["approver-1"] = {"pubkey": "abc"}
    assert cfg["clients"]["approver-1"] == {"pubkey": "abc"}
    assert "clients" in cfg
