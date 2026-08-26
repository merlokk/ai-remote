// The console (CLAUDE.md §10.7), on UART0 — the CH343P bridge, which on this
// board is a different socket from the one a security key plugs into (§10.1).
// That is the whole reason a device which is a USB *host* can still be talked to
// over USB, and it is worth knowing before wondering why this is not on USB
// Serial/JTAG the way the sibling board's is.
//
// **Sixteen commands, and there is a command per piece of hardware this board
// actually has.** `led` (§10.17) and `key` with its verbs (§10.18) are this
// device's own; `status`, `config`, `wifi`, `nats`, `keys`, `register`, `forget`,
// `ls`, `cat`, `term` and `reboot` are the same commands the sibling board
// answers, deliberately unchanged so that an operator who knows one of these two
// devices knows the other.
//
// **There is no command here for a display, a speaker, a power rail or a clock**,
// because none of those is on this board (§10.13). The clock is the one worth a
// sentence: §7's `ts` is echoed from the request and never re-derived
// (`signing.h`), and every duration this console prints is a monotonic one off
// `esp_timer`.
//
// `commands.md` in this folder is the list of what you can type. Design
// documents describe why; that one describes what.

#include "console.h"

#include <sys/time.h>

#include <cctype>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "board.h"
#include "buttons.h"
#include "config.h"
#include "device_key.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_console.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "linenoise/linenoise.h"
#include "approval.h"
#include "fido.h"
#include "indicator.h"
#include "led.h"
#include "registrar.h"
#include "registration.h"
#include "responder.h"
#include "mbedtls/base64.h"
#include "nats_link.h"
#include "age_text.h"
#include "request_card.h"
#include "storage.h"
#include "wifi_manager.h"
#include "wifi_radio.h"

namespace console {

namespace {

constexpr const char *TAG = "cli";

// `cat` prints out of a fixed buffer — no heap in our code (§10.14.1), and a
// bound on an operator-supplied path is a bound worth having anyway. The
// configuration files of §10.15 are a kilobyte or so; anything that does not
// fit is refused with its size rather than truncated into something that reads
// as complete.
constexpr size_t kFileBufferSize = 4096;
char file_buffer[kFileBufferSize];

// How many lines `term smart` gives the up-arrow. `esp_console`'s own default,
// named here because `term` prints it.
constexpr int kHistoryLength = 32;

int CmdStatus(int, char **) {
    const esp_app_desc_t *app = esp_app_get_description();
    printf("firmware   %s %s (%s %s)\n", app->project_name, app->version, app->date,
           app->time);
    printf("idf        %s (built with %s)\n", esp_get_idf_version(), app->idf_ver);

    esp_chip_info_t chip = {};
    esp_chip_info(&chip);
    printf("chip       %s rev %d.%d, %d core(s)\n", CONFIG_IDF_TARGET,
           chip.revision / 100, chip.revision % 100, chip.cores);

    uint8_t mac[6] = {};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        printf("mac        %02x:%02x:%02x:%02x:%02x:%02x\n", mac[0], mac[1], mac[2], mac[3],
               mac[4], mac[5]);
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running != nullptr) {
        printf("running    %s at 0x%06" PRIx32 ", %" PRIu32 " KB\n", running->label,
               running->address, running->size / 1024);
    }

    const int64_t up = esp_timer_get_time() / 1000000;
    printf("uptime     %lldd %02lldh %02lldm %02llds\n", up / 86400, (up % 86400) / 3600,
           (up % 3600) / 60, up % 60);

    // The low-water mark is the number that says whether the device is safe
    // (§10.14.1); the current free heap only says what this instant looks like.
    printf("heap       %" PRIu32 " free, %" PRIu32 " lowest ever\n",
           esp_get_free_heap_size(), esp_get_minimum_free_heap_size());

    size_t total = 0;
    size_t used = 0;
    if (storage::Mounted() && storage::Info(&total, &used) == ESP_OK) {
        printf("storage    %s, %u of %u bytes used\n", storage::kBasePath,
               static_cast<unsigned>(used), static_cast<unsigned>(total));
    } else {
        printf("storage    not mounted\n");
    }

    return 0;
}

// Sixteen names is far more than §10.15 puts in this partition; over that, the
// listing says so rather than looking complete.
constexpr size_t kMaxListed = 16;
storage::Entry entries[kMaxListed];

// A duration a person reads at a glance. The internet check prints plain
// seconds because its numbers *are* seconds; the clock's are hours, and
// "21600 s ago" is a conversion nobody should have to do in their head.
// **The three bands live in `ui::AgeText`**, and this calls it rather than
// keeping a second copy: three commands here print an age, and two
// implementations of "how long ago" is how one readout and the next come to
// describe the same instant two different ways. It is also the half that is
// host-tested — nothing in this file is.
void PrintDuration(uint32_t ms) {
    char text[ui::kAgeTextSize];
    ui::AgeText(ms / 1000, text, sizeof text);
    printf("%s", text);
}

int CmdReboot(int argc, char **) {
    if (argc != 1) {
        printf("usage: reboot        restart now; anything set and not saved is lost\n");
        return 1;
    }

    // **No confirmation word, and that is the difference from `poweroff`.**
    // That one asks for `now` because succeeding means the operator has to
    // walk over and press a button — from the console it is one-way. This one
    // undoes itself in a few seconds, and making the most ordinary debugging
    // action two words would be friction with nothing behind it.
    //
    // What it does cost is *said* rather than guarded against: `config set`
    // writes to memory and `config save` is what reaches the filesystem, so a
    // reboot is exactly where unsaved edits go.
    printf("rebooting — anything set and not saved is gone\n");

    // **The line has to leave before the peripheral does.** The console is the
    // C6's own USB Serial/JTAG, so the port goes down with the chip: restarting
    // on the next statement takes the message with it, and what the operator
    // sees is a console that died rather than one that answered. Flush, then
    // leave the host a moment to read it.
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(200));

    // Nothing is quiesced first, deliberately. A `config.json` write
    // interrupted here is the power cut §10.15 already recovers from at boot —
    // temp file, then rename, and `config::Init` closes the window — so there
    // is nothing this could usefully wait for that is not already handled.
    esp_restart();
    return 0;  // not reached: the chip is gone by here
}

int CmdLs(int, char **) {
    size_t count = 0;
    const esp_err_t err = storage::List(entries, kMaxListed, &count);
    if (err != ESP_OK && err != ESP_ERR_INVALID_SIZE) {
        printf("list failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    size_t listed_bytes = 0;
    for (size_t i = 0; i < count; ++i) {
        printf("%8u  %s\n", static_cast<unsigned>(entries[i].size), entries[i].name);
        listed_bytes += entries[i].size;
    }

    if (err == ESP_ERR_INVALID_SIZE) {
        printf("... and more; this console lists at most %u\n",
               static_cast<unsigned>(kMaxListed));
    }

    size_t total = 0;
    size_t used = 0;
    if (storage::Info(&total, &used) == ESP_OK) {
        // The percentage is the number that answers "is this filling up", which
        // neither of the two absolutes does at a glance against an 11 MB
        // partition. Rounded to whole percent, in 64-bit because used * 100
        // does not have to fit in a size_t on a 32-bit target.
        const unsigned percent =
            total == 0 ? 0
                       : static_cast<unsigned>((static_cast<uint64_t>(used) * 100 + total / 2) /
                                               total);
        printf("%u file(s), %u bytes; partition %u of %u bytes used (%u%%)\n",
               static_cast<unsigned>(count), static_cast<unsigned>(listed_bytes),
               static_cast<unsigned>(used), static_cast<unsigned>(total), percent);
    }
    return 0;
}

int CmdCat(int argc, char **argv) {
    if (argc != 2) {
        printf("usage: cat <path>   e.g. cat config.json\n");
        return 1;
    }

    size_t length = 0;
    const esp_err_t err = storage::ReadFile(argv[1], file_buffer, sizeof(file_buffer), &length);

    switch (err) {
        case ESP_OK:
            fwrite(file_buffer, 1, length, stdout);
            if (length > 0 && file_buffer[length - 1] != '\n') {
                printf("\n");
            }
            return 0;
        case ESP_ERR_NOT_FOUND:
            printf("no such file: %s\n", argv[1]);
            return 1;
        case ESP_ERR_INVALID_SIZE:
            printf("%s is %u bytes, the console reads at most %u\n", argv[1],
                   static_cast<unsigned>(length), static_cast<unsigned>(kFileBufferSize - 1));
            return 1;
        case ESP_ERR_INVALID_STATE:
            printf("the storage partition is not mounted\n");
            return 1;
        default:
            printf("read failed: %s\n", esp_err_to_name(err));
            return 1;
    }
}

// `buttons watch` runs for this long unless told otherwise, and no longer than
// the cap — the REPL task is blocked while it runs, so a watch that outlives the
// operator's attention is a console that looks hung.
constexpr uint32_t kWatchDefaultSeconds = 10;
constexpr uint32_t kWatchMaxSeconds = 120;

// **A single poll can never confirm a change.** The debounce promotes a level
// only once it has held for `kDebounceMs` *across* polls, and one call has no
// elapsed time in it — so a one-shot snapshot would print every button as it was
// before the command ran. A button held while `buttons` is typed would come out
// as `released` next to `raw pressed`, and that disagreement is the console's
// fault rather than the wire's, which is precisely the signal the two columns
// exist to carry. So: sample for a little longer than the window first.
void Settle(buttons::Buttons &keys) {
    const int64_t until =
        esp_timer_get_time() + static_cast<int64_t>(buttons::kDebounceMs + 15) * 1000;
    do {
        keys.PollAll();
        vTaskDelay(pdMS_TO_TICKS(buttons::kPollIntervalMs));
    } while (esp_timer_get_time() < until);
}

void PrintButtonRow(buttons::Buttons &keys, size_t index) {
    // Both answers, because they disagree exactly when something is wrong: the
    // raw level is the wire, `Pressed()` is what the debounce believes. A pin
    // stuck low reads pressed in both and held for the whole uptime, which is
    // how a broken button tells itself apart from an idle one.
    printf("%-6s GPIO%-3d %-8s raw %-8s %lu ms in this state\n", keys.Name(index),
           static_cast<int>(keys.Gpio(index)), keys.Pressed(index) ? "pressed" : "released",
           keys.RawPressed(index) ? "pressed" : "released",
           static_cast<unsigned long>(keys.StableMs(index)));
}

int CmdButtons(int argc, char **argv) {
    buttons::Buttons &keys = board::Buttons();
    if (!keys.Ready()) {
        printf("the buttons were not initialised at boot — nothing to read\n");
        return 1;
    }
    if (argc == 1) {
        Settle(keys);
        for (size_t i = 0; i < keys.Count(); ++i) {
            PrintButtonRow(keys, i);
        }
        return 0;
    }

    if (strcmp(argv[1], "watch") != 0 || argc > 3) {
        printf("usage: buttons              the state of the one this board has\n");
        printf("       buttons watch [s]    print edges for a while (default %lu s)\n",
               static_cast<unsigned long>(kWatchDefaultSeconds));
        return 1;
    }

    uint32_t seconds = kWatchDefaultSeconds;
    if (argc == 3) {
        char *end = nullptr;
        const unsigned long parsed = strtoul(argv[2], &end, 10);
        if (end == argv[2] || *end != '\0' || parsed == 0 || parsed > kWatchMaxSeconds) {
            printf("expected 1..%lu seconds, got '%s'\n",
                   static_cast<unsigned long>(kWatchMaxSeconds), argv[2]);
            return 1;
        }
        seconds = static_cast<uint32_t>(parsed);
    }

    printf("watching for %lu s — press BOOT. **Not through a reset**: GPIO0 held\n",
           static_cast<unsigned long>(seconds));
    printf("across one is the ROM download strap, and nothing here would run (§10.1)\n");

    const int64_t started_us = esp_timer_get_time();
    const int64_t deadline_us = started_us + static_cast<int64_t>(seconds) * 1000000;
    // How long a press lasted is the number worth having — a debounce that is
    // too short shows up here as a handful of 30 ms presses where the operator
    // made one. `StableMs` cannot answer it: by the time the release edge is
    // reported, it is timing the release.
    int64_t pressed_at_us[buttons::kMaxButtons] = {};
    for (size_t i = 0; i < keys.Count(); ++i) {
        // A button already down when the watch starts gets its duration counted
        // from here rather than from the epoch — an honest understatement beats
        // "released after 47 minutes".
        pressed_at_us[i] = started_us;
    }
    unsigned edges = 0;
    while (esp_timer_get_time() < deadline_us) {
        for (size_t i = 0; i < keys.Count(); ++i) {
            const buttons::Event event = keys.Poll(i);
            if (event == buttons::Event::kNone) {
                continue;
            }
            ++edges;
            const int64_t now_us = esp_timer_get_time();
            const double at = static_cast<double>(now_us - started_us) / 1000000.0;
            if (event == buttons::Event::kPressed) {
                pressed_at_us[i] = now_us;
                printf("  +%6.2fs  %-6s pressed\n", at, keys.Name(i));
            } else {
                printf("  +%6.2fs  %-6s released after %lu ms\n", at, keys.Name(i),
                       static_cast<unsigned long>((now_us - pressed_at_us[i]) / 1000));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(buttons::kPollIntervalMs));
    }

    printf("done, %u edge(s)\n", edges);
    return 0;
}

// A number field, its bounds, and where it lives. Strings are handled below;
// the Wi-Fi networks are not settable from here — they are a list of pairs and
// belong to `wifi join`, not to a one-line console setter.
//
// **One copy of the list**, because §10.7's four-places rule bites hardest here:
// the setter's own "unknown field" line and the usage block below both have to
// name the same fields, and two hand-kept enumerations of the same six words is
// the drift that rule exists to prevent.
constexpr const char *kSettableFields =
    "led, ledidle, denybutton, touchtimeout, nats, wifi";

int SetConfigField(const char *key, const char *value) {
    config::Data &c = config::Get();

    struct NumberField {
        const char *name;
        long min;
        long max;
        const char *unit;
    };
    constexpr NumberField kNumbers[] = {
        // The two ceilings of §10.17. `led` is what a state that wants a human
        // is allowed to reach; `ledidle` is what the resting breath settles to.
        // **Neither of them chooses a colour** — which colour means what is
        // protocol on this device, not preference, and `led_frames.h` says why.
        {"led", 0, 100, "%"},
        {"ledidle", 0, 100, "%"},
        // How long a request waits for a fingertip (§10.18). Shorter than the
        // hook's own timeout on purpose.
        {"touchtimeout", 1, 600, " s"},
    };

    for (const NumberField &field : kNumbers) {
        if (strcmp(key, field.name) != 0) {
            continue;
        }
        char *end = nullptr;
        const long parsed = strtol(value, &end, 10);
        if (end == value || *end != '\0' || parsed < field.min || parsed > field.max) {
            printf("%s takes %ld..%ld, got '%s'\n", field.name, field.min, field.max, value);
            return 1;
        }
        if (strcmp(key, "led") == 0 || strcmp(key, "ledidle") == 0) {
            if (strcmp(key, "led") == 0) {
                c.led.percent = static_cast<uint8_t>(parsed);
            } else {
                c.led.idle_percent = static_cast<uint8_t>(parsed);
            }
            // **Applied at once, like the zone**, and reaching only the light —
            // the lesson `wifi check` taught, which is that a settings command
            // reaching further than the thing it changed is a settings command
            // people stop making. It is also the only way to judge a brightness:
            // by looking at it.
            led::SetBrightness(c.led.percent, c.led.idle_percent);
        } else {
            c.approval.touch_timeout_seconds = static_cast<uint16_t>(parsed);
            // Nothing to apply: the gate reads it when the next request arrives.
        }
        printf("%s = %ld%s, in memory only — 'config save' writes it to %s\n", field.name, parsed,
               field.unit, config::kPath);
        return 0;
    }

    struct StringField {
        const char *name;
        char *target;
        size_t capacity;
    };
    // **The same check `nats url` makes**, because otherwise this is the way
    // round it — and a URL that will not parse is a bus the device silently
    // never connects to, which looks exactly like a bus that is down. Empty is
    // not a typo: it is how the connection is switched off.
    if (strcmp(key, "nats") == 0 && value[0] != '\0') {
        nats::Endpoint parsed = {};
        if (!nats::ParseUrl(value, &parsed)) {
            printf("'%s' is not an address this can use — 'nats help' has the forms\n", value);
            return 1;
        }
    }

    const StringField strings[] = {
        {"nats", c.nats.url, sizeof(c.nats.url)},
    };

    for (const StringField &field : strings) {
        if (strcmp(key, field.name) != 0) {
            continue;
        }
        if (strlen(value) >= field.capacity) {
            // Refused rather than truncated, the same call `config.cpp` makes
            // when parsing: half a URL fails in a way that looks like the
            // network.
            printf("%s holds %u characters, '%s' is %u\n", field.name,
                   static_cast<unsigned>(field.capacity - 1), value,
                   static_cast<unsigned>(strlen(value)));
            return 1;
        }
        snprintf(field.target, field.capacity, "%s", value);
        const bool cleared = value[0] == '\0';
        if (strcmp(key, "nats") == 0) {
            // The narrowest thing that changed: a new address drops the
            // connection, an unchanged one costs nothing.
            nats::Apply();
        }
        if (cleared) {
            // `nats = , in memory only` is what the general form prints for an
            // empty value, and it reads like a bug. Clearing a field is a
            // deliberate thing to do here — an empty URL is how the bus is
            // switched off — so it gets said in words.
            printf("%s cleared%s, in memory only — 'config save' writes it to %s\n", field.name,
                   strcmp(key, "nats") == 0 ? "; nothing will be connected" : "",
                   config::kPath);
            return 0;
        }
        printf("%s = %s, in memory only — 'config save' writes it to %s\n", field.name, value,
               config::kPath);
        return 0;
    }

    // --- The gate of §10.18 -----------------------------------------------
    //
    // **`requirekey` is gone and saying so is the point** (§10.18). It switched off
    // the security key back when this device signed with a key of its own; there is
    // no such key any more, so there is nothing for it to switch. An operator who
    // types it is holding an old instruction, and "unknown setting" would send them
    // looking for a typo.
    if (strcmp(key, "requirekey") == 0) {
        printf("there is no requirekey any more: the signing key lives inside the\n");
        printf("security key, so a device with none cannot approve OR deny (§10.18).\n");
        return 1;
    }
    if (strcmp(key, "denybutton") == 0) {
        if (strcmp(value, "on") != 0 && strcmp(value, "off") != 0) {
            printf("%s takes on or off, got '%s'\n", key, value);
            return 1;
        }
        const bool on = strcmp(value, "on") == 0;
        {
            c.approval.deny_button = on;
            printf("denybutton = %s — %s\n", value,
                   on ? "a tap on BOOT denies a pending request"
                      : "nothing but a timeout can refuse a request now");
        }
        printf("in memory only — 'config save' writes it to %s\n", config::kPath);
        return 0;
    }

    if (strcmp(key, "wifi") == 0) {
        if (strcmp(value, "on") == 0 || strcmp(value, "off") == 0) {
            c.wifi.active = strcmp(value, "on") == 0;
            printf("wifi = %s, in memory only — 'config save' writes it to %s\n", value,
                   config::kPath);
            return 0;
        }
        printf("wifi takes on or off, got '%s'\n", value);
        return 1;
    }

    printf("unknown field '%s'. settable: %s\n", key, kSettableFields);
    printf("the Wi-Fi networks are a list of ssid/password pairs and are not set from here\n");
    return 1;
}

// One function, reached both by an explicit `config help` and by anything this
// command does not recognise — §10.7's rule, and the reason it is a rule: finding
// out what a command takes should not require typing something wrong first, and
// two copies of a usage block drift.
void PrintConfigUsage() {
    printf("usage: config                        the settings, as the file has them\n");
    printf("       config reload                 re-read the file, discarding edits\n");
    printf("       config save                   write the current settings back\n");
    printf("       config restore                the factory defaults over them\n");
    printf("       config set <field> <value>    %s\n", kSettableFields);
    printf("the networks and the ping targets are lists — 'wifi join', 'wifi check'\n");
    printf("'set' changes memory only; 'save' is what writes %s\n", config::kPath);
}

int CmdConfig(int argc, char **argv) {
    if (argc == 4 && strcmp(argv[1], "set") == 0) {
        return SetConfigField(argv[2], argv[3]);
    }

    if (argc == 2 && (strcmp(argv[1], "help") == 0 || strcmp(argv[1], "-h") == 0 ||
                      strcmp(argv[1], "--help") == 0)) {
        PrintConfigUsage();
        return 0;
    }

    if (argc > 2) {
        // A recognised verb with the wrong number of words is a different
        // mistake from an unrecognised one, and saying so is the difference
        // between "I mistyped the count" and "I mistyped the word".
        if (strcmp(argv[1], "set") == 0) {
            printf("set takes a field and a value, got %d word(s)\n", argc - 2);
        } else {
            printf("no such thing as 'config %s' with %d argument(s)\n", argv[1], argc - 2);
        }
        PrintConfigUsage();
        return 1;
    }

    if (argc == 2) {
        esp_err_t err = ESP_OK;
        const char *what = argv[1];
        if (strcmp(what, "reload") == 0) {
            err = config::Reload();
        } else if (strcmp(what, "save") == 0) {
            err = config::Save();
        } else if (strcmp(what, "restore") == 0) {
            // §10.15's restore, minus the five seconds of holding BOOT at
            // boot. It puts the defaults back over the settings and leaves the
            // registration alone — the whole reason those are two files.
            err = config::Restore();
        } else if (strcmp(what, "set") == 0) {
            printf("set takes a field and a value\n");
            PrintConfigUsage();
            return 1;
        } else {
            printf("no such thing as 'config %s'\n", what);
            PrintConfigUsage();
            return 1;
        }
        if (err != ESP_OK) {
            printf("%s failed: %s\n", what, esp_err_to_name(err));
            return 1;
        }
        printf("%s ok\n", what);
        if (strcmp(what, "save") == 0) {
            return 0;
        }
        // **Nothing is re-applied here any more, and that is the point.** A
        // reload or a restore moves every field at once, and three subsystems are
        // holding copies of some of them — the LED a brightness, the Wi-Fi manager
        // a network list, the bus a URL. That list used to live here, in the one
        // caller there was; there are two now (this and the boot restore), so it
        // lives where it cannot drift: `config::OnChanged`, registered by `main`,
        // called by `Reload` and `Restore` themselves.
    }

    const config::Data &c = config::Get();
    printf("source     %s%s\n", config::kPath, config::Loaded() ? "" : " (built-in defaults)");
    // §10.15's restore, if it happened. **After the boot log has scrolled away**
    // this is the only place left that says so — which is why it is printed every
    // time rather than once. Nothing at all on an ordinary boot.
    if (const char *restored = config::BootRestoreText(); restored != nullptr) {
        printf("boot       %s (BOOT was held)\n", restored);
    }
    printf("wifi       %s, mode %s, %u network(s)\n", c.wifi.active ? "on" : "off",
           c.wifi.mode == config::WifiMode::kAp ? "ap" : "client",
           static_cast<unsigned>(c.wifi.network_count));
    printf("           fallback ap '%s' (%s) on ch %u after %u round(s), window %u s\n",
           c.wifi.ap_ssid, c.wifi.ap_password[0] == '\0' ? "open" : "password set",
           static_cast<unsigned>(c.wifi.ap_channel),
           static_cast<unsigned>(c.wifi.rounds_before_ap),
           static_cast<unsigned>(c.wifi.ap_window_seconds));
    for (uint8_t i = 0; i < c.wifi.network_count; ++i) {
        // **The password is never printed** (§10.15): it is a secret
        // from the moment it is typed, and a console dump is exactly the place
        // it must not turn up. An address is not a secret and is printed.
        printf("           %u. %s (password %s, %s)\n", static_cast<unsigned>(i + 1),
               c.wifi.networks[i].ssid,
               c.wifi.networks[i].password[0] == '\0' ? "not set" : "set",
               c.wifi.networks[i].ip.enabled ? c.wifi.networks[i].ip.address : "dhcp");
    }
    printf("nats       %s\n", c.nats.url);
    printf("led        %u%%, idle %u%%\n", static_cast<unsigned>(c.led.percent),
           static_cast<unsigned>(c.led.idle_percent));
    // The gate (§10.18). There is no line here about whether the key is required,
    // because there is no answer but yes — see `config.h`'s `Approval`.
    printf("approval   deny button %s, %u s to touch\n", c.approval.deny_button ? "on" : "off",
           static_cast<unsigned>(c.approval.touch_timeout_seconds));
    return 0;
}

// **Where a scan lands, and it is a static rather than a local** (§10.14.1): the
// REPL task has 12 KB and `wifi::kMaxScanResults` entries of `ScanResult` is more
// than a comfortable slice of it. There is one scan at a time by construction —
// the command blocks until it has an answer.
wifi::ScanResult scan_results[wifi::kMaxScanResults];

void PrintWifiStatus() {
    const wifimgr::Snapshot snapshot = wifimgr::Get();
    const config::Wifi &settings = config::Get().wifi;

    // **Two answers, side by side, because they are two different facts.**
    // What was asked for, and what is actually happening on the way there —
    // §10.9's whole shape is the difference between them, and a readout that
    // showed one would make a device that is trying look like a device that
    // is broken.
    printf("wanted     %s  (config: wifi %s, mode %s)\n", wifimgr::Name(snapshot.desired),
           settings.active ? "on" : "off",
           settings.mode == config::WifiMode::kAp ? "ap" : "client");

    // **The network clause belongs to the states that are about a network**,
    // and to no others. Printed unconditionally it read `temporary ap
    // 'YOUR_SSID' … round 3 of 2` on a device that had given up on that
    // network two rounds ago — a stale index and a counter past its own limit,
    // both true internally and both nonsense in a readout.
    printf("state      %s", wifimgr::Name(snapshot.state));
    const bool about_a_network = snapshot.state == wifimgr::State::kConnecting ||
                                 snapshot.state == wifimgr::State::kWaiting ||
                                 snapshot.state == wifimgr::State::kOnline;
    if (about_a_network && snapshot.network != wifimgr::kNoNetwork &&
        snapshot.network < settings.network_count) {
        const unsigned rounds = settings.rounds_before_ap == 0 ? 1u : settings.rounds_before_ap;
        printf(" '%s' (network %u of %u, round %u of %u)", settings.networks[snapshot.network].ssid,
               static_cast<unsigned>(snapshot.network + 1),
               static_cast<unsigned>(settings.network_count),
               static_cast<unsigned>(snapshot.round + 1) > rounds
                   ? rounds
                   : static_cast<unsigned>(snapshot.round + 1),
               rounds);
    }
    if (snapshot.state == wifimgr::State::kWaiting) {
        printf(", next attempt in %u ms", static_cast<unsigned>(snapshot.wait_remaining_ms));
    }
    printf("\n");

    if (!snapshot.radio_ready) {
        printf("radio      not started%s%s\n", snapshot.radio_error == ESP_OK ? "" : " — ",
               snapshot.radio_error == ESP_OK ? "" : esp_err_to_name(snapshot.radio_error));
    } else {
        const wifi::Status &radio = snapshot.radio;
        printf("radio      %s, %s", wifi::Radio::Name(radio.mode), wifi::Radio::Name(radio.link));
        if (radio.ssid[0] != '\0') {
            printf(", '%s'", radio.ssid);
        }
        if (radio.link == wifi::Link::kConnected) {
            printf(", rssi %d dBm", static_cast<int>(radio.rssi));
        }
        // Read off the radio, so it is the channel in use rather than the one
        // asked for — and the two differ whenever an access point is up
        // alongside a station, which is a support question waiting to happen.
        if (radio.channel != 0) {
            printf(", ch %u", static_cast<unsigned>(radio.channel));
        }
        if (radio.ip != 0) {
            printf(", ip %u.%u.%u.%u", static_cast<unsigned>(radio.ip & 0xFF),
                   static_cast<unsigned>((radio.ip >> 8) & 0xFF),
                   static_cast<unsigned>((radio.ip >> 16) & 0xFF),
                   static_cast<unsigned>((radio.ip >> 24) & 0xFF));
            // Where the address came from, next to the address: "it has one"
            // and "it has the one that was asked for" are different facts, and
            // a static config that silently did not take looks exactly like a
            // working one until somebody tries to reach the device.
            //
            // **Only as a client.** An access point's own address is neither
            // leased nor configured here — printing `(dhcp)` next to
            // 192.168.4.1 said it came from a server that is in fact this
            // device.
            if (radio.mode == wifi::Mode::kClient) {
                printf(" (%s)", radio.ip_is_static ? "static" : "dhcp");
            }
        }
        printf("\n");
        if (radio.link == wifi::Link::kFailed) {
            printf("last error %s (reason %u)\n", wifi::Radio::Name(radio.failure),
                   static_cast<unsigned>(radio.reason));
        }
    }

    // **Connected and online are two different lines**, because they are two
    // different facts (§10.9): a router with no uplink, a captive portal and a
    // guest network that only allows port 80 all look like a healthy link from
    // here. `unknown` is honest and is what shows before the first round has
    // answered.
    printf("internet   %s", wifimgr::Name(snapshot.internet));
    if (snapshot.internet_last_ok_ms != wifimgr::kNeverSucceeded) {
        printf(", last reply %u s ago", static_cast<unsigned>(snapshot.internet_last_ok_ms / 1000));
    } else if (config::Get().internet.check) {
        printf(", nothing has answered yet");
    }
    if (snapshot.internet_failed_rounds > 0) {
        printf(", %u round(s) failed", static_cast<unsigned>(snapshot.internet_failed_rounds));
    }
    if (!config::Get().internet.check) {
        printf(" (checking is off)");
    } else if (config::Get().internet.target_count == 0) {
        printf(" (nothing to ping)");
    } else if (snapshot.internet_next_probe_ms != wifimgr::kNeverSucceeded) {
        printf(", next in %u s", static_cast<unsigned>(snapshot.internet_next_probe_ms / 1000));
    }
    printf("\n");
    if (config::Get().internet.target_count > 0) {
        printf("           ping");
        for (uint8_t i = 0; i < config::Get().internet.target_count; ++i) {
            printf(" %s%s", config::Get().internet.targets[i],
                   i == snapshot.internet_target ? "*" : "");
        }
        printf(" every %u s\n", static_cast<unsigned>(config::Get().internet.interval_seconds));
    }

    if (settings.network_count == 0) {
        printf("networks   none remembered — 'wifi join <ssid> [password]'\n");
    }
    for (uint8_t i = 0; i < settings.network_count; ++i) {
        const config::StaticIp &ip = settings.networks[i].ip;
        printf("           %u. %-20s %s%s%s\n", static_cast<unsigned>(i + 1),
               settings.networks[i].ssid,
               settings.networks[i].password[0] == '\0' ? "open" : "password set",
               about_a_network && i == snapshot.network ? "  <- current" : "",
               snapshot.auth_failed[i] ? "  (password refused)" : "");
        if (ip.enabled) {
            printf("              %s/%s gw %s%s%s%s%s\n", ip.address, ip.netmask, ip.gateway,
                   ip.dns1[0] != '\0' ? " dns " : "", ip.dns1,
                   ip.dns2[0] != '\0' ? "," : "", ip.dns2);
        }
    }

    printf("fallback   ap '%s' %s on ch %u after %u round(s)", settings.ap_ssid,
           settings.ap_password[0] == '\0' ? "open" : "wpa2",
           static_cast<unsigned>(settings.ap_channel),
           static_cast<unsigned>(settings.rounds_before_ap));
    if (snapshot.state == wifimgr::State::kApWindow) {
        if (snapshot.ap_window_remaining_ms == wifimgr::kHeldOpen) {
            printf(" — up, held open (%u station(s) attached)",
                   static_cast<unsigned>(snapshot.radio.clients));
        } else {
            printf(" — up, %u s left", static_cast<unsigned>(snapshot.ap_window_remaining_ms / 1000));
        }
    }
    printf("\n");
}

int CmdWifiJoin(int argc, char **argv) {
    if (argc < 3 || argc > 4) {
        printf("usage: wifi join <ssid> [password]\n");
        return 1;
    }
    const char *ssid = argv[2];
    const char *password = argc == 4 ? argv[3] : "";
    config::Wifi &settings = config::Get().wifi;

    if (strlen(ssid) >= config::kSsidSize || strlen(password) >= config::kPasswordSize) {
        printf("that ssid or password is longer than 802.11 allows\n");
        return 1;
    }

    // An existing entry is replaced rather than duplicated: the second copy
    // would be a second chance for the same wrong password, spending a round
    // on it each time.
    uint8_t slot = settings.network_count;
    for (uint8_t i = 0; i < settings.network_count; ++i) {
        if (strcmp(settings.networks[i].ssid, ssid) == 0) {
            slot = i;
            break;
        }
    }
    if (slot >= config::kMaxNetworks) {
        printf("all %u slots are taken — 'wifi forget <ssid>' first\n",
               static_cast<unsigned>(config::kMaxNetworks));
        return 1;
    }

    snprintf(settings.networks[slot].ssid, sizeof(settings.networks[slot].ssid), "%s", ssid);
    snprintf(settings.networks[slot].password, sizeof(settings.networks[slot].password), "%s",
             password);
    if (slot == settings.network_count) {
        ++settings.network_count;
    }
    // Joining a network is also asking for the radio to be a client — nobody
    // types this wanting nothing to happen.
    settings.active = true;
    settings.mode = config::WifiMode::kClient;
    wifimgr::Apply();

    printf("network %u is now '%s' (%s), trying it — in memory only, 'config save' writes it to %s\n",
           static_cast<unsigned>(slot + 1), ssid, password[0] == '\0' ? "open" : "password set",
           config::kPath);
    return 0;
}

// `wifi static <n> <address> <netmask> <gateway> [dns1] [dns2]`, or
// `wifi static <n> off`. By network number, because that is how the status
// listing above numbers them and an SSID with a space in it is not something a
// console argument survives.
int CmdWifiStatic(int argc, char **argv) {
    config::Wifi &settings = config::Get().wifi;
    if (argc < 3) {
        printf("usage: wifi static <n> <address> <netmask> <gateway> [dns1] [dns2]\n");
        printf("       wifi static <n> off        go back to DHCP\n");
        return 1;
    }

    char *end = nullptr;
    const long index = strtol(argv[2], &end, 10);
    if (end == argv[2] || *end != '\0' || index < 1 || index > settings.network_count) {
        printf("network %s does not exist — 'wifi' lists them 1..%u\n", argv[2],
               static_cast<unsigned>(settings.network_count));
        return 1;
    }
    config::StaticIp &ip = settings.networks[index - 1].ip;

    if (argc == 4 && strcmp(argv[3], "off") == 0) {
        // The strings are kept, so turning it back on does not mean typing the
        // address again — the same thing `config.cpp` does when it writes the
        // block for a disabled entry.
        ip.enabled = false;
        printf("network %ld is back on DHCP — in memory only, 'config save' writes it to %s\n",
               index, config::kPath);
        wifimgr::Apply();
        return 0;
    }

    if (argc < 6 || argc > 8) {
        printf("usage: wifi static <n> <address> <netmask> <gateway> [dns1] [dns2]\n");
        printf("       wifi static <n> off        go back to DHCP\n");
        return 1;
    }

    // **Every field is parsed before any of it is stored.** §10.7's rule about
    // deciding first and touching nothing until then: half an address written
    // into the config is a network entry that will not connect and will not
    // say why.
    const char *const fields[] = {argv[3], argv[4], argv[5], argc > 6 ? argv[6] : "",
                                  argc > 7 ? argv[7] : ""};
    static const char *const names[] = {"address", "netmask", "gateway", "dns1", "dns2"};
    for (size_t i = 0; i < 5; ++i) {
        if (fields[i][0] == '\0') {
            continue;  // the two DNS entries are optional
        }
        uint32_t parsed = 0;
        if (!config::ParseIpv4(fields[i], &parsed)) {
            printf("%s '%s' is not an IPv4 address (four octets 0..255, no leading zeros)\n",
                   names[i], fields[i]);
            return 1;
        }
        if (strlen(fields[i]) >= config::kIpTextSize) {
            printf("%s is longer than the field holds\n", names[i]);
            return 1;
        }
    }

    snprintf(ip.address, sizeof(ip.address), "%s", fields[0]);
    snprintf(ip.netmask, sizeof(ip.netmask), "%s", fields[1]);
    snprintf(ip.gateway, sizeof(ip.gateway), "%s", fields[2]);
    snprintf(ip.dns1, sizeof(ip.dns1), "%s", fields[3]);
    snprintf(ip.dns2, sizeof(ip.dns2), "%s", fields[4]);
    ip.enabled = true;

    printf("network %ld (%s) is %s/%s gw %s — in memory only, 'config save' writes it to %s\n",
           index, settings.networks[index - 1].ssid, ip.address, ip.netmask, ip.gateway,
           config::kPath);
    // The address only takes effect on the next association, so restart the
    // cycle rather than leaving the operator to wonder why nothing moved.
    wifimgr::Apply();
    return 0;
}

// `wifi check on|off`, or `wifi check <address> [address…]` to replace the
// list. Every other setting has a console setter; without this one the ping
// targets would be the only thing on the device that can only be changed by
// reflashing.
int CmdWifiCheck(int argc, char **argv) {
    config::InternetCheck &net = config::Get().internet;

    if (argc == 2) {
        printf("check      %s, every %u s, %u ms timeout, offline after %u failed round(s)\n",
               net.check ? "on" : "off", static_cast<unsigned>(net.interval_seconds),
               static_cast<unsigned>(net.timeout_ms),
               static_cast<unsigned>(net.failures_before_offline));
        for (uint8_t i = 0; i < net.target_count; ++i) {
            printf("           %s\n", net.targets[i]);
        }
        if (net.target_count == 0) {
            printf("           (no targets — nothing to ping)\n");
        }
        return 0;
    }

    if (argc == 3 && (strcmp(argv[2], "on") == 0 || strcmp(argv[2], "off") == 0)) {
        net.check = strcmp(argv[2], "on") == 0;
        wifimgr::ApplyInternetCheck();
        printf("internet check %s — in memory only, 'config save' writes it to %s\n", argv[2],
               config::kPath);
        return 0;
    }

    const int count = argc - 2;
    if (count > static_cast<int>(config::kMaxProbeTargets)) {
        printf("at most %u addresses\n", static_cast<unsigned>(config::kMaxProbeTargets));
        return 1;
    }

    // Parsed before anything is stored — the same rule `wifi static` follows,
    // and here it matters twice over: a hostname would be a check that can
    // never pass, which reads as an outage that never ends.
    for (int i = 0; i < count; ++i) {
        uint32_t parsed = 0;
        if (!config::ParseIpv4(argv[2 + i], &parsed)) {
            printf("'%s' is not an IPv4 address — these are pinged, so a name cannot work\n",
                   argv[2 + i]);
            return 1;
        }
    }

    for (int i = 0; i < count; ++i) {
        snprintf(net.targets[i], config::kIpTextSize, "%s", argv[2 + i]);
    }
    net.target_count = static_cast<uint8_t>(count);
    net.check = true;
    wifimgr::ApplyInternetCheck();

    printf("checking %d address(es), starting now — in memory only, 'config save' writes it to %s\n",
           count, config::kPath);
    return 0;
}

int CmdWifiForget(int argc, char **argv) {
    if (argc != 3) {
        printf("usage: wifi forget <ssid>\n");
        return 1;
    }
    config::Wifi &settings = config::Get().wifi;
    for (uint8_t i = 0; i < settings.network_count; ++i) {
        if (strcmp(settings.networks[i].ssid, argv[2]) != 0) {
            continue;
        }
        for (uint8_t j = static_cast<uint8_t>(i + 1); j < settings.network_count; ++j) {
            settings.networks[j - 1] = settings.networks[j];
        }
        --settings.network_count;
        settings.networks[settings.network_count] = {};
        wifimgr::Apply();
        printf("'%s' forgotten — in memory only, 'config save' writes it to %s\n", argv[2],
               config::kPath);
        return 0;
    }
    printf("no remembered network called '%s'\n", argv[2]);
    return 1;
}

int CmdWifiScan() {
    // No "turn the radio on first": the driver brings the station up for the
    // scan and puts it back. Asking what is on the air is the question an
    // operator has *before* the radio is anywhere.
    const wifimgr::Snapshot before = wifimgr::Get();
    size_t found = 0;
    const esp_err_t err = wifimgr::Scan(scan_results, wifi::kMaxScanResults, &found);
    if (err != ESP_OK) {
        printf("scan failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    // §10.9: **this chip has one radio and it is 2.4 GHz** — a 5 GHz-only SSID
    // cannot appear here, and saying that on every scan is the difference
    // between "my network is broken" and "my network is on a band this device
    // cannot hear".
    printf("%u network(s) on 2.4 GHz — the ESP32-C6 has no 5 GHz radio, so a\n",
           static_cast<unsigned>(found));
    printf("5 GHz-only SSID is not missing here, it is unhearable\n");
    if (before.state == wifimgr::State::kOff) {
        printf("(the radio was off: brought up for the scan and switched back)\n");
    }
    for (size_t i = 0; i < found; ++i) {
        printf("  %-32s ch %-3u %4d dBm  %s\n", scan_results[i].ssid,
               static_cast<unsigned>(scan_results[i].channel), static_cast<int>(scan_results[i].rssi),
               scan_results[i].secured ? "locked" : "open");
    }
    if (found == wifi::kMaxScanResults) {
        // The same call `ls` makes: a bounded listing that looks complete is
        // worse than a short one that admits it.
        printf("  ...and possibly more — this is the %u the buffer holds\n",
               static_cast<unsigned>(wifi::kMaxScanResults));
    }
    return 0;
}

// The full list, in one place. **`esp_console`'s own `help` prints the `hint`
// string and nothing else**, and that string has to stay short enough to read
// in a column — so it names the verbs and this names the forms. The two got
// out of step once already, with `ping` and `check` reaching the usage text
// and never the registered hint; a reader looking for a command in `help`
// concluded it did not exist.
void PrintWifiUsage() {
    printf("usage: wifi                          what it wants, and what it is doing\n");
    printf("       wifi mode off|client|ap       what it should want\n");
    printf("       wifi join <ssid> [password]   remember a network and try it\n");
    printf("       wifi forget <ssid>            drop one\n");
    printf("       wifi static <n> <address> <netmask> <gateway> [dns1] [dns2]\n");
    printf("       wifi static <n> off           that network goes back to DHCP\n");
    printf("       wifi scan                     what is on the air (2.4 GHz)\n");
    printf("       wifi ping                     is there an internet, right now\n");
    printf("       wifi check                    the internet check and what it pings\n");
    printf("       wifi check on|off             stop or start asking\n");
    printf("       wifi check <address> […]      ping these instead (up to %u)\n",
           static_cast<unsigned>(config::kMaxProbeTargets));
    printf("       wifi retry                    start the cycle again from the top\n");
    printf("none of these write %s — 'config save' does\n", config::kPath);
}

int CmdWifi(int argc, char **argv) {
    if (!wifimgr::Ready()) {
        printf("the wi-fi manager did not start\n");
        return 1;
    }

    if (argc == 1) {
        PrintWifiStatus();
        return 0;
    }

    // Asked for on purpose, so it is not an error. `esp_console`'s `help`
    // cannot show this much, and finding out what a command takes should not
    // require typing something wrong first.
    if (strcmp(argv[1], "help") == 0 || strcmp(argv[1], "-h") == 0 ||
        strcmp(argv[1], "--help") == 0) {
        PrintWifiUsage();
        return 0;
    }

    if (strcmp(argv[1], "scan") == 0) {
        return CmdWifiScan();
    }
    if (strcmp(argv[1], "join") == 0) {
        return CmdWifiJoin(argc, argv);
    }
    if (strcmp(argv[1], "forget") == 0) {
        return CmdWifiForget(argc, argv);
    }
    if (strcmp(argv[1], "static") == 0) {
        return CmdWifiStatic(argc, argv);
    }
    if (strcmp(argv[1], "check") == 0) {
        return CmdWifiCheck(argc, argv);
    }
    if (strcmp(argv[1], "ping") == 0) {
        // Ask now rather than at the next interval, and wait long enough for
        // the round to have run — a command that returns before the answer
        // does is a command people run twice.
        wifimgr::CheckInternetNow();
        const config::InternetCheck &net = config::Get().internet;
        const uint32_t budget_ms =
            static_cast<uint32_t>(net.timeout_ms + 500) * (net.target_count + 1);
        const uint32_t deadline = budget_ms > 15000 ? 15000 : budget_ms;
        for (uint32_t waited = 0; waited < deadline; waited += 100) {
            vTaskDelay(pdMS_TO_TICKS(100));
            const wifimgr::Snapshot snapshot = wifimgr::Get();
            if (!snapshot.radio_ready) {
                break;
            }
            if (snapshot.internet_next_probe_ms > 0 &&
                snapshot.internet_next_probe_ms != wifimgr::kNeverSucceeded) {
                break;  // the round finished and the next one is scheduled
            }
        }
        PrintWifiStatus();
        return 0;
    }
    if (strcmp(argv[1], "retry") == 0) {
        // Start the cycle again from the top, which is what an operator means
        // by "try now": it clears the sticky refusals and the round counter.
        wifimgr::Apply();
        printf("cycle restarted\n");
        return 0;
    }
    if (strcmp(argv[1], "mode") == 0 && argc == 3) {
        config::Wifi &settings = config::Get().wifi;
        if (strcmp(argv[2], "off") == 0) {
            settings.active = false;
        } else if (strcmp(argv[2], "client") == 0) {
            settings.active = true;
            settings.mode = config::WifiMode::kClient;
        } else if (strcmp(argv[2], "ap") == 0) {
            settings.active = true;
            settings.mode = config::WifiMode::kAp;
        } else {
            printf("mode takes off, client or ap; got '%s'\n", argv[2]);
            return 1;
        }
        // The manager reads the desired mode off the config on every pass, so
        // this needs no second call — but saying so beats leaving the reader
        // to wonder where the effect comes from.
        printf("wifi mode = %s, in memory only — 'config save' writes it to %s\n", argv[2],
               config::kPath);
        return 0;
    }

    printf("no such thing as 'wifi %s'\n", argv[1]);
    PrintWifiUsage();
    return 1;
}

// ---------------------------------------------------------------------------
// The bus (§10.3, §10.5)
// ---------------------------------------------------------------------------

// How much of an arriving payload `nats sub` shows. Everything off the bus is
// attacker-shaped (§10.10) — a 4 MB message, a `tool_input` with no
// terminator, control characters that would drive the terminal — so this is a
// bound rather than a preference, and what does not fit is *said* to be
// missing rather than quietly dropped.
constexpr size_t kPayloadPreview = 240;

// Printed from the bus task, not from the REPL: a message arrives when it
// arrives, and the prompt is redrawn by the next thing typed. The same shape
// `ESP_LOG` already has on this console.
void OnBusMessage(const nats::Message &message, void *) {
    printf("\n[%s] %u byte(s)", message.subject, static_cast<unsigned>(message.size));
    if (message.reply[0] != '\0') {
        // The whole of request-reply, and the field §7 answers into.
        printf(", reply-to %s", message.reply);
    }
    printf("\n  ");

    const size_t shown = message.size > kPayloadPreview ? kPayloadPreview : message.size;
    for (size_t i = 0; i < shown; ++i) {
        const unsigned char c = static_cast<unsigned char>(message.data[i]);
        // Anything that is not plainly printable becomes a dot. A subject that
        // anyone on the LAN can publish to (§10.3) is not somewhere to take
        // escape sequences from.
        putchar(c >= 0x20 && c < 0x7F ? static_cast<int>(c) : '.');
    }
    if (message.size > shown) {
        printf("… (%u more)", static_cast<unsigned>(message.size - shown));
    }
    printf("\n");
}

void PrintNatsStatus() {
    if (!nats::Ready()) {
        printf("the bus link did not start\n");
        return;
    }

    const nats::Status link = nats::Get();
    const char *url = config::Get().nats.url;

    // **Two answers, side by side**, for the reason `wifi` prints two: what was
    // asked for, and what is happening on the way there.
    printf("wanted     %s  (config: %s)\n", link.wanted ? "connected" : "disconnected",
           url[0] == '\0' ? "no address set" : url);

    printf("state      %s", nats::Name(link.state));
    if (link.state == nats::State::kConnected) {
        printf(", up for ");
        PrintDuration(link.connected_for_ms);
    } else if (link.next_attempt_ms == 0) {
        printf(", due now");
    } else if (link.next_attempt_ms != nats::kNever) {
        printf(", next attempt in %u ms", static_cast<unsigned>(link.next_attempt_ms));
    }
    printf("\n");

    if (link.configured) {
        printf("server     %s:%u\n", link.endpoint.host,
               static_cast<unsigned>(link.endpoint.port));
    } else {
        printf("server     nothing to connect to — 'nats url nats://<host>[:port]'\n");
    }

    // **A client link, not an internet** — the server is on the LAN, so the
    // ping check has no vote here; and an access point is not a way to reach
    // anything, which is the case worth spelling out rather than leaving as
    // "no network" on a device whose radio is plainly up.
    printf("network    %s\n",
           link.network ? "client link with an address"
                        : "none — this needs a client link, not an access point");

    if (link.last_error != ESP_OK && link.state != nats::State::kConnected) {
        printf("last error %s\n", esp_err_to_name(link.last_error));
    }

    printf("history    %u connect(s), %u drop(s), %u failed attempt(s) since\n",
           static_cast<unsigned>(link.connects), static_cast<unsigned>(link.drops),
           static_cast<unsigned>(link.failures));
    printf("traffic    %" PRIu64 " in / %" PRIu64 " out, %" PRIu64 " / %" PRIu64 " byte(s)\n",
           link.counters.messages_in, link.counters.messages_out, link.counters.bytes_in,
           link.counters.bytes_out);
    // The client library's frames are large enough that this task's stack was
    // sized after an overflow rather than before one, so the margin is on show.
    printf("stack      %u byte(s) never used, of %u\n",
           static_cast<unsigned>(link.stack_low_water),
           static_cast<unsigned>(nats::kTaskStackBytes));

    if (link.subscriptions == 0) {
        printf("subs       none — 'nats sub <subject> [group]'\n");
    }
    for (size_t i = 0; i < nats::kMaxSubscriptions; ++i) {
        nats::SubscriptionRow row = {};
        if (!nats::SubscriptionAt(i, &row)) {
            continue;
        }
        printf("subs       %s%s%s (sid %d)\n", row.subject, row.queue[0] == '\0' ? "" : " in group ",
               row.queue, row.sid);
    }
}

int CmdNatsUrl(int argc, char **argv) {
    if (argc != 3) {
        printf("usage: nats url <nats://host[:port]>\n");
        return 1;
    }
    // Parsed here rather than at the task, so a typo is refused while the person
    // who made it is still looking at the console — `wifi static` makes the same
    // call, and the reason is the same one: lwIP and this parser both have a way
    // of reading a wrong string as *something*.
    nats::Endpoint parsed = {};
    if (!nats::ParseUrl(argv[2], &parsed)) {
        printf("'%s' is not an address this can use\n", argv[2]);
        printf("expected nats://host[:port], host:port or host — no path, no\n");
        printf("credentials, and ws:// / wss:// / tls:// are not wired up\n");
        return 1;
    }

    config::Nats &settings = config::Get().nats;
    if (strlen(argv[2]) >= sizeof(settings.url)) {
        printf("that address is longer than the %u bytes the config field holds\n",
               static_cast<unsigned>(sizeof(settings.url) - 1));
        return 1;
    }
    snprintf(settings.url, sizeof(settings.url), "%s", argv[2]);

    // Applied as it is set, like `volume` and `tz` (§10.15) — and a changed
    // address is one of the few settings that really does invalidate the
    // connection, so this is the narrow call rather than a blanket restart.
    nats::Apply();
    printf("bus = %s:%u, in memory only — 'config save' writes it to %s\n", parsed.host,
           static_cast<unsigned>(parsed.port), config::kPath);
    return 0;
}

int CmdNatsPublish(int argc, char **argv) {
    if (argc < 3) {
        printf("usage: nats pub <subject> [text …]\n");
        return 1;
    }

    // The words after the subject, rejoined with single spaces. A fixed buffer
    // (§10.14.1) and a refusal rather than a truncation: half a payload on a
    // bus is a message somebody has to debug.
    char payload[256] = {};
    size_t used = 0;
    for (int i = 3; i < argc; ++i) {
        const size_t length = strlen(argv[i]);
        if (used + length + 2 > sizeof(payload)) {
            printf("that payload is longer than the %u bytes this command holds\n",
                   static_cast<unsigned>(sizeof(payload) - 1));
            return 1;
        }
        if (used > 0) {
            payload[used++] = ' ';
        }
        memcpy(payload + used, argv[i], length);
        used += length;
    }
    payload[used] = '\0';

    const esp_err_t err = nats::Publish(argv[2], payload, nullptr);
    if (err != ESP_OK) {
        printf("not published: %s\n", esp_err_to_name(err));
        return 1;
    }

    // **Published is not delivered** (§4), which is exactly the confusion this
    // command would otherwise create on a console: the flush is what says the
    // server has it, and it is worth the wait here because a person is
    // watching.
    if (!nats::Flush(2000)) {
        printf("sent %u byte(s) to %s, but the server did not confirm within 2 s\n",
               static_cast<unsigned>(used), argv[2]);
        return 1;
    }
    printf("%u byte(s) to %s, confirmed by the server\n", static_cast<unsigned>(used), argv[2]);
    return 0;
}

int CmdNatsSubscribe(int argc, char **argv) {
    if (argc < 3 || argc > 4) {
        printf("usage: nats sub <subject> [queue group]\n");
        return 1;
    }
    // The queue group is an argument rather than a flag because §6 makes it
    // one: `approvals.*` in the group `approvers` is a different subscription
    // from `approvals.*` on its own, and the difference is who else gets the
    // message.
    const char *queue = argc == 4 ? argv[3] : "";
    const esp_err_t err = nats::Subscribe(argv[2], queue, &OnBusMessage, nullptr);
    if (err != ESP_OK) {
        printf("not subscribed: %s%s\n", esp_err_to_name(err),
               err == ESP_ERR_INVALID_STATE ? " — SUB is a line on the wire, so this needs a"
                                              " connection first"
                                            : "");
        return 1;
    }
    printf("watching %s%s%s — messages print as they arrive\n", argv[2],
           queue[0] == '\0' ? "" : " in group ", queue);
    return 0;
}

void PrintNatsUsage() {
    printf("usage: nats                          where the bus is, and what the link is doing\n");
    printf("       nats connect                  try now, without waiting out the backoff\n");
    printf("       nats disconnect               drop it, and stay off until 'nats connect'\n");
    printf("       nats retry                    drop what is up and start again\n");
    printf("       nats url <nats://host[:port]> point it somewhere else\n");
    printf("       nats sub <subject> [group]    watch a subject; arrivals print themselves\n");
    printf("       nats unsub <subject>          stop watching one\n");
    printf("       nats pub <subject> [text …]   publish, and wait for the server to say so\n");
    printf("none of these write %s — 'config save' does\n", config::kPath);
}

int CmdNats(int argc, char **argv) {
    if (!nats::Ready()) {
        printf("the bus link did not start\n");
        return 1;
    }

    if (argc == 1) {
        PrintNatsStatus();
        return 0;
    }

    if (strcmp(argv[1], "help") == 0 || strcmp(argv[1], "-h") == 0 ||
        strcmp(argv[1], "--help") == 0) {
        PrintNatsUsage();
        return 0;
    }

    if (strcmp(argv[1], "connect") == 0 && argc == 2) {
        // Both halves, because "connect" from a console means both: switch it
        // back on if somebody had switched it off, and do not make them wait
        // out a backoff earned while the server was down.
        nats::SetDesired(true);
        nats::ConnectNow();
        printf("connecting — 'nats' says how it went\n");
        return 0;
    }
    if (strcmp(argv[1], "disconnect") == 0 && argc == 2) {
        nats::SetDesired(false);
        printf("disconnecting, and staying off until 'nats connect' or a reboot\n");
        return 0;
    }
    if (strcmp(argv[1], "retry") == 0 && argc == 2) {
        nats::Restart();
        printf("dropping what is up and starting again\n");
        return 0;
    }
    if (strcmp(argv[1], "url") == 0) {
        return CmdNatsUrl(argc, argv);
    }
    if (strcmp(argv[1], "pub") == 0) {
        return CmdNatsPublish(argc, argv);
    }
    if (strcmp(argv[1], "sub") == 0) {
        return CmdNatsSubscribe(argc, argv);
    }
    if (strcmp(argv[1], "unsub") == 0 && argc == 3) {
        const esp_err_t err = nats::Unsubscribe(argv[2]);
        if (err != ESP_OK) {
            printf("not unsubscribed: %s\n", esp_err_to_name(err));
            return 1;
        }
        printf("stopped watching %s\n", argv[2]);
        return 0;
    }

    printf("no such thing as 'nats %s'\n", argv[1]);
    PrintNatsUsage();
    return 1;
}

void PrintKeysUsage() {
    printf("usage: keys              this device's identity, and whether it can sign\n");
    printf("       keys selftest     run the signature check again, now\n");
    printf("       keys forget now   delete the saved key; a new one is made at the next boot\n");
}

// **What this device is on the bus, and whether it can prove it.** The whole
// value of the readout is that "connected" and "able to answer" are different
// facts, and a device that is one and not the other looks identical from
// outside — the same argument the link indicator makes on the clock.
//
// The public key is printed in full because §10.7 has an operator compare it by
// eye, once, against what the handler prints at startup. The private half is not
// printed, cannot be printed, and does not exist outside RAM.
int CmdKeys(int argc, char **argv) {
    if (argc == 2 && (strcmp(argv[1], "help") == 0 || strcmp(argv[1], "-h") == 0 ||
                      strcmp(argv[1], "--help") == 0)) {
        PrintKeysUsage();
        return 0;
    }

    if (argc == 2 && strcmp(argv[1], "selftest") == 0) {
        const bool library = crypto::SelfTest();
        printf("library    %s\n",
               library ? "passed - signatures match the host's" : "FAILED - do not trust this build");

        // The second half, and it is a different question: the library being
        // right says nothing about the key this particular device derived.
        char proof[crypto::kSignatureB64Size];
        if (!crypto::Ready()) {
            printf("this key   no key to check\n");
            return library ? 0 : 1;
        }
        const bool own = crypto::ProveKey(proof, sizeof proof);
        printf("this key   %s\n", own ? "passed - it signs and its own public key verifies it"
                                      : "FAILED");
        if (own) {
            printf("message    %s\n", crypto::kProofMessage);
            printf("signature  %s\n", proof);
            printf("public key %s\n", crypto::PublicKeyBase64());
        }
        return (library && own) ? 0 : 1;
    }

    // **A confirmation word, because this one cannot be undone from here.** What
    // is lost is a registration, and getting it back means a fresh token minted on
    // the host.
    if (argc >= 2 && strcmp(argv[1], "forget") == 0) {
        if (argc != 3 || strcmp(argv[2], "now") != 0) {
            printf("this deletes the saved signing key. the next boot makes a new one, so this\n");
            printf("device becomes a different responder and needs a new registration token.\n");
            printf("type 'keys forget now' if that is what you want.\n");
            return 1;
        }
        const esp_err_t err = crypto::ForgetStoredSeed();
        if (err != ESP_OK) {
            printf("not forgotten: %s\n", esp_err_to_name(err));
            return 1;
        }
        // Deliberately not a reboot: the key already in RAM keeps working, so
        // nothing breaks mid-session. The change lands at the next boot, and
        // saying so is better than surprising somebody with one.
        printf("forgotten. this session still signs with the old key; the next boot makes a new\n");
        printf("one. nothing was published either way - there is no registration yet.\n");
        return 0;
    }

    if (argc != 1) {
        PrintKeysUsage();
        return 1;
    }

    // **`protocol::kKeyId`, not a literal.** This said `approver-esp32` — the
    // sibling board's id, inherited with the file — which is an identity this
    // device has never registered as, printed by the one command an operator reads
    // to find out what it *is*.
    printf("key id     %s\n", protocol::kKeyId);
    printf("key type   ed25519\n");
    printf("state      %s\n", crypto::StateText());

    if (crypto::Ready()) {
        // The block number only means something on the route that has one, so it
        // is added here rather than baked into the driver's own sentence.
        if (crypto::EfuseBlock() >= 0) {
            printf("source     %s (chip key block %d)\n", crypto::SourceText(),
                   crypto::EfuseBlock());
        } else {
            printf("source     %s\n", crypto::SourceText());
        }
        printf("public key %s\n", crypto::PublicKeyBase64());
    } else {
        printf("source     %s\n", crypto::SourceText());
        printf("public key none\n");
    }

    // **The fallback says so, every time it is asked.** A key in flash and a key
    // that cannot be read at all look identical from outside, and only one of them
    // is what this device set out to have. Saying it once in a boot log is not
    // enough; this is the readout somebody looks at.
    if (crypto::KeyIsInFlash()) {
        printf("            this is the fallback: the key is saved on the device and a flash\n");
        printf("            dump gives it away. burning a key into the chip is what fixes it,\n");
        printf("            and it changes this device's identity when it happens.\n");
    }

    // **The other half of being able to answer**, and it is a separate fact from
    // having a key: a device with a key and no registration is one the handler
    // has never heard of, and its signatures verify against nothing.
    if (registration::Registered()) {
        printf("registered yes, as %s\n", registration::KeyId());
        printf("handler key %s\n", registration::ServerKey());
        printf("            compare that once with what the registration handler printed when\n");
        printf("            it started. after that it is pinned and nobody else can answer.\n");
        const int64_t when = registration::RegisteredTs();
        if (when > 0) {
            // **UTC, and it says so.** This is the handler's own clock as it
            // stamped the reply; there is no clock on this board to render it
            // through (§10.13), so nothing here could turn it into local time
            // even if there were somewhere to show one.
            char stamp[32];
            const time_t seconds = static_cast<time_t>(when);
            std::tm utc = {};
            gmtime_r(&seconds, &utc);
            strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &utc);
            printf("registered %s UTC\n", stamp);
        }
    } else {
        printf("registered no - 'register <token>' is what does it\n");
        printf("handler key none\n");
    }
    return 0;
}

void PrintRegisterUsage() {
    printf("usage: register <token>   register this device with the handler\n");
    printf("\n");
    printf("the token is minted on the host and looks like '%s.<44 characters>'.\n",
           protocol::kKeyId);
    printf("it is one-time: a token that has been used, or has expired, is refused.\n");
}

// §6, over USB, and on this board there is no alternative to argue against: with
// no display and no touch surface (§10.13) the console is the only way to get 50
// characters of base64 into this device.
int CmdRegister(int argc, char **argv) {
    if (argc != 2 || strcmp(argv[1], "help") == 0) {
        PrintRegisterUsage();
        return argc == 2 ? 0 : 1;
    }

    // **The progress line is a decision, not a greeting.** The exchange takes
    // seconds, so a console that says nothing looks hung — but printing
    // "registering as X..." above "that is not a token" is the console announcing
    // an action it never took, which is the rule §10.7 states for `poweroff`
    // arriving here.
    //
    // `ParseToken` is used as a predicate and nothing more: every sentence the
    // operator reads still comes from `Register`, so there is no message living
    // in two places waiting to drift.
    char parsed[protocol::kKeyIdMax + 1];
    if (protocol::ParseToken(argv[1], parsed, sizeof(parsed))) {
        printf("registering as %s...\n", protocol::kKeyId);
    }

    char detail[192];
    const esp_err_t err = registration::Register(argv[1], detail, sizeof(detail));
    printf("%s\n", detail[0] != '\0' ? detail : esp_err_to_name(err));

    if (err == ESP_OK) {
        // The comparison §10.7 asks for, put in front of the operator at the one
        // moment they are looking at both strings.
        printf("\n");
        printf("check that handler key against what the handler printed at startup. it is\n");
        printf("pinned now: a reply signed by any other key will be refused from here on.\n");
    }
    return err == ESP_OK ? 0 : 1;
}

// The registration half of §10.7's `forget`. `keys forget now` is the other one,
// and they are deliberately separate: this costs a token, that costs an identity.
int CmdForget(int argc, char **argv) {
    if (argc != 2 || strcmp(argv[1], "now") != 0) {
        if (!registration::Registered()) {
            printf("this device is not registered - there is nothing to forget.\n");
            return 1;
        }
        printf("this drops the registration and the pinned handler key. the handler will\n");
        printf("still list a key for '%s' that this device can no longer use, so a new\n",
               registration::KeyId());
        printf("token has to be minted before it can register again.\n");
        printf("type 'forget now' if that is what you want.\n");
        return 1;
    }

    const esp_err_t err = registration::Forget();
    if (err != ESP_OK) {
        printf("not forgotten: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("forgotten. this device is unregistered; its signing key is untouched.\n");
    return 0;
}

int CmdTerm(int argc, char **argv) {
    if (argc > 2) {
        printf("usage: term          ask the terminal again, and follow its answer\n");
        printf("       term smart    line editing and history on, regardless\n");
        printf("       term dumb     back to plain lines\n");
        return 1;
    }

    if (argc == 2) {
        if (strcmp(argv[1], "smart") == 0) {
            linenoiseSetDumbMode(0);
            printf("line editing on — up-arrow walks the last %d commands.\n", kHistoryLength);
            printf("if the console goes silent from here, the other end of this port does\n");
            printf("not answer escape sequences; reset the board to get it back.\n");
            return 0;
        }
        if (strcmp(argv[1], "dumb") == 0) {
            linenoiseSetDumbMode(1);
            printf("plain lines; no history, no editing, nothing to hang on\n");
            return 0;
        }
        printf("expected 'smart' or 'dumb', got '%s'\n", argv[1]);
        return 1;
    }

    // The probe the REPL ran at boot, run again now that somebody is here to
    // answer it. It is bounded (500 ms, non-blocking reads), which is what
    // makes this safe to type where forcing the mode is not.
    const bool answered = linenoiseProbe() == 0;
    linenoiseSetDumbMode(!answered);
    if (answered) {
        printf("the terminal answered: line editing and history are on\n");
    } else {
        printf("no answer — staying on plain lines.\n");
        printf("that is the right result for a script driving this port, and the wrong\n");
        printf("one for a terminal that was slow to reply; 'term smart' overrides it.\n");
    }
    return 0;
}

// --- `request` (CLAUDE.md §10.18, §10.10) --------------------------------
//
// What is on the desk, what the gate said about the last few, and a way to put a
// synthetic request there.
//
// **`request test` goes through the whole path and is stopped one step from the
// wire.** It is queued like a real one, it lights the LED like a real one, and it
// asks the key for a fingertip like a real one — and its reply subject is
// `responder::kTestReplySubject`, which the responder refuses to publish into.
// That is the difference between a test that exercises the device and one that
// exercises a mock.
//
// The synthetic request carries a plausible §7 payload because the fields are the
// ones a human reads before deciding: an implausible one would be a test of the
// wrong thing.
int CmdRequest(int argc, char **argv) {
    auto usage = []() {
        printf("usage: request                    what is pending, and the tally\n");
        printf("       request test [seconds]     inject a synthetic request\n");
        printf("       request test <tool> <text …>\n");
    };

    if (argc == 1) {
        const responder::Status now = responder::Get();
        if (!now.ready) {
            printf("responder  not started — see the boot log\n");
            return 1;
        }
        printf("responder  %s\n", now.subscribed
                                      ? "answering approvals.* in the group 'approvers'"
                                      : responder::BlockerText(now.blocked_by));

        // **The three things that have to be true**, spelled out rather than
        // summarised, because "not subscribed" is one word for three different
        // afternoons.
        printf("key        %s\n",
               fido::Enrolled() ? fido::PublicKeyBase64() : "none - run 'key enrol'");
        // **"Registered" is two questions now** (§10.18.1): whether the handler was
        // ever told about this device, and whether what it was told is still the key
        // this device holds. A re-enrolment answers yes to the first and no to the
        // second, and only the second decides whether anything can be approved.
        if (registration::Registered()) {
            printf("registered %s\n", registration::KeyId());
        } else if (registration::RegistrationPresent()) {
            printf("registered STALE - for %s, not the key enrolled now\n",
                   registration::RegisteredPublicKey());
        } else {
            printf("registered no - run 'register <token>'\n");
        }
        printf("gate       %s\n",
               fido::Enrolled()
                   ? (fido::Present() ? "a key is on the port"
                                      : "enrolled, and no key plugged in")
                   : "no key enrolled - run 'key enrol'");

        const responder::PendingView front = responder::Front();
        if (front.present) {
            printf("pending    %s in %s, ", front.tool_name, front.cwd);
            PrintDuration(front.remaining_ms);
            printf(" left");
            if (front.waiting > 0) {
                printf(", +%u waiting", static_cast<unsigned>(front.waiting));
            }
            printf("\n");
        } else {
            printf("pending    nothing\n");
        }

        printf("off wire   %" PRIu32 " received, %" PRIu32 " queued, %" PRIu32 " refused\n",
               now.received, now.queued, now.refused);
        printf("gate said  %" PRIu32 " asked, %" PRIu32 " approved, %" PRIu32
               " button-denied, %" PRIu32 " nothing\n",
               now.gate_asked, now.gate_approved, now.button_denied, now.gate_declined);
        printf("on wire    %" PRIu32 " replied (%" PRIu32 " allow, %" PRIu32 " deny)\n",
               now.replied, now.allowed, now.denied);
        // **Every one of these is a decision a human made that nobody heard**
        // (§10.10). Zero is the number they should all be.
        if (now.sign_failed != 0 || now.publish_failed != 0 || now.stale_dropped != 0 ||
            now.overflowed != 0) {
            printf("lost       %" PRIu32 " unsigned, %" PRIu32 " unpublished, %" PRIu32
                   " stale, %" PRIu32 " overflowed\n",
                   now.sign_failed, now.publish_failed, now.stale_dropped, now.overflowed);
        }
        printf("stack      responder %" PRIu32 " B free, gate %" PRIu32 " B free\n",
               now.stack_low_water, now.gate_stack_low_water);
        return 0;
    }

    if (strcmp(argv[1], "test") != 0) {
        usage();
        return 1;
    }

    // `request test 20` is a duration; `request test Bash rm -rf /tmp/x` is a
    // tool and a command. They are told apart by whether the second word is a
    // number, which is the same rule `buttons watch` follows.
    uint32_t seconds = 30;
    const char *tool = "Bash";
    char text[256] = "rm -rf /tmp/scratch";

    if (argc >= 3) {
        char *end = nullptr;
        const unsigned long parsed = strtoul(argv[2], &end, 10);
        if (end != argv[2] && *end == '\0' && argc == 3) {
            if (parsed == 0 || parsed > 600) {
                printf("expected 1..600 seconds, got '%s'\n", argv[2]);
                return 1;
            }
            seconds = static_cast<uint32_t>(parsed);
        } else {
            tool = argv[2];
            size_t used = 0;
            text[0] = '\0';
            for (int i = 3; i < argc && used + 1 < sizeof(text); ++i) {
                const int written =
                    snprintf(text + used, sizeof(text) - used, "%s%s", used == 0 ? "" : " ",
                             argv[i]);
                if (written <= 0) {
                    break;
                }
                used += static_cast<size_t>(written);
            }
        }
    }

    if (!responder::InjectTestRequest(tool, text, seconds * 1000)) {
        printf("the queue refused it — %u already waiting?\n",
               static_cast<unsigned>(ui::RequestCard::kMaxPending));
        return 1;
    }

    printf("queued: %s, %" PRIu32 " s\n", tool, seconds);
    printf("touch the key on the OTG port to allow%s\n",
           config::Get().approval.deny_button ? ", or tap BOOT to deny and touch it to sign that"
                                              : "");
    printf("nothing will be published — this request's reply subject is the test one\n");
    return 0;
}

// --- `led` (CLAUDE.md §10.17) --------------------------------------------
//
// **The one output this device has, and therefore the one that has to be
// checkable from a script.** The question is "is the light saying what the device
// is", and it has two halves: what the ranking decided, and what the emitter is
// actually being driven with.
//
// `led test` walks every state's look in turn. It does **not** change what the
// device is — the ranking is untouched and the next tick puts the real colour
// back — so it is safe to run with a request pending, which is exactly when
// somebody wants to know what a colour looks like.
const char *ColourName(led::Rgb colour) {
    struct Named {
        led::Rgb value;
        const char *name;
    };
    static const Named kNamed[] = {
        {led::colour::kOff, "off"},       {led::colour::kWhite, "white"},
        {led::colour::kRed, "red"},       {led::colour::kGreen, "green"},
        {led::colour::kBlue, "blue"},     {led::colour::kAmber, "amber"},
        {led::colour::kYellow, "yellow"}, {led::colour::kCyan, "cyan"},
        {led::colour::kMagenta, "magenta"},
    };
    for (const Named &named : kNamed) {
        if (named.value == colour) {
            return named.name;
        }
    }
    return "?";
}

int CmdLed(int argc, char **argv) {
    if (argc == 1) {
        const led::Status light = led::Get();
        const indicator::State state = indicator::Current();

        if (!light.ready) {
            printf("led        not running — the UART did not come up\n");
        }
        // **What the device is, then what the light is doing about it.** The two
        // lines are separate on purpose: a ranking that says `ready` next to an
        // emitter showing red is a bug you can only see if both are printed.
        printf("state      %s — %s\n", indicator::StateName(state), indicator::StateText(state));
        printf("showing    %s, %s, %u%%%s\n", ColourName(light.colour),
               led::EffectName(light.effect), static_cast<unsigned>(light.percent),
               light.overriding ? " (a verdict flash is up)" : "");
        printf("rgb        %u,%u,%u\n", static_cast<unsigned>(light.colour.r),
               static_cast<unsigned>(light.colour.g), static_cast<unsigned>(light.colour.b));
        printf("ceilings   %u%% normal, %u%% idle\n", static_cast<unsigned>(light.percent),
               static_cast<unsigned>(light.idle_percent));
        printf("wire       %" PRIu32 " writes, %" PRIu32 " failed\n", light.writes,
               light.write_failures);
        printf("changes    %" PRIu32 " state transitions since boot\n", indicator::Transitions());
        printf("stack      %" PRIu32 " B free\n", light.stack_low_water);
        return 0;
    }

    if (argc == 2 && strcmp(argv[1], "test") == 0) {
        // Every state, in the order `Decide` ranks them, so that what the walk
        // shows is also the order the device would prefer them in.
        const indicator::State kAll[] = {
            indicator::State::kBooting,     indicator::State::kRestoreWindow,
            indicator::State::kSigning,     indicator::State::kPending,
            indicator::State::kFault,       indicator::State::kNoStorage,
            indicator::State::kNoDeviceKey, indicator::State::kNoWifi,
            indicator::State::kNoInternet,  indicator::State::kNoBus,
            indicator::State::kNotRegistered, indicator::State::kNotEnrolled,
            indicator::State::kNoFidoKey,   indicator::State::kWatching,
            indicator::State::kReady,
        };
        printf("walking %u states, 1.5 s each — the device is unaffected\n",
               static_cast<unsigned>(sizeof(kAll) / sizeof(kAll[0])));
        for (const indicator::State state : kAll) {
            const indicator::Look look = indicator::LookOf(state);
            printf("  %-16s %-8s %s\n", indicator::StateName(state), ColourName(look.colour),
                   led::EffectName(look.effect));
            // Through `SetFor` rather than `Set`, which is what makes this safe:
            // an override expires by itself and the ranking underneath is never
            // touched.
            led::SetFor(look.colour, look.effect, 1500);
            vTaskDelay(pdMS_TO_TICKS(1500));
        }
        printf("done — the light is back to whatever the device actually is\n");
        return 0;
    }

    printf("usage: led           what the device is, and what the light is doing\n");
    printf("       led test      walk every state's colour, 1.5 s each\n");
    printf("the two brightness ceilings are 'config set led' and 'config set ledidle';\n");
    printf("which colour means what is compiled in and is not a setting (§10.17)\n");
    return 1;
}

// --- `key` (CLAUDE.md §10.18) --------------------------------------------
//
// The security key on the OTG port: what is plugged in, what this device was
// enrolled against, and the two verbs that change that.
//
// **`key test` asks for a real assertion over a random challenge.** It needs a
// touch, it verifies the answer against the enrolled public key, and it approves
// nothing — which makes it the one command that can answer "would this device
// approve, if it were asked" without asking it.
void PrintKeyUsage() {
    printf("usage: key                what is on the port, and what is enrolled\n");
    printf("       key info           ask the key what it can do\n");
    printf("       key enrol          make this device's signing key (needs a touch)\n");
    printf("       key test           ask for an assertion (needs a touch, approves nothing)\n");
    printf("       key selftest       check the derivation on this chip — no key needed\n");
    printf("       key forget now     delete %s; the credential stays on the key\n", fido::kPath);
}

// **The cause, not the layer it travelled through.** A key that answers a request
// with a refusal leaves the transport at `Fault::kNone` — whose name is `"ok"` — so
// printing the fault first put the word `ok` on a line reporting a failure and left
// the CTAP status, which held the whole answer, looking like an aside. When the key
// spoke, what it said *is* the cause; the transport gets to explain only the
// failures it caused itself.
//
// `kErrOperationDenied` earns a second line because it is the one an operator will
// actually meet, and its spec wording does not say what happened: it is what a key
// returns when the touch never came, and "operation denied" reads like a device
// that refused rather than a human who was not there (§10.10 rule 2 — that is a
// nothing, not a deny).
// How long the two console commands that need a fingertip wait for one, and so how
// long their prompt is shown. It matches the `30000` they pass to `fido::Sign` and
// `fido::Enrol` — a prompt that outlived the wait would be asking for a touch
// nothing is listening for.
inline constexpr uint32_t kConsoleTouchMs = 30000;

void PrintKeyFailure(fido::usb::Fault fault, uint8_t status) {
    if (status != 0) {
        printf("the key said no — %s (CTAP %02x)\n", ctap2::StatusName(status), status);
        if (status == ctap2::kErrOperationDenied) {
            printf("           the usual cause is that nobody touched it in time\n");
        }
        return;
    }
    printf("%s\n", fido::usb::FaultName(fault));
}

int CmdKey(int argc, char **argv) {
    if (argc == 1) {
        if (!fido::Ready()) {
            printf("usb        the host did not start — see the boot log\n");
        }
        const fido::usb::DeviceInfo device = fido::Device();
        if (device.present) {
            printf("plugged    %04x:%04x %s%s%s\n", device.vendor_id, device.product_id,
                   device.manufacturer, device.manufacturer[0] != 0 ? " " : "", device.product);
            printf("interface  %u, in 0x%02x, out 0x%02x\n",
                   static_cast<unsigned>(device.interface_number),
                   static_cast<unsigned>(device.endpoint_in),
                   static_cast<unsigned>(device.endpoint_out));
        } else {
            printf("plugged    nothing (or nothing with a FIDO interface)\n");
        }

        if (fido::Enrolled()) {
            const fido::Enrolment &e = fido::Current();
            printf("enrolled   %04x:%04x, aaguid %s\n", e.vendor_id, e.product_id, e.aaguid);
            printf("credential %u bytes, public key %u bytes\n",
                   static_cast<unsigned>(e.credential_id_length),
                   static_cast<unsigned>(sizeof(e.public_key)));
            printf("rp id      %s\n", ctap2::kRelyingPartyId);
        } else {
            printf("enrolled   no — run 'key enrol' with a key plugged in\n");
        }

        // The key this device answers as, which is the string the handler's allowlist
        // has to contain (§10.2). Printed here because comparing the two by eye is
        // how a stale registration gets found in seconds.
        if (fido::Enrolled()) {
            printf("signs as   %s (p256)\n", fido::PublicKeyBase64());
        }

        const fido::Stats gate = fido::GetStats();
        printf("gate       %" PRIu32 " asked, %" PRIu32 " approved, %" PRIu32 " timed out\n",
               gate.asked, gate.approved, gate.timed_out);
        if (gate.bad_signature != 0 || gate.wrong_key != 0) {
            // **The two that are never routine.** A bad signature is either the
            // wrong key, a bug here, or something pretending to be a key.
            printf("suspicious %" PRIu32 " did not verify, %" PRIu32 " were the wrong credential\n",
                   gate.bad_signature, gate.wrong_key);
        }

        const fido::usb::Stats cable = fido::usb::GetStats();
        printf("cable      %" PRIu32 " attached, %" PRIu32 " detached, %" PRIu32 " claimed, %" PRIu32
               " rejected\n",
               cable.attached, cable.detached, cable.claimed, cable.rejected);
        printf("exchanges  %" PRIu32 " total, %" PRIu32 " timed out, %" PRIu32 " framing, %" PRIu32
               " key errors, %" PRIu32 " transfer\n",
               cable.exchanges, cable.timeouts, cable.protocol_errors, cable.key_errors,
               cable.transfer_errors);
        return 0;
    }

    if (argc == 2 && strcmp(argv[1], "info") == 0) {
        ctap2::Info info;
        fido::usb::Fault fault = fido::usb::Fault::kNone;
        uint8_t status = 0;
        const esp_err_t err = fido::Info(&info, &fault, &status);
        if (err != ESP_OK) {
            printf("no answer: ");
            PrintKeyFailure(fault, status);
            return 1;
        }
        printf("versions   %s%s%s\n", info.fido21 ? "FIDO_2_1 " : (info.fido2 ? "FIDO_2_0 " : ""),
               info.u2f ? "U2F_V2" : "", (!info.fido2 && !info.u2f) ? "none this device knows" : "");
        printf("options    rk %s, up %s, uv %s\n", info.option_rk ? "yes" : "no",
               info.option_up ? "yes" : "no", info.option_uv ? "yes" : "no");
        // **The line that decides whether this key is usable here at all**
        // (§10.18). Printed next to the PIN because it is the harder no: a PIN can
        // be removed, and a firmware without `previewSign` cannot be talked into
        // having it.
        printf("previewSign %s\n",
               info.sign_extension ? "yes — this key can be enrolled"
                                   : "no — this key cannot be enrolled here (§10.18)");
        // **A PIN is the one thing that would stop this working** (§10.18): this
        // device has one button and cannot enter one, so a key that insists on
        // one is a key it cannot use. Said here rather than discovered mid-approval.
        printf("pin        %s%s\n", info.client_pin_set ? "set" : "not set",
               info.client_pin_set ? " — this device cannot enter one; enrolment may refuse"
                                   : "");
        if (info.aaguid != nullptr) {
            printf("aaguid     ");
            for (int i = 0; i < 16; ++i) {
                printf("%02x", info.aaguid[i]);
            }
            printf("\n");
        }
        if (info.max_message_size != 0) {
            printf("maxmsg     %" PRIu64 " bytes\n", info.max_message_size);
        }
        return 0;
    }

    if (argc == 2 && strcmp(argv[1], "enrol") == 0) {
        if (!fido::Present()) {
            printf("nothing on the OTG port\n");
            return 1;
        }
        // **Asked before anything is spent.** A key without `previewSign` answers
        // a `makeCredential` with a CTAP status that names no cause, which reads
        // from the desk like a device that is broken rather than a key that is the
        // wrong one. `getInfo` costs no touch, so the refusal is free — and a key
        // that will not even answer `getInfo` is left to the enrolment to report,
        // because that is a cable problem and not a wrong key.
        {
            ctap2::Info advertised;
            fido::usb::Fault info_fault = fido::usb::Fault::kNone;
            uint8_t info_status = 0;
            if (fido::Info(&advertised, &info_fault, &info_status) == ESP_OK &&
                !advertised.sign_extension) {
                printf("this key does not advertise previewSign, so it cannot be enrolled here.\n");
                printf("the signing key is derived from that extension (§10.18) — without it\n");
                printf("there is nothing to derive from. `key info` lists what this key does.\n");
                return 1;
            }
        }
        if (fido::Enrolled()) {
            printf("this device is already enrolled — enrolling again replaces %s,\n", fido::kPath);
            printf("and the old credential stays on whatever key holds it.\n");
        }
        printf("touch the key… (the light is blue and fast while it waits)\n");
        fflush(stdout);
        fido::usb::Fault fault = fido::usb::Fault::kNone;
        uint8_t status = 0;
        indicator::ShowTouchPrompt(kConsoleTouchMs);
        const esp_err_t err = fido::Enrol(30000, &fault, &status);
        indicator::EndTouchPrompt();
        if (err != ESP_OK) {
            printf("not enrolled: ");
            PrintKeyFailure(fault, status);
            return 1;
        }
        const fido::Enrolment &e = fido::Current();
        printf("enrolled on %04x:%04x, aaguid %s, credential %u bytes\n", e.vendor_id,
               e.product_id, e.aaguid, static_cast<unsigned>(e.credential_id_length));
        printf("written to %s\n", fido::kPath);
        // **The two lines somebody has to act on.** The signing key is new, so
        // whatever the handler has in its allowlist is now worthless: this device
        // cannot be verified until it registers again (§10.18.1). Printed rather
        // than implied, because a device that looks enrolled and is refused on every
        // approval is the hardest failure here to read from the outside.
        printf("signs as   %s (p256)\n", fido::PublicKeyBase64());
        printf("this is a NEW key — the registration is stale. run 'register <token>'\n");
        printf("with a fresh token from the handler before this device can answer.\n");
        return 0;
    }

    if (argc == 2 && strcmp(argv[1], "test") == 0) {
        if (!fido::Enrolled()) {
            printf("nothing to test — run 'key enrol' first\n");
            return 1;
        }
        // A random challenge, not a request's. **Nothing this produces can
        // become a verdict**: the assertion is verified and thrown away, which
        // is what makes this safe to type at any moment.
        uint8_t challenge[32];
        esp_fill_random(challenge, sizeof(challenge));

        printf("touch the key… (the light is blue and fast while it waits)\n");
        fflush(stdout);
        fido::usb::Fault fault = fido::usb::Fault::kNone;
        uint8_t status = 0;
        const int64_t started = esp_timer_get_time();
        uint8_t signature[ctap2::kMaxSignatureSize];
        size_t signature_length = 0;
        // **The console is not where somebody is looking** (§10.17). A real request
        // has `pending` for this; a command typed into a serial port had nothing,
        // and guessing when to put a finger on the key is how a wait that ends in
        // `CTAP 0x27` gets read as a broken device.
        indicator::ShowTouchPrompt(kConsoleTouchMs);
        const fido::Gate gate = fido::Sign(challenge, 30000, nullptr, nullptr, signature,
                                           sizeof(signature), &signature_length, &fault, &status);
        indicator::EndTouchPrompt();
        const int64_t took_ms = (esp_timer_get_time() - started) / 1000;

        printf("%s — %s\n", fido::GateName(gate), fido::GateText(gate));
        if (gate != fido::Gate::kApproved) {
            printf("transport  ");
            PrintKeyFailure(fault, status);
            return 1;
        }
        // **This is the whole approval path except the request.** The key made a
        // signature over a challenge, and it verified against the public key this
        // device registered — which is exactly what a verdict is, minus any bytes
        // that mean anything.
        char encoded[ctap2::kMaxSignatureSize * 2 + 4] = {};
        crypto::Base64Encode(signature, signature_length, encoded, sizeof(encoded));
        printf("signed and verified in %lld ms, %u bytes: %.12s…\n",
               static_cast<long long>(took_ms), static_cast<unsigned>(signature_length), encoded);
        printf("nothing was approved: this challenge belongs to no request, so the\n");
        printf("bytes it signed are not a decision about anything (§10.18).\n");
        return 0;
    }

    if (argc == 2 && strcmp(argv[1], "selftest") == 0) {
        // **The half of §10.18 that can be checked with nothing plugged in.** The
        // pure steps of the derivation are host-tested against Python's own numbers
        // (§10.11 tier 2); the ECDH and the point addition need the chip, and this
        // is where they get run — against the same vector, on this silicon.
        //
        // A failure here means no security key would ever have worked: the device
        // would register a public key whose private half the authenticator cannot
        // reconstruct, every reply would be rejected by the hook, and from the desk
        // that looks exactly like a device that is not answering.
        char detail[128] = {};
        const int64_t started = esp_timer_get_time();
        const bool ok = fido::SelfTest(detail, sizeof(detail));
        const int64_t took_ms = (esp_timer_get_time() - started) / 1000;
        printf("%s (%lld ms)\n", detail, static_cast<long long>(took_ms));
        if (!ok) {
            printf("this chip cannot derive the key it is supposed to. nothing this\n");
            printf("device signs would ever verify — do not register it.\n");
            return 1;
        }
        return 0;
    }

    if (argc == 3 && strcmp(argv[1], "forget") == 0 && strcmp(argv[2], "now") == 0) {
        const esp_err_t err = fido::Forget();
        if (err != ESP_OK) {
            printf("not forgotten: %s\n", esp_err_to_name(err));
            return 1;
        }
        printf("%s deleted. this device can no longer approve anything until\n", fido::kPath);
        printf("'key enrol' runs again. the credential is still on the key and\n");
        printf("this firmware has no way to remove it — a key's own manager does.\n");
        return 0;
    }

    if (argc >= 2 && strcmp(argv[1], "forget") == 0) {
        // The same confirmation `forget` uses, and for the same reason: this one
        // costs a touch and a walk to wherever the key is.
        printf("that would leave this device unable to approve anything.\n");
        printf("type 'key forget now' if that is what you want.\n");
        return 1;
    }

    PrintKeyUsage();
    return 1;
}


// `devstatus` (CLAUDE.md §10.7) — every readout at once, in the order in which
// one of them being wrong stops the next from working.
//
// **It exists so there is one thing to paste into a bug report.** It is split out
// as a function the rest of the firmware can call, because §10.7's four-places
// rule is about copies of a readout rather than about how many callers there are.
int CmdDevStatus(int argc, char **) {
    if (argc != 1) {
        printf("usage: devstatus     the board, the light, the key and the network\n");
        return 1;
    }

    struct Section {
        const char *name;
        int (*run)(int, char **);
    };
    static const Section kSections[] = {
        {"status", &CmdStatus},
        // The two pieces of hardware this board has.
        {"buttons", &CmdButtons},
        {"led", &CmdLed},
        // Then the approval loop, and the key that gates it.
        {"request", &CmdRequest},
        {"key", &CmdKey},
        {"wifi", &CmdWifi},
        // And the bus after the network that carries it.
        {"nats", &CmdNats},
        // And the identity last, because it is the question the two above lead
        // to: a socket that is open and a key that cannot sign are the same
        // silence from the other end.
        {"keys", &CmdKeys},
    };

    bool first = true;
    for (const Section &section : kSections) {
        if (!first) {
            printf("\n");
        }
        first = false;
        printf("== %s\n", section.name);
        section.run(1, nullptr);
    }
    return 0;
}


// **The table, and the order in it is the order `help` prints.** Grouped the way
// somebody learning this device would want them: what it is, what it is doing
// about approvals, the two pieces of hardware it has, the settings, the network,
// the bus, the identity, and the filesystem last.
//
// Sixteen of them; the file's opening comment says what there is no command for
// and why.
const esp_console_cmd_t kCommands[] = {
    {
        .command = "devstatus",
        .help = "everything at once: the board, the light, the key and the network",
        .hint = nullptr,
        .func = &CmdDevStatus,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "status",
        .help = "firmware, IDF and chip versions, running slot, uptime, heap, storage",
        .hint = nullptr,
        .func = &CmdStatus,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "request",
        .help = "the approval loop: what is pending, the tally, or a synthetic request",
        .hint = "[test [seconds|<tool> <text>]]",
        .func = &CmdRequest,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "led",
        .help = "what the device is, what the light is showing, and a walk through the palette",
        .hint = "[test]",
        .func = &CmdLed,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "key",
        .help = "the security key on the OTG port: what is plugged in, enrol it, test it, forget it",
        .hint = "[info|enrol|test|selftest|forget now]",
        .func = &CmdKey,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "buttons",
        .help = "the BOOT button: state now, or watch it and print edges",
        .hint = "[watch [seconds]]",
        .func = &CmdButtons,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "config",
        .help = "the settings file: print it, set a field, reload / save / restore it",
        .hint = "[reload|save|restore|set] — 'config help' for the forms",
        .func = &CmdConfig,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "wifi",
        .help = "the radio: what it wants, what it is doing, join / forget / scan / static / check",
        .hint = "[mode|join|forget|static|scan|ping|check|retry]",
        .func = &CmdWifi,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "nats",
        .help = "the bus: connect / disconnect / retry, set the url, sub / unsub / pub",
        .hint = "[connect|disconnect|retry|url|sub|unsub|pub]",
        .func = &CmdNats,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "keys",
        .help = "this device's own signing key: id, public key, self-test, or forget it",
        .hint = "[selftest|forget now]",
        .func = &CmdKeys,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "register",
        .help = "register with the handler using a one-time token",
        .hint = "<token>",
        .func = &CmdRegister,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "forget",
        .help = "delete registration.json; the signing key is untouched",
        .hint = "now",
        .func = &CmdForget,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "term",
        .help = "ask the terminal whether it does line editing, or force the answer",
        .hint = "[smart|dumb]",
        .func = &CmdTerm,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "reboot",
        .help = "restart now; anything set and not saved is lost",
        .hint = nullptr,
        .func = &CmdReboot,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "ls",
        .help = "what is on the storage partition, with sizes",
        .hint = nullptr,
        .func = &CmdLs,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "cat",
        .help = "print a file off the storage partition",
        .hint = "<path>",
        .func = &CmdCat,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
};

}  // namespace

void PrintDevStatus() { CmdDevStatus(1, nullptr); }

esp_err_t Init() {
    esp_console_repl_t *repl = nullptr;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "approver>";
    repl_config.max_cmdline_length = 256;
    repl_config.max_history_len = kHistoryLength;

    // **Room for a signature, because one command runs one.** `keys selftest`
    // reaches `crypto_sign`, which needs 4,112 bytes of stack on its own; the
    // REPL's default 4 KB is less than that, and the first version of that
    // command panicked here with a stack protection fault rather than printing
    // an answer. The console is not where a *decision* gets signed — that
    // belongs to a task of its own — but a self-test the operator can run is
    // worth the 8 KB this costs, and the number is `crypto::kSignStackBytes`
    // plus what the deepest ordinary command already uses.
    repl_config.task_stack_size = crypto::kSignStackBytes + 4096;

    // **UART, not USB Serial/JTAG, and this is the one line where this file most
    // differs from the sibling board's** (§10.1, §10.18.4). That board has one
    // USB-C wired to the chip's own USB peripheral, so its console has to live
    // there. This one has two: a CH343P bridge on UART0, and the native USB —
    // which is a *host* here, with a security key on the end of it.
    //
    // A console on USB Serial/JTAG would mean choosing between talking to the
    // device and using a key on it. `sdkconfig.defaults` sets
    // `CONFIG_ESP_CONSOLE_UART_DEFAULT` for the same reason, and the two have to
    // agree: the config decides where the *logs* go and this decides where the
    // *prompt* goes, and a device with those split across two ports is one nobody
    // can debug.
    esp_console_dev_uart_config_t dev_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    dev_config.channel = board::console::kUart;

    esp_err_t err = esp_console_new_repl_uart(&dev_config, &repl_config, &repl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "console init failed: %s", esp_err_to_name(err));
        return err;
    }

    // **History exists; the editor that reaches it is switched off, and `term`
    // is how it gets switched on.** `esp_console` already keeps the last
    // `max_history_len` lines and adds every line typed, so up-arrow costs
    // nothing to have. What disables it is `linenoiseProbe()`, run once inside
    // the call above: it asks the terminal to identify itself, and on USB
    // bridge, nobody is attached that early — the host opens the port seconds
    // later — so it times out and linenoise latches dumb mode for the rest of
    // the session, whoever attaches afterwards.
    //
    // **This is inherited from the sibling board and it may well not apply
    // here.** There, the port did not exist until a host enumerated it; here the
    // CH343P bridge is a real UART that exists from power-on, so the probe may
    // simply be answered. It is left as it is because the failure it guards
    // against — a blocked prompt on a port whose other end does not speak escape
    // sequences — is much worse than an extra `term`, and because the honest
    // answer is that nobody has measured it on this board yet. §10.11's device
    // tier is where that gets checked.
    //
    // Overruling that at boot was tried and is worse than the problem: with
    // dumb mode off, linenoise asks for the cursor position before printing
    // each prompt and **blocks** reading the answer, so a port whose other end
    // does not speak escape sequences goes silent until the board is reset.
    // Measured on this board, not feared in the abstract.
    //
    // So the mode is a command, taken when there is someone there to answer.

    ESP_ERROR_CHECK(esp_console_register_help_command());
    for (const esp_console_cmd_t &cmd : kCommands) {
        err = esp_console_cmd_register(&cmd);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "'%s' not registered: %s", cmd.command, esp_err_to_name(err));
            return err;
        }
    }

    err = esp_console_start_repl(repl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "repl did not start: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "console on UART%d (the CH343P bridge) — type 'help'",
             static_cast<int>(board::console::kUart));
    return ESP_OK;
}

}  // namespace console
