// The configuration site (CLAUDE.md 10.16): the stylesheet, the helpers and every
// page's script, in one file fetched *after* the page rather than beside it — one
// socket at a time is what the device's heap can carry, and that is why there is no
// <link> in any page and why the CSS is injected from here.
(function () {
  "use strict";

  var CSS = [
    ":root{color-scheme:dark;--bg:#0b0d0c;--plate:#141816;--live:#1a221d;",
    "--line:#232825;--text:#d6ded8;--faint:#7a827e;--dim:#4e5451;",
    "--green:#45c46a;--amber:#d69e2e;--red:#e05252}",
    "*{box-sizing:border-box}",
    "body{margin:0;padding:0 16px 40px;background:var(--bg);color:var(--text);",
    "font:16px/1.5 system-ui,-apple-system,'Segoe UI',Roboto,sans-serif;",
    "-webkit-text-size-adjust:100%}",
    ".wrap{max-width:560px;margin:0 auto}",
    "header{display:flex;align-items:baseline;gap:10px;padding:20px 0 14px}",
    "h1{font-size:19px;font-weight:600;margin:0}",
    ".lamp{margin-left:auto;display:inline-flex;align-items:center;gap:7px;",
    "font-size:13px;color:var(--faint);white-space:nowrap}",
    ".lamp::before{content:'';width:9px;height:9px;border-radius:50%;background:var(--dim)}",
    ".lamp.ok::before{background:var(--green)}",
    ".lamp.warn::before{background:var(--amber)}",
    ".lamp.bad::before{background:var(--red)}",
    "nav{display:flex;flex-direction:column;gap:10px;margin:4px 0 26px}",
    "a.btn,button.btn{display:flex;align-items:center;gap:12px;width:100%;",
    "min-height:58px;padding:14px 18px;border:1px solid var(--line);border-radius:12px;",
    "background:var(--plate);color:var(--text);font:inherit;font-weight:500;",
    "text-align:left;text-decoration:none;cursor:pointer;-webkit-tap-highlight-color:transparent}",
    "a.btn:active,button.btn:active{background:var(--live)}",
    "a.btn .sub,button.btn .sub{display:block;font-size:13px;font-weight:400;color:var(--faint)}",
    ".grow{flex:1;min-width:0}.chev{color:var(--dim)}",
    ".btn.warn{border-color:#3a2f14}.btn.warn .name{color:var(--amber)}",
    "button:disabled{opacity:.5;cursor:default}",
    "h2{font-size:13px;font-weight:600;text-transform:uppercase;letter-spacing:.08em;",
    "color:var(--faint);margin:26px 0 8px}",
    "table{border-collapse:collapse;width:100%}",
    "th,td{text-align:left;padding:9px 0;border-bottom:1px solid var(--line);",
    "font-size:15px;vertical-align:top}",
    "th{color:var(--faint);font-weight:400;width:45%}",
    "td{font-variant-numeric:tabular-nums}",
    "tr:last-child th,tr:last-child td{border-bottom:0}",
    ".ok{color:var(--green)}.warn{color:var(--amber)}.bad{color:var(--red)}",
    ".faint{color:var(--faint)}",
    ".meter{height:6px;margin-top:6px;border-radius:3px;background:var(--line);overflow:hidden}",
    ".meter>i{display:block;height:100%;background:var(--green)}",
    ".meter>i.warn{background:var(--amber)}.meter>i.bad{background:var(--red)}",
    "pre{margin:0;padding:14px;border:1px solid var(--line);border-radius:10px;",
    "background:#0e1110;color:#a8b0aa;font-size:12px;line-height:1.45;",
    "overflow-x:auto;white-space:pre}",
    "p.lead{margin:0 0 20px;color:var(--faint);font-size:14px}",
    "footer{margin-top:30px;padding-top:14px;border-top:1px solid var(--line);",
    "color:var(--dim);font-size:13px}",
    ".notice{margin:0 0 18px;padding:12px 14px;border:1px solid var(--line);",
    "border-left:3px solid var(--amber);border-radius:8px;background:var(--plate);",
    "font-size:14px;color:var(--text)}",
    ".notice.bad{border-left-color:var(--red)}.notice.ok{border-left-color:var(--green)}",
    ".center{text-align:center}",
    ".big{font-size:64px;line-height:1;margin:40px 0 10px;color:var(--dim)}",
    ".hint{margin:0 0 12px;color:var(--faint);font-size:13px}",
    ".hint code,td code{color:var(--text)}",
    ".field{display:flex;align-items:center;gap:10px;margin:0 0 10px}",
    ".field label{flex:0 0 88px;color:var(--faint);font-size:14px}",
    ".field input{flex:1;min-width:0;min-height:48px;padding:10px 12px;",
    "border:1px solid var(--line);border-radius:10px;background:#0e1110;",
    "color:var(--text);font:inherit;font-size:16px}",
    ".field input:focus{outline:none;border-color:#3a4d42}",
    ".field input:disabled{opacity:.5}",
    "button.reveal{flex:0 0 auto;min-height:48px;padding:0 12px;border:1px solid var(--line);",
    "border-radius:10px;background:var(--plate);color:var(--faint);font:inherit;",
    "font-size:13px;cursor:pointer}",
    ".seg{display:flex;gap:8px;margin:0 0 12px}",
    ".seg button{flex:1;min-height:52px;padding:8px 6px;border:1px solid var(--line);",
    "border-radius:10px;background:var(--plate);color:var(--faint);font:inherit;",
    "font-size:14px;cursor:pointer}",
    ".seg button.on{background:#1c2b22;border-color:#3a5c46;color:var(--text);font-weight:600}",
    ".card{padding:12px;margin:0 0 10px;border:1px solid var(--line);",
    "border-radius:12px;background:var(--plate)}",
    ".card .field:last-child{margin-bottom:0}",
    ".card-head{display:flex;align-items:center;margin:0 0 10px;color:var(--faint);font-size:13px}",
    "button.drop{margin-left:auto;width:40px;height:40px;border:1px solid var(--line);",
    "border-radius:10px;background:transparent;color:var(--faint);font-size:20px;",
    "line-height:1;cursor:pointer}",
    ".row-actions{display:flex;gap:10px;margin:0 0 14px}",
    ".btn.small{min-height:48px;padding:10px 14px;font-size:14px;font-weight:400}",
    ".row-actions .btn.small{flex:1;justify-content:center}",
    ".sticky{display:flex;flex-direction:column;gap:10px;margin:22px 0 0;",
    "padding:14px 0 0;border-top:1px solid var(--line)}",
    ".sticky .btn{justify-content:center}",
    "#scan-list{display:flex;flex-direction:column;gap:8px;margin:0 0 14px}",
    "#scan-list .btn.small .sub{display:inline;margin-left:10px}"
  ].join("");

  var style = document.createElement("style");
  style.textContent = CSS;
  document.head.appendChild(style);

  var by = function (id) { return document.getElementById(id); };

  function bytes(n) {
    if (typeof n !== "number" || !isFinite(n)) return "-";
    if (n < 1024) return n + " B";
    if (n < 1048576) return (n / 1024).toFixed(1) + " KB";
    return (n / 1048576).toFixed(2) + " MB";
  }

  function duration(seconds) {
    if (typeof seconds !== "number" || !isFinite(seconds)) return "-";
    var s = Math.floor(seconds), d = Math.floor(s / 86400);
    var h = Math.floor((s % 86400) / 3600), m = Math.floor((s % 3600) / 60);
    if (d > 0) return d + "d " + h + "h " + m + "m";
    if (h > 0) return h + "h " + m + "m";
    if (m > 0) return m + "m " + (s % 60) + "s";
    return s + "s";
  }

  function text(el, value) {
    if (el) el.textContent = value === undefined || value === null ? "-" : String(value);
  }

  function lampFor(d) {
    if (d.subscribed) return { cls: "ok", word: "ready" };
    if (!d.registered) return { cls: "bad", word: "not registered" };
    if (!d.bus_up) return { cls: "warn", word: "no bus" };
    return { cls: "warn", word: d.blocked_by || "not subscribed" };
  }

  function meter(el, percent, warnAt, badAt) {
    if (!el || !el.firstElementChild) return;
    var value = Math.max(0, Math.min(100, percent));
    el.firstElementChild.style.width = value + "%";
    el.firstElementChild.className = value <= badAt ? "bad" : value <= warnAt ? "warn" : "";
  }

  function ask(method, url, body, onOk, onFail) {
    var r = new XMLHttpRequest();
    r.open(method, url, true);
    r.timeout = 20000;
    if (body) r.setRequestHeader("Content-Type", "application/json");
    r.onload = function () {
      var parsed = null;
      try { parsed = JSON.parse(r.responseText); } catch (e) { parsed = null; }
      if (r.status >= 200 && r.status < 300) { onOk(parsed || {}, r); return; }
      // The device's own words, which name the field rather than the line number.
      var why = parsed && parsed.error ? parsed.error : "the device answered " + r.status;
      if (parsed && parsed.detail) why += ": " + parsed.detail;
      onFail(why, r.status);
    };
    r.onerror = function () { onFail("no answer", 0); };
    r.ontimeout = function () { onFail("no answer in 20 s", 0); };
    r.send(body ? JSON.stringify(body) : null);
  }

  function getJson(url, onOk, onFail) { ask("GET", url, null, onOk, onFail || function () {}); }
  function post(url, body, onOk, onFail) { ask("POST", url, body, onOk, onFail); }

  function lamp(cls, word) {
    var el = by("lamp");
    if (!el) return;
    el.className = "lamp " + cls;
    el.textContent = word;
  }

  function said(text, kind) {
    var box = by("said");
    if (!box) return;
    box.hidden = false;
    box.className = "notice" + (kind ? " " + kind : "");
    box.textContent = text;
  }

  var D = {
    by: by, bytes: bytes, duration: duration, text: text, lampFor: lampFor,
    meter: meter, getJson: getJson, post: post, lamp: lamp, said: said
  };
  window.Device = D;

  // --- The front page ------------------------------------------------------

  function frontPage() {
    function paint(d) {
      var state = lampFor(d);
      lamp(state.cls, state.word);
      text(by("approval-state"), state.word);
      by("approval-state").className = state.cls;
      text(by("key-id"), d.key_id || "-");
      text(by("received"), d.received);
      text(by("answered"), d.replied + " (" + d.allowed + " allow, " + d.denied + " deny)");

      text(by("wifi"), d.wifi);
      text(by("ssid"), d.ssid || "-");
      var rssi = by("rssi");
      if (typeof d.rssi === "number" && d.rssi < 0) {
        rssi.textContent = d.rssi + " dBm";
        rssi.className = d.rssi > -67 ? "ok" : d.rssi > -80 ? "warn" : "bad";
      } else { rssi.textContent = "-"; rssi.className = "faint"; }
      text(by("ip"), d.ip === "0.0.0.0" ? "-" : d.ip);
      by("bus").textContent = d.bus;
      by("bus").className = d.bus_up ? "ok" : "warn";

      // A cable is its own answer rather than an empty gauge.
      if (typeof d.battery === "number" && d.battery >= 0) {
        by("power").textContent = d.battery + "%, " + (d.battery_mv / 1000).toFixed(2) + " V" +
          (d.charging ? ", charging" : d.usb ? ", on usb" : "");
        by("battery-meter").style.display = "";
        meter(by("battery-meter"), d.battery, 30, 10);
      } else {
        by("power").textContent = d.usb ? "on usb, no battery" : "no battery";
        by("battery-meter").style.display = "none";
      }

      text(by("uptime"), duration(d.uptime_s));
      text(by("heap"), bytes(d.heap_free) + ", lowest " + bytes(d.heap_low));
      var used = d.spiffs_total > 0 ? (d.spiffs_used / d.spiffs_total) * 100 : 0;
      by("spiffs").textContent = bytes(d.spiffs_used) + " of " + bytes(d.spiffs_total) +
        " (" + used.toFixed(0) + "%)";
      meter(by("spiffs-meter"), 100 - used, 20, 5);
      text(by("firmware"), d.firmware + " on esp-idf " + d.idf);
    }

    function failed(why) {
      lamp("bad", why);
      var cells = document.querySelectorAll("td, td span");
      for (var i = 0; i < cells.length; i++) {
        if (cells[i].textContent === "…") {
          cells[i].textContent = "-";
          cells[i].className = "faint";
        }
      }
    }

    function refresh() { getJson("/api/status", paint, failed); }
    refresh();
    setInterval(refresh, 5000);
  }

  // --- The console dump ----------------------------------------------------

  function devStatusPage() {
    var dump = by("dump"), button = by("again");

    function read() {
      button.disabled = true;
      lamp("", "reading…");
      var started = Date.now();
      ask("GET", "/api/devstatus", null, function (ignored, r) {
        button.disabled = false;
        lamp("ok", "read");
        dump.textContent = r.responseText;
        text(by("taken"), (r.responseText.length / 1024).toFixed(1) + " KB in " +
          ((Date.now() - started) / 1000).toFixed(1) + " s");
      }, function (why, status) {
        button.disabled = false;
        if (status === 503) {
          lamp("warn", "no dump");
          dump.textContent = "This firmware has no diagnostics wired up.";
          return;
        }
        lamp("bad", why);
        // The last good dump is kept: stale text that says so beats an empty page.
        if (dump.textContent === "reading…") dump.textContent = "-";
      });
    }

    button.addEventListener("click", read);
    read();
  }

  // --- The restart ---------------------------------------------------------

  function rebootPage() {
    var button = by("go"), name = by("go-name"), sub = by("go-sub");
    var ARMED = 5000, armedUntil = 0, timer = null;

    function disarm() {
      armedUntil = 0;
      if (timer) { clearTimeout(timer); timer = null; }
      name.textContent = "Restart the device";
      sub.textContent = "tap, then confirm";
    }

    function arm() {
      armedUntil = Date.now() + ARMED;
      name.textContent = "Tap again to restart";
      sub.textContent = "or wait five seconds and nothing happens";
      timer = setTimeout(disarm, ARMED);
    }

    function waitForIt() {
      lamp("warn", "waiting for it");
      var deadline = Date.now() + 60000;
      (function poll() {
        setTimeout(function () {
          getJson("/api/status", function (d) {
            lamp("ok", "up again");
            name.textContent = "It is back";
            sub.textContent = "uptime " + duration(d.uptime_s);
          }, function () {
            if (Date.now() < deadline) { poll(); return; }
            lamp("bad", "no answer");
            name.textContent = "No answer in a minute";
            sub.textContent = "it may be on another address, or it may need the button";
          });
        }, 3000);
      })();
    }

    button.addEventListener("click", function () {
      if (armedUntil !== 0 && Date.now() < armedUntil) {
        button.disabled = true;
        name.textContent = "Restarting";
        sub.textContent = "the device is going down";
        lamp("warn", "restarting");
        post("/api/reboot?confirm=reboot", null, waitForIt, waitForIt);
        return;
      }
      arm();
    });
  }

  // --- Wi-Fi ---------------------------------------------------------------

  function wifiPage() {
    var state = { mode: "off", networks: [], max: 4, writable: true };

    function drawMode() {
      var buttons = by("mode").querySelectorAll("button");
      for (var i = 0; i < buttons.length; i++) {
        buttons[i].className = buttons[i].getAttribute("data-mode") === state.mode ? "on" : "";
      }
    }

    function drawNetworks() {
      var box = by("networks");
      box.innerHTML = "";
      if (state.networks.length === 0) {
        var empty = document.createElement("p");
        empty.className = "hint";
        empty.textContent = "None. This device will raise its own access point.";
        box.appendChild(empty);
      }
      state.networks.forEach(function (network, index) {
        var card = document.createElement("div");
        card.className = "card";
        card.innerHTML =
          '<div class="card-head"><b>' + (index + 1) + '</b>' +
          '<button class="drop" type="button" title="forget">&times;</button></div>' +
          '<div class="field"><label>Name</label><input class="ssid" type="text" ' +
          'autocapitalize="off" autocomplete="off" spellcheck="false"></div>' +
          '<div class="field"><label>Password</label>' +
          '<input class="pass" type="password" autocomplete="new-password">' +
          '<button class="reveal" type="button">show</button></div>';
        card.querySelector(".ssid").value = network.ssid;
        var pass = card.querySelector(".pass");
        pass.placeholder = network.secured ? "unchanged" : "none";
        card.querySelector(".drop").addEventListener("click", function () {
          state.networks.splice(index, 1);
          drawNetworks();
        });
        card.querySelector(".reveal").addEventListener("click", function () {
          pass.type = pass.type === "password" ? "text" : "password";
          this.textContent = pass.type === "password" ? "show" : "hide";
        });
        box.appendChild(card);
      });
      by("add").disabled = state.networks.length >= state.max;
    }

    function collect() {
      var cards = by("networks").querySelectorAll(".card");
      var networks = [];
      for (var i = 0; i < cards.length; i++) {
        var entry = { ssid: cards[i].querySelector(".ssid").value.trim() };
        var typed = cards[i].querySelector(".pass").value;
        if (typed !== "") entry.password = typed;
        networks.push(entry);
      }
      var ap = { ssid: by("ap-ssid").value.trim() };
      if (by("ap-pass").value !== "") ap.password = by("ap-pass").value;
      return { wifi: { mode: state.mode, networks: networks, ap: ap } };
    }

    function load() {
      getJson("/api/settings", function (d) {
        var wifi = d.wifi || {};
        state.mode = wifi.mode || "off";
        state.max = d.max || 4;
        state.writable = d.writable !== false;
        state.networks = wifi.networks || [];
        by("ap-ssid").value = (wifi.ap && wifi.ap.ssid) || "";
        by("ap-pass").placeholder = wifi.ap && wifi.ap.secured ? "unchanged" : "none";
        drawMode();
        drawNetworks();

        var link = d.state || {};
        lamp(link.wifi === "connected" ? "ok" : "warn", link.wifi || "?");
        by("state").textContent = link.wifi === "connected"
          ? "Connected to " + link.ssid + " at " + link.ip + ", " + link.rssi + " dBm."
          : "Not connected" + (link.failure && link.failure !== "none"
              ? " — last attempt: " + link.failure : "") + ".";

        if (!state.writable) {
          said("this device is serving read-only — config.json has web.write false", "bad");
          var all = document.querySelectorAll("button:not(.reveal), input");
          for (var i = 0; i < all.length; i++) all[i].disabled = true;
        }
      }, function (why) { lamp("bad", why); });
    }

    function apply(alsoSave) {
      said("applying…");
      post("/api/settings", collect(), function () {
        if (!alsoSave) {
          said("applied — in memory only, so a restart forgets it", "ok");
          load();
          return;
        }
        post("/api/action?do=save", null, function () {
          said("saved to config.json", "ok");
          load();
        }, function (why) { said(why, "bad"); });
      }, function (why) { said(why, "bad"); });
    }

    by("mode").addEventListener("click", function (event) {
      var mode = event.target.getAttribute("data-mode");
      if (!mode) return;
      state.mode = mode;
      drawMode();
      said("not applied yet — press Apply");
    });

    by("add").addEventListener("click", function () {
      if (state.networks.length >= state.max) return;
      state.networks.push({ ssid: "", secured: false });
      drawNetworks();
    });

    by("scan").addEventListener("click", function () {
      said("looking… a second or two, and it costs the connection a beat");
      getJson("/api/wifi/scan", function (d) {
        var box = by("scan-list");
        box.innerHTML = "";
        if (!d.networks || d.networks.length === 0) { said("nothing on the air", "warn"); return; }
        said(d.networks.length + " on the air — pick one to fill in a row", "ok");
        d.networks.sort(function (a, b) { return b.rssi - a.rssi; });
        d.networks.forEach(function (found) {
          var pick = document.createElement("button");
          pick.className = "btn small";
          pick.type = "button";
          pick.innerHTML = "<span class='grow'>" + found.ssid + "</span><span class='sub'>" +
            found.rssi + " dBm " + (found.secured ? "🔒" : "open") + "</span>";
          pick.addEventListener("click", function () {
            var rows = by("networks").querySelectorAll(".ssid");
            for (var i = 0; i < rows.length; i++) {
              if (rows[i].value.trim() === "") { rows[i].value = found.ssid; return; }
            }
            if (state.networks.length >= state.max) {
              said("all " + state.max + " slots are taken — drop one first", "warn");
              return;
            }
            state.networks.push({ ssid: found.ssid, secured: found.secured });
            drawNetworks();
          });
          box.appendChild(pick);
        });
      }, function (why) { said(why, "bad"); });
    });

    document.querySelector(".reveal[data-for='ap-pass']").addEventListener("click", function () {
      var input = by("ap-pass");
      input.type = input.type === "password" ? "text" : "password";
      this.textContent = input.type === "password" ? "show" : "hide";
    });

    by("apply").addEventListener("click", function () { apply(false); });
    by("save").addEventListener("click", function () { apply(true); });
    by("retry").addEventListener("click", function () {
      said("reconnecting — this page may go quiet if the link changes");
      post("/api/action?do=retry", null, function () { setTimeout(load, 3000); },
           function (why) { said(why, "bad"); });
    });

    load();
  }

  // --- The bus -------------------------------------------------------------

  function natsPage() {
    function load() {
      getJson("/api/status", function (d) {
        lamp(d.bus_up ? "ok" : "warn", d.bus);
        by("state").textContent = d.bus;
        by("state").className = d.bus_up ? "ok" : "warn";
        by("who").textContent = d.key_id + (d.registered ? ", registered" : ", not registered");
        by("who").className = d.registered ? "" : "warn";
        text(by("received"), d.received);
        text(by("answered"), d.replied + " (" + d.allowed + " allow, " + d.denied + " deny)");
      }, function (why) { lamp("bad", why); });

      getJson("/api/settings", function (d) {
        var field = by("url");
        if (document.activeElement !== field) field.value = (d.nats && d.nats.url) || "";
        if (d.writable === false) {
          said("this device is serving read-only — config.json has web.write false", "bad");
          var all = document.querySelectorAll("button, input");
          for (var i = 0; i < all.length; i++) all[i].disabled = true;
        }
      });
    }

    function apply(alsoSave) {
      said("applying…");
      post("/api/settings", { nats: { url: by("url").value.trim() } }, function () {
        if (!alsoSave) {
          said("applied and reconnecting — in memory only, so a restart forgets it", "ok");
          setTimeout(load, 2500);
          return;
        }
        post("/api/action?do=save", null, function () {
          said("saved to config.json, and reconnecting", "ok");
          setTimeout(load, 2500);
        }, function (why) { said(why, "bad"); });
      }, function (why) { said(why, "bad"); });
    }

    by("apply").addEventListener("click", function () { apply(false); });
    by("save").addEventListener("click", function () { apply(true); });
    by("reconnect").addEventListener("click", function () {
      said("reconnecting…");
      post("/api/action?do=reconnect", null, function () { setTimeout(load, 2500); },
           function (why) { said(why, "bad"); });
    });

    load();
  }

  var pages = {
    front: frontPage,
    devstatus: devStatusPage,
    reboot: rebootPage,
    wifi: wifiPage,
    nats: natsPage
  };
  var which = document.body.getAttribute("data-page");
  if (pages[which]) pages[which]();
})();
