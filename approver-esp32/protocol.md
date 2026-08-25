# approver-esp32 — the approval path: the bus, the key, and the registration

This file owns **§10.5** the NATS client and the subset of it this device uses,
**§10.6** key custody — as designed, and as it actually shipped — and **§10.7**
registration on the device, together with the console it is driven from. Section
numbers are global and stable ([`../CLAUDE.md`](../CLAUDE.md) §2), so each keeps
its number here.

What holds these three together is that they are **the path a verdict travels**,
and the only part of this firmware where being wrong is silent: a socket that
reconnects, a key that signs bytes nobody recomputes the same way, a handler
reply believed before its signature was checked — none of them shows on the glass
and all of them end with Claude Code asking in its own terminal. Two sections
elsewhere state the rules they obey and are not restated here: **§10.2** — what
the device is in the protocol and the exact signing bytes — and **§10.10** — the
rules that may not be softened, of which *no reply is the safe outcome* is the
one every failure path below lands on. Both are in [`CLAUDE.md`](CLAUDE.md).

Where the rest of §10 lives:

- [`CLAUDE.md`](CLAUDE.md) — what this device is, its status, and the map;
- [`firmware.md`](firmware.md) — the device's own housekeeping: Wi-Fi (§10.9),
  the language and the layering everything here obeys (§10.14), and the settings
  file these components read (§10.15);
- [`hardware.md`](hardware.md) — the board, its drivers, and the I²C lease;
- [`screens.md`](screens.md) — the request card a signature comes from;
- [`web.md`](web.md) — the configuration site, which can never reach a verdict;
- [`tests.md`](tests.md) — the signing bytes, the exchange and the link policy, as
  assertions with no board and no key;
- [`build.md`](build.md) — libsodium and the NATS client, and what each cost;
- [`commands.md`](commands.md) — `register`, `keys`, `nats` and the rest, as a
  reference rather than as an argument.

### 10.5 The NATS client — the subset that is actually needed

The client is `debsahu/espidf-nats` (§10.4), so this section stopped being an
implementation plan and became two things instead: **the contract the wrapper
has to expose** — nothing outside this list is called, and anything the
component cannot do is a blocking finding, not a detail — and **the complete
specification of what to write** if the component ever has to be dropped. The
wire protocol is line-based text; this device speaks four verbs and listens for
three, which is why either way stays small.

Sends:

- `CONNECT {…}` — no credentials, per §10.3; `user`/`pass` or `auth_token` only if that bus ever gains auth. **What actually goes on the wire is the library's, not ours, and it is not what this line used to claim** — see below
- `SUB approvals.* approvers 1` — the queue group is not optional (§6)
- `SUB status 3` — read-only, **no queue group** (§10.8.3): a broadcast current value is meant to reach every subscriber, and joining a group would mean taking it from the other watchers
- `SUB <inbox> 2` — one private inbox for the registration reply, `_INBOX.<32 random hex>`
- `PUB <reply-subject> <n>\r\n<payload>\r\n` — a decision, and the registration request
- `PONG` — in answer to every `PING`

Receives: `INFO {…}` (once, at connect — read it, don't parse it into a model),
`PING`, `MSG <subject> <sid> [reply-to] <n>\r\n<payload>\r\n`, and `-ERR <why>`
(log it and reconnect; a `-ERR` is the server telling you the connection is over).

Everything the request-reply pattern needs is in that `MSG` line: **the
`reply-to` field is the inbox to `PUB` the decision into.** There is no
correlation to invent and no state to keep beyond "which reply subject belongs to
the card on screen".

**The `CONNECT` line above was written from this specification and the wire
disagrees with it**, which is worth the paragraph because the correction is not
only pedantic. `debsahu/espidf-nats` builds the handshake itself and sends
`verbose`, `pedantic`, `lang`, `version`, `protocol`, `headers` and
`no_responders` — and **no `name` field at all**. The server confirms it: this
device shows up in `/connz` as `name: null, lang: "espidf", version: "1.4.0"`.

Two things follow, and the second is the one that costs something:

- `"headers":true` and `"no_responders":true` are advertised on every connect,
  which is the client saying it *can* take those, not this firmware using them.
  §10.4's rule that unused is not absent applies as written: nothing here sends
  or reads a header.
- **the responder is anonymous on the bus, and now it is the only one.** §10.2
  gives it a `key_id` precisely so that it can be told apart, and `/connz` is
  where an operator looks when two clients are on one subject and only one of
  them is answering (§6's "Multiple clients"). Every other client in the
  repository answers that question now — `responder:<key_id>`,
  `responder-yubikey:<key_id>`, `approver-web`, `hook:<session_id>`,
  `registration-handler`, `statusline` ([`nats/CLAUDE.md`](../nats/CLAUDE.md) §4,
  "Naming a connection") — so the useful consequence is that **the `name: null`
  row in `/connz` is the board**, by elimination rather than by design.

  It is still not fixable from here, and the check was repeated rather than
  assumed: `NATS_CLIENT_LANG` and `NATS_CLIENT_VERSION` are unguarded `#define`s
  in the component's `config.h` rather than `#ifndef`-wrapped like its
  `NATS_CONF_*` knobs; `send_connect()` is private and non-virtual, so it cannot
  be overridden by a subclass; and 1.4.0 is still the newest version on the
  registry, with no name option in it. That leaves an upstream patch, a vendored
  fork of 7,228 lines — which would also make the frame parser this repository's
  to maintain, and `tasks.md` §2.7 wants it attacked rather than owned — or a trick: the
  `lang` macro is expanded straight into the `CONNECT` JSON, so a value carrying
  a quote and a comma would inject a real `name` field. **All three were weighed
  and declined**, at the repository owner's decision: the trick would put a
  malformed handshake one dependency bump away from a device that cannot connect
  at all, and the IP identifies the board today.

Behaviour that is not about the protocol but about this repo's rules. With a
third-party client these stop being things to implement and become things to
**verify** — by test, against the component, before trusting it (§10.11's host
tier cannot reach them, so they belong to the device tier):

- **`PUB` is not delivery.** §4: "Published" means sent. On a one-shot exchange
  (registration) wait for the reply before believing anything; on a decision,
  the reply *is* the delivery — but do not tear down the socket the instant
  after writing it. `flush` in the other clients exists for exactly this reason
  (§6, §9.7).
- **Junk on an open subject must not crash anything** — the rule `lib/bus.py`
  and `approver-web` both state. A payload that is not JSON, an object missing
  fields, a `MSG` with no `reply-to`, a length that does not match the bytes
  that follow: drop it, one log line, keep the socket. On a device the stakes
  are higher than a traceback — an unhandled parse is a reboot loop. The
  frame-level half of this is the component's parser now, not ours, which is
  exactly why something has to fire a lying length at it on purpose.

  **Half of that has now been fired at it, and the size half is what found
  something.** Against the real server: an empty payload, a payload that is not
  JSON, one full of control characters (the `ESC` becomes a dot, so a payload
  cannot drive the operator's terminal), 300 bytes against the 240-byte preview,
  and 64 KB — all delivered, socket kept, no reboot, uptime continuous. Then, at
  128 KB, 256 KB, 512 KB and a full 1 MB, every time the same three lines:
  `Failed to allocate read buffer for N bytes`, the library **drops the socket**,
  and `link_policy` reconnects 2 s later and the library restores the
  subscription. Never a panic, and the heap low-water never moved — the
  allocation refuses rather than succeeding and squeezing everything else, which
  is the good half of this.

  The bad half is that a drop is a denial of service by another route: `history`
  counted one drop per oversized message, exactly, so a loop of 1 MB publishes on
  `approvals.*` keeps this responder reconnecting and a real request never
  arrives — §10.10's scenario, reached without a malformed frame and without
  crashing anything. **It is bounded at the server**: `nats/docker-compose.yml`
  runs with `--max_payload=65536`, so the publisher is refused instead of the
  device, and `nats/CLAUDE.md` §3 carries what that costs (a `Write` over 64 KB
  can no longer be approved, and falls back to §7's timeout). Bounding it on the
  device is not possible from here — the library allocates off the `MSG` header
  before any of our code is reached.

  What is still unfired is the frame-level half proper: a length that lies about
  the bytes that follow, a truncated header, a server that stops mid-payload.
  Those need something pretending to be a NATS server rather than a real one, and
  they remain owed.

  And one number worth keeping: the 64 KB message that **succeeded** took the heap
  low-water mark from 89,480 to **23,068** free. That is §10.14.1's "number that
  says whether the device is safe", and 23 KB is what an attacker-chosen payload
  size could take it to before the server bound above existed.

  **That measurement has since been redone against a finished firmware, and the
  boundary has moved a long way** (§10.14.1 has the table). The 64 KB that was
  delivered intact here was delivered into 89,480 free bytes; a working build with
  the configuration site of §10.16 up has about **37,000**, and the boundary
  between "delivered" and "socket dropped" is now around **15 KB**. Nobody changed
  a limit — it is the same `Failed to allocate read buffer` path, and what moved
  is the heap it asks from. The proof is one variable: the same 15,634-byte message
  is **accepted** with that site down and **refused** with it up.

  Two things follow for this section. The good half above is confirmed on a
  fuller device: the allocation refuses rather than succeeding and squeezing
  everything else, so the low-water never approached zero — 4,260 free was the
  worst ever seen, and that took a sequence of near-limit messages. And the bad
  half is now **cheaper for an attacker than it was**: the server's
  `--max_payload=65536` bounds the megabyte case and does nothing about a 15 KB
  one, which drops this responder's socket just as effectively and is under the
  bound. The fix is the one named above and it has not moved: a cap of *ours*
  ahead of the library's allocation, rather than a limit inside it.
- **Bound every read.** A truncated `MSG` header, a server that stops mid-payload
  and a `PING` that never comes must all end in a socket timeout and a
  reconnect, not a blocked task and a watchdog panic.
- **Reconnect with backoff, and say so on screen** (§10.8): the dot in
  `statusline` (§9.8) is the precedent — the operator has to be able to tell "no
  requests are arriving" from "nothing is asking".

#### What is written, and the four decisions inside it

`components/nats`, in the shape §10.9 established next door — a driver with no
opinions, a policy with nothing but opinions, and a task where the two meet:

| File | What it is |
|---|---|
| `endpoint.h/.cpp` | one string from `config.json` into a host and a port. `<cstdint>`/`<cstddef>` only, so it runs under Unity |
| `link_policy.h/.cpp` | **when** to have a connection. `<cstdint>` and nothing else — the fifth file in this firmware to manage that, after `ui/navigator.h`, `wifi_policy.h`, `reachability.h` and `sync_policy.h` |
| `nats_bus.h/.cpp` | **`class nats::Bus`** — this section's list of verbs over `debsahu/espidf-nats`, and the only file that includes it |
| `nats_link.h/.cpp` | the task: reads the address, watches `wifimgr`, keeps a snapshot for the console |

**What the board has actually done**, against the server on the LAN: connected,
subscribed to `approvals.*` in the queue group `approvers`, received a
request-shaped payload with its reply-to subject, published a decision-shaped
one into that subject, and had the server confirm the flush. Both directions of
§7's exchange, with nothing of §7 in them.

Four things are decisions rather than plumbing:

- **The client lives in a static arena, and that is what a `new` would have
  bought.** The library takes its endpoint at construction and offers no way to
  change it, so pointing the device elsewhere means destroying the object and
  building another one. Placement new into `alignas(NATS) uint8_t[sizeof(NATS)]`
  keeps §10.14.1's rule (no heap of ours) and costs one thing worth stating:
  exactly one `Bus` can be open at a time, and a second one asking is refused
  with `ESP_ERR_NO_MEM` rather than served. That singularity is also what makes
  the library's context-free callback usable — the trampoline finds the open
  `Bus` through a file static, which is legitimate only because there is one.
- **Two of the library's own opinions are switched off.** Its reconnect backoff,
  because `link_policy.h` is the half that can be tested and two things
  deciding when to reconnect is one too many; and its offline message buffer,
  because a responder whose decision is delivered when the socket comes back is
  answering a request that timed out minutes ago (§10.10).
- **A network releases it, not an internet.** §10.9's rule is "only `ONLINE`
  releases the bus task", and on this device that means a client link with an
  address: the server is on the LAN (§10.3), so `wifimgr`'s ping verdict about
  8.8.8.8 has no vote, and a router with its uplink down is a perfectly good
  place to approve a command. This is the one place the bus and the clock read
  the same manager and want different answers from it (§10.8.2).
- **Two locks, because one of them can block for five seconds.** `wire_lock`
  guards the client's *lifetime* and is held by anything that publishes through
  it; `state_lock` guards the policy and the snapshot and is never held across
  anything that waits. One lock for both would mean `nats` hanging for the
  length of a connect attempt just to print a status line.

**Three things only the board could have said**, all fixed, and the shape they
share is the §10.9 one: each was invisible on the host and obvious the first
time real hardware met a real server.

- **A 4 KB task stack panicked the moment a server answered** —
  `Guru Meditation Error: Core 0 panic'ed (Stack protection fault)`, in task
  `nats`, immediately after the `INFO`. It is the library's frame:
  `send_connect()` declares two `char[NATS_MAX_CREDENTIAL_LEN * 2 + 1]` buffers
  to escape a username and a password, 4 KB reserved in one function whether or
  not the branch runs, on a device that sends no credentials at all. 8 KB now,
  and `nats` prints the low-water mark so the number is measured rather than
  guessed: **2,852 bytes never used** after a connect, a subscribe and a
  publish — 4 KB was never going to be enough, and 8 KB has a real margin.
- **A restart asked for mid-attempt was remembered forever.** `nats url` typed
  while the task was inside a five-second connect set a flag that only the
  teardown branch clears — and that branch is only reached while something is
  up. The attempt failed, the flag stayed, and the next connection that
  *worked* was dropped on its first tick. A pending restart is now consumed
  from idle as well, where it means "try now" — which is also what an operator
  who has just changed the address wants.
- **The status line printed the address the device was leaving.** The snapshot
  is written by the task, and the task was blocked in that same connect, so a
  `nats` typed straight after `nats url` showed the old server under a config
  line naming the new one. What was *asked for* is read live now; only what is
  *happening* waits for the task to notice. The log line had the mirror-image
  bug — it named the configured address for an attempt against the previous
  one, which is a line that sends somebody hunting the wrong fault.

### 10.6 Key custody — the part worth doing properly

The repository has a pattern by now, and it is not "a private key in a config
file": the YubiKey responder keeps the key in hardware (§8.7), the web responder
keeps it non-extractable in the browser's key store. The device should hold to
that standard, and the ESP32-C6 has the hardware for it.

**The proposal: derive the Ed25519 seed from an eFuse key through the HMAC
peripheral.** ESP32-C6 can hold a key in one of eFuse blocks 4–9 with a *purpose*
that makes it usable **only** by the HMAC peripheral and unreadable by software
([HMAC docs](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c6/api-reference/peripherals/hmac.html)).
So:

```
seed  = esp_hmac_calculate(HMAC_KEY0, "ai-remote-approver-esp32-v1")   // 32 bytes
pair  = crypto_sign_seed_keypair(seed)                                  // ed25519
```

- The private key exists only in RAM, for as long as the firmware runs, and is
  **not in flash at all** — a dumped flash image yields the public key and
  nothing else.
- It is reproducible across reboots, so registration survives a restart with
  nothing secret persisted.
- It is bound to the chip: the same firmware on another board is a different
  responder, which is the correct behaviour (that board is not this key).
- The label in the HMAC message is the domain separator. Change it and you have
  rotated the key deliberately — which then requires re-registration (§6), the
  same felt cost the server-key rotation has.

The fallback, if the eFuse route stalls: generate once and store in **encrypted
NVS** with flash encryption enabled. Strictly worse (the key is in flash,
protected by a key that is also on the chip) but still not a plaintext file, and
honest as a first milestone as long as this doc says which one shipped.

#### Which one shipped: the fallback, and it is not even the encrypted version

**Both routes are written and the fallback is what is running**, at the
repository owner's decision, because burning an eFuse key is a one-way operation
on the only board there is and the eFuse route can be switched to later at no
cost in code. `components/crypto/device_key.h` is where the two live; the
firmware tries the fuse first at every boot and falls back, so **the day a key is
burned the device picks it up with no new firmware.**

What the fallback actually does, and the two places it is worse than the
paragraph above:

- **the seed is 32 random bytes in NVS**, generated once, in the `approver`
  namespace. §10.15 says nothing of ours goes in NVS and this is now its one
  exception, argued there: a seed is not a setting — it must not be restored by
  §10.15's button, must not appear in `cat config.json`, and must not be edited
  by hand.
- **it is not encrypted, because nothing here can be.** NVS encryption needs
  flash encryption, which needs the same one-way eFuse operation this decision
  postponed, so `esptool read_flash` gives up the signing key. That is the honest
  statement of it: this device's private key is currently no better protected than
  its Wi-Fi password, and the property §10.6 was written to buy is the one thing
  not yet bought.

Three decisions inside it that are not obvious:

- **the randomness is real, without waiting for the radio.** §10.7's rule — the
  RNG is only a true source with the RF subsystem up — applies to a *seed* far
  more than to a nonce, and `crypto::Init` runs long before Wi-Fi. So the SAR-ADC
  entropy source is switched on around the one 32-byte read that needs it, which
  is what `bootloader_random_enable` documents for exactly this case. It also
  fixes where in the boot this can run: before anything touches the ADC or the
  radio, which `main.cpp` states where it calls it.
- **a seed that cannot be stored is refused, not used.** A device whose identity
  changed on every boot would register successfully and then be rejected forever
  afterwards — and the operator would be debugging the handler. `kNoSeed` is a
  device that says it cannot sign, which is §10.10's end of the trade.
- **the fuse wins, and it takes the identity with it.** A board running on a
  stored seed that then has a key burned into it is a *different responder* and
  needs a new token (§6). The firmware deletes the stale seed when that happens
  and says so, because the whole cost of the fallback is a private key sitting in
  flash and there is no reason to keep paying it once the fuse works.

| Responder | Where the private key lives | Can the host forge a decision? |
|-----------|------------------------------|-------------------------------|
| `responder.py` | `responder-config.json` on disk | yes, trivially |
| `responder_yubikey.py` | inside the YubiKey | no |
| `approver-web` | non-extractable `CryptoKey` in IndexedDB | no |
| **this**, as designed | RAM only, derived from an eFuse key per boot | no — and there is no file to steal |
| **this**, as shipped | a seed in **unencrypted** NVS, the key derived from it per boot | **yes**, given a flash dump — the row this section exists to eliminate, and the one `espefuse.py burn-key` closes |

That last row is why `keys` prints the source every time it is asked rather than
mentioning it once in a boot log: a key in flash and a key that cannot be read at
all are the same device from the outside.

**A boot self-test, because a miscompiled crypto library is silent.** ESP-IDF's
libsodium has a Kconfig switch for using mbedTLS's SHA-512 underneath, and that
seam has historically produced *valid-looking but wrong* `crypto_sign` output
([esp-idf#1044](https://github.com/espressif/esp-idf/issues/1044)). A wrong
signature here is indistinguishable, from the operator's side, from a working
device: the hook simply rejects every reply and Claude Code keeps asking in its
own terminal. So at boot, sign a **fixed test vector with a fixed test key** and
compare against bytes generated by `lib/crypto.py`; refuse to subscribe if it
does not match, and say so on the screen. Ed25519 is deterministic, which is
what makes this check possible at all.

**That seam has now been fired at, before any of §10.6 was written, and it is
clean — which changes what the self-test is for rather than removing it.** A
throwaway probe in `main.cpp` ran the four calls this section needs against a
vector generated by `lib/crypto.py` (the seed `00 01 … 1f`, and the message
`ai-remote approver-esp32 libsodium self-test v1`), on ESP-IDF v6.0.2 and this
board, **both ways**: with `CONFIG_LIBSODIUM_USE_MBEDTLS_SHA` set and clear, the
derived public key matches, the signature matches byte for byte, and the verify
of Python's own signature succeeds. The historical bug is not present here. So
the switch stays at its default `y` — 10,780 bytes of flash cheaper, and
`libmbedtls.a` is byte-identical either way because mbedTLS is linked for
`esp-tls` regardless — and `sdkconfig.defaults` carries the reasoning where a
menuconfig session will trip over it.

The self-test stays, for the reason it was written down: this measured one
build of one library on one day, and the failure it guards against is a *silent*
one. What it no longer has to be is a first contact with an unknown.

**And the probe found something this section will need, which is the point of
having run it.** The first version panicked — `Guru Meditation Error: Core 0
panic'ed (Stack protection fault)` in task `main`, inside `crypto_sign`. Signing
uses **4,112 bytes of stack** (measured as a high-water mark against a 16 KB
task; 4,128 with the mbedTLS SHA wrapper), and the main task's is 3,584. So the
key derivation and the signature of §10.6 do not run wherever it is convenient:
they need a task sized for them, which is §10.5's lesson about the 4 KB NATS
stack arriving a second time and from a different direction. It is also the
second argument, after §10.8.1's, for the signature never running inside an LVGL
callback.

**And the self-test as written is two checks rather than one**, because the
section above only asked for half of the question. `library` is what §10.6
specifies: a fixed key signs a fixed message and the bytes are compared against
`lib/crypto.py`'s — in both directions, since a library that signs correctly and
verifies nothing would pass a one-way test, plus a signature with one flipped bit
that it has to *reject*. `this key` is the other half, and nothing above asked
for it: the library being right says nothing about the key **this** device
derived, and a keypair whose public and private halves do not correspond signs
perfectly happily and verifies against nothing at all. It signs a labelled
constant with the real key and checks it against the real public key.

Two things about that second check. It prints the message, the signature and the
public key, so the check can be **finished on the host** — pasting them into
`lib/crypto.py`'s verify is what actually proved this board's derived key against
the Python side, and `working-with-code.md` has the line. And it is deliberately
**not a signing oracle**: the message is a constant with no argument, and it
begins with a letter where §7's signing bytes begin with the version digits and a
`\n`, so nothing it prints can be rearranged into a verdict. §10.10's "the only
path to allow is a human press" survives it existing, which is the reason the
console is allowed to have it.

### 10.7 Registration on the device (§6, without a keyboard)

The token is `<key_id>.<b64 32 bytes>` — around 50 characters, minted on the host
by `py approver/registration_handler.py --get-token approver-esp32`. Typing that
on a 2.16″ touchscreen is a bad joke, so registration is driven over **USB**,
through `esp_console` on the USB Serial/JTAG port:

**All four commands this section owes exist now.** `register <token>` runs the
exchange below, `keys` prints the identity and the pinned handler key, `forget`
drops the registration — and `bus <url>` is **`nats url <url>`**, alongside a status readout and the `sub` / `pub` pair that made
the bus visible on the wire, and it grew subcommands for the same reason `wifi`
did. **The ones that exist are listed, with what each
of them does, in [`commands.md`](commands.md)**, and that file is the reference
rather than this one: a list of commands in a design document is a list that
goes stale the first time somebody adds a subcommand and updates the code.

What stays here is why the console is shaped this way. How to *reach* it —
which serial command opens the port, and what it fights with for it — is in
[`working-with-code.md`](working-with-code.md).

**The Wi-Fi half of the owed list exists now, spelled with verbs**: `wifi join
<ssid> [password]` rather than the bare `wifi <ssid> <password>` this section
first sketched, because `wifi` had to grow a status readout and subcommands,
and a bare pair of words that is sometimes an SSID and sometimes a subcommand
is a parser with a trap in it. §10.9 has the rest.

`status` is the answer to "is this the build I think it is, in the slot I think
it is" — the question every other command will be debugged through. `cat` is
how §10.15's files are read back off a device without a reflash, and it was the
first thing to prove the SPIFFS image had actually landed.

**`devstatus` is all of them at once, and it is composed rather than written.**
It calls `status`, `power`, `buttons`, `imu`, `audio`, `display`, `date`, `wifi`
and `nats` in turn instead of printing its own version of each — a second copy of
the `power` readout would drift from the first the day somebody adds a field to
one of them, which is the drift the four-places rule above exists to prevent.

Two things fall out of that. **Every section header is the name of a command**,
so the dump doubles as a map — something odd under `== power`, type `power` to
look at it alone — and keeping that rule true is why `audio` now exists as a
command: it was the one section with nothing behind it, and adding the command
was better than documenting an exception. And the headers are not decoration:
run together the sections share label names (`die temp` is the PMIC's and the
IMU's, `system` is a voltage in one section and a clock in the next), and a
wall of aligned lines with no marks in it is a wall nobody reads twice.

It prints **state, not settings**. `config` is the other half of that pair and
answers what the device was *told* to do; this answers what it is doing.

It is ESP-IDF's `esp_console` REPL, not a hand-written line reader: history,
editing, argument splitting and `help` come with the component §10.4 already
approved. The house firmware of §10.14.4 writes its own — worth knowing when
comparing the two, and not worth copying when the in-tree one is already a
dependency.

#### Two rules for anything this console prints

**Every command and every subcommand goes in the help, in the same change that
adds it** — and in [`commands.md`](commands.md) with it. A command nobody can
find does not exist, and `help` is the only place anybody looks. For this
console that means four things and they are easy to get out of step — three of
them already did, with `wifi ping` and `wifi check` reaching the usage text and
never the registered hint, so a reader who ran `help` concluded the commands
were not there:

- the `.help` string in the `kCommands` table, which is the one line `help`
  prints per command;
- the `.hint` string next to it — the argument summary on the same line. It has
  to stay short enough to read in a column, so it names the **verbs**;
- the command's own `usage` text, which names the **forms**. Print it from one
  function, reached both by an explicit `<command> help` and by anything
  unrecognised: finding out what a command takes should not require typing
  something wrong first, and two copies of a usage block drift;
- **[`commands.md`](commands.md)**, which is the only one of the four with room
  to say what a command is *for* — that `display brightness` prints the live
  value next to the stored one, that `poweroff` refuses over USB, that `wifi
  scan` works with the radio off. A hint has twelve words; this has as many as
  it needs.

**No section numbers in anything the operator sees.** `§10.9` is how this
repository talks to itself; on a console it is a reference to a document the
person reading has probably never opened. Every `printf`, every `ESP_LOG` and
every `.help` string says the thing itself — "the reset is the ALDO3 rail",
not "(§10.1)". In the **code comments** the citations stay exactly as they are:
that is where they earn their keep.

`cat` reads through a **fixed 4 KB buffer** (§10.14.1 — nothing here allocates),
and a file too big for it is refused *with its size* rather than truncated into
something that reads as complete. `ls` lists into a fixed sixteen-entry array
and says "and more" when it fills, for the same reason: a bounded listing that
looks complete is worse than a short one that admits it. That is the rule
§10.15 states for parsing the config, arrived at from the other direction.

`ls` also has nothing to recurse into — **SPIFFS is flat**, so its output is the
whole filesystem rather than one level of it, and it prints two totals rather
than one: the bytes in the files it listed, and the bytes the partition itself
reports used. They never match, and that gap is the filesystem's own overhead
made visible. The percentage is beside them because neither absolute answers "is
this filling up" at a glance against an 11 MB partition. What is in the image
today reads as about six per cent, and nearly all of it is `splash.bin` and the
two sounds rather than anything the device wrote — which is the shape to expect
rather than a number to check: `spiffs_image/` is where that figure moves.

**`imu` prints a magnitude, and that line is the point of the command.** Six
plausible-looking numbers say nothing on their own: a range bit that did not
take, or a burst read that returned one register six times, both produce a
steady, believable table. At rest the acceleration vector must be 1 g, so
`magnitude 0.964 g (1.000 at rest)` is the single line that says the other six
mean something — and it is what caught the two real traps in this chip
(§10.1's inverted addresses, and CTRL1's address auto-increment being **off** by
default). The sign convention is the other thing worth stating once: an
accelerometer at rest reads +1 g along the axis pointing *up*, so the axis
gravity acts along is the negation of the dominant reading. Getting that
backwards is invisible on a desk and exactly wrong the moment the board is
turned over.

**The up-arrow, and why it needs asking for.** `esp_console` already keeps the
last 32 lines and adds every one typed, so history costs nothing — but the line
editor that reaches it is switched off on this port. `linenoiseProbe()` runs
once, while the REPL is being created, and asks the terminal to identify
itself; on USB Serial/JTAG the host opens the port seconds later, so nobody
answers, and dumb mode is latched for the session no matter who attaches
afterwards. **Turning it on at boot instead was tried and is worse than the
problem**: with dumb mode off, linenoise asks for the cursor position before
every prompt and *blocks* reading the reply, so a port driven by something that
does not speak escape sequences — the pyserial snippet in
[`working-with-code.md`](working-with-code.md), for one — goes silent until the
board is reset. Measured on this board, not feared in the abstract. Hence
`term`: the probe re-run when there is somebody there to answer it, bounded at
500 ms, and `term smart` / `term dumb` when the answer is wrong.

**`buttons` prints two answers per button — the debounced state and the raw
pin — because they disagree exactly when something is wrong.** A pin held low
by a fault reads pressed in both, for the whole uptime, which is how a broken
button tells itself apart from an idle one; it is also how §10.1's inverted
`PWR` was found. `buttons watch` is the other half: it blocks the REPL for a
bounded number of seconds (default 10, capped at 120 — a watch that outlives
the operator's attention is a console that looks hung) and prints each edge
with the duration of the press it ended. A run that shows several 30 ms presses
where one finger went down is a debounce window that is too short.

**`poweroff` refuses while USB is connected, and that is a driver rule rather
than a console one** — `Axp2101::PowerOff()` reads the VBUS bits and returns
`ESP_ERR_INVALID_STATE` without writing anything, under the same lease it would
have written through (§10.14.3). VBUS powers this chip back on, so a shutdown
with the cable in is one the hardware undoes: what the operator would see is
not a device switching off but a device rebooting, which on a desk object reads
as a crash. Saying "unplug it first" is the true answer; performing a power-off
that does not happen is not. The console adds a confirmation word (`poweroff
now`) for the same reason §10.8.5 makes its destructive entries two-step.

**`reboot` is next to it and takes no confirmation word, which is the same
argument reaching the opposite answer.** Two-stepping a destructive action is
worth it when the console cannot undo what it did — `poweroff` succeeds by
ending with a finger on a button — and a reboot undoes itself in a few seconds,
so a second word there would be friction on the most ordinary debugging action
there is. What it costs is *said* instead: `config set` writes to memory and
`config save` reaches the filesystem (§10.15), so a reboot is exactly where
unsaved edits go, and the command prints that before it goes.

Two details that are the hardware rather than the code. The line has to be
flushed **and given a moment** before `esp_restart`, because the console is the
C6's own USB Serial/JTAG and the port goes down with the chip — restarting on
the next statement takes the message with it, and what the operator sees is a
console that died rather than one that answered. And nothing is quiesced first
on purpose: a `config.json` write interrupted mid-reboot is the power cut
§10.15 already recovers from at boot, so there is nothing here worth waiting
for that is not already handled.

**Both halves are tested on hardware now.** The refusal always could be, from a
console over the cable that causes it. The shutdown could not: it needs the cable
out, and with the cable out there is no console to watch it from — so it waited for
a battery-powered session, and that session has happened. The board switches off.
Worth keeping the sentence rather than deleting it, because it names the one class
of behaviour on this device that no amount of tooling here can reach: what happens
after the port goes away.

The exchange itself is §6 verbatim, and the order of operations is the part that
must not be "simplified":

1. Generate a fresh 32-byte `nonce`. **After Wi-Fi is up** — the ESP32's RNG is
   only a true random source with the radio enabled; before that it is a PRNG,
   and a predictable nonce gives up the replay protection the nonce exists for.
2. `PUB registrations` with `{v, token, key_id, pubkey, key_type: "ed25519",
   nonce, ts}` and wait on the private inbox.
3. **Verify the handler's Ed25519 signature over the reply before reading
   `ok`** — `registration_reply_signing_bytes`, context string included; check
   the `nonce` echo; if a `server_key` is already pinned, require it to be
   exactly that one. This is `responder.verify_server_reply` in C, and it is not
   optional: an unsigned `{"ok":false,"error":"expired"}` from anyone on the bus
   would otherwise send the operator hunting a problem that does not exist.
4. Only on a verified `ok:true`: write `key_id` and the pinned `server_key` to
   `registration.json` (§10.15). A rejection changes nothing — the same ordering
   all three existing responders use.

##### What is written, and the four decisions inside it

The split is §10.14.2's, and here it is load-bearing rather than tidy:
`components/protocol` holds the bytes **and the order they are checked in**, with
cJSON and nothing else; `components/registration` holds the socket, the file and
the random number.

| File | What it is |
|---|---|
| `protocol/registration.h/.cpp` | the request JSON, `registration_reply_signing_bytes`, and `ParseRegistrationReply` — every check above, in order. cJSON only, so all of it is host-tested (§10.11) |
| `registration/registrar.h/.cpp` | the exchange: the nonce, the inbox, the wait, and `registration.json` |

**What the board has actually done**, against the real handler on the LAN:
registered, printed a handler key matching the one the handler printed at
startup, written the file, survived a reboot with it, refused the same token a
second time (`the handler refused: token unknown` — a *signed* rejection,
verified against the now-pinned key before it was believed), and refused a
perfectly valid `ok:true` from a second handler with a key of its own. The file
was unchanged after every one of those.

Four things are decisions rather than plumbing:

- **The verifier is an argument, so the ordering is not the caller's
  discipline.** Step 3 above is the whole point of this exchange, and a parse
  function that handed back the fields and left the checking to whoever called it
  would make that rule a convention. So `ParseRegistrationReply` takes the
  signature check as a plain function pointer (§10.14.1 — no `std::function`) and
  calls it at the one place it belongs: there is no way to get a field out
  without it having run and said yes. It takes base64 strings rather than bytes
  because decoding is libsodium's, and that keeps the crypto library out of a
  component §10.11 wants to compile with a bare host compiler — where the test's
  verifier **records the message it was handed**, which turns "these are §6's
  signing bytes" into an assertion.
- **The pin is checked before the signature, not after.** A valid signature by a
  key this device does not already trust is not a bad signature; it is somebody
  taking over the slot, and calling it `kBadSignature` would name the wrong
  problem to whoever is reading the console. It is its own status with its own
  sentence.
- **The nonce is made after the bus is connected**, which is how §10.7's "after
  Wi-Fi is up" becomes something the code enforces rather than something the
  order of statements happens to satisfy: a connection implies a client link
  implies the radio, and the radio is what makes the RNG a true source instead of
  a PRNG. `Register` refuses without one anyway.
- **The argument is checked before the world.** A mistyped token is wrong
  whatever the network is doing and costs nothing to detect, so it is refused
  before the bus is consulted. That is the other way round from how it was first
  written, and the board is what showed it: a token with no dot in it was
  answered with a complaint about the bus.

And two smaller ones. A `ts` beyond 2^53 is **refused** rather than read, because
cJSON parses every number into a `double` and a rounded timestamp reproduces
different signing bytes — a signature that stops verifying with nothing to point
at. And a `registration.json` whose `server_key` is not exactly 44 characters is
treated as no registration at all: a device that came up pinned to a truncated
key would refuse every future registration with "signed by a different key",
which is a sentence that sends somebody hunting an attacker who is not there.

**The atomic write moved to `storage` to make this possible** (§10.15). There are
two files that must not half-happen now, and a second copy of the SPIFFS
remove-then-rename dance would be the copy that drifts — so
`storage::WriteFileAtomically` and `storage::RecoverInterruptedWrite` take a path,
`config.cpp` calls them, and they live in `storage/file_ops.cpp`, which has no
ESP-IDF in it and is therefore the sequence the host tests exercise rather than a
model of one.

**Trust on first use, and the screen is what closes it.** The first registration
has nothing to compare the handler's key against, so show it: the device displays
`handler key <b64>` and the handler prints `server key (ed25519): <b64>` on
stderr at startup. Two strings, compared by eye, once. `approver-web` does the
same thing in its register panel.

`ts` for the request comes from the RTC (PCF85063) or SNTP — and if neither has
been set, from `0`: the handler does not check the request's `ts`, and inventing
a plausible-looking wrong one is worse than an obviously unset one.

