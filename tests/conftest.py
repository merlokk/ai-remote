"""Shared pytest helpers.

Bus tests need a live NATS server (see CLAUDE.md §3). When it is unreachable the
whole test session should still pass, so we probe once and expose a skip marker.
"""
import asyncio
import socket

import pytest

from lib.bus import DEFAULT_SERVERS


def _nats_reachable(host: str = "127.0.0.1", port: int = 4222) -> bool:
    try:
        with socket.create_connection((host, port), timeout=1):
            return True
    except OSError:
        return False


requires_nats = pytest.mark.skipif(
    not _nats_reachable(),
    reason="NATS server not reachable on 127.0.0.1:4222 (see CLAUDE.md §3)",
)


def run_async(coro):
    """Drive an async coroutine to completion without pytest-asyncio (not an approved dep)."""
    return asyncio.run(coro)


__all__ = ["requires_nats", "run_async", "DEFAULT_SERVERS"]
