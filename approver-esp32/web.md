# approver-esp32 — the configuration site on the device

This file owns **§10.16** of the project docs: the `esp_http_server` behind
`components/web`, the six pages it serves off SPIFFS, the desired-state switch
that brings it up and down with the network, the whitelist that keeps a WPA
passphrase off the LAN, the write path and what it refuses, and the numbers —
including the 16 KB taken off LVGL to make a page arrive at all. Section numbers
are global and stable ([`../CLAUDE.md`](../CLAUDE.md) §2), so §10.16 keeps its
number here.

Two rules from elsewhere bind everything below, and neither is restated as
though it were new: **nothing here can reach a verdict** (§10.10, in
[`CLAUDE.md`](CLAUDE.md)), and the **trust boundary is the router** (§10.3, same
file) — so `web.write` is a switch and not authentication.

Where the rest of §10 lives:

- [`CLAUDE.md`](CLAUDE.md) — what this device is, its status, and the map;
- [`firmware.md`](firmware.md) — the settings this site writes and the Wi-Fi
  manager whose tick it borrows;
- [`protocol.md`](protocol.md) — the approval path this site may not reach;
- [`screens.md`](screens.md) — the 6 mm keyboard this site is the way round;
- [`hardware.md`](hardware.md) — the board it reports on;
- [`tests.md`](tests.md) — the whitelist and the write path, as refusals;
- [`build.md`](build.md) — what it costs in flash, and where it took it from.

### 10.16 The configuration web server — measured before it is trusted

A page served off SPIFFS, over `esp_http_server`, started and stopped on demand:
`components/web`, and `web` on the console. It exists because §10.8.6 arrived at
a keyboard 6 mm wide and named the alternative in the same breath — a phone's
keyboard, over the device's own access point — and because the repository owner
asked the question that decides whether that is affordable at all: **what does it
cost while it is up, and does stopping it give every byte back.**

Both are answered, on this board, and the answers are the reason this section is
short:

| | |
|---|---|
| the server, while up | **6,824–6,940 bytes** of heap (the 24-byte spread between rounds is the allocator, not a setting) |
| flash | **46,032 bytes**, app 1,861,024 → 1,907,056 of the 2.5 MB slot |
| our own static RAM | 1,536 bytes — a 1 KB chunk buffer and a 512-byte JSON buffer, both in `.bss` |
| **twenty start/stop rounds** | **+0 bytes end to end, and a 4-byte spread between the lowest and highest resting point** |
| pages, in SPIFFS | `index.html` is 1,723 bytes of a partition with 9.8 MB free — the one part of this that is free |
| **a page actually served, over the LAN** | the low-water mark went from 21,632 to **12,892** free while a browser pulled the page and the JSON — about 8.7 KB of pbufs, sockets and scratch on top of the server itself, and it came straight back afterwards. That is the number the arithmetic above could only bound |

**So there is no leak**, which was the open question: `httpd_stop` on ESP-IDF
v6.0.2 gives back the task, its stack and the socket database, twenty times in a
row, to the byte. `web cycle` is the command that says so and it is kept rather
than thrown away — the same experiment §10.9 runs on the Wi-Fi stack, and the
same shape of answer: a spread with no direction is the allocator, a drift that
grows with the rounds is a leak.

One number in that run needs its own sentence, because it looks like a leak and
is not: the **first** stop reported 168 bytes short. It came from the access point
that had been raised two seconds earlier — a DHCP server and a beacon settling —
and the twenty rounds that follow are flat at 27,180 bytes with the same AP up.
A single before/after pair cannot tell those apart, which is the whole argument
for cycling.

#### The site — six pages, one column, a phone's width

The single `index.html` this section started with was a table and a `<pre>` on one
page: enough to answer "does the server work", which is what it was for. What is
there now is the site the repository owner asked for, in the order they asked for
it — **the front page of buttons, the `devstatus` page, the restart, and the
404**, and then **the Wi-Fi and bus pages**, which were drawn on the list and
marked `soon` until they were asked for and are the two that write (below).

| File | What it is |
|---|---|
| `index.html` | the state of the device in three tables, and the buttons that go anywhere |
| `devstatus.html` | the whole console dump, and a button that reads it again |
| `wifi.html` | the radio's mode, the four remembered networks, this device's own access point, and what is on the air |
| `nats.html` | one address, and what the bus is doing with it |
| `reboot.html` | one action, asked for twice |
| `404.html` | what a refusal and a missing file both look like |
| `app.js` | **the stylesheet and every page's script**, in one file — and the shape of that is a memory decision rather than a taste one, below |

**Mobile first and mobile only, deliberately.** This page exists to be opened on a
phone that has just joined the device's own access point, one-handed: one column,
58-pixel targets, nothing behind a hover, 16-pixel text in every input (anything
smaller and iOS zooms the field), and 38 KB for the whole site. It is wide enough to
look deliberate on a laptop and is not designed for one. No framework and no build
step, for the same reason the firmware has no image decoder.

**And each page is fetched on its own, which is the one piece of this that is
architecture rather than styling.** No page carries a `<link rel=stylesheet>`; each
has four lines of critical CSS in its head and one `<script src="/app.js">` at the
end of its body, so a browser asks for the HTML, finishes it, and *then* asks for
the script — one socket at a time. Three parallel requests for a page and its two
files is precisely what this device could not survive, and the section below is that
diagnosis. The cost is that `app.js` carries the stylesheet as a string and every
page's script behind a `data-page` attribute: one file of 23 KB, cached for a day,
instead of two of 12 that arrive together.

**And the one page that broke that rule looked like a different product, which is
how it was found.** `wifi.html` shipped with an *inline* copy of its own script —
a leftover from before the pages were consolidated into `app.js` — and that copy
was truncated mid-string. HTML has no notion of an unterminated script: the parser
ends the block at the **first** `</script>` it meets, which was the closing half of
the `<script src="/app.js"></script>` on the next line. So the tag that loads the
stylesheet and every page's logic was swallowed as script text, and the browser got
one page with none of the CSS, none of the fetches and no error anywhere — a page
whose *only* visible symptom is that it does not look like the others. The fix is a
deletion: `app.js` already had the whole of `wifiPage`, ids and all. The rule to
take from it is narrower than "do not duplicate": **a page with its own `<script>`
block cannot fail safely on this site**, because the shared tag is what it takes
down with it, so every page here carries markup and one script reference and
nothing else. `tests/test_esp32_web_pages.py` is that rule as five assertions
read straight off the files that get flashed (§10.11).

Four rules it keeps, and each of them is the panel's rule rather than a new one:

- **the palette is the panel's** (`screens/*.cpp`): near-black, the same greens,
  amber for something that wants attention. A page that looked like somebody
  else's product would read as somebody else's device;
- **one lamp says the whole thing.** Top right, three states and not two: ready,
  a reason worth acting on, or nothing has been asked of it — which is §10.8.1's
  rule about the clock's own indicator, and the words come from
  `responder::BlockerText` so the page never invents a diagnosis of its own;
- **a field that did not arrive is a dash, not a zero**, and a battery that is not
  there says `on usb` rather than drawing an empty gauge (§10.8.2 makes the same
  call about the icon). The `devstatus` page keeps the last good dump when a read
  fails, which is §10.8.3's rule about a document that did not arrive;
- **rows with nothing behind them are drawn and disabled**, marked `soon`, exactly
  as the settings list does it (§10.8.5). A link to a page that 404s would be
  worse than a row that admits it.

The front page's tables are the three questions this device is about, which is
also what `/api/status` grew to answer: **could it approve something right now**
(the bus, the registration, the subscription and what is blocking it), **what has
it approved** (§7's counters, allow and deny apart rather than summed — a total
hides the interesting half), and **is it healthy** (battery, uptime, heap beside
its low-water mark, filesystem).

Those counters could not come from a dependency, and the way round it is worth
recording because it looks like over-engineering until you try the obvious thing:
`web` cannot require `responder`, because `responder` requires `screens` and
`screens` requires `web` for the row that says whether the server is up. A cycle
in `REQUIRES` is a build that does not happen. So `main` fills a small struct
through `web::SetApprovals` — the sixth inversion of this kind here — and the
fields read zero on a device where nobody registered one.

**What the board has actually done with it**, over the LAN at
`192.168.11.134`, and it is the whole of the site rather than a sample: all six
files served with their own content types (6,187 bytes of front page, 5,534 of CSS,
3,906 of JavaScript, 3,375, 5,129 and 1,144 of the three other pages);
`/api/status` answering the full 25-field document — `"bus":"connected"`,
`"registered":true`, `"subscribed":true`, `"blocked_by":"nothing"`,
`"battery":100`, `"battery_mv":4186`, `"usb":true`; `/api/devstatus` streaming
5,031 bytes and sixteen sections through the `stdout` cookie; and **every refusal a
404 with the page in it** — `/config.json`, `/registration.json`,
`/config.init.json`, `/../config.json` and a name that simply is not there, all
1,144 bytes of `text/html` and none of them telling the difference apart. 6,928
bytes of heap while up, which is inside the 6,824–6,940 this section measured
before the site existed: the pages cost flash and the filesystem, not RAM.

#### The settings pages, and the write path §10.16 said it would need

This section shipped read-only and named what a write would need first: "a writable
config over HTTP is a way to point this device at another NATS server, and that
needs the authentication question answered first". The repository owner then asked
for the Wi-Fi and bus pages, which is the ordinary way that sentence gets tested.
What was built is the narrowest thing that answers it, and the five rules are in
`web_settings.h` next to the code:

| | |
|---|---|
| `GET /api/settings` | **the same document the POST takes** — `wifi` and `nats` — so a form is filled from the answer and submits the answer. `writable` and the radio's current `state` travel with it |
| `POST /api/settings` | a **whitelist of fields**, not a merge: `wifi.mode`, `wifi.ap`, `wifi.networks[]` and `nats.url`, and *anything else is refused by name*. The touch calibration, the idle timers, the clock and the display's brightness are all in the same file and none of them is reachable |
| `POST /api/action?do=…` | the four verbs that are not a field: `save`, `reload`, `retry` (walk the network list again), `reconnect` (the bus) |
| `GET /api/wifi/scan` | what is on the air, hidden names and duplicates dropped, for the picker |

**The rule the whole thing hangs off is that a password can be written and never
read back.** §10.15 keeps a passphrase out of every log line and every console dump,
so a JSON document served over the LAN is out of the question — a record says
`secured` and nothing more. Which leaves the problem of a form that must submit
*something*: an absent or null password means **keep the one that is there, matched
by SSID**, and an empty string means clear it. That is the difference between "I did
not retype it" and "this network is open", and it is two tests. The page never sends
a password box that was not typed in.

**The same rule caught a bug the board found rather than a test**: an omitted `ip`
block was *clearing* a static address, because the page has no address fields at
all — so every Apply quietly forgot an address somebody had set from the console.
Watched happening to a `192.168.1.42`. An absent block keeps what is there now, and
`"static": false` inside one is how a network goes back to DHCP.

Three more decisions worth stating:

- **memory only, like every other setter** (§10.15). Applying changes the live
  fields; `save` is its own action, so a mistaken or hostile write does not survive a
  reboot on its own and `reload` is the undo;
- **the bus is reconnected and the radio is not.** `nats::Apply` drops a connection
  the operator is not looking at; `wifimgr::Apply` would drop the link the page
  arrived over, mid-edit. So the network retry is a button, and it says on the page
  that it may take the page away;
- **`config.json`'s `web.write`** refuses the whole write path, `GET` included in the
  answer (`writable: false`, and the pages grey themselves out and say why). It is
  **not** authentication — anyone who can reach the server can submit the form,
  exactly as anyone on that LAN can already read every permission request off the bus
  (§10.3) — and it is the one switch a device on a network its owner does not trust
  has. TLS with credentials stays the real fix. The field cannot be written through
  the form, which is a test, and it is refused by the whitelist rather than by a
  special case.

**What the board did with it**, over the LAN: every refusal came back as itself with
the field named — `{"error":"this device has no such setting","detail":"touch"}`,
`"detail":"yes"` for a mode that is not one of the three, a `ws://` URL, a nameless
network, and `web.write` itself; a scan answered with seven networks off the air; and
then the case that matters — **the form as a browser submits it, every network with
no password in it, applied and saved, left all four keys on the filesystem exactly
as they were**, and the static address with them. `config` on the console is what
says so.

#### The one thing it can do without asking: `POST /api/reboot`

This section used to say "nothing writes", and one thing does now, at the
repository owner's request. Three rules make it defensible rather than merely
convenient:

- **it needs a word in the query** (`web_paths.h`), so a stray `POST` from a
  scanner, a link somebody sent or a page reloaded by mistake is not a device that
  restarts. It is **not authentication and must not be read as any**: §10.3 puts
  the trust boundary at the router, so anybody on that LAN can send it;
- **it is the safest write there is.** A reboot takes nothing away that does not
  come back by itself in ten seconds — which is exactly §10.7's argument for the
  *console's* `reboot` needing no confirmation word. What it can be abused for is a
  denial of service, and §10.10 already accepts that class of thing from anything
  that can reach the bus: ten seconds of a device answering nothing is a hook that
  times out and a question that goes back to its own terminal. **It cannot produce
  a verdict**, and that is the line this server does not cross;
- **and it is two steps on the page**, like the row on the panel (§10.8.5) and for
  the same reason: nobody typed anything to get here, so one tap must not restart a
  device somebody else is using. The arming expires on its own after five seconds.

Two details that are the hardware rather than the page. The response is sent and
then the handler waits half a second before `esp_restart`, which is §10.7's finding
about the console's `reboot` arriving at a port that goes down with the chip — the
answer is sitting in a TCP buffer, and restarting on the next statement takes it
with the stack. And the page treats a **dropped connection as the success case**,
because from a browser a device that went down and a device that failed are the
same silence; it then polls `/api/status` for a minute and says which of the two it
turned out to be, including the honest third answer — that it may have come back on
another address.

Being a `POST` is load-bearing, not decoration: the file handler is registered for
`GET` only, so every other route on this server is read-only *by construction*
rather than by intent.

**Confirmed on the board, in all four of its answers**: `POST /api/reboot` with no
query is `400 {"ok":false,"error":"not confirmed"}`, and so is `?confirm=reboots` —
the near-miss that a `strstr` would have accepted; `GET /api/reboot?confirm=reboot`
is a 404, because there is no `GET` handler for it to reach; and the real one
answered `200 {"ok":true,"rebooting":true}` and the device came back with the
registration and every setting intact.

And the thing that looked like a failure and is the design working: **the server
did not come back with it.** `web on` is memory-only like every other setter
(§10.15), so after the restart `web.mode` is `auto` again — and `auto` means "only
while this device is an access point", which a device on a client link is not. The
page waits a minute, says so, and names the honest third possibility (that it may
be on another address). `config save` is what makes a server that comes back.

#### What it serves, and the one clever thing in it

Four routes: `/` and the files the pages are made of, `/api/status` — the numbers
the front page shows, built with `snprintf` into a static buffer — the reboot
above, and **`/api/devstatus`, which is the console's own dump, byte for byte**.

That last one is the interesting one, and it exists because of §10.7's rule rather
than in spite of it: `devstatus` is composed of the other commands rather than
printing its own version of each, because "a second copy of the `power` readout
would drift from the first the day somebody adds a field to one of them". A dump
served over HTTP is exactly that temptation, at the scale of fifteen sections. So
there is no second copy: `console::PrintDevStatus` prints with `printf` like every
other readout here, and the server swaps `stdout` for the length of the call.

**Which is only safe because of what the sink knows.** This firmware's libc is
**picolibc** (`CONFIG_LIBC_PICOLIBC`), where `stdout` is *one global pointer* —
not a per-task one — so for the few milliseconds the dump takes, every task's
`printf` arrives at that sink, an `ESP_LOG` line from the screen task included.
Two consequences, and the second is not cosmetic:

- a foreign line landing in the middle of the response would be read as part of
  the dump;
- and it would reach `httpd_resp_send_chunk` **from a task that does not own the
  request**, which is a socket written from two places at once.

So the cookie records the task that opened it: its own writes become chunks, and
anybody else's are forwarded to the real `stdout` — where they were going anyway.
Nothing is interleaved, nothing is lost from the boot log, and the console is
still there afterwards, which is checked rather than assumed. The stream is also
deliberately **unbuffered**: a buffer would be shared with whatever foreign
`printf` the global `stdout` catches, and the whole point of the cookie is that
foreign text never lands in this response. The cost is one small chunk per
`printf`, for a diagnostic nobody is timing.

The dependency runs the way it has to: **`web` never mentions the console.**
`cli` already depends on `web` for its command, so `console::Init` hands its
printer over (`web::SetDiagnostics`) and the endpoint answers `503` until
somebody does — the same inversion `screens::OnDecision` and `wifimgr::OnTick`
make, for the third and fourth time in this firmware.

**Measured, serving it over the LAN**: 4,827 bytes of dump, the heap's low-water
mark at 12,644 free while it streamed, and the server task with **2,000 of its
4,096 bytes never used** — which is what settles the 4 KB default against the
house firmware's 8 KB (§10.14.4), on the heaviest handler there is: `devstatus`
reads a dozen I²C registers through the lease and formats a few hundred lines.

#### It is a wish, not a call

The operator sets a **desired state** and the server comes up only when the
network allows it — the repository owner's shape, and the same split §10.9 draws
between what was asked for and what is happening:

| | |
|---|---|
| `web off` | never |
| `web on` | whenever there is a network — a client link or an access point |
| `web auto` | **only while this device is an access point**, its own by request or §10.9's fallback one. That is the case the server exists for: nothing would have this device, so it became findable, and an operator has no other way in. On a working client link `auto` keeps it down and the seven kilobytes stay free |
| `web.mode` in `config.json` | the same three words, and **`auto` is the default** — including for a file written before this field existed |

Three things about that which are not obvious from the table:

- **the reconcile runs on the Wi-Fi manager's tick**, not on a task of its own.
  That manager already polls the radio five times a second and the server has
  nothing to do between link changes — which is `components/watcher`'s argument
  for having no task, arriving a second time. It cost a function pointer
  (`wifimgr::OnTick`) instead of 2.5 KB of permanent RAM, and `wifimgr` still
  knows nothing about what is on the other end of it;
- **the console sets the wish and waits for the tick**, so the readout it prints
  afterwards is the answer rather than the question — the same reason
  `screens::Navigate` waits for the screen task to take a move;
- **a failed start is not retried five times a second.** Five seconds, and typing
  a mode clears the wait, because an operator who just asked for something expects
  it tried now.

**What it looks like on the board**, and both directions of it are the point:
`web on` while connected to a network brought the server up inside one tick and
printed `reach http://192.168.11.134/`; `wifi mode off` took it back down *before*
the radio went, with `web` then saying `stopped - the radio is switched off`; and
`wifi mode client` brought it back **by itself**, no command typed, as soon as the
link returned.

**What it is configured to, against `HTTPD_DEFAULT_CONFIG`**, and each of the four
is a number this device cannot afford at its default:

- **three sockets, not seven.** Each open one is a `sock_db` and an lwIP PCB, and
  each *active* one can hold up to `CONFIG_LWIP_TCP_SND_BUF_DEFAULT` — 5,760
  bytes — of pbufs in flight. A browser opens two or three for one page;
- **LRU purge on**, which the default leaves off: with three sockets, a browser
  that leaves one open would otherwise make the next request fail rather than
  closing the oldest;
- **priority 2, not 5.** At 5 it preempts LVGL (4) and the screen task (3) to
  serve a page, and a page is never more urgent than the press on a permission
  request (§10.8.1);
- **the 4 KB stack kept**, where the house firmware of §10.14.4 raises it to 8 —
  because its handlers put a 2,500-byte buffer *on that stack* and ours puts a
  1 KB one in `.bss` (§10.14.1).

#### The page that hung, and the 16 kilobytes it took to fix

The site of the section above did not work when it was first flashed: a page loaded,
then a second one hung, and the heap did not come back. It is the most useful thing
in this section and the diagnosis went through three wrong answers, so it is written
down in the order it happened.

**What was observed.** An 11.7 KB page served *on its own* went out in 0.3 s and gave
every byte back. The same page after two others timed out, and about 5 KB stayed
gone. `web` reported `cost 22108 bytes while up` where §10.16 had measured 6,824.

**The first wrong answer: the TCP window.** Three sockets able to queue
`CONFIG_LWIP_TCP_SND_BUF_DEFAULT` (5,760) bytes each is 17 KB of in-flight data on a
device with 18 KB free, and a browser opening three sockets for a page and its two
files is exactly that. So the send buffer was lowered to 2,880 — and the result was
*worse*: 2.4 KB took eleven seconds. Two segments of window against a peer that ACKs
every second segment is a round trip per 2,880 bytes, and a slow socket is a socket
held open longer, so the pressure went up rather than down. The value is back at the
framework's default and `sdkconfig.defaults` carries that finding where somebody will
reach for the knob again.

**The second wrong answer: the socket count.** Two sockets instead of three, which
bounds the same arithmetic more cheaply. It changed nothing: pages still hung.

**What it actually was, and the device said it in four characters.** Watching the
console during a fetch:

```
>>> fetching /index.html
W (19924) wifi:m f null
>>> failed after 12.6 s: timed out
```

`m f null` is the Wi-Fi driver failing to *malloc a transmit buffer*
(`CONFIG_ESP_WIFI_TX_BUFFER_TYPE=1` — dynamic, from the heap). And the counters said
`served 1 file(s), 2774 byte(s)`: the handler had read the whole file and handed every
chunk to `httpd_resp_send_chunk` successfully. **The response was complete and the
packets never left**, because there was no memory to put them in — at 10 KB free,
fragmented, a 1.6 KB DMA-capable buffer is not there.

So the problem was never the protocol, the window or the socket count. It was that
this firmware had about 20 KB of heap with the server up, and a web server needs the
radio to be able to allocate while it works.

**The fix was 16 KB from LVGL.** `CONFIG_LV_MEM_SIZE_KILOBYTES` was the vendor's 64;
that pool is a static buffer, so every kilobyte of it is a kilobyte the heap does not
have. At **48 KB** the seven screens of §10.8 still build — the request card, the
heaviest of them, was photographed afterwards — and the numbers changed like this:

| | before | after |
|---|---|---|
| free at a fresh boot | 27,904–29,440 | **47,328** |
| free with the server up | ~18,500 | **37,920** |
| the low-water mark through a whole page and its script | 296–6,140 | **21,736** |
| a page, its script and an API poll at once | timed out | **0.08–0.47 s each** |

Two smaller things went with it, and they are worth the lines they cost: the chunk
buffer is 512 bytes rather than 1,024, the settings body is 1,280 rather than 2,048,
the scan list holds ten networks rather than sixteen, and the JSON document shares
the body's buffer — no request is ever both. About 2.3 KB of `.bss` back.

**The rule that came out of it**, and it is the one to keep: on this device the
number that matters is **how many requests a page makes**, not how big any of them
is. A 23 KB script on its own is fine; three 8 KB files at once is not. That is why
no page here has a stylesheet link.

#### What the board said, and one of them was a reboot

- **Two tasks, one pointer, and the diagnostic was the one that found it.**
  `web cycle` — the command this section exists for — panicked the board on its
  second round: `Load access fault`, and the stack decoded to
  `web::Stop` → `httpd_stop` → `httpd_delete` → `free` → **inside the ROM's
  allocator**, walking heap metadata that was no longer a heap. A double free,
  and the two callers were in the file all along: `Maintain()` runs on the Wi-Fi
  manager's task five times a second, `web cycle` runs on the console's, and both
  read `g_server`, compared it with null, and called `httpd_stop` on it. Nothing
  serialised them.

  What made it a *certainty* rather than a rare race is the guard the command
  already had. It refused to run unless the wish was `off` — written to stop the
  reconciler *starting* the server between rounds — and with the wish off the tick
  does the opposite: it tears down what the loop has just started, every round,
  50 ms after it goes up. So the two `Stop()` calls were not merely possible, they
  were scheduled. The tell was in the log and had been read as noise: **two
  `web: stopped` lines per round** and an `E httpd: Failed to send shutdown signal
  err=-1` between them, which is the second caller finding a control socket the
  first had already closed.

  Three things came out of it, and only the first is the crash:

  * **the lifetime is under a lock now** — a static mutex, `Start`/`Stop`/
    `Maintain` taking it and `…Locked` halves underneath, which is `Es8311`'s
    shape (§10.14.3) and for the same reason: the mutex is not recursive, so the
    caller that holds it must not call something that takes it again;
  * **`web cycle` takes the lifetime away from the reconciler** for the length of
    its loop, and gives it back on every path out. That is `web::Hold`, and the
    rule lives in `web_policy.h` as a *value* — `Reconcile Next(…, bool held)`
    answers `kNothing` while it is held, whatever the radio is doing — so the one
    thing that crashed the device is a comparison with tests on it rather than an
    `if` inside a task. The old `set 'web off' first` refusal is gone with it:
    the mode no longer changes what the loop measures;
  * **a failed stop is no longer reported as a stop.** `httpd_stop` returns
    *before doing anything* when it cannot signal its own task — so the previous
    code, which nulled the handle regardless, left a server task alive with its
    handle thrown away while `web` printed `stopped`. That was the
    `512 byte(s) unreturned by its stop` nobody could explain. It keeps the handle
    and says so now, and the next tick tries again.

  Measured after the fix, on the sequence that used to panic — a browser's worth
  of pages served over the LAN, then twenty rounds: **`+0` drift on every round
  but the first, a 104-byte spread, one `stopped` line per round, and the uptime
  continuous.** Then ten more under `auto`, which the old guard refused outright:
  `+0`, spread **0**, `all of it came back`.

- **`httpd_start` before lwIP exists is a panic, not an error.** §10.9 brings the
  network stack up lazily — `esp_netif_init` and the netifs happen inside the
  radio's first use — so on a freshly flashed device, where `wifi.active` is
  false, `web on` walked into `assert failed: tcpip_send_msg_wait_sem (Invalid
  mbox)` and rebooted the board. It refuses now, in two places: the component
  checks `wifimgr::Snapshot::stack_up` — a field that exists because of this —
  and the console prints the fix rather than the error code. That snapshot field
  is the general lesson: **"is the radio ready" and "does the TCP/IP stack exist"
  are different questions**, and everything that opens a socket wants the second
  one.
- **Closing a socket after its netif is destroyed is a null call, and that cost a
  second reboot.** `wifi mode off` under a running server panicked with
  `MEPC 0x00000000`, and the return address named it exactly: `lwip_close` →
  `netconn_delete` → `netconn_drain` → `pbuf_free` → `esp_pbuf_free` →
  **`esp_netif_free_rx_buffer`**, which calls the free-callback of a driver that
  `esp_netif_destroy` had already taken away. So the rule is an ordering rather
  than a check: **the server has to let go while the radio is still up and merely
  no longer wanted.** Two lines make it true — the manager calls the tick handler
  *before* its own pump rather than after, and `web::ShouldRun` takes
  `network_wanted` (the radio is *supposed* to be up) as a separate input from
  `stack_up` (it currently is). Both are host-tested; the first would otherwise be
  a comment nobody could check.

  Worth reading next to the assert above, because they are the same lesson from
  either end: **a socket needs the network stack to exist for its whole life, not
  just at `bind`.** Starting one before lwIP is an assert; closing one after lwIP
  has been dismantled underneath it is a jump to zero.
- **A full flash takes the device's settings with it.** Adding a page under
  `spiffs_image/` means `idf.py flash` rather than `app-flash`, and that writes
  `storage.bin` over the *whole* partition — so the live `config.json` (the real
  networks and their passwords) and `registration.json` are gone, and the device
  comes back on the factory defaults, unregistered. §10.15 designs for a config
  that can be restored; it does not design for one that is overwritten by a
  build. **`cat config.json` before a full flash** is the cheap habit, and
  §10.12's flashing notes now say so.

  **And there is a way round it that costs nothing**, used to flash the site onto
  a registered board: read both files off the console, stage them over a copy of
  `spiffs_image/` *outside the repository* — a real WPA key must never land in a
  committed file — and run `spiffsgen.py` with the arguments the build itself uses
  (they are in `build/build.ninja`: `0xae0000`, page 256, name 32, meta 4,
  `--use-magic --use-magic-len`). Then `idf.py app-flash` and one `esptool
  write-flash 0x520000` of that image. The device came back with its networks, its
  pinned handler key and its registration date unchanged, which is a token not
  minted and a registration not repeated.

#### What it deliberately does not do yet

- **Most settings are unreachable, and that is the whitelist rather than an
  omission.** Wi-Fi and the bus address write (two sections up); *everything else
  in `config.json`* — the touch calibration, the idle timers, the clock, the
  display's brightness, the audio volume, `web.write` itself — is refused by name
  by `POST /api/settings`, and there is no endpoint that reaches them. This bullet
  used to read "nothing writes a setting", which was true until the repository
  owner asked for those two pages; what survived the change is the shape of the
  answer, which is a list of what may be written rather than a merge of whatever
  arrives.
- **Nothing authenticates.** `web.write: false` refuses the whole write path, and
  it is a switch rather than a lock — anybody who can reach the server can submit
  the form. That is §10.3's boundary and the decision at the end of this section
  is where it gets moved on purpose.
- **Nothing decides for the operator.** `web auto` is as close as this gets to
  automatic, and it is deliberately about the *access point* rather than about
  §10.9's window: the fallback AP going up is what raises the server, and its
  expiry takes it down again, without the manager having to know that the web
  server exists. What is still not wired is a *screen* for any of it — the Wi-Fi
  screen of §10.8.6 has no row that says whether the page is up, and it should.
- **It can never reach a verdict** (§10.10). It serves files and reports state;
  the only path to `allow` is a human press on a card.

#### The one rule that is not about memory

**The pages and `config.json` are on the same flat filesystem.** SPIFFS carries
`index.html` next to the Wi-Fi passphrase, the pinned handler key and the factory
defaults — so a server that serves whatever is asked for hands the WPA password to
anyone who types `/config.json`. §10.15's rule is that a password is a secret from
the moment it is typed: never logged, never in a console dump. A URL is a wider
audience than either.

So `web_paths.h` is a **whitelist of extensions** rather than a blacklist of
names: `.html`, `.css`, `.js`, `.png`, `.ico`, `.svg`, `.txt`, and nothing else.
`404.html` is on that whitelist like any other page, deliberately — the server
opens it by name rather than from a URL, and a file it may open under a rule the
rest of it does not follow is exactly the hole this file exists to keep shut. A
device flashed before that page existed falls back to the one-line answer, because
a 404 that failed to be a 404 would be the confusing outcome.
A secret added to that filesystem next year is refused by a rule that already
exists rather than by somebody remembering to add its name. Two smaller rules come
from the same place: **SPIFFS is flat**, so a name has no directories in it and a
second `/` is refused outright — traversal is unrepresentable rather than
defended against — and **nothing is percent-decoded**, because decoding is where
the traversal bugs live. All of it is `<cstddef>`-only and host-tested (28 tests, with 29 more next door over the write path),
and the suite reads as a list of things that must not leave the device.

And the decision that is still open, which the *write* path shipped ahead of:
this section named authentication as the thing a writable config needed first,
the pages above were then asked for, and what was built is the narrowest write
there is — a whitelist, write-only passwords, memory until a save, and
`web.write` to refuse the lot. **None of that is the missing piece.** The
boundary moved when the first `POST` landed, and moving it back on purpose means
one of two things: authentication (the house firmware of §10.14.4 answers this
with `users.json` and basic auth), or — more likely for this device — **binding
the server to the access-point interface only**. The server exists to configure a
device that has no network yet; on the LAN §10.3 already put the trust boundary
at the router for *reading*, and reading is what that argument covers.

Until one of those is taken, `web.write` is the whole of the answer and it is a
switch rather than a lock: it is what a device on a network its owner does not
trust has, and it is not authentication.

