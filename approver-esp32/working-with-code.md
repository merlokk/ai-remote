# Working with the code — the commands, and the toolchain on this machine

Everything you actually type to build, flash, talk to the board, render a screen
or regenerate a test vector. The **design** lives in [`CLAUDE.md`](CLAUDE.md) §10
and stays there; this file is the mechanics, which have machine-specific answers
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

Installed version: **v6.0.2**. §10.4 pins v5.5.3, and the paragraph in §10.12
says what to do about that — a decision to take at the first real build, not
something to work around here. Targets this install carries: `esp32`, `esp32c6`,
`esp32p4`, `esp32s3` — so `set-target esp32c6` works.

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

These answer today (§10.7 has the rest, and why they are not here yet):

```
status                        # firmware / IDF / chip versions, OTA slot, uptime, heap, storage
power                         # the AXP2101: charge state, VBUS, battery, system rail, die temp
date                          # the RTC and the system clock
date set 2026-08-15 16:41:13  # write both
buttons                       # BOOT / KEY / PWR: debounced state + the raw pin
buttons watch 30              # print edges for 30 s (default 10, max 120)
imu                           # the QMI8658C: six axes, tilt, die temperature
imu watch 10                  # a line a second of the same numbers
display                       # panel, LVGL and touch state, with missed I2C reads
display brightness 40         # the panel only; `config set brightness` is the stored one
display off                   # blank it; `display on` brings it back
play                          # play alert.wav; `play poweron.wav` for the other one
play volume 45                # the same setter as `config set volume`; memory only
config                        # the parsed settings; `config reload|save|restore`
config set volume 35          # into memory only
config set tz Europe/Kyiv     # named zones; `config zones [filter]` lists them
date set 2026-08-16 12:00:00  # local time, per that zone — stored as UTC
date set utc 2026-08-16 09:00:00
config set nats nats://192.168.1.77:4222
config save                   # …and this is what puts it in the file
term                          # switch on up-arrow history — see below
poweroff now                  # cut power; refused over USB, so it does nothing on the bench
ls                            # what is in the storage partition, with sizes
cat <path>                    # print a file from the storage partition, e.g. cat config.json
help                          # esp_console's own, listing the above
```

The five the console exists for, once there is a protocol under them:

```
register <token>              # the §6 exchange
keys                          # this device's public key + the pinned server key
forget                        # drop the registration and the pinned server key
bus  nats://192.168.1.5:4222
wifi <ssid> <password>
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

The token comes from the host, minted by the handler:

```powershell
py approver/registration_handler.py --get-token approver-esp32
```

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

## The host tests (§10.11, tier 1)

One command, no board, nothing to install:

```powershell
approver-esp32\host_test\run.cmd            # all of them
approver-esp32\host_test\run.cmd config     # one suite
approver-esp32\host_test\run.cmd i2c pmic   # several
```

A suite name is matched as a substring, and the names are `navigator`, `i2c`,
`pmic`, `rtc`, `imu`, `es8311`, `config`, `buttons`, `timezone` and `speaker`. Run everything before committing;
filter while debugging, because scrolling past a hundred lines of `PASS` to
find the one that matters is how a suite stops being run.

The tail of a good run:

```
188 Tests 0 Failures 0 Ignored
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
line that always passes. Sixteen invariants have been through it so far; §10.11
lists them — including one that *survived*, which turned out to be the most
useful run of all: a mutation nothing catches is a question about the code, not
a gap in the tests.

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

**The drivers are compiled unmodified.** `host_test/fakes/` shadows ESP-IDF's
headers on the include path, so `i2c_bus.cpp`, `axp2101.cpp`, `pcf85063.cpp`,
`qmi8658.cpp` and `es8311.cpp` are the files that ship. Two consequences worth
knowing before they bite:

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

## Parity vectors for the tests (§10.11)

Fixtures are generated by the Python implementation itself, then compiled into
the host tests — that is what makes "does the device still speak §7?" a question
a test answers:

```powershell
py -c "import sys; sys.path.insert(0, r'E:\projects\ai-remote'); from approver import protocol as p; print(p.signing_bytes(v=1, session_id='abc123', nonce='n', tool_name='Bash', input_sha256='0'*64, behavior='allow', updated_input_sha256='', ts=1737345600, reason=''))"
```

Do the same for `registration_reply_signing_bytes`, and for an Ed25519 key pair
with a known signature — the latter is what the boot self-test of §10.6 consumes.

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
