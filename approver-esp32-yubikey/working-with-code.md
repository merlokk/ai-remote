# Working with the code — the commands, and the toolchain on this machine

Everything you actually type to build, flash, talk to the board or run the tests.
The **design** lives in §10 — [`CLAUDE.md`](CLAUDE.md) is its map — and stays
there; this file is the mechanics, which have machine-specific answers and no
place in a document made of decisions.

Paths below are **this machine's**, the same way root [`../CLAUDE.md`](../CLAUDE.md)
§5 records the exact `python.exe` behind the `py` launcher. If one stops being
true, do not guess the new one — read the manifest named below, which is what
wrote them.

## Where ESP-IDF is

Not on `PATH`, and there is no Start Menu entry. ESP-IDF here was installed by the
**ESP-IDF Installation Manager** (`eim`, `C:\Program Files\eim\eim.exe`), which
splits the install in two and leaves a manifest naming both halves —
`C:\Espressif\tools\eim_idf.json`, the source of truth if any path here moves.

| Piece | Path |
|-------|------|
| The framework checkout (`IDF_PATH`) | `E:\esp\v6.0.2\esp-idf` |
| Tools + toolchains (`IDF_TOOLS_PATH`) | `C:\Espressif\tools` |
| IDF's own Python venv | `C:\Espressif\tools\python\v6.0.2\venv` |
| **The activation script** | `C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1` |
| Its undo | `C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_deactivate.ps1` |

Installed version: **v6.0.2**, and this is what it builds against. That version is
the reason for three decisions elsewhere: cJSON, libsodium and the USB Host
Library are all managed components here where on v5.5.x they are in the tree
(§10.4), and mbedTLS is the TF-PSA-Crypto one, so the classic entry points have
moved into `mbedtls/private/` and this firmware uses PSA instead (§10.18.2).

## The one line that runs `idf.py`

```powershell
pwsh -NoProfile -Command "& { . 'C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1' *> `$null; idf.py --version }"
# ESP-IDF v6.0.2
```

Replace `idf.py --version` with the real command. That is the form to use in
scripts and by hand alike, because this repo's PowerShell tool keeps the working
directory but **not** shell state between calls — every call re-sources the
profile.

`idf.py` is a **PowerShell function**, not an executable. It does not exist for
`cmd.exe`, for a Bash shell, or for any child process — only inside a PowerShell
session that sourced the profile first. That is the trap worth knowing before it
costs an hour.

Working interactively, source it once:

```powershell
. 'C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1'
idf.py build
```

## Which port is which

**Two USB-C sockets, and they are not interchangeable** (§10.1).

| Socket | What | On this machine |
|--------|------|-----------------|
| **UART** | the CH343P bridge | **`COM6`** — flashing, the log, the console |
| **OTG** | the S3's native USB | a **host** for a security key. Not a serial port and never will be |

If the board does not appear as `COM6`, check which socket the cable is in before
checking anything else.

`COM6` exists whenever the board is powered, because the bridge is a separate chip
— unlike the sibling board, where the port is the chip's own USB and disappears
with a reset.

## Build and flash

From the project directory, in an activated shell:

```powershell
idf.py set-target esp32s3          # once, and it writes sdkconfig
idf.py build
idf.py -p COM6 app-flash           # the app only - seconds
idf.py -p COM6 flash               # + bootloader, partition table, storage.bin
idf.py -p COM6 monitor             # Ctrl+] to leave
```

**Prefer `app-flash`.** A full `flash` writes the SPIFFS image, and that erases
three files the device wrote for itself:

* **`registration.json`** — gone, and it needs a **new** one-time token minted on
  the host (§10.7). The old one is spent and cannot be reused;
* **`config.json`** — every Wi-Fi network, passphrase and static address, replaced
  by the `YOUR_SSID` / `CHANGEME` placeholders this repository ships;
* **`fido.json`** — the key enrolment, so `key enrol` has to run again with the key
  in hand.

**And since §10.18 the enrolment *is* the identity, so a full flash loses the
signing key.** This paragraph used to say the opposite — that the Ed25519 seed in
NVS survives a `flash`, so the device came back with the same `key_id` and the
same public key, unregistered rather than unknown. That is no longer true and was
the most expensive sentence in this file to believe: §10.6's identity is deleted,
the firmware *erases* the seed an older build left in NVS, and the key this device
signs as is derived from `fido.json`. A device that has lost that file comes back
with the same `key_id` and **no key at all**, and `key enrol` — which needs the
security key in hand and a touch — has to run before `register` will even start
(§10.18.1). [`build.md`](build.md) §10.12 has the same warning in its corrected
form.

Before a full flash, read off what you can and keep it:

```
cat config.json
cat registration.json
cat fido.json
```

`fido.json` is worth reading for the record, but note that there is **no console
command that writes a file** — reading it does not let you put it back. The
enrolment is redone with the key, not restored from a dump.

Two other things worth knowing:

* `idf.py size-components` is how §10.4's dependency decisions get costed;
  [`build.md`](build.md) has the last numbers taken;
* `idf.py partition-table` prints the layout `partitions.csv` produced.

## Talking to the console from a script

`idf.py monitor` is interactive, which makes it useless from a script. Use
pyserial out of IDF's own venv — it is already there:

```powershell
& { . 'C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1' *> $null; python -c @'
import serial, time, sys
s = serial.Serial("COM6", 115200, timeout=0.4)
time.sleep(0.5); s.reset_input_buffer()
buf = b""
for cmd in (b"status\r\n", b"led\r\n", b"request\r\n"):
    s.write(cmd); time.sleep(1.5); buf += s.read(16384)
s.close()
sys.stdout.write(buf.decode("utf-8", "replace"))
'@ }
```

To reset the board first, toggle RTS before reading:

```python
s.setDTR(False); s.setRTS(True); time.sleep(0.1); s.setRTS(False); time.sleep(0.3)
```

**Never send `term` or `term smart` from a script.** In that mode linenoise blocks
on a cursor-position query before each prompt, and a script that does not answer
leaves the console silent until the board is reset. If it happens: send
`\x1b[24;80R` to unblock it, then `term dumb`.

Commands worth knowing before a first session — [`commands.md`](commands.md) is
the full list:

```
help          esp_console's own listing
status        is this the build I think it is, in the slot I think it is
led           what the device thinks it is, and what the light is doing
request       is it on approvals.*, and if not, which of the four things is missing
key           what is on the OTG port, and the p256 key this device signs as
key selftest  the ARKG derivation on this chip - needs nothing plugged in
```

## The tests

```powershell
host_test\run.cmd            # everything
host_test\run.cmd led fido   # only the matching suites
```

It needs MSVC, CMake and Ninja — all three already on this machine for other
reasons, and `run.cmd` names the exact paths it expects. It also needs
`managed_components/` to exist, which means `idf.py build` has been run at least
once (that is where cJSON comes from).

[`tests.md`](tests.md) is what each tier pins.

To regenerate the cross-language parity vectors — **two generators, and this
folder's tests need both**:

```
..\scripts\make-vectors.cmd                  # §7's decision bytes (lives next door)
..\scripts\esp32yk-make-vectors.cmd          # the ARKG derivation (§10.18, lives here)
..\scripts\make-vectors.cmd --check          # and these two are what CI would run
..\scripts\esp32yk-make-vectors.cmd --check
```

Neither needs a board or a key. The second one also writes
`components/arkg/arkg_selftest_vector.h`, which ships **in the firmware** — so
after running it, rebuild before believing `key selftest` on the device.

## Registering the device end to end

Three commands and one paste. The handler has to be running while the device
sends, and NATS has to be up (`cd nats && docker compose up -d`).

```powershell
# 1. mint a token
E:\projects\ai-remote\.venv\Scripts\python.exe `
    E:\projects\ai-remote\approver\registration_handler.py --get-token approver-esp32-yubikey

# 2. leave the handler serving in another window
E:\projects\ai-remote\.venv\Scripts\python.exe `
    E:\projects\ai-remote\approver\registration_handler.py --once
```

```
# 3. on the device console
register approver-esp32-yubikey.<the secret it printed>
```

**Since §10.18 there is a step 0, and it is not optional**: the key being
registered is derived from a security key, so the device has nothing to register
until one is enrolled. `register` refuses with a sentence saying so.

```
# 0. on the device console, with a security key in the OTG port
key selftest        # needs no key: the derivation, on this chip, against Python
key enrol           # one touch. prints the p256 key this device will sign as
```

And the ordering has a cost worth knowing before you pay it: **a re-enrolment
invalidates the registration**, because the derived key is new. The device says
`registered STALE` and stays off `approvals.*` until a fresh token is minted. The
token is one-time, so an enrolment done *after* a registration costs a trip back
to step 1.

**A PowerShell background job will not do for step 2.** Each PowerShell invocation
here is a fresh process, so `Start-Job` dies with it — which looks from the device
like `the answer was empty`, and is not a bus problem. Run it in a window you keep
open, or as a genuinely detached background process.

Compare the handler key the device pins against what the handler printed at
startup, once, by eye. After that it is pinned and a reply signed by any other key
is refused.
