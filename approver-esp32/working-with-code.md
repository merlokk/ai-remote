# Working with the code — the commands, and the toolchain on this machine

Everything you actually type to build, flash, talk to the board, render a screen
or regenerate a test vector. The **design** lives in §10 — [`CLAUDE.md`](CLAUDE.md) is
its map, and `hardware.md`, `protocol.md`, `firmware.md`, `screens.md`, `web.md`,
`tests.md` and `build.md` own the sections between them — and stays there; this file is the mechanics, which have machine-specific answers
and no place in a document made of decisions. Where a command exists because of
a decision, the section number is next to it — follow it for the *why*.

Paths below are **this machine's**, the same way root [`../CLAUDE.md`](../CLAUDE.md)
§5 records the exact `python.exe` behind the `py` launcher. If one of them stops
being true, do not guess the new one — read the manifest named below, which is
what wrote them.

## Where ESP-IDF is

Not on `PATH`, and there is no Start Menu entry: ESP-IDF here was installed by
the **ESP-IDF Installation Manager** (`eim`, `C:\Program Files\eim\eim.exe`),
which splits the install in two and leaves a manifest naming both halves —
`C:\Espressif\tools\eim_idf.json`, the source of truth if any path here moves.

| Piece | Path |
|-------|------|
| The framework checkout (`IDF_PATH`) | `E:\esp\v6.0.2\esp-idf` |
| Tools + toolchains (`IDF_TOOLS_PATH`) | `C:\Espressif\tools` |
| IDF's own Python venv | `C:\Espressif\tools\python\v6.0.2\venv` |
| **The activation script** | `C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1` |
| Its undo | `C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_deactivate.ps1` |

Installed version: **v6.0.2**, and this is the one it builds against. §10.4
names v5.5.3 as the version Waveshare's examples are written against; §10.12
settled that argument in favour of what is installed, because the
version-sensitive half — panel, touch, LVGL — resolves and builds here, and
`main/idf_component.yml` asks for `idf: ">=5.5"` rather than a pin. So there is
nothing to work around: v5.5.3 is the version to reach for only if a display or
touch fault ever starts looking like a framework fault. Targets this install
carries: `esp32`, `esp32c6`, `esp32p4`, `esp32s3` — so `set-target esp32c6`
works.

## The one line that runs `idf.py`

```powershell
pwsh -NoProfile -Command "& { . 'C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1' *> `$null; idf.py --version }"
# ESP-IDF v6.0.2
```

Replace `idf.py --version` with the real command. That is the form to use in
scripts and by hand alike, because this repo's PowerShell tool keeps the working
directory but **not** shell state between calls — every call re-sources the
profile.

Dot-sourcing that profile is the whole of "an IDF-enabled shell": it sets
`IDF_PATH` / `IDF_TOOLS_PATH` / `IDF_PYTHON_ENV_PATH`, prepends the toolchain,
CMake, Ninja and ccache directories to `PATH`, activates the venv, and defines
`idf.py` — plus `esptool.py`, `espefuse.py`, `espsecure.py`, `parttool.py` — as
**PowerShell functions**.

That last word is the trap worth knowing before it costs an hour: `idf.py` is an
alias for `Invoke-idfpy`, not an executable. It does not exist for `cmd.exe`, for
the Bash tool, or for any child process — only inside a PowerShell session that
sourced the profile first.

Two details of that line:

- **`*> $null` only swallows the profile's banner.** Drop it when something goes
  wrong: the banner is where it prints which `IDF_PATH` it picked.
- **`-e` prints instead of sets.** `. '…PowerShell_profile.ps1' -e` dumps the
  environment it *would* apply and returns — the quick way to check a path
  without changing the shell.

Working interactively, source it once and forget the wrapper:

```powershell
. 'C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1'
idf.py build
. 'C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_deactivate.ps1'   # to undo it
```

### From `cmd.exe`

The checkout carries the classic activator: `E:\esp\v6.0.2\esp-idf\export.bat`,
run once per session, after which `idf.py` works in that session. `install.bat`
beside it is the re-installer, not the activator — running it to "fix" a shell
that has not been activated is the wrong move.

`eim` did not leave a `.cmd` launcher on this box, so the PowerShell line above
is the one to copy.

### When the version has to change

`eim` installs versions side by side; `eim select <version>` switches which one
the manifest points at, and each version brings **its own** activation profile
(`Microsoft.<version>.PowerShell_profile.ps1`) next to the one above. So
switching means sourcing a different file, not editing this one — after which
every path in the table changes and this file needs a pass.

## Build and flash

In an activated shell, from the project directory:

```powershell
idf.py set-target esp32c6
idf.py build
idf.py -p COM4 flash monitor      # Ctrl+] to leave the monitor
```

- **The port is the USB Serial/JTAG device**, and it is the same port
  `esp_console` serves the §10.7 commands on — the monitor and a manual
  `register` cannot both hold it.
- `idf.py size-components` is how the dependency decisions of §10.4 get costed;
  §10.12 asks for those numbers to be recorded when they are taken.
- `idf.py partition-table` prints the layout `partitions.csv` produced — the
  quick check that an offset edit landed where it was meant to.

**`flash` writes 11 MB of SPIFFS every time, and usually you do not want that.**
`spiffs_create_partition_image` builds an image the size of the whole `storage`
partition (§10.15), not the size of its contents, so a full flash carries ~11 MB
past two files. For the edit-build-run loop:

```powershell
idf.py -p COM4 app-flash monitor   # the app only — seconds
idf.py -p COM4 flash monitor       # + bootloader, partition table, storage.bin
```

The full one is needed after a change to `partitions.csv` or to anything under
`spiffs_image/`.

**And the full one erases the two files the device wrote for itself**, which is
the sentence this section was missing until it cost a registration: `storage.bin`
is built from `spiffs_image/`, so flashing it puts the *committed* `config.json`
back over the one the device has been editing and takes `registration.json` with
it. What that costs, in order of annoyance:

- **the registration is gone** and needs a **new one-time token** minted on the
  host (§10.7) — the old one is spent and cannot be reused;
- **every Wi-Fi network, passphrase and static address is gone**, replaced by the
  `YOUR_SSID` / `CHANGEME` placeholders the repository ships;
- the identity is **not** gone: the Ed25519 seed lives in NVS (§10.6), which a
  `flash` does not touch, so the device comes back with the same `key_id` and the
  same public key — unregistered rather than unknown.

So before a full flash, read them off and keep them:

```powershell
# via the pyserial snippet below
cat config.json
cat registration.json
```

`app-flash` has none of these consequences, which is the other reason to prefer
it: a change confined to `main/` or `components/` never needs the storage
partition rewritten.

Creating the tree, once — it is **generated**, not copied from another board
(§10.12, §10.14.4):

```powershell
idf.py create-project approver-esp32
idf.py create-component <name>                     # per library-layer piece (§10.14.2)
idf.py add-dependency "debsahu/espidf-nats^1.4.0"  # and the rest of §10.4
```

## The device console (§10.7)

**The port on this machine is `COM4`** — the board's USB-C is the C6's own USB
Serial/JTAG, so it appears only while the board is powered and it is the same
port the monitor uses. Open one and type into it:

```powershell
idf.py -p COM4 monitor
```

**What you can type is in [`commands.md`](commands.md)** — every command, every
subcommand, and what each one does. It is not repeated here: two copies of a
command list means one of them is wrong, and the one in a *mechanics* file is
always the one nobody remembers to update.

A few worth knowing before the first session, because they are how you find out
the flash worked at all:

```
help                          # esp_console's own listing
status                        # is this the build I think it is, in the slot I think it is
cat config.json               # read a file back off the device without reflashing
wifi                          # what the radio wants, and what it is doing
```

**The up-arrow is off until you type `term`.** History is there (the last 32
lines), but the line editor is disabled: the probe that would enable it runs
while the REPL is created, and on USB Serial/JTAG nobody is attached that early
to answer it. In a monitor session, `term` re-runs the probe, your terminal
answers, and arrows, Ctrl-A/E and the rest start working for that session — §10.7
has why this is a command rather than the default.

**Never type `term` or `term smart` in a scripted session.** In that mode
linenoise blocks on a cursor-position query before each prompt, and a script
that does not answer it leaves the console silent until the board is reset. If
it happens: send `\x1b[24;80R` to unblock it, then `term dumb`.

**`idf.py monitor` is interactive, which makes it useless from a script or an
agent.** To send a command and read the answer without holding a terminal, drive
the port directly — IDF's own venv already has `pyserial`, so nothing needs
installing:

```powershell
& 'C:\Espressif\tools\python\v6.0.2\venv\Scripts\python.exe' -c @'
import serial, time, sys
s = serial.Serial(); s.port = "COM4"; s.timeout = 0.2
s.dtr = False; s.rts = False          # do not reset the board on open
s.open()
for cmd in ["status", "cat config.json"]:
    s.write((cmd + "\r\n").encode()); s.flush()
    end = time.time() + 2
    while time.time() < end:
        sys.stdout.write(s.read(4096).decode("utf-8", "replace"))
s.close()
'@
```

Nothing else may hold the port while this runs — a monitor left open in another
window is the usual reason it returns silence.

**`s.dtr = False; s.rts = False` before `open()` is not decoration, and getting
it wrong costs an evening.** On the C6's native USB Serial/JTAG those two line
states are how a host asks the chip for a reset and for download boot — and
`serial.Serial("COM4")`, the one-line constructor form, asserts *both* when it
opens. So the obvious way to open this port is a request to enter the ROM
downloader, and the board that answers afterwards is the ROM rather than the
firmware. Always the three-line form above: build the object, set the lines,
then open.

**And a board that has ended up there does not come out over USB.** The ROM
prints `boot:0x15 (DOWNLOAD…)` and `waiting for download`; every reset esptool
can send arrives as `rst:0x15 (USB_UART_HPSYS)`, which does not clear the latch,
and `--after watchdog-reset` answers *"not supported on ESP32-C6"*. What clears
it is a power-on reset: **hold `PWR` for six seconds, then press it briefly.**
[`screens.md`](screens.md) §10.8.5 has the whole diagnosis.

**To capture a boot log you have to reopen the port**, because a hardware reset
drops the USB device and takes any open handle with it. Loop on `serial.Serial()`
until it comes back — the app's own logging starts about 700 ms in, so nothing
of it is missed:

```python
deadline = time.time() + 20
seen, s = [], None
while time.time() < deadline:
    if s is None:
        try:
            s = serial.Serial(); s.port = "COM4"; s.timeout = 0.1
            s.dtr = False; s.rts = False
            s.open()
        except Exception:
            s = None; time.sleep(0.05); continue
    try:
        chunk = s.read(4096)
    except Exception:
        s = None; continue          # it went away again
    if chunk:
        seen.append(chunk.decode("utf-8", "replace"))
```

**A killed `idf.py monitor` leaves orphans holding COM4.** Timing one out (or
Ctrl-C'ing the wrapper) can leave `idf.py`, `idf_monitor.py` and
`esp_idf_monitor` processes alive, and the next flash fails with *"Could not
open COM4, the port is busy"*. Find and clear them:

```powershell
Get-CimInstance Win32_Process |
  Where-Object { $_.Name -match 'python' -and $_.CommandLine -match 'monitor' } |
  Select-Object ProcessId, CommandLine
Stop-Process -Id <ids> -Force
```

Setting the clock from the host's own time, in one line:

```powershell
$now = (Get-Date).ToUniversalTime()
"date set utc $($now.ToString('yyyy-MM-dd')) $($now.ToString('HH:mm:ss'))"
```

— paste the result into the console, or feed it to the pyserial snippet above.

**`ToUniversalTime()` and `set utc` are the point**: the device keeps UTC
(§10.8.2), and sending it the host's local time without saying so would set the
clock wrong by the offset and look right on screen, because the zone would then
shift it a second time. `date set` without `utc` is the other half — it takes
local time deliberately, for when you are reading a wall clock rather than
scripting.

## Registering the device (§6, §10.7)

Three things on the host, then one line on the device. **The venv, not `py`** —
`nats-py` and `cryptography` are not in the launcher's interpreter, and the
failure is a `ModuleNotFoundError` several frames deep:

```powershell
docker compose -f nats\docker-compose.yml up -d          # if the bus is not already up
& .\.venv\Scripts\python.exe approver/registration_handler.py --get-token approver-esp32
& .\.venv\Scripts\python.exe approver/registration_handler.py     # and leave it serving
```

The first prints the token **and the server key**; the second prints the server
key again as it starts. That string is what the device shows after registering,
and comparing the two by eye, once, is the whole of trust on first use.

Then, on the device (`commands.md` has the failure cases):

```
register approver-esp32.<44 characters>
```

The device needs a **client link** for this — the bus, not just the radio. It
takes its `nats.url` from `config.json`; `wifi join <ssid> <password>` and
`wifi mode client` are what get it there, and without a `config save` the network
lives in RAM only and is gone at the next boot.

**Testing the pin without touching the real handler**: run a second one against a
throwaway config, which gives it a server key of its own. A device already pinned
must refuse its perfectly valid `ok:true`:

```powershell
$cfg = "$env:TEMP\impostor.json"
& .\.venv\Scripts\python.exe approver/registration_handler.py --config $cfg --get-token approver-esp32
& .\.venv\Scripts\python.exe approver/registration_handler.py --config $cfg
```

— and the device should answer `signed by a different key than this device
already trusts`, with `registration.json` unchanged.

## Putting one request through the whole loop (§7)

`tools/test_request.py` is the probe: it sends exactly what `hook.py` would send
and judges the answer with `hook.verify_reply` against the real allowlist, so the
verdict it prints is the verdict Claude Code would act on.

```powershell
& .\.venv\Scripts\python.exe tools/test_request.py --command "echo hello" --timeout 75
```

**Mind the quoting**: `--command "two words"` needs the quotes *inside* the
argument list when it is started from `Start-Process`, or PowerShell splits it and
argparse refuses the tail.

Then press **BOOT** on the board to allow or **PWR** to deny — there is no console
command for either, and there will not be. A good run ends in

```
  behavior  : allow
  key_id    : approver-esp32
  verdict   : TRUSTED - Claude Code would allow this
```

Press nothing and you get the other half, which is just as much a test:
`no answer: nobody decided in time (the hook would fall back to its own prompt)`.

The device's side of the same exchange is `request` on the console
([`commands.md`](commands.md)) — `answering` says whether it is on the subject at
all, and `wire` / `sent` say what went through it.

## Sounds: mp3 in, WAV out (§10.8.1)

The firmware has **no decoder** and does not want one (`components/audio/speaker.h`
argues it), so anything that gets played is converted on this machine first.
`ffmpeg` is installed here by winget — `Gyan.FFmpeg`, which puts it under
`%LOCALAPPDATA%\Microsoft\WinGet\Packages\...` and on `PATH` for new shells:

```powershell
ffmpeg -y -i poweron.mp3 -ac 1 -ar 16000 -c:a pcm_s16le -map_metadata -1 -fflags +bitexact poweron.wav
```

- `-ac 1 -ar 16000 -c:a pcm_s16le` is exactly what the driver plays: mono,
  16 kHz, signed 16-bit PCM. Other rates work (8 k / 32 k / 44.1 k / 48 k) and
  cost proportionally more flash; stereo and anything compressed are refused by
  name at `play` time.
- `-map_metadata -1 -fflags +bitexact` keeps the encoder from adding a `LIST`
  chunk. The parser walks chunks and would survive one, but a header that is
  only `fmt ` and `data` is the one to ship.
- Three seconds of 16 kHz mono is ~100 KB. The `storage` partition is 10.9 MB,
  so this is the resource the board has to spare.

**After changing anything under `spiffs_image/`, the full `idf.py flash` is
required** — `app-flash` does not write the image, and the symptom is a device
happily playing yesterday's file.

**And the full flash takes the device's own files with it.** `storage.bin` is
written over the *whole* partition, so a live `config.json` — the real networks
and their passwords — and `registration.json` are gone, and the device comes back
on the factory defaults, unregistered (§6 has what re-registering costs: a token
minted on the host and typed over USB). §10.15 designs for a config that can be
*restored*; it does not design for one overwritten by a build. So before a full
flash:

```
cat config.json          # on the device's console, and keep what it prints
cat registration.json
```

Learned the expensive way while adding a page for §10.16.

**Only a full flash does that, and most changes are not one.** `idf.py -p COM4
app-flash` writes the app partition and nothing else, so a firmware change — no
edit under `spiffs_image/` — needs none of what follows: the device keeps its
files because nothing wrote over them. The staging below is for the case where a
*page*, a sound or the splash changed and the board is one you do not want to
re-register.

**And there is a way to keep them, which is what the §10.16 site was flashed
with.** Read the two files off the console, build the image from a staging copy
rather than from `spiffs_image/` with those two put back into it, and write only
the `storage` partition. Stage it **outside the repository**: a real WPA key in a
committed file is a real WPA key in the history.

Step 1, and it is the one with a trap in it — read them back, and check what you
read:

```powershell
$stage = "$env:TEMP\spiffs_stage"          # outside the repo, deliberately
& 'C:\Espressif\tools\python\v6.0.2\venv\Scripts\python.exe' -c @'
import serial, time, os, json, sys
stage = sys.argv[1]
s = serial.Serial(); s.port = "COM4"; s.timeout = 0.2
s.dtr = False; s.rts = False          # the three-line form, always
s.open()

def cat(name):
    s.reset_input_buffer()
    s.write(("cat " + name + "\r\n").encode()); s.flush()
    end = time.time() + 3.0; buf = b""
    while time.time() < end:
        buf += s.read(4096)
    # **The port doubles the carriage returns**, and this is the whole trap: a
    # file whose lines end `\r\n` arrives as `\r\r\n`, because the USB console
    # translates the `\n` a second time. Undo that pair *first* -- the obvious
    # `.replace("\r\n", "\n")` on its own turns each break into two.
    text = buf.replace(b"\r\r\n", b"\n").replace(b"\r\n", b"\n")
    head = ("cat " + name + "\n").encode()
    return text.split(head, 1)[1].split(b"\napprover>", 1)[0]

cfg, reg = cat("config.json"), cat("registration.json")
c, r = json.loads(cfg.decode()), json.loads(reg.decode())
print("config.json", len(cfg), "| networks:", [n["ssid"] for n in c["wifi"]["networks"]])
print("registration.json", len(reg), "| key_id", r["key_id"], "| ts", r["registered_ts"])
print("server_key", r["server_key"], len(r["server_key"]), "chars")
open(os.path.join(stage, "config.json"), "wb").write(cfg)
open(os.path.join(stage, "registration.json"), "wb").write(reg)
s.close()
'@ $stage
```

**Check the values, not the byte count.** `ls` on the console prints each file's
size and comparing against it is the obvious check — `config.json` came back at
exactly its 1,138 bytes here — but `registration.json` did not: 133 captured
against 144 on the device, and the difference is line endings in whatever wrote
it rather than anything lost. What matters is that the device *parses* the file,
so what has to be right is the content: the `key_id`, a `server_key` of exactly
44 characters and equal to the one `keys` prints, and `registered_ts`. Assert
those three before writing anything, and a truncated capture cannot get past you.

**And never print a password into anything that gets committed.** `config.json`
carries the real WPA keys; the capture above puts them straight into a scratch
directory and nowhere else, and the sizes and SSIDs are all it echoes.

Then the image, and only the partition that changed:

```powershell
# the same spiffsgen the build runs -- the arguments are in build\build.ninja,
# not guessed: size, page 256, name 32, meta 4, and both magic flags
& 'C:\Espressif\tools\python\v6.0.2\venv\Scripts\python.exe' `
    E:\esp\v6.0.2\esp-idf\components\spiffs\spiffsgen.py 0xae0000 `
    $stage "$env:TEMP\storage-live.bin" `
    --page-size=256 --obj-name-len=32 --meta-len=4 --use-magic --use-magic-len

# the app if it changed, then that image at the `storage` offset from
# partitions.csv -- 0xae0000 above and 0x520000 here are that file's own numbers
idf.py -p COM4 app-flash
python -m esptool --chip esp32c6 -p COM4 -b 460800 --before default-reset `
    --after hard-reset write-flash --flash-mode dio --flash-size 16MB `
    --flash-freq 80m 0x520000 "$env:TEMP\storage-live.bin"
```

Done this way the board comes back with its networks, its pinned handler key and
its registration date unchanged — a token not minted and a registration not
repeated. Three checks on the console, and each answers a different question:
`ls` says the new files are there and the sizes moved the way you expected,
`keys` says the pinned handler key and the registration date are the ones from
before, and `wifi` says it rejoined a network it could only know from the file.

## The host tests (§10.11, tier 1)

One command, no board, nothing to install:

```powershell
approver-esp32\host_test\run.cmd            # all of them
approver-esp32\host_test\run.cmd config     # one suite
approver-esp32\host_test\run.cmd i2c pmic   # several
```

A suite name is matched as a substring, and the names are `navigator`, `clock`,
`request`, `signing`, `i2c`, `pmic`, `rtc`, `imu`, `es8311`, `config`,
`buttons`, `timezone`, `speaker`, `wifi`, `reach`, `timesync`, `nats` and
`vectors`. Several answer to a second name because the obvious one is
ambiguous: `sync` also selects `timesync`, `bus` also selects `nats`, `face`
also selects the clock face, `card` also selects the request card, `parity`
also selects the vectors, and `protocol` selects every §6/§7 suite at once —
while `wifi` selects the policy **and** the internet check, since they are two
halves of one component. `test_main.cpp` is the list, and adding a suite means
adding a line there.

Run everything before committing; filter while debugging, because scrolling
past three hundred lines of `PASS` to find the one that matters is how a suite
stops being run.

The tail of a good run:

```
381 Tests 0 Failures 0 Ignored
OK
```

It sources MSVC, configures with CMake + Ninja, builds and runs. All three
tools are already on this machine for other reasons — MSVC for the LVGL
preview below, CMake and Ninja from ESP-IDF. Two paths it needs:

- **`IDF_PATH`**, for Unity's sources, with `E:\esp\v6.0.2\esp-idf` as the
  built-in fallback;
- **`managed_components/espressif__cjson`**, because the config tests use the
  real parser rather than a stand-in. That directory is fetched by `idf.py
  build` and is not in git, so **run the firmware build once** before the tests
  on a fresh checkout. CMake says so by name if it is missing.

**Mutation-check anything worth trusting.** The habit this suite is built on:
break the rule the test claims to protect, run it, watch the right test fail,
put it back. It takes a minute and it is the difference between a test and a
line that always passes. Forty-odd invariants have been through it; §10.11 lists
them by name, grouped by the pass that produced them — including the ones that
*survived*, which turn out to be the most useful runs of all: a mutation nothing
catches is a question about the code, not a gap in the tests.

The cheap way to do a batch of them: copy the driver aside, `git checkout --`
it back to the last commit, run the suite, and check that **exactly** the
tests you expected fail. Reverting the whole file is a coarse mutation, but it
is the honest one when a change added several rules at once, and "seven
failures, and they are these seven" is a stronger statement than seven
separate runs.

One trap in doing it: **`/W4 /WX` turns a now-unused variable into a build
error**, so a mutation has to keep consuming whatever it stops using —
`Reset(false, now)` fails to compile where `Reset(level && false, now)` does
the same damage and builds.

```powershell
# the shape of it
git stash              # or just edit and undo by hand
# …break one line in the driver…
approver-esp32\host_test\run.cmd i2c
git checkout -- approver-esp32/components/i2cbus/i2c_bus.cpp
```

**The test filesystem is `host_test/build/fs`**, and it is deliberately a
short *relative* path: `storage::kMaxPathLength` is 64 and `ResolvePath`
refuses rather than truncating, which the host's own temp directory
(`C:\Users\…\AppData\Local\Temp\…`) blows straight past. Widening the buffer in
the fake would have hidden a constraint the device really has.

**The sources under test are the ones that ship.** `host_test/fakes/` shadows
ESP-IDF's headers on the include path, so the drivers that talk to hardware —
`i2c_bus.cpp`, `axp2101.cpp`, `pcf85063.cpp`, `qmi8658.cpp`, `es8311.cpp`,
`speaker.cpp`, `buttons.cpp`, `config.cpp` — compile unmodified against a fake
platform. The pure-logic files (`navigator.cpp`, `timezone.cpp`, `wifi_policy.cpp`,
`reachability.cpp`, `sync_policy.cpp`, `endpoint.cpp`, `link_policy.cpp`) need no
fake at all, which is the point of the split in §10.14.2 rather than a
convenience. `host_test/CMakeLists.txt` is the current list.

Two consequences of the shadowing, worth knowing before they bite:

- **The fakes have to match ESP-IDF's struct field order.** The drivers fill
  those configs with designated initialisers, so a field out of place is a
  compile error here rather than a silent difference — which is the good case,
  but it means an IDF upgrade that reorders one is a `fakes/` edit.
- **MSVC is stricter in different places than GCC.** It needs `/std:c++20` for
  designated initialisers that GCC accepts far earlier, and its `/W4 /WX`
  objects to things `-Wall -Wextra` does not (a bare `0xFF` in a Unity
  `HEX8` assertion, for one). Fix the test, not the driver.

Adding a suite is a file, one `Register…Tests()` line in `test_main.cpp`, and
the source under test in `CMakeLists.txt`.

**It is not `idf.py --preview set-target linux`, and the reason is worth not
rediscovering.** That target is listed by this install and does not work on a
Windows host: it selects esp-clang and then tries to link a Windows PE against
`kernel32`/`user32` with `ld.lld` —

```
ld.lld: error: unable to find library -lkernel32
```

— after which `idf.py build` quietly falls back to the `esp32` target and
succeeds, which is the confusing part. §10.11 has what that costs and what was
kept from the plan anyway.

## The boot splash (§10.8)

Regenerate it after changing the generator, then reflash the image:

```powershell
powershell.exe -ExecutionPolicy Bypass -File approver-esp32\tools\make-splash.ps1
idf.py -p COM4 flash          # the full one: app-flash does not write SPIFFS
```

- **`powershell.exe`, not `pwsh`.** The rasteriser is `System.Drawing`, which
  is in the box on Windows PowerShell 5.1 and a NuGet package on PowerShell 7.
- The output is `spiffs_image/splash.bin` — 480×480 raw RGB565 **big-endian**,
  no header, 460 800 bytes. Big-endian is the panel's own byte order, so the
  bytes are sent untouched; getting it backwards does not fail, it produces
  plausible wrong colours.
- It is deterministic (a fixed `-Seed`), so re-running it does not show up as a
  460 KB diff. Pass a different seed for a different rain.

To look at it without a board, ffmpeg reads the raw format directly:

```powershell
ffmpeg -f rawvideo -pix_fmt rgb565be -s 480x480 -i approver-esp32\spiffs_image\splash.bin -y splash.png
```

## A screenshot of the panel (§10.8)

One command, and it needs ESP-IDF's own python because that is the interpreter
with `pyserial` in it — the same one the console snippet above uses:

```powershell
& 'C:\Espressif\tools\python\v6.0.2\venv\Scripts\python.exe' `
    approver-esp32\tools\screenshot.py --port COM4 screen.png
```

The device half is the `screenshot` command ([`commands.md`](commands.md)); this
is the host half, and it needs **nothing installed** — the PNG is written with
`zlib` and `struct`, because an image library is not on root §1's list and this
is not worth a conversation.

- **Nothing else may hold the port**, the usual reason a capture returns
  silence. A serial monitor left open in an IDE window counts, and it does not
  show up as a stray `python` process the way a killed `idf.py monitor` does.
- **It takes a few seconds and stalls the screen while it runs**, which is
  expected: `clock` reports a handful of frames given up for the display
  afterwards.
- **Decoding a capture taken some other way** needs no port at all, so this
  works with the plain `py` launcher:

  ```powershell
  py approver-esp32\tools\screenshot.py --from dump.txt screen.png
  ```

  Which is also how the decoder is tested without a board: feed it a synthetic
  capture with a known picture in it, and a shear, a byte-order slip or an
  upside-down frame are all visible in the result.
- **Checking that the picture is the screen** rather than a plausible image: run
  `clock` next to it. It prints the digits, the drift and every indicator's
  state, so the two are independent answers to the same question — and the drift
  is what places the geometry, so a picture that decodes to the time `clock`
  reported is a picture in the right place as well.

## Parity vectors for the tests (§10.11, tier 2)

Fixtures are generated by the Python implementation itself and **committed**, so
a fresh checkout builds and tests with no Python step. One command writes both
headers:

```powershell
E:\projects\ai-remote\.venv\Scripts\python.exe approver-esp32\tools\make_vectors.py
# up to date
```

**Note the interpreter, and it is not optional here.** `py` has no `cryptography`
— that lives in the project's venv, so anything touching `lib/crypto.py` runs as
`.venv\Scripts\python.exe`.

What it writes:

| File | Consumed by |
|------|-------------|
| `host_test/vectors/parity_vectors.h` | `host_test/test_vectors.cpp` — and `test_signing.cpp`, whose sample request is now the `allow-bash` vector |
| `components/crypto/selftest_vector.h` | `components/crypto/device_key.cpp`, the boot self-test of §10.6 |

`--check` writes nothing and exits 1 when either is stale, which is what the
pytest guard reports:

```powershell
E:\projects\ai-remote\.venv\Scripts\python.exe -m pytest tests/test_esp32_vectors.py -q
```

**Run that after changing `approver/protocol.py`.** It is the half of the tier
that cannot live in C++: the firmware suite compiles the vectors, and only Python
can say whether they are still what Python produces. Without it a fixture is a
pasted literal with extra steps.

The Ed25519 vector is the one thing here worth **not** regenerating casually —
it is the identity the boot self-test compares against, and the seed and message
are fixed for that reason: seed `00 01 02 … 1f`, message
`ai-remote approver-esp32 libsodium self-test v1`,

```
pub A6EHv/POEL4dcN0Y50vAmWfk1jCbpQ1fHdyGZBJVMbg=
sig BAzU2LyejENvC1nkIPHSfBpKP30SSGPZpGfjpPzApJUEg3kkTQP10cKjFKD15gQjlTq19AilUqET4kvTkwG/CQ==
```

— which `crypto_sign_seed_keypair` and `crypto_sign_detached` on this board
reproduce exactly, both with and without `CONFIG_LIBSODIUM_USE_MBEDTLS_SHA`
(§10.6). Changing either constant in the generator rotates that vector and needs
a reflash before the board's self-test passes again.

## The device tier — the board, answered by hand (§10.11, tier 3)

The acceptance test for this whole folder, and the only place the key bound to
this chip meets the allowlist Claude Code verifies against:

```powershell
scripts\esp32-approval.cmd
```

It asks for two interactions and says so on screen: press **ALLOW** (`BOOT`) on
the card that appears, then press **nothing at all** for about twenty seconds —
§10.10's fail-safe, which is a property no host test can reach. Everything else
is derived from the one signed reply without asking again.

By hand, and `-s` is not optional — without it pytest swallows the prompt and a
run waiting for a finger looks like a run that has hung:

```powershell
$env:AI_REMOTE_ESP32_DEVICE="1"
E:\projects\ai-remote\.venv\Scripts\python.exe -m pytest tests/test_esp32_device.py -v -s
```

**Stop every other responder first.** §6's queue group hands each request to
exactly one subscriber, so a `responder.py serve` or an open browser tab may
answer instead of the board. Who is actually on the bus, without raising a card:

```powershell
(Invoke-RestMethod http://127.0.0.1:8222/connz?subs=1).connections |
    Select-Object ip, lang, version, subscriptions_list
# ip           lang   version subscriptions_list
# 172.20.0.1   espidf 1.4.0   {approvals.*, status, activity}
```

`lang: espidf` is the board — and `name` being empty is §10.5's recorded
limitation, not a fault. **Three subjects, and all three are expected**: the one
it answers on (`responder.cpp`) and the two it only watches (`watcher.cpp` —
§9.7's numbers and §9.10's activity line). Two of them is this readout taken
before the activity subject existed, not a device that failed to subscribe.

## The device's key (§10.6)

**Checking a signature the board made**, which is the host half of
`keys selftest` — run that on the device, then paste its three strings in:

```powershell
& E:\projects\ai-remote\.venv\Scripts\python.exe -c @'
import sys; sys.path.insert(0, r"E:\projects\ai-remote")
from lib import crypto
pub = "dFEabltdxgbWgh3CE5XL7ul7oMAAtc248oYeAKfJyzA="
sig = "Ryhn2qqYvXjeG6YZKi48sra1zXXcaQcIlcEv08uiyR6p78S++Y355v+AJ2lQYoEgssKdXEJNGBJ2XtTcmuuCBg=="
print(crypto.verify(pub, b"approver-esp32 key check v1", sig, "ed25519"))
'@
```

`True` means this board's derived key and `lib/crypto.py` agree. **The venv, not
`py`** — `cryptography` is not in the launcher's interpreter.

**Which route the key came from** is `keys`, and the reading of it is in
[`commands.md`](commands.md). What decides it is the chip:

```powershell
espefuse.py -p COM4 summary          # read-only, and the KEY_PURPOSE_n lines are the answer
```

All six reading `USER R/W (0x0)` means no key is burned and the firmware is on
§10.6's fallback — the seed is in NVS, and `esptool read_flash` would give it up.

**Burning one is permanent**, so it is written here rather than done by the
firmware, which has no business altering its own fuses:

```powershell
# 32 random bytes, on the host, never committed
& E:\projects\ai-remote\.venv\Scripts\python.exe -c "import os,sys; sys.stdout.buffer.write(os.urandom(32))" > hmac.bin
espefuse.py -p COM4 burn-key BLOCK_KEY0 hmac.bin HMAC_UP
```

Three things to know before running it, none of them recoverable afterwards:

- **the block cannot be rewritten and the purpose cannot be cleared.** One key
  block of six is spent, and `HMAC_UP` is what makes it unreadable by software —
  which is the point, and also why a lost `hmac.bin` cannot be recovered from the
  chip. Deleting `hmac.bin` afterwards is the correct move, not a mistake;
- **the device's identity changes**, because the seed then comes from the fuse
  instead of from NVS. The registration goes with it and needs a fresh token
  (§6). The firmware notices, drops the stale seed and says so in the log;
- **keep a board that has not been through it.** §10.12 says the same about
  flash encryption and secure boot, for the same reason.

## The LVGL preview (§10.12.1)

`lvgl-mcp-server` is registered project-wide in [`../.mcp.json`](../.mcp.json),
so Claude Code asks to approve it once per machine (`/mcp`) and the tools appear
as `lvgl_render`, `lvgl_render_full`, `lvgl_inspect`, `lvgl_set_resolution`.
§10.12.1 says what it is for and what it does *not* prove; this is how it is
wired, and it will look arbitrary later:

| In `.mcp.json` | Why it is like that |
|----------------|---------------------|
| `command` is `node` against `C:\Users\User\AppData\Roaming\npm\node_modules\lvgl-mcp-server\bin\lvgl-mcp-server.mjs` — the global install, not `npx` | `npx` resolves a second copy into its own cache, and that copy re-runs the postinstall that fails below |
| `VCVARSALL_PATH` = `C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat` | rendering *compiles* the snippet with the host MSVC, and the server's autodetect only scans `Microsoft Visual Studio\{2019,2022}\…`; this machine has `\18\BuildTools`. Without it every render dies on `CMAKE_C_COMPILER: cl … not found in the PATH` |
| `NINJA_PATH` = `C:\Espressif\tools\ninja\1.12.1\ninja.exe` | nothing else puts a ninja on `PATH`; ESP-IDF's is right there. `cmake` from `PATH` is fine |

**The install has a silent failure worth writing down.** The postinstall pulls a
106 MB release archive and unpacks it with PowerShell `Expand-Archive` under a
60-second timeout — on this box it times out, catches its own error and exits
**0**, so `npm install` reports success. The symptom is a package with no
`simulator/` directory, a leftover `.zip` and `_release_tmp/` beside it, and
every render failing later for reasons that have nothing to do with the code.
Unpack the archive by hand and move `simulator/` to the package root; the file
that has to exist is
`…\node_modules\lvgl-mcp-server\simulator\build\lvgl_sim.exe`.

And **set 480×480 before believing a layout** (`lvgl_set_resolution`, or the
per-call `width`/`height`) — the default is 800×480. §10.12.1 has the rest of
what a picture from it does and does not prove.
