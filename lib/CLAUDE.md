# lib/ — shared modules

Reusable modules over the standard library and the approved dependencies (no new
ones of their own — [`../CLAUDE.md`](../CLAUDE.md) §1). Everything here is
protocol-agnostic: `bus.py` moves JSON, `crypto.py` signs bytes, `yubikey.py`
talks to a device. Assembling the approval protocol out of them is
[`approver/`](../approver/CLAUDE.md)'s job (§6–§7).

This file owns section **8** — YubiKey / ARKG — except the two parts that live
with their front ends: **§8.5** (the `yubikey-exec` CLI) in
[`tools/CLAUDE.md`](../tools/CLAUDE.md) and **§8.7** (the YubiKey-backed
responder) in [`approver/CLAUDE.md`](../approver/CLAUDE.md).

## Modules

- `lib/bus.py` — JSON request-reply over NATS (a thin async wrapper over `nats-py`). `connect()` (async context manager, yields a `Bus`, drains on exit; defaults to `nats://127.0.0.1:4222`; `name=` is what `/connz` will call the connection — build it with `client_name(role, ident=None)`, which is the repo-wide convention every client here follows, see [`nats/CLAUDE.md`](../nats/CLAUDE.md) §4 "Naming a connection"); `Bus.request(subject, payload, timeout=)` (NATS errors → `RequestTimeout` / `NoResponders`); `Bus.reply(subject, handler, queue=)` (handler sync or async; returning `None` publishes no reply; `queue` — queue group for multiple responders, see §6; a message that is not a JSON **object** is dropped with a one-line warning and never reaches `handler` — subjects are open, so a stray `nats pub`/`nats req "x"` must not surface as a `JSONDecodeError` traceback out of the subscription callback, and must not be answered); `Bus.publish(subject, payload)` (fire-and-forget); `Bus.flush()` (push buffered messages to the server — needed before draining, see the handler's `--once` in [`approver/CLAUDE.md`](../approver/CLAUDE.md) §6). Used by both sides of the §7 flow.
- `lib/config.py` — versioned, atomic JSON config store (`handler-config.json` / `responder-config.json` — their contents are §6, in [`approver/CLAUDE.md`](../approver/CLAUDE.md)). `Config.load(path, default=)` (deep-copies the default if the file is missing; **missing file and no `default` → `ConfigError`** — that is how `responder.py serve` fails fast on an unregistered key; a mismatched/absent `v` → `ConfigVersionError`); `Config.save()` (atomic: temp + fsync + `os.replace`, creates parent directories, stamps `v`); dict-like access (`[]`, `[]=`, `get`, `setdefault`, `in`, iteration).
- `lib/yubikey.py` — YubiKey / ARKG helpers over the **optional** `fido2` extra (see §8). Pure, always available: `parse_firmware_version` / `FirmwareVersion` / `MIN_ARKG_FIRMWARE`, `supports_preview_sign`, `device_info_from_info` / `DeviceInfo`, `validate_ikm` / `validate_ctx`, `load_yubico_roots` / `load_yubico_intermediates`, `verify_certificate_chain`, `identify_yubico_certificate`. Pure too, and the bridge to `lib/crypto.py`: `p256_public_b64` (a derived key → the base64 compressed SEC1 point `key_type="p256"` verifies with — see §8.7 in [`approver/CLAUDE.md`](../approver/CLAUDE.md)). Needs `fido2`: `get_device_info` / `get_version`, `make_credential`, `parse_seed_public_key` / `seed_public_key`, `verify_attestation_object` / `verify_yubikey_attestation`, `sign_with_derived_key` / `verify_signature`, `enumerate_devices`, `console_user_interaction` (the shared console touch/PIN prompt, used by both front ends). Persistence (so `make_credential` and derivation can be separate processes): `result_to_dict` / `result_from_dict` / `save_result` / `load_result` (`RESULT_FORMAT_VERSION`). Errors: `YubiKeyError` ← `Fido2NotInstalled` / `NoAuthenticator` / `ExtensionUnsupported` / `AttestationError`. The degrade-don't-crash seam root [`../CLAUDE.md`](../CLAUDE.md) §1 points at is here concretely: `fido2` is imported inside a `try` at module top, the outcome is the `FIDO2_AVAILABLE` flag (what `tests/conftest.py` branches on), and `require_fido2()` is what every device-touching function calls first so the error names `uv sync --extra yubikey` instead of surfacing an `ImportError`. `no_authenticator_hint` does the same job one level down — it checks `IsUserAnAdmin()` so a `NoAuthenticator` on Windows blames elevation rather than the cable. **Library only — no CLI**; the command line lives in `tools/yubikey_exec.py` (§8.5, [`tools/CLAUDE.md`](../tools/CLAUDE.md)).
- `lib/yubico-fido-ca.pem` — the three Yubico **FIDO** attestation roots (U2F Root CA 457200631, FIDO Root CA 450203556, Attestation Root 1) from `developers.yubico.com/PKI`; the trust anchors `verify_yubikey_attestation` pins to. Their sha256 fingerprints are asserted in tests, so replacing this file fails the suite.
- `lib/yubico-fido-intermediates.pem` — the five FIDO-relevant Yubico **intermediates** (FIDO Attestation A 1 / B 1 / B2 1, Attestation Intermediate A 1 / B 1) from `developers.yubico.com/PKI/yubico-intermediate.pem`; the PIV / OpenPGP / Secure Domain / YubiHSM entries of that bundle are deliberately left out. **Not trust anchors** — a YubiKey ships only its end-entity cert in `x5c` (confirmed on firmware 5.8.0: `x5c` length 1) while that cert sits two tiers below the root, so without these the chain cannot be pinned at all and `--attest` reports a false negative on a genuine key. Fingerprints asserted in tests, same as the roots file.
- `lib/crypto.py` — signatures over `cryptography`, two schemes selected by a `key_type` tag: `"ed25519"` (default) and `"p256"` (ECDSA P-256 / secp256r1 with SHA-256). Constants `ED25519` / `P256` / `KEY_TYPES` / `DEFAULT_KEY_TYPE`. API: `generate_keypair(key_type=ED25519)` / `KeyPair` (`.generate(key_type)`, `.from_private_b64(b64, key_type)`, `.private_b64()`, `.public_b64()`, `.sign(bytes)`, attr `.key_type`), `sign(private_b64, bytes, key_type=ED25519)`, `verify(public_b64, bytes, sig_b64, key_type=ED25519) -> bool`. Keys/signatures are standard base64 — ed25519: priv/pub 32B, sig 64B, deterministic; p256: priv 32B scalar, pub 33B compressed point, sig DER (variable length), randomized. The `key_type` is **not** part of the signed bytes: it is pinned by the trusted allowlist entry (bound to `key_id`), so the verifier always uses the registered scheme, never one chosen by the reply. The module is protocol-agnostic: it signs/verifies raw `bytes`, assembling the §7 "signing bytes" is up to the caller. `verify` is fail-safe: any malformed input / unknown `key_type` → `False`, never raises (matching the hook's fail-safe, §7).

## 8. YubiKey / ARKG (`lib/yubikey.py`, optional `fido2` extra)

A library around a YubiKey's **ARKG** (Asynchronous Remote Key Generation) support, exposed through the CTAP2 `previewSign` extension. §8.1–§8.6 are the library and its CLI, usable on their own — §8.5, the CLI, is in [`tools/CLAUDE.md`](../tools/CLAUDE.md); **§8.7 wires it into the §6/§7 approval flow** as `approver/responder_yubikey.py`, in [`approver/CLAUDE.md`](../approver/CLAUDE.md).

**Why ARKG.** The authenticator holds one *seed* key pair. Whoever has the seed **public** key can derive unlimited fresh public keys **offline** — mutually unlinkable, and unlinkable to the seed — by choosing a random `ikm` plus a purpose label `ctx`. Only the authenticator can produce the matching private key, and only when handed the `key_handle` that came out of the derivation. Hence step 2 below needs no hardware and no touch.

**Install:** `uv sync --extra yubikey`. **Hardware:** a YubiKey on firmware **5.8.0+** advertising `previewSign`.

**On Windows: everything here needs an administrator terminal.** Two separate reasons, both confirmed on this machine:

1. Windows hands FIDO authenticators to the WebAuthn API and denies raw HID access to unelevated processes, so `CtapHidDevice.list_devices()` returns an **empty list even with a key plugged in**. `get_device_info` / `get_version` / `yubikey-exec version` therefore fail with `NoAuthenticator` before any ARKG work starts. The message says so explicitly: `no_authenticator_hint` checks `IsUserAnAdmin()` and names elevation as the likely cause instead of asking "is it plugged in?".
2. Even where a client can be built, the native WebAuthn path silently drops the `previewSign` output; `make_credential` raises `ExtensionUnsupported` rather than returning a half-built result.

**Elevating loses the virtualenv.** `py` picks the project `.venv` only because `VIRTUAL_ENV` is set in the shell; `sudo` (and "Run as administrator") starts with a fresh environment, so `py` falls back to the global interpreter and the run dies with `ModuleNotFoundError: No module named 'cryptography'`. Under elevation, name the venv interpreter explicitly:

```
sudo E:\projects\ai-remote\.venv\Scripts\python.exe E:\projects\ai-remote\tools\yubikey_exec.py version
```

The same applies to elevated pytest runs (§8.6) — use `.venv\Scripts\python.exe -m pytest`, not `py -m pytest`.

**Console output stays ASCII** in printed strings (error messages, argparse help) — the Windows console codepage renders `—` as `?`. Docstrings and these docs keep normal typography.


### 8.1 The entry points

| # | Call | Hardware? | Returns |
|---|------|-----------|---------|
| 0 | `get_version()` / `get_device_info()` | plugged in, no touch | `FirmwareVersion` / `DeviceInfo` (firmware, aaguid, extensions, `supports_preview_sign`, `meets_arkg_firmware`) |
| 1 | `make_credential(...)` | touch (maybe PIN) | `MakeCredentialResult` — `key_handle`, `seed_public_key_cbor`, `credential_id`, `aaguid`, `client_data_hash`, `attestation_object`, `seed_attestation_object` |
| 2 | `seed_public_key(result, ctx=..., ikm=None)` | **no** | `DerivedKey` — `derived_public_key` (+`_cbor`), `arkg_args` (+`_cbor`), `ikm`, `ctx`, `key_handle` |
| 3 | `verify_yubikey_attestation(result)` | **no** | `AttestationCheck` — `is_yubikey` plus `reasons` explaining a False |
| 4 | `sign_with_derived_key(result, derived, data=/digest=)` | second touch | raw signature `bytes` |
| 5 | `verify_signature(derived, sig, data=/digest=)` | **no** | `bool`, fail-safe |

- **Versions.** CTAP2 reports `firmwareVersion` packed as `major<<16 | minor<<8 | patch` (5.8.0 = `0x050800`). `parse_firmware_version` decodes it; `raw == 0` means the device did not report one → `is_known is False`. `FirmwareVersion` orders by major/minor/patch (`raw` is excluded from comparison), so `v >= MIN_ARKG_FIRMWARE` is the ARKG floor check.
- **`make_credential`** asks for `algorithms: [ESP256_SPLIT_ARKG_PLACEHOLDER]` under `previewSign.generateKey`, and defaults to `attestation="direct"` so step 3 has a certificate chain to check.
- **`seed_public_key`** is the offline half: it parses the seed key (must be an ARKG-P256 key, else `ExtensionUnsupported`) and returns the derived public key together with the COSE_Sign_Args (`{3: ESP256_SPLIT_ARKG_PLACEHOLDER, -1: key_handle_blob, -2: ctx}`) that the authenticator needs later. `ikm` defaults to 32 fresh random bytes and **must** be ≥ 32 bytes (`MIN_IKM_BYTES`) — the ARKG draft's 256-bit entropy recommendation is enforced, not suggested, because a predictable `ikm` destroys unlinkability. Same `(ikm, ctx)` reproduces the same key; either differing gives a different one.
- **Signing** (`sign_with_derived_key`) is a `getAssertion` with `previewSign.signByCredential[<cred>] = {keyHandle, tbs, additionalArgs: cbor(arkg_args)}` — a second touch. The device reconstructs the private half from `key_handle` + `arkg_args`; nothing secret is stored off-device. Verify with `verify_signature` against `derived_public_key`.

**The `tbs` hashing rule — easy to get wrong.** `tbs` is the **SHA-256 digest**, and the authenticator signs that digest **as-is** (it does not hash again). Meanwhile `ESP256.verify(message, sig)` hashes `message` internally. So:

| You have | `signing_digest(...)` | `verify_signature(...)` | Under the hood |
|----------|----------------------|-------------------------|----------------|
| the data | `data=msg` → `sha256(msg)` | `data=msg` | `ECDSA(SHA256)` |
| only its hash | `digest=h` (must be 32 B) | `digest=h` | `ECDSA(Prehashed(SHA256))` |

Both accept the *same* signature — a test asserts that agreement. Verifying a digest with plain `ECDSA(SHA256)` would hash it a second time and never match, which is why `verify_signature` rebuilds the EC point itself instead of calling `CoseKey.verify` (that one always hashes). `verify_signature` is fail-safe like `lib/crypto.verify`: malformed key/signature/input → `False`, never raises.


### 8.2 "Is it a YubiKey?"

`verify_yubikey_attestation` (thin wrapper over `verify_attestation_object`) requires **three** independent gates, all of which must pass for `is_yubikey`:

1. the attestation statement verifies over `authData || client_data_hash` — the device really holds the attestation private key;
2. the certificate chain pins to a Yubico root from `lib/yubico-fido-ca.pem` (`verify_certificate_chain`; override with `roots=`, or `require_root=False` for a dev key);
3. the attestation certificate names Yubico (`identify_yubico_certificate`).

By default it checks the **credential's** attestation object, which is defined to be signed over `authData || clientDataHash` — the reliable path. `use_seed_attestation=True` instead checks the separate attestation `previewSign` returns for the generated seed key. **On firmware 5.8.0 that object is `fmt: none` with an empty `att_stmt`** (checked on hardware 2026-07-30) — it carries no certificate at all, so it can never yield `is_yubikey: True` and the question of what it binds to is moot. The code degrades cleanly (`is_yubikey False` + a `reasons` entry saying the format carries no chain) rather than raising. Use the default; `--seed-attestation` is only worth revisiting if a later firmware starts returning a real attestation format.

- **Gate 2 needs the intermediates.** A YubiKey's `x5c` contains **only** the end-entity cert (hardware-confirmed: `x5c` length 1), but the real path is three hops — `Yubico FIDO EE Serial <n>` → `Yubico FIDO Attestation B2 1` → `Yubico Attestation Intermediate B 1` → `Yubico Attestation Root 1`. `verify_certificate_chain(chain, roots, intermediates=)` therefore walks upward, splicing in certs from `lib/yubico-fido-intermediates.pem` (the default for both `verify_attestation_object` and `verify_yubikey_attestation`; override with `intermediates=`, or the CLI's `--intermediates PEM`). Every spliced link has its signature verified exactly like a supplied one, and the top of the built path must still be issued by a member of `roots` — so **supplying an intermediate confers no trust by itself**; it widens the reachable *paths*, never the anchor set. A test asserts this (`test_intermediates_are_not_trust_anchors`). Without them the check returns a **false negative on a genuine current YubiKey**, which is how this was found.
- `chain` verification is deliberately **not** full RFC 5280 path validation (no revocation, no name constraints): it verifies each cert is directly issued by the next, bridges gaps from the intermediates pool, and requires the top to chain to a pinned root, which is what device attestation needs.
- `chain_length` in `AttestationCheck` reports what the **device** sent (so 1 on real hardware), not the length of the path that was built; `trusted_root_subject` is the signal that pinning succeeded.
- **AAGUIDs are not hardcoded.** `allowed_aaguids=` narrows to specific models if a caller wants that, but there is no baked-in list: Yubico ships new AAGUIDs with new hardware and treats the FIDO MDS as authoritative, so a bundled list would reject *future* YubiKeys. The chain check is the durable identity signal; the AAGUID is reported for information.
- A name check alone proves nothing — gate 3 exists for a readable `reasons` entry and to reject an obviously foreign authenticator early. Never use it without gate 2.


### 8.3 Reference example vs. this module

Yubico's [`example_arkg.py`](https://github.com/YubicoLabs/build-with-us/blob/main/quickstart/python/example_arkg.py) was the specification for this module, but it targets an **older `previewSign` snapshot**: there the generated key arrived as a dict with a websafe-base64 `"publicKey"`. In `fido2` 2.2.1 the output is a `_SignGeneratedKey` dataclass with raw `bytes` (`key_handle`, `public_key`) plus an already-parsed `attestation_object` — so `websafe_decode` must **not** be applied, and the seed key is `CoseKey.parse(cbor.decode(generated.public_key))`. This module speaks the 2.2.1 shape; `previewSign` is marked DRAFT/experimental upstream, so pin `fido2` and re-check this on upgrade.


### 8.4 Testing without hardware

`tests/test_yubikey_fido2.py` needs no YubiKey: the ARKG seed key is assembled synthetically from two P-256 pairs in exactly the COSE shape the authenticator returns (real derivation, real curve math), and attestation runs against a throwaway CA generated in the test. The `Chain` helper takes `intermediate_cns=` (ordered leaf-first) to build a **deep** root→intermediates→EE chain mirroring the real Yubico shape, with `x5c` carrying only the EE cert — the 1-deep default is what let the §8.2 false negative through, so anything touching chain pinning must be tested at both depths. Separately, `test_every_bundled_fido_attestation_ca_pins_to_a_bundled_root` checks the committed PEMs against each other: every `FIDO Attestation` CA in the intermediates bundle must reach a root in `yubico-fido-ca.pem`. That is the real regression guard and it needs no device and no captured attestation cert. `tests/test_yubikey_exec.py` covers the utility, including the whole `derive` subcommand (it needs no device by design). What *cannot* be covered this way: `make_credential` itself and a full sign/verify round-trip (`fido2` exposes only ARKG `derive_public_key` — the private half lives in the authenticator) — that is what §8.6 is for.


### 8.6 Integration tests with a real key (`tests/test_yubikey_integration.py`)

Optional and skipped by default, in three independent tiers (markers in `tests/conftest.py`):

| Marker | Skips unless | Covers |
|--------|--------------|--------|
| `requires_yubikey` | a key is plugged in and `getInfo` works | enumeration, `get_version`, `DeviceInfo` coherence |
| `requires_preview_sign` | that key advertises `previewSign` | gate for everything ARKG |
| `requires_yubikey_touch` | `AI_REMOTE_YUBIKEY_TOUCH=1` | `make_credential`, real derivation, save/load, attestation, real sign/verify (data **and** digest paths), `yubikey-exec run` / `sign` → offline `verify`, and the §8.7 responder round trip: a real device signature accepted by `hook.verify_reply` |

The touch tier is env-gated **on purpose**: an unattended `py -m pytest` must never block waiting for a finger. The device probe itself (`_probe_yubikey` in `conftest.py`) needs no PIN and no touch, runs once at collection, and turns any failure into a skip reason rather than a collection error.

Run them (Windows: **administrator** terminal):

```powershell
$env:AI_REMOTE_YUBIKEY_TOUCH="1"; py -m pytest tests/test_yubikey_integration.py -v -s
```

`-s` is required, otherwise pytest captures the "touch your YubiKey now" prompt and the run looks hung. Most touch-tier tests share one module-scoped `live_credential` fixture, but each signature costs its own press, so a full pass asks for **several** — the prompts say which step is asking.

**What still cannot be tested without hardware:** a *positive* sign→verify round trip. `fido2` exposes only ARKG `derive_public_key`; the private half exists solely inside the authenticator, so no derived-key signature can be manufactured offline. The unit tests therefore cover `verify_signature` with a stand-in ESP256 pair whose private half we do hold (identical verification path), and assert the negative cases for real derived keys. The positive round trip lives in §8.6.
