"""Reusable building blocks for the Claude permission approver.

- ``lib.config`` — versioned, atomic JSON config store.
- ``lib.bus``    — JSON request-reply over NATS (thin async wrapper over nats-py).
- ``lib.crypto`` — Ed25519 key generation, signing and verification.
"""
