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
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "linenoise/linenoise.h"
#include "approval.h"
#include "registrar.h"
#include "registration.h"
#include "responder.h"
#include "mbedtls/base64.h"
#include "lvgl_display.h"
#include "nats_link.h"
#include "qmi8658.h"
#include "request_card.h"
#include "screens.h"
#include "speaker.h"
#include "storage.h"
#include "timesync.h"
#include "timezone.h"
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
void PrintDuration(uint32_t ms) {
    const uint32_t seconds = ms / 1000;
    if (seconds < 90) {
        printf("%u s", static_cast<unsigned>(seconds));
        return;
    }
    const uint32_t minutes = seconds / 60;
    if (minutes < 90) {
        printf("%u m", static_cast<unsigned>(minutes));
        return;
    }
    printf("%uh %02um", static_cast<unsigned>(minutes / 60),
           static_cast<unsigned>(minutes % 60));
}

// Where the clock's time actually comes from, which is a different question
// from what time it is — and the one nobody can answer by looking at a clock
// face. Whether syncing is on, when it last worked, **how far it moved the
// clock** (a device stepped by four seconds every time has an RTC to be
// suspicious of, and nothing else here would ever say so), and when the next
// one is due.
void PrintSyncStatus() {
    if (!timesync::Ready()) {
        printf("sync       not running\n");
        return;
    }

    const timesync::Status sync = timesync::Get();
    const config::Time &settings = config::Get().time;

    // What the setting is.
    if (!sync.enabled) {
        printf("sync       off%s\n",
               settings.sntp_server[0] == '\0' ? " — no server set" : " — sync hours is 0");
    } else {
        printf("sync       %s every %u h%s\n", settings.sntp_server,
               static_cast<unsigned>(settings.sync_hours), sync.syncing ? ", asking now" : "");
    }

    // **When it last heard from a server, printed whether or not syncing is
    // still on.** That is a fact about the time above rather than about the
    // schedule, and switching the schedule off afterwards does not unmake it —
    // it makes it the *only* thing that says where this clock's time came
    // from.
    //
    // Shown in the configured zone, like the `local` line above it: the device
    // keeps UTC and a zone is presentation (§10.8.2), and "when did it last
    // sync" is a question people ask in wall-clock time.
    if (sync.ever_synced) {
        struct tm fields = {};
        localtime_r(&sync.last_utc, &fields);
        printf("           last %04d-%02d-%02d %02d:%02d:%02d %s, ", fields.tm_year + 1900,
               fields.tm_mon + 1, fields.tm_mday, fields.tm_hour, fields.tm_min, fields.tm_sec,
               settings.zone);
        PrintDuration(sync.since_last_ms);
        printf(" ago, moved the clock %+d s\n", static_cast<int>(sync.last_step_seconds));
    } else {
        printf("           never synced\n");
    }

    if (!sync.enabled) {
        // Nothing is scheduled and the line above already said why. A "next"
        // clause here would be a countdown to something that will not happen.
        return;
    }

    printf("           ");
    if (sync.next_in_ms == timesync::kNever) {
        // Enabled and nothing scheduled can only mean one thing, and saying it
        // is the difference between a broken device and a waiting one.
        printf("no internet to ask through");
    } else if (sync.next_in_ms == 0) {
        printf("due now");
    } else {
        printf("next in ");
        PrintDuration(sync.next_in_ms);
    }
    if (sync.failures > 0) {
        printf(", %u attempt(s) failed since", static_cast<unsigned>(sync.failures));
    }
    printf("\n");
}

int CmdDate(int argc, char **argv) {
    rtc::Pcf85063 &clock = board::Clock();

    // **Before the RTC check**, because this one does not need the chip: SNTP
    // sets the system clock whether or not there is anything to store it in,
    // and a board whose RTC did not answer is exactly the board that needs the
    // network's time most.
    if (argc == 2 && strcmp(argv[1], "sync") == 0) {
        if (!timesync::Ready()) {
            printf("the clock sync task is not running — see the boot log\n");
            return 1;
        }
        timesync::Status sync = timesync::Get();
        if (!sync.enabled) {
            printf("clock sync is off — 'config set sync <hours>' turns it on\n");
            return 1;
        }
        if (!sync.internet) {
            printf("no internet to ask through; it will sync by itself as soon as there is\n");
            return 1;
        }

        const uint32_t before = static_cast<uint32_t>(sync.successes) + sync.failures;
        timesync::SyncNow();
        printf("asking %s…\n", config::Get().time.sntp_server);
        // Waiting for the answer rather than returning to the prompt, the same
        // call `wifi ping` makes: a command that returns before its result
        // does is a command people run twice.
        for (uint32_t waited = 0; waited < 20000; waited += 200) {
            vTaskDelay(pdMS_TO_TICKS(200));
            sync = timesync::Get();
            if (static_cast<uint32_t>(sync.successes) + sync.failures != before) {
                break;
            }
        }
        PrintSyncStatus();
        return 0;
    }

    if (!clock.Present()) {
        printf("the PCF85063 did not answer at boot — no clock to read\n");
        return 1;
    }

    if (argc == 1) {
        rtc::DateTime now = {};
        bool valid = false;
        const esp_err_t err = clock.Read(&now, &valid);
        if (err != ESP_OK) {
            printf("read failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        if (!valid) {
            // §10.8.2: an obviously unset clock beats a plausible wrong one.
            printf("rtc        -------/-- --:--:--  (oscillator stopped or never set)\n");
        } else {
            printf("rtc        %04u-%02u-%02u %02u:%02u:%02u UTC\n", now.year, now.month, now.day,
                   now.hour, now.minute, now.second);
        }

        const time_t system_now = time(nullptr);
        struct tm utc_fields = {};
        gmtime_r(&system_now, &utc_fields);
        printf("system     %04d-%02d-%02d %02d:%02d:%02d UTC\n", utc_fields.tm_year + 1900,
               utc_fields.tm_mon + 1, utc_fields.tm_mday, utc_fields.tm_hour, utc_fields.tm_min,
               utc_fields.tm_sec);

        // The same instant, shown through the configured zone. UTC is what the
        // device keeps; this line is the only place a zone means anything.
        struct tm local_fields = {};
        localtime_r(&system_now, &local_fields);
        const int offset = tz::OffsetSeconds(system_now);
        printf("local      %04d-%02d-%02d %02d:%02d:%02d %s (UTC%+03d:%02d%s)\n",
               local_fields.tm_year + 1900, local_fields.tm_mon + 1, local_fields.tm_mday,
               local_fields.tm_hour, local_fields.tm_min, local_fields.tm_sec,
               config::Get().time.zone, offset / 3600, abs(offset % 3600) / 60,
               tz::IsDaylightSaving(system_now) ? ", DST" : "");
        PrintSyncStatus();
        return 0;
    }

    // `date set` takes **local** time, because that is what an operator reads
    // off a wall or a phone. `date set utc …` is the escape hatch for the
    // moment when the zone is wrong and the clock still has to be right.
    //
    // The two forms are picked apart by naming the arguments rather than by
    // shifting `argv`, which is how the first version of this got it wrong: a
    // shift moves the word the *next* check is looking for.
    bool as_utc = false;
    const char *date_arg = nullptr;
    const char *time_arg = nullptr;
    if (argc == 4 && strcmp(argv[1], "set") == 0) {
        date_arg = argv[2];
        time_arg = argv[3];
    } else if (argc == 5 && strcmp(argv[1], "set") == 0 && strcmp(argv[2], "utc") == 0) {
        as_utc = true;
        date_arg = argv[3];
        time_arg = argv[4];
    }

    if (date_arg == nullptr) {
        printf("usage: date                    read it: RTC and system in UTC, plus local\n");
        printf("       date sync                              ask the time server now\n");
        printf("       date set <YYYY-MM-DD> <HH:MM:SS>       in %s\n", config::Get().time.zone);
        printf("       date set utc <YYYY-MM-DD> <HH:MM:SS>   in UTC\n");
        return 1;
    }

    unsigned year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (sscanf(date_arg, "%4u-%2u-%2u", &year, &month, &day) != 3 ||
        sscanf(time_arg, "%2u:%2u:%2u", &hour, &minute, &second) != 3) {
        printf("could not parse '%s %s'; expected YYYY-MM-DD HH:MM:SS\n", date_arg, time_arg);
        return 1;
    }

    // **The typed time is converted to UTC first, and UTC is what is stored.**
    // `mktime` reads the fields as local and applies the zone; `timegm` takes
    // them as UTC already. `tm_isdst = -1` is the important half of the local
    // case: it tells libc to work out for itself whether summer time was in
    // force on that date, instead of this code assuming.
    struct tm fields = {};
    fields.tm_year = static_cast<int>(year) - 1900;
    fields.tm_mon = static_cast<int>(month) - 1;
    fields.tm_mday = static_cast<int>(day);
    fields.tm_hour = static_cast<int>(hour);
    fields.tm_min = static_cast<int>(minute);
    fields.tm_sec = static_cast<int>(second);
    fields.tm_isdst = as_utc ? 0 : -1;

    const time_t seconds = as_utc ? timegm(&fields) : mktime(&fields);
    if (seconds <= 0) {
        printf("that is not a date this clock can hold\n");
        return 1;
    }

    struct tm utc_fields = {};
    gmtime_r(&seconds, &utc_fields);

    rtc::DateTime value = {};
    value.year = static_cast<uint16_t>(utc_fields.tm_year + 1900);
    value.month = static_cast<uint8_t>(utc_fields.tm_mon + 1);
    value.day = static_cast<uint8_t>(utc_fields.tm_mday);
    value.hour = static_cast<uint8_t>(utc_fields.tm_hour);
    value.minute = static_cast<uint8_t>(utc_fields.tm_min);
    value.second = static_cast<uint8_t>(utc_fields.tm_sec);
    value.weekday = 0;  // nothing here reads it; the chip keeps counting it anyway

    const esp_err_t err = clock.Write(value);
    if (err == ESP_ERR_INVALID_ARG) {
        printf("out of range: the chip stores 2000..2099 and no century\n");
        return 1;
    }
    if (err != ESP_OK) {
        printf("write failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    // The system clock follows, so logs and `status` agree with the chip
    // without waiting for a reboot.
    const timeval tv = {.tv_sec = seconds, .tv_usec = 0};
    settimeofday(&tv, nullptr);

    printf("set to %04u-%02u-%02u %02u:%02u:%02u %s\n", year, month, day, hour, minute, second,
           as_utc ? "UTC" : config::Get().time.zone);
    if (!as_utc) {
        printf("stored %04d-%02d-%02d %02d:%02d:%02d UTC — the clock keeps UTC, always\n",
               value.year, value.month, value.day, value.hour, value.minute, value.second);
    }
    return 0;
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

int CmdPowerOff(int argc, char **argv) {
    // A confirmation word, because this is the one console command whose
    // success means the operator has to walk over and press a button. The
    // repository's stance on destructive actions (§10.8.5) is that they say
    // what they will do and are not one keystroke away.
    if (argc != 2 || strcmp(argv[1], "now") != 0) {
        printf("usage: poweroff now   (cuts power; only the PWR button or a charger returns)\n");
        return 1;
    }

    const esp_err_t err = board::Pmic().PowerOff();
    if (err == ESP_ERR_INVALID_STATE) {
        printf("refused: USB is connected, so the chip would power straight back on.\n");
        printf("unplug the cable and run it again.\n");
        return 1;
    }
    if (err != ESP_OK) {
        printf("power off failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    // Unreachable in practice — the board is gone by now.
    printf("powering off\n");
    return 0;
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
        printf("usage: buttons              the state of all three\n");
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

    printf("watching for %lu s — press a button; PWR is the AXP2101's, so a long\n",
           static_cast<unsigned long>(seconds));
    printf("press of it switches the board off rather than printing anything\n");

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

// Which way the case is lying, named by the device axis gravity pulls along.
//
// **All three axes are tied to the case, and every one of them was measured by
// putting the board in a known position and reading it** — never derived from a
// drawing, which is how a sign ends up backwards. §10.13 has the table; the
// short form is +Z out of the back (the screen faces −Z), +Y at the button
// edge, +X at the card-slot edge.
//
// **The sign is the part that is easy to get backwards.** An accelerometer at
// rest reads +1 g along the axis pointing *up* — it measures the support force,
// not the pull — so the axis gravity acts along is the negation of the dominant
// reading. Getting this wrong is invisible on a desk and exactly wrong when the
// board is turned over.
const char *GravityAxis(const imu::Sample &sample) {
    const float x = sample.accel_g[0];
    const float y = sample.accel_g[1];
    const float z = sample.accel_g[2];
    const float ax = fabsf(x), ay = fabsf(y), az = fabsf(z);

    // Below this, no axis is dominant enough to name one — the device is on a
    // corner, or moving.
    constexpr float kDominant = 0.7f;
    if (az >= ax && az >= ay && az >= kDominant) {
        return z > 0 ? "along -Z (screen down)" : "along +Z (screen up)";
    }
    if (ay >= ax && ay >= az && ay >= kDominant) {
        return y > 0 ? "along -Y (standing on the USB edge)"
                     : "along +Y (standing on the button edge)";
    }
    if (ax >= ay && ax >= az && ax >= kDominant) {
        return x > 0 ? "along -X (standing on the speaker edge)"
                     : "along +X (standing on the card-slot edge)";
    }
    return "spread across axes — tilted or moving";
}

void PrintImuSample(const imu::Sample &sample) {
    printf("accel      x %+8.3f  y %+8.3f  z %+8.3f  g\n", static_cast<double>(sample.accel_g[0]),
           static_cast<double>(sample.accel_g[1]), static_cast<double>(sample.accel_g[2]));
    printf("gyro       x %+8.2f  y %+8.2f  z %+8.2f  dps\n",
           static_cast<double>(sample.gyro_dps[0]), static_cast<double>(sample.gyro_dps[1]),
           static_cast<double>(sample.gyro_dps[2]));

    float pitch = 0.0f;
    float roll = 0.0f;
    imu::Qmi8658::Tilt(sample, &pitch, &roll);
    printf("tilt       pitch %+.1f, roll %+.1f degrees; gravity %s\n",
           static_cast<double>(pitch), static_cast<double>(roll), GravityAxis(sample));

    // At rest this is 1.000 g. It is the cheapest statement that the six
    // numbers above mean anything — a scale factor off by a range setting, or a
    // burst read that returned the same byte six times, both show up here.
    printf("magnitude  %.3f g (1.000 at rest)\n",
           static_cast<double>(imu::Qmi8658::Magnitude(sample)));
    printf("die temp   %.1f C\n", static_cast<double>(sample.celsius));
    printf("status     %s, %s\n", sample.accel_fresh ? "accel fresh" : "accel stale",
           sample.gyro_fresh ? "gyro fresh" : "gyro stale");
}

// **What the two interrupt lines are doing, measured rather than reasoned
// about.** §6.1 of the datasheet says INT1 is general purpose (a ~4 ms
// chip-ready pulse after reset, the CTRL9 handshake, wake-on-motion) and INT2
// means data-ready — and that with `syncSmpl = 0`, which is what this driver
// leaves in CTRL7, INT2 is *pulsed at the output data rate* rather than held.
// Whether the pins are enabled at all is the ambiguous part: rev 0.9 calls
// CTRL1 bits 4:1 reserved while its own revision history mentions "the
// INT1/INT2 enable bit in CTRL1". So this counts edges for a window instead of
// believing either reading. Nothing acts on these lines (§10.13); this is the
// console looking at them.
void PrintImuInterrupts() {
    constexpr int64_t kWindowUs = 20000;  // 20 ms: ~5 pulses at the 250 Hz ODR

    bool level1 = board::ImuInterrupt1();
    bool level2 = board::ImuInterrupt2();
    const bool started1 = level1;
    const bool started2 = level2;
    unsigned edges1 = 0;
    unsigned edges2 = 0;

    const int64_t deadline = esp_timer_get_time() + kWindowUs;
    while (esp_timer_get_time() < deadline) {
        const bool now1 = board::ImuInterrupt1();
        const bool now2 = board::ImuInterrupt2();
        if (now1 != level1) {
            ++edges1;
            level1 = now1;
        }
        if (now2 != level2) {
            ++edges2;
            level2 = now2;
        }
    }

    printf("int1       GPIO%d ", static_cast<int>(board::imu::kInterrupt1));
    if (edges1 == 0) {
        printf("steady %s (idle: nothing here uses it)\n", started1 ? "high" : "low");
    } else {
        printf("%u edges in 20 ms\n", edges1);
    }

    printf("int2       GPIO%d ", static_cast<int>(board::imu::kInterrupt2));
    if (edges2 == 0) {
        printf("steady %s (data-ready line not pulsing)\n", started2 ? "high" : "low");
    } else {
        // Two edges per pulse, and one pulse per sample at the ODR.
        printf("%u edges in 20 ms — pulsing at about %u Hz (data-ready, ODR)\n", edges2,
               edges2 * 25);
    }
}

int CmdImu(int argc, char **argv) {
    imu::Qmi8658 &chip = board::Imu();
    if (!chip.Present()) {
        printf("the QMI8658C did not answer at boot — nothing to read\n");
        return 1;
    }

    uint32_t seconds = 0;
    if (argc > 1) {
        if (strcmp(argv[1], "watch") != 0 || argc > 3) {
            printf("usage: imu              one sample: all six axes, tilt, temperature\n");
            printf("       imu watch [s]    a line a second (default %lu s, max %lu)\n",
                   static_cast<unsigned long>(kWatchDefaultSeconds),
                   static_cast<unsigned long>(kWatchMaxSeconds));
            return 1;
        }
        seconds = kWatchDefaultSeconds;
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
    }

    imu::Sample sample = {};
    const esp_err_t err = chip.Read(&sample);
    if (err != ESP_OK) {
        printf("read failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    if (seconds == 0) {
        printf("chip       QMI8658C at 0x%02x, revision 0x%02x\n", chip.Address(),
               chip.Revision());
        PrintImuSample(sample);
        PrintImuInterrupts();
        return 0;
    }

    printf("      accel x       y       z  |   gyro x       y       z\n");
    const int64_t deadline_us = esp_timer_get_time() + static_cast<int64_t>(seconds) * 1000000;
    while (esp_timer_get_time() < deadline_us) {
        if (chip.Read(&sample) != ESP_OK) {
            printf("read failed\n");
            return 1;
        }
        printf("%+8.3f%+8.3f%+8.3f  |%+8.2f%+8.2f%+8.2f\n",
               static_cast<double>(sample.accel_g[0]), static_cast<double>(sample.accel_g[1]),
               static_cast<double>(sample.accel_g[2]), static_cast<double>(sample.gyro_dps[0]),
               static_cast<double>(sample.gyro_dps[1]), static_cast<double>(sample.gyro_dps[2]));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    return 0;
}

// `config set` writes a field and **nothing else** — no file, on purpose. The
// split is the point: editing and persisting are two decisions, and a device
// where every keystroke reaches flash is one that wears the partition out
// experimenting. `config save` is the second half, and every path here says so
// rather than leaving the operator to wonder.
//
// A number field, its bounds, and where it lives. Strings are handled below;
// the Wi-Fi networks are not settable from here — they are a list of pairs and
// belong to the screen of §10.8.6, not to a one-line console setter.
//
// **One copy of the list**, because §10.7's four-places rule bites hardest here:
// the setter's own "unknown field" line and the usage block below both have to
// name the same fields, and two hand-kept enumerations of the same nine words is
// the drift that rule exists to prevent.
constexpr const char *kSettableFields =
    "volume, brightness, dim, blank, nats, tz, sntp, sync, wifi";

int SetConfigField(const char *key, const char *value) {
    config::Data &c = config::Get();

    struct NumberField {
        const char *name;
        long min;
        long max;
        const char *unit;
    };
    constexpr NumberField kNumbers[] = {
        {"volume", 0, 100, "%"},
        {"brightness", 0, 100, "%"},
        {"dim", 0, 65535, " s"},
        {"blank", 0, 65535, " s"},
        {"sync", 0, 255, " h"},
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
        if (strcmp(key, "volume") == 0) {
            c.audio.volume_percent = static_cast<uint8_t>(parsed);
            // Applied where it belongs, so the next `play` is audibly the
            // number just typed. That is a live setting, not a saved one.
            if (board::Codec().Present()) {
                board::Codec().SetVolume(c.audio.volume_percent);
            }
        } else if (strcmp(key, "brightness") == 0) {
            c.display.brightness = static_cast<uint8_t>(parsed);
        } else if (strcmp(key, "dim") == 0) {
            c.display.dim_seconds = static_cast<uint16_t>(parsed);
        } else if (strcmp(key, "sync") == 0) {
            c.time.sync_hours = static_cast<uint8_t>(parsed);
            // Applied at once, like the volume and the zone — and **only the
            // sync**, not the connection: the lesson `wifi check` taught is
            // that a settings call reaching for more than it changed is a
            // settings call people stop making.
            timesync::Apply();
        } else {
            c.display.blank_seconds = static_cast<uint16_t>(parsed);
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
    if (strcmp(key, "tz") == 0) {
        // A name from the table, or a raw POSIX rule for a zone it does not
        // know. Both are accepted; what is refused is a *third* thing —
        // something that is neither, which is almost always a typo and which
        // libc would quietly read as UTC.
        const char *posix = tz::Lookup(value);
        const char *name = value;
        if (posix == nullptr) {
            if (!tz::LooksLikePosix(value)) {
                printf("'%s' is neither a zone this firmware knows nor a POSIX rule.\n", value);
                printf("try 'config zones' for the list, or give a rule like EET-2EEST,M3.5.0/3,M10.5.0/4\n");
                return 1;
            }
            // Stored as `Custom` rather than named after a zone that happens
            // to share the rule: one rule serves many zones, so naming it
            // would tell the operator they are in Athens when they typed the
            // rule for Kyiv.
            posix = value;
            name = tz::kCustomName;
        }
        if (strlen(name) >= sizeof(c.time.zone) || strlen(posix) >= sizeof(c.time.posix)) {
            printf("that name or rule is longer than the %u characters the field holds\n",
                   static_cast<unsigned>(sizeof(c.time.zone) - 1));
            return 1;
        }

        snprintf(c.time.zone, sizeof(c.time.zone), "%s", name);
        snprintf(c.time.posix, sizeof(c.time.posix), "%s", posix);
        // Applied at once, like the volume: the point of a zone is what `date`
        // prints, and a setting you cannot see the effect of is one you cannot
        // check.
        tz::Apply(c.time.posix);
        const int offset = tz::OffsetSeconds();
        printf("tz = %s (%s, UTC%+03d:%02d%s), in memory only — 'config save' writes it to %s\n",
               c.time.zone, c.time.posix, offset / 3600, abs(offset % 3600) / 60,
               tz::IsDaylightSaving() ? ", DST now" : "", config::kPath);
        return 0;
    }

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
        {"sntp", c.time.sntp_server, sizeof(c.time.sntp_server)},
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
        if (strcmp(key, "sntp") == 0) {
            // The server is half of whether syncing happens at all (an empty
            // one is off), so the task is told — and told nothing else.
            timesync::Apply();
        }
        if (strcmp(key, "nats") == 0) {
            // Likewise the narrowest thing that changed: a new address drops
            // the connection, an unchanged one costs nothing.
            nats::Apply();
        }
        if (cleared) {
            // `sntp = , in memory only` is what the general form prints for an
            // empty value, and it reads like a bug. Clearing a field is a
            // deliberate thing to do here — an empty time server is how
            // syncing is switched off — so it gets said in words.
            printf("%s cleared%s, in memory only — 'config save' writes it to %s\n", field.name,
                   strcmp(key, "sntp") == 0   ? "; the clock will not sync"
                   : strcmp(key, "nats") == 0 ? "; nothing will be connected"
                                              : "",
                   config::kPath);
            return 0;
        }
        printf("%s = %s, in memory only — 'config save' writes it to %s\n", field.name, value,
               config::kPath);
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
    printf("       config zones [filter]         the time zones known by name\n");
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

    // At most one filter: `config zones a b` used to list the whole table and
    // silently drop the second word, which reads as "no zone matched neither".
    if (argc >= 2 && argc <= 3 && strcmp(argv[1], "zones") == 0) {
        // The table, optionally filtered — `config zones Europe` is how an
        // operator finds the spelling without scrolling past Australia.
        const char *filter = argc == 3 ? argv[2] : nullptr;
        size_t shown = 0;
        for (size_t i = 0; i < tz::Count(); ++i) {
            const tz::Zone &zone = tz::At(i);
            if (filter != nullptr && strcasestr(zone.name, filter) == nullptr) {
                continue;
            }
            printf("  %-22s %s\n", zone.name, zone.posix);
            ++shown;
        }
        printf("%u of %u zone(s)%s. Any POSIX rule is accepted too.\n",
               static_cast<unsigned>(shown), static_cast<unsigned>(tz::Count()),
               filter != nullptr ? " matched" : "");
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
            // §10.15's restore, minus the five blind seconds of holding KEY at
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
        // A reload or a restore changed the fields; the codec is holding the
        // old number until something tells it otherwise.
        if (board::Codec().Present()) {
            board::Codec().SetVolume(config::Get().audio.volume_percent);
        }
        // And so is the Wi-Fi manager, which is holding a network list that
        // may have just been replaced — an attempt against index 2 of the old
        // list is an attempt against a network that is no longer there.
        wifimgr::Apply();
        // And the clock's sync task, holding an interval and a server that may
        // both have just changed.
        timesync::Apply();
        // And the bus link, which may now be pointed at a different server —
        // and, if it is not, is left exactly as it was, connection included.
        nats::Apply();
    }

    const config::Data &c = config::Get();
    printf("source     %s%s\n", config::kPath, config::Loaded() ? "" : " (built-in defaults)");
    printf("wifi       %s, mode %s, %u network(s)\n", c.wifi.active ? "on" : "off",
           c.wifi.mode == config::WifiMode::kAp ? "ap" : "client",
           static_cast<unsigned>(c.wifi.network_count));
    printf("           fallback ap '%s' (%s) on ch %u after %u round(s), window %u s\n",
           c.wifi.ap_ssid, c.wifi.ap_password[0] == '\0' ? "open" : "password set",
           static_cast<unsigned>(c.wifi.ap_channel),
           static_cast<unsigned>(c.wifi.rounds_before_ap),
           static_cast<unsigned>(c.wifi.ap_window_seconds));
    for (uint8_t i = 0; i < c.wifi.network_count; ++i) {
        // **The password is never printed** (§10.8.6, §10.15): it is a secret
        // from the moment it is typed, and a console dump is exactly the place
        // it must not turn up. An address is not a secret and is printed.
        printf("           %u. %s (password %s, %s)\n", static_cast<unsigned>(i + 1),
               c.wifi.networks[i].ssid,
               c.wifi.networks[i].password[0] == '\0' ? "not set" : "set",
               c.wifi.networks[i].ip.enabled ? c.wifi.networks[i].ip.address : "dhcp");
    }
    printf("nats       %s\n", c.nats.url);
    const int offset = tz::OffsetSeconds();
    printf("time       %s (%s), UTC%+03d:%02d%s\n", c.time.zone, c.time.posix, offset / 3600,
           abs(offset % 3600) / 60, tz::IsDaylightSaving() ? ", DST now" : "");
    if (c.time.sync_hours == 0) {
        printf("           sntp %s, sync off\n", c.time.sntp_server);
    } else {
        printf("           sntp %s every %u h\n", c.time.sntp_server,
               static_cast<unsigned>(c.time.sync_hours));
    }
    printf("display    %u%%, dim after %us, blank after %us\n",
           static_cast<unsigned>(c.display.brightness),
           static_cast<unsigned>(c.display.dim_seconds),
           static_cast<unsigned>(c.display.blank_seconds));
    printf("audio      volume %u%%\n", static_cast<unsigned>(c.audio.volume_percent));
    return 0;
}

int CmdPlay(int argc, char **argv) {
    ::audio::Speaker &sound = board::Sound();
    ::audio::Es8311 &codec = board::Codec();

    if (argc > 1 && strcmp(argv[1], "volume") == 0) {
        if (argc == 2) {
            printf("volume %u%% (config says %u%%)\n", static_cast<unsigned>(codec.Volume()),
                   static_cast<unsigned>(config::Get().audio.volume_percent));
            return 0;
        }
        if (argc != 3) {
            printf("usage: play volume [0..100]\n");
            return 1;
        }
        // **The same field, through the same setter as `config set volume`** —
        // one behaviour and one implementation, rather than two commands that
        // differ in whether they touch the filesystem. This one used to save;
        // it no longer does, and `config save` is what writes.
        return SetConfigField("volume", argv[2]);
    }

    if (!codec.Present()) {
        printf("the ES8311 did not answer at boot — no codec to play through\n");
        return 1;
    }
    if (!sound.Ready()) {
        printf("the I2S channel is not running — see the boot log\n");
        return 1;
    }

    const char *path = argc > 1 ? argv[1] : "alert.wav";
    if (argc > 2) {
        printf("usage: play [file]        default alert.wav\n");
        printf("       play volume <0..100>\n");
        return 1;
    }

    ::audio::WavFormat format = {};
    esp_err_t err = sound.Describe(path, &format);
    switch (err) {
        case ESP_OK:
            break;
        case ESP_ERR_NOT_FOUND:
            printf("no such file: %s\n", path);
            return 1;
        case ESP_ERR_NOT_SUPPORTED:
            // The container is right and the contents are not — the one case
            // worth spelling out, because "it is a .wav" is exactly what the
            // operator will be sure of.
            printf("%s is not uncompressed PCM; convert it (see working-with-code.md)\n", path);
            return 1;
        default:
            printf("could not read %s: %s\n", path, esp_err_to_name(err));
            return 1;
    }

    printf("%s: %" PRIu32 " Hz, %u channel(s), %u-bit, %.1f s\n", path, format.sample_rate,
           static_cast<unsigned>(format.channels), static_cast<unsigned>(format.bits),
           format.sample_rate == 0 || format.channels == 0
               ? 0.0
               : static_cast<double>(format.data_bytes) /
                     static_cast<double>(format.sample_rate * format.channels * (format.bits / 8)));

    // The volume for this playback comes from the file, not from whatever the
    // codec happens to be set to: `config.json` is the record of what the
    // operator chose, and a `play` that is quieter than the last one for no
    // visible reason is a bug report waiting to happen.
    const uint8_t wanted = config::Get().audio.volume_percent;
    if (codec.Volume() != wanted) {
        codec.SetVolume(wanted);
    }

    err = sound.PlayWav(path);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        printf("this driver plays 16-bit mono PCM at 8/16/32/44.1/48 kHz, and that is not it\n");
        return 1;
    }
    if (err != ESP_OK) {
        printf("playback failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("played at volume %u%%\n", static_cast<unsigned>(codec.Volume()));
    return 0;
}

// --- Wi-Fi (§10.9) ---------------------------------------------------------
//
// The console half of §10.9, and it is deliberately the same shape as
// `config set`: everything here changes what the device is doing **now** and
// says "in memory only", and `config save` is what makes it survive a reboot.
// A console where each keystroke lands in flash is a console that wears the
// partition out during an experiment (§10.15), and joining networks is exactly
// the kind of experiment this is for.
//
// The one thing it never prints is a password. §10.15's rule — a secret from
// the moment it is typed — and a console dump is the place it would leak.

// Where a scan lands. Static for the same reason everything else here is:
// nothing allocates (§10.14.1), and the REPL's stack is not the place for
// half a kilobyte of results.
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
    // both true internally and both nonsense on a screen.
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
    // Parsed here rather than at the task, so a typo is refused while the
    // person who made it is still looking at the screen — `wifi static` and
    // `config set tz` make the same call, and the reason is the same one:
    // libc, lwIP and this parser all have a way of reading a wrong string as
    // *something*.
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

    // **A confirmation word, because this one cannot be undone from here.** The
    // rule §10.8.5 states for its destructive entries and `poweroff now` follows:
    // what is lost is a registration, and getting it back means a fresh token
    // minted on the host.
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

    printf("key id     approver-esp32\n");
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
            char stamp[32];
            const time_t seconds = static_cast<time_t>(when);
            std::tm local = {};
            localtime_r(&seconds, &local);
            strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local);
            printf("registered %s\n", stamp);
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

// §6, over USB, because the alternative is typing 50 characters of base64 on a
// 2.16-inch touchscreen.
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

int CmdPower(int, char **) {
    pmic::Axp2101 &axp = board::Pmic();
    if (!axp.Present()) {
        printf("the AXP2101 did not answer at boot — nothing to report\n");
        return 1;
    }

    pmic::Status s = {};
    const esp_err_t err = axp.Read(&s);
    if (err != ESP_OK) {
        printf("read failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("charger    %s (code %u)%s\n", pmic::Axp2101::ChargeStateName(s.charge_code),
           static_cast<unsigned>(s.charge_code),
           s.charging ? ", charging" : (s.discharging ? ", discharging" : ""));
    printf("vbus       %s", s.vbus_present ? "present" : "absent");
    if (s.vbus_present) {
        printf(", %u mV", static_cast<unsigned>(s.vbus_mv));
    }
    printf("\n");

    if (s.battery_present) {
        printf("battery    %u mV", static_cast<unsigned>(s.battery_mv));
        if (s.battery_percent >= 0) {
            printf(", %d%%", s.battery_percent);
        }
        printf("\n");
    } else {
        printf("battery    none connected\n");
    }

    printf("system     %u mV\n", static_cast<unsigned>(s.system_mv));
    printf("die temp   %.1f C\n", static_cast<double>(s.die_celsius));
    // The two rails that are not decoration: ALDO3 resets the panel, ALDO2
    // powers the amplifier (§10.1). ALDO3 is **already on out of reset** — the
    // PMIC's own default on this board, not something the firmware writes —
    // while ALDO2 is off until `board::Init` turns it on for the codec.
    printf("pwr key    on after %s, off after %s long press (%s)\n",
           pmic::PressOnTimeName(s.press_on_code), pmic::PressOffTimeName(s.press_off_code),
           s.long_press_shutdown ? "enabled" : "DISABLED — long press does nothing");
    printf("woke by    %s (0x%02x)\n", pmic::PowerOnSourceName(s.power_on_source),
           static_cast<unsigned>(s.power_on_source));
    printf("rails      dc1 %u mV | aldo2 (audio) %u mV %s | aldo3 (panel) %u mV %s\n",
           static_cast<unsigned>(s.dc1_mv), static_cast<unsigned>(s.aldo2_mv),
           s.aldo2_enabled ? "on" : "off", static_cast<unsigned>(s.aldo3_mv),
           s.aldo3_enabled ? "on" : "off");
    return 0;
}

// The display, and the same class of command `power` and `imu` are: a way to
// find out that a part of the board is alive without reflashing to test it
// (§10.7). It answers the three questions a panel raises — is it there, is it
// lit, and is the touch controller reporting — and the last of those is why
// `missed reads` is printed: a number that climbs is contention on the I²C
// lease (§10.14.3), which no other readout would show.
int CmdDisplay(int argc, char **argv) {
    ::display::Panel &panel = board::Display();
    ::display::Touch &glass = board::Touch();

    if (argc == 1) {
        printf("panel      %s", panel.Ready() ? "up" : "not initialised");
        if (panel.Ready()) {
            printf(", %dx%d, %s at %u%%", panel.Width(), panel.Height(),
                   panel.On() ? "on" : "blanked", static_cast<unsigned>(panel.Brightness()));
        }
        printf("\n");
        printf("lvgl       %s\n", ::display::LvglReady() ? "running" : "not started");
        printf("touch      %s, %" PRIu32 " missed read(s)\n",
               glass.Ready() ? "up" : "not initialised", glass.MissedReads());
        return 0;
    }

    if (!panel.Ready()) {
        printf("the panel did not come up — see the boot log\n");
        return 1;
    }

    if (argc == 2 && strcmp(argv[1], "on") == 0) {
        return panel.SetOn(true) == ESP_OK ? 0 : 1;
    }
    if (argc == 2 && strcmp(argv[1], "off") == 0) {
        return panel.SetOn(false) == ESP_OK ? 0 : 1;
    }
    if (argc > 1 && strcmp(argv[1], "brightness") == 0) {
        // **Reading is a form of this command, not a misuse of it.** §10.7
        // writes it `display brightness [0..100]`, the same shape as
        // `play volume [0..100]`, and for the same reason: the panel's live
        // value and the one `config.json` carries are two different numbers,
        // and the only way to find out they have diverged is to ask. Requiring
        // the argument made the documented spelling an error — and an error
        // whose usage text then printed `<0..100>`, so the docs and the
        // console disagreed about which one was wrong.
        if (argc == 2) {
            printf("brightness %u%% (config says %u%%)\n",
                   static_cast<unsigned>(panel.Brightness()),
                   static_cast<unsigned>(config::Get().display.brightness));
            return 0;
        }
        if (argc != 3) {
            printf("usage: display brightness [0..100]\n");
            return 1;
        }
        char *end = nullptr;
        const long value = strtol(argv[2], &end, 10);
        if (end == argv[2] || *end != '\0' || value < 0 || value > 100) {
            printf("brightness takes 0..100\n");
            return 1;
        }
        // Not written to `config.json` — `config set brightness` is what does
        // that, and this is the one that only moves the panel (§10.15's rule
        // that editing and persisting are two commands).
        const esp_err_t err = panel.SetBrightness(static_cast<uint8_t>(value));
        if (err != ESP_OK) {
            printf("brightness not set: %s\n", esp_err_to_name(err));
            return 1;
        }
        printf("brightness %ld%% (panel only — `config set brightness` is the stored one)\n",
               value);
        return 0;
    }

    printf("usage: display                    panel, LVGL and touch state\n");
    printf("       display on|off             blank the panel without dimming it\n");
    printf("       display brightness [0..100]   read it, or set the panel only\n");
    return 1;
}

// The codec and the channel in front of it — the last piece of this board with
// no readout of its own. `play volume` shows the volume; whether the ES8311
// answered at all, and whether the I²S channel is running, have only ever been
// in the boot log, which is gone by the time somebody asks.
int CmdAudio(int argc, char **) {
    if (argc != 1) {
        printf("usage: audio         the codec and the I2S channel; 'play volume' sets the level\n");
        return 1;
    }

    ::audio::Es8311 &codec = board::Codec();
    ::audio::Speaker &sound = board::Sound();

    if (!codec.Present()) {
        printf("codec      the ES8311 did not answer at boot\n");
        return 0;
    }
    // **Muted is the resting state, not a fault.** §10.13 gives this hardware
    // one job — a chirp on a new request — and `Speaker::PlayWav` unmutes
    // around the file, so a codec found unmuted here is the odd one.
    printf("codec      ES8311 up, volume %u%%, %s\n", static_cast<unsigned>(codec.Volume()),
           codec.Muted() ? "muted" : "unmuted");
    printf("i2s        %s", sound.Ready() ? "running" : "not started");
    if (sound.Ready()) {
        printf(" at %" PRIu32 " Hz", sound.SampleRate());
    }
    printf("\n");
    return 0;
}

// Everything, in one command.
//
// **It calls the other commands rather than reprinting what they print**, and
// that is the whole design: a second copy of the `power` readout would drift
// from the first the day somebody adds a field to one of them, which is exactly
// the drift §10.7's four-places rule exists to prevent. The cost of this
// command is one line per section, and it cannot go stale.
//
// **Every header is the name of a command**, so the dump doubles as a map: see
// something odd under `== power`, type `power` to look at it alone. That rule
// is also why `audio` above exists — it was the one section with no command
// behind it, and inventing an exception was worse than adding the command.
//
// The headers are not decoration either. Run together, the sections share label
// names — `die temp` is the PMIC's and the IMU's, `system` is a voltage in one
// and a clock in the next — and a wall of aligned lines with no marks in it is
// a wall nobody reads twice.
//
// It is **state, not settings**: `config` prints what the file says, this
// prints what the device is doing, and the two answer different questions.
// The clock screen (CLAUDE.md §10.8.2), and the one command here whose subject
// the operator can already see. It exists anyway for two reasons: a screen is the
// only output of this firmware that cannot be captured from a script, so "the
// drift is moving" and "the water is flowing" would otherwise be claims nobody
// can check without a camera; and it says *why* an indicator is the colour it is,
// which the glyph itself cannot.
//
// It reads `screens::Get()` — a snapshot the screen task keeps — and never
// touches a widget: §10.8.1 gives the display to one task, and a console command
// is not it.
int CmdClock(int argc, char **) {
    if (argc != 1) {
        printf("usage: clock     what the clock screen is showing, and why\n");
        return 1;
    }

    const screens::Status status = screens::Get();
    if (!status.ready) {
        printf("screen     not running — the panel or LVGL did not come up\n");
        return 1;
    }
    const ui::ClockView &view = status.view;

    if (view.time_valid) {
        printf("face       %d%d:%d%d\n", view.digit[0], view.digit[1], view.digit[2],
               view.digit[3]);
    } else {
        // §10.8.2: dashes, not a plausible 00:00. Saying so here as well, because
        // "the screen shows dashes" and "the screen is broken" look the same from
        // across a room.
        printf("face       --:--  no believable time yet — 'date set' or a sync\n");
    }

    // The two numbers that say the AMOLED is being looked after: where the face
    // is inside its box, and where the water is in its cycle. Both move on their
    // own, so two runs of this command a second apart are the check.
    printf("drift      %+d,%+d px of +-%d,+-%d\n", static_cast<int>(view.drift_x),
           static_cast<int>(view.drift_y), static_cast<int>(ui::ClockFace::kDriftX),
           static_cast<int>(ui::ClockFace::kDriftY));
    printf("water      phase %u of 256, %u ms per cycle\n", static_cast<unsigned>(view.phase),
           static_cast<unsigned>(ui::ClockFace::kPhasePeriodMs));

    const char *wifi = "off";
    switch (view.wifi) {
        case ui::WifiIcon::kOff:
            wifi = "off — hollow bars";
            break;
        case ui::WifiIcon::kConnecting:
            wifi = "connecting — the bars cycle";
            break;
        case ui::WifiIcon::kClient:
            wifi = "client";
            break;
        case ui::WifiIcon::kAp:
            wifi = "access point — hollow bars with a T in them";
            break;
    }
    printf("wifi       %s, %u of %u bar(s) lit\n", wifi, static_cast<unsigned>(view.bars),
           static_cast<unsigned>(ui::ClockFace::kMaxBars));

    const char *bus = "";
    switch (view.bus) {
        case ui::BusIcon::kOff:
            bus = "hollow — no server configured, which is not a fault";
            break;
        case ui::BusIcon::kDown:
            bus = "red — there is a server and we are not on it";
            break;
        case ui::BusIcon::kUp:
            bus = "green — connected, nothing has arrived lately";
            break;
        case ui::BusIcon::kActive:
            bus = "green with a hole — something arrived in the last 2 min";
            break;
    }
    printf("bus        %s\n", bus);

    const char *battery = "";
    switch (view.battery) {
        case ui::BatteryIcon::kAbsent:
            battery = "no cell on the connector, running off the cable";
            break;
        case ui::BatteryIcon::kDischarging:
            battery = "on the battery";
            break;
        case ui::BatteryIcon::kCharging:
            battery = "charging";
            break;
        case ui::BatteryIcon::kExternal:
            battery = "cable in, not taking current";
            break;
    }
    printf("battery    %s", battery);
    if (view.battery_known) {
        printf(", %u%%", static_cast<unsigned>(view.battery_percent));
    }
    printf("\n");

    printf("updates    %" PRIu32 ", %" PRIu32 " gave the frame up waiting for the display\n",
           status.updates, status.lock_misses);
    printf("stack      %" PRIu32 " byte(s) never used, of %" PRIu32 "\n", status.stack_low_water,
           screens::kTaskStackBytes);
    return 0;
}

// --- `screenshot` (CLAUDE.md §10.8) --------------------------------------
//
// The one command here whose output is not for a person to read. `display`
// answers "is the panel up", `clock` answers "what does the screen think it is
// showing", and neither can answer "show me". This can: it streams the frame out
// as base64 and `tools/screenshot.py` turns it into a PNG.
//
// **Why it is streamed and not stored** is `lvgl_display.h`'s argument and worth
// not rediscovering: the panel is write-only over QSPI and a 480×480 frame is
// 460,800 bytes on a part with 512 KB of SRAM and no PSRAM (§10.1). So the pixels
// are taken a rendered piece at a time and encoded straight to the console,
// costing the two buffers below and nothing else.
//
// It **holds up the display for as long as the transfer takes** — a few seconds,
// because 460,800 bytes of RGB565 is 614,400 characters of base64. That is
// exactly what §10.8.1 says not to do in the LVGL task, and it is deliberate
// here: somebody typed it, and the alternative is a screenshot that does not
// exist.

// 720 bytes in, 960 characters out. A multiple of three, so a line is a whole
// number of base64 groups and the decoder never has to stitch one together
// across two lines.
// Long enough for 614,400 characters to leave over USB Serial/JTAG. Generous on
// purpose: a capture that times out half way through prints half a picture, and
// the failure is then in the decoder rather than here.
constexpr uint32_t kShotTimeoutMs = 60000;

constexpr size_t kShotGroupBytes = 720;
constexpr size_t kShotLineChars = (kShotGroupBytes / 3) * 4;

// Static rather than on the REPL task's stack (§10.14.1), and one of them
// because `display::Capture` refuses a second caller anyway.
struct ShotState {
    uint8_t pending[kShotGroupBytes];
    char line[kShotLineChars + 1];
    size_t pending_len;
    size_t bytes;
    uint32_t pieces;
    bool failed;
};

ShotState shot;

void ShotFlushLine(bool force) {
    if (shot.pending_len == 0) {
        return;
    }
    if (!force && shot.pending_len < kShotGroupBytes) {
        return;
    }
    size_t written = 0;
    const int err = mbedtls_base64_encode(reinterpret_cast<unsigned char *>(shot.line),
                                          sizeof(shot.line), &written, shot.pending,
                                          shot.pending_len);
    if (err != 0) {
        shot.failed = true;
        shot.pending_len = 0;
        return;
    }
    shot.line[written] = '\0';
    printf("%s\n", shot.line);
    shot.pending_len = 0;
}

void ShotFeed(const uint8_t *bytes, size_t count) {
    while (count > 0) {
        const size_t room = kShotGroupBytes - shot.pending_len;
        const size_t take = count < room ? count : room;
        memcpy(shot.pending + shot.pending_len, bytes, take);
        shot.pending_len += take;
        bytes += take;
        count -= take;
        shot.bytes += take;
        ShotFlushLine(false);
    }
}

// One rendered piece. The header goes out as text and the rows as base64, and the
// pending group is flushed at the boundary so that **no line straddles two
// pieces** — which is what lets the decoder trust a line's length.
void ShotSink(const lv_area_t &area, uint32_t stride, const uint8_t *rows, void *) {
    const int32_t width = area.x2 - area.x1 + 1;
    const int32_t height = area.y2 - area.y1 + 1;
    if (width <= 0 || height <= 0) {
        return;
    }

    shot.pieces++;
    printf("@ %" PRId32 " %" PRId32 " %" PRId32 " %" PRId32 "\n", area.x1, area.y1, area.x2,
           area.y2);

    // A row is the area's width, not the buffer's stride: the two can differ, and
    // sending the padding would be sending whatever was in it last frame.
    const size_t row_bytes = static_cast<size_t>(width) * 2;
    for (int32_t y = 0; y < height; ++y) {
        ShotFeed(rows + static_cast<size_t>(y) * stride, row_bytes);
    }
    ShotFlushLine(true);
}

int CmdScreenshot(int argc, char **) {
    if (argc != 1) {
        printf("usage: screenshot     the frame as base64 — tools/screenshot.py makes the png\n");
        return 1;
    }
    if (!::display::LvglReady()) {
        printf("no display — nothing to photograph\n");
        return 1;
    }

    memset(&shot, 0, sizeof(shot));

    // The width and the pixel format are in the opening marker rather than
    // assumed by the decoder: a screenshot that has to be told its own geometry
    // is a screenshot that silently decodes wrong the day a panel changes.
    printf("-----BEGIN SCREENSHOT %d %d rgb565le-----\n", board::kScreenWidth,
           board::kScreenHeight);

    const esp_err_t err = ::display::Capture(&ShotSink, nullptr, kShotTimeoutMs);
    ShotFlushLine(true);

    printf("-----END SCREENSHOT %" PRIu32 " %u-----\n", shot.pieces,
           static_cast<unsigned>(shot.bytes));

    if (err != ESP_OK) {
        printf("capture incomplete: %s\n", esp_err_to_name(err));
        return 1;
    }
    if (shot.failed) {
        printf("capture incomplete: base64 encoder refused a group\n");
        return 1;
    }
    return 0;
}

// --- `request` (CLAUDE.md §10.8.4, §10.10) -------------------------------
//
// The card, and the only way to put one on the screen today.
//
// **There is deliberately no bus behind it.** Subscribing to `approvals.*` in the
// `approvers` queue group would take real requests away from the responders that
// can actually sign one, and answer them with nothing — §6's "Multiple clients"
// and §10.2, and the reason this is a console command rather than a subscription.
// When §10.6 gives this device a key, the bus handler becomes a second caller of
// `screens::Inject` and this command stays exactly as it is.
//
// The synthetic card carries a plausible §7 payload because the fields are the
// ones the screen shows: an implausible one would test the layout against text
// nobody will ever see.
int CmdRequest(int argc, char **argv) {
    auto usage = []() {
        printf("usage: request                    what is on the card, and the tally\n");
        printf("       request test [seconds]     put a synthetic card up\n");
        printf("       request test <tool> <text> ...with a tool and arguments of your own\n");
        printf("the card is answered on the board: BOOT allows, PWR denies\n");
    };

    if (argc == 1) {
        const screens::CardStatus card = screens::Card();
        if (!card.ready) {
            printf("card       not running - the panel or LVGL did not come up\n");
            return 1;
        }

        switch (card.state) {
            case ui::CardState::kIdle:
                printf("card       nothing pending\n");
                break;
            case ui::CardState::kCard:
                printf("card       %s, %" PRIu32 " s left\n", card.tool,
                       (card.remaining_ms + 999) / 1000);
                printf("cwd        %s\n", card.cwd);
                printf("input      %s", card.input_preview);
                if (card.input_length > std::strlen(card.input_preview)) {
                    // The screen is where a command is read in full (§10.8.4); a
                    // console preview that looked complete would be the truncation
                    // that section forbids, arriving through the back door.
                    printf("  ... %u byte(s) in all", static_cast<unsigned>(card.input_length));
                }
                printf("\n");
                if (card.waiting > 0) {
                    printf("waiting    %u more\n", static_cast<unsigned>(card.waiting));
                }
                break;
            case ui::CardState::kReceipt:
                printf("card       showing what happened to the last one\n");
                break;
        }

        const char *last = "nothing yet";
        switch (card.last_outcome) {
            case ui::Outcome::kAllowed:
                last = "allowed";
                break;
            case ui::Outcome::kDenied:
                last = "denied";
                break;
            case ui::Outcome::kTimedOut:
                last = "timed out - nobody answered, and nothing was sent";
                break;
            case ui::Outcome::kNone:
                break;
        }
        printf("last       %s%s%s\n", last, card.last_tool[0] != '\0' ? " - " : "",
               card.last_tool);
        printf("tally      %u allowed, %u denied, %u timed out\n",
               static_cast<unsigned>(card.allowed), static_cast<unsigned>(card.denied),
               static_cast<unsigned>(card.timed_out));
        // Both of these are guards working rather than faults, which is why they
        // are on their own line: `refused` is a payload this device would not show
        // somebody, `ignored` is a press that began before the card did.
        printf("guards     %u refused, %u press(es) ignored\n",
               static_cast<unsigned>(card.refused), static_cast<unsigned>(card.ignored));
        // **What is behind the card**, and it is the half `card` cannot answer: a
        // device showing requests and a device answering them look the same from
        // the glass. The blocker is named because each of the three has its own
        // command to go and look at.
        const responder::Status wire = responder::Get();
        if (!wire.ready) {
            printf("answering  the responder did not start\n");
            return 0;
        }
        if (wire.subscribed) {
            printf("answering  yes - listening on %s in the group %s\n",
                   protocol::kApprovalsSubject, protocol::kApproversQueue);
        } else {
            printf("answering  no - %s\n", responder::BlockerText(wire.blocked_by));
            printf("           'request test' still works; nothing arrives on its own\n");
        }
        printf("wire       %" PRIu32 " arrived, %" PRIu32 " shown, %" PRIu32 " dropped\n",
               wire.received, wire.queued, wire.refused);
        printf("sent       %" PRIu32 " repl(ies): %" PRIu32 " allow, %" PRIu32 " deny\n",
               wire.replied, wire.allowed, wire.denied);

        // **Every one of these is a human press nobody heard**, which is why they
        // get their own line and are printed even at zero: §10.10's fail-safe is
        // correct behaviour and still worth counting.
        const uint32_t lost =
            wire.sign_failed + wire.publish_failed + wire.stale_dropped + wire.overflowed;
        printf("unanswered %" PRIu32 " press(es) that never reached the bus", lost);
        if (lost > 0) {
            printf(" - %" PRIu32 " could not sign, %" PRIu32 " could not send, %" PRIu32
                   " too late, %" PRIu32 " no room",
                   wire.sign_failed, wire.publish_failed, wire.stale_dropped, wire.overflowed);
        }
        printf("\n");
        printf("stack      %" PRIu32 " byte(s) never used, of %" PRIu32 "\n", wire.stack_low_water,
               responder::kTaskStackBytes);
        return 0;
    }

    if (strcmp(argv[1], "help") == 0) {
        usage();
        return 0;
    }

    if (strcmp(argv[1], "test") != 0) {
        usage();
        return 1;
    }

    // Static, because it is 2 KB of §7 fields and the REPL task's stack is not
    // where that belongs (§10.14.1).
    static ui::Request request;
    request = ui::Request{};
    request.v = 1;
    request.ts = static_cast<int64_t>(time(nullptr));
    snprintf(request.session_id, sizeof(request.session_id), "console-test");
    // Not a real nonce and it does not need to be: nothing signs this, and the
    // screen uses it only to tell one card from the next. A real one is 32 bytes
    // from the RNG **after Wi-Fi is up** (§10.7), which is the signer's problem.
    snprintf(request.nonce, sizeof(request.nonce), "test-%llu",
             static_cast<unsigned long long>(esp_timer_get_time()));
    snprintf(request.input_sha256, sizeof(request.input_sha256),
             "0000000000000000000000000000000000000000000000000000000000000000");
    snprintf(request.reply, sizeof(request.reply), "_INBOX.console.test");
    snprintf(request.cwd, sizeof(request.cwd), "E:\\projects\\ai-remote");

    uint32_t seconds = 30;
    if (argc == 3 && isdigit(static_cast<unsigned char>(argv[2][0])) != 0) {
        char *end = nullptr;
        const long value = strtol(argv[2], &end, 10);
        if (end == argv[2] || *end != '\0' || value < 1 || value > 600) {
            printf("request test takes 1..600 seconds\n");
            return 1;
        }
        seconds = static_cast<uint32_t>(value);
        snprintf(request.tool_name, sizeof(request.tool_name), "Bash");
        snprintf(request.tool_input, sizeof(request.tool_input), "{\"command\": \"rm -rf build\"}");
    } else if (argc >= 4) {
        snprintf(request.tool_name, sizeof(request.tool_name), "%s", argv[2]);
        // The rest of the line, joined with spaces — an argument list is what a
        // `tool_input` looks like from a console.
        size_t at = 0;
        for (int i = 3; i < argc && at + 1 < sizeof(request.tool_input); ++i) {
            const int written = snprintf(request.tool_input + at, sizeof(request.tool_input) - at,
                                         "%s%s", i > 3 ? " " : "", argv[i]);
            if (written <= 0) {
                break;
            }
            at += static_cast<size_t>(written);
        }
    } else {
        snprintf(request.tool_name, sizeof(request.tool_name), "Bash");
        snprintf(request.tool_input, sizeof(request.tool_input), "{\"command\": \"rm -rf build\"}");
    }
    request.ttl_ms = seconds * 1000;

    if (!screens::Inject(request)) {
        printf("refused - the queue is full, or a field did not fit\n");
        return 1;
    }

    // **The chirp is here and not in `screens`**, and the reason is in
    // `screens.h`: `PlayWav` blocks for the length of the file, and the task that
    // watches for a press must not be the task that waits for a sound.
    if (board::Sound().Ready()) {
        board::Sound().PlayWav("alert.wav");
    }

    printf("card up: %s, %" PRIu32 " s - BOOT allows, PWR denies\n", request.tool_name, seconds);
    return 0;
}

int CmdDevStatus(int argc, char **) {
    if (argc != 1) {
        printf("usage: devstatus     the board, the chips, the screen, time and the network\n");
        return 1;
    }

    // The frame first, then the chips roughly in the order `board::Init` brings
    // them up, then time, then the network — which is also the order in which
    // one of them being wrong stops the next from working.
    struct Section {
        const char *name;
        int (*run)(int, char **);
    };
    static const Section kSections[] = {
        {"status", &CmdStatus},   {"power", &CmdPower},     {"buttons", &CmdButtons},
        {"imu", &CmdImu},         {"audio", &CmdAudio},     {"display", &CmdDisplay},
        // `date` carries the clock **and** where its time came from, which is
        // why there is no separate sync section.
        // And the screens, after the panel they are drawn on.
        {"clock", &CmdClock},     {"request", &CmdRequest},
        {"date", &CmdDate},       {"wifi", &CmdWifi},
        // And the bus after the network that carries it, which is also the
        // order in which one of them being wrong stops the next from working.
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

const esp_console_cmd_t kCommands[] = {
    {
        .command = "devstatus",
        .help = "everything at once: the board, every chip, the screen, the time and the network",
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
        .command = "screenshot",
        .help = "the frame as base64 for tools/screenshot.py — holds the display while it runs",
        .hint = nullptr,
        .func = &CmdScreenshot,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "request",
        .help = "the permission card: what is on it, the tally, or 'request test' to raise one",
        .hint = "[test [seconds|<tool> <text>]]",
        .func = &CmdRequest,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "clock",
        .help = "what the clock screen shows: the time, the drift, and why each icon is that colour",
        .hint = nullptr,
        .func = &CmdClock,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "power",
        .help = "charge state, VBUS, battery and system voltage, die temperature",
        .hint = nullptr,
        .func = &CmdPower,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "buttons",
        .help = "the three buttons: state now, or 'buttons watch [s]' to print edges",
        .hint = "[watch [seconds]]",
        .func = &CmdButtons,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "imu",
        .help = "the QMI8658C: acceleration and rotation on all six axes, tilt, temperature",
        .hint = "[watch [seconds]]",
        .func = &CmdImu,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "config",
        .help = "the settings file: print it, set a field, reload / save / restore it",
        .hint = "[reload|save|restore|set|zones] — 'config help' for the forms",
        .func = &CmdConfig,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "audio",
        .help = "the ES8311 and the I2S channel: present, volume, muted, sample rate",
        .hint = nullptr,
        .func = &CmdAudio,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "play",
        .help = "play a WAV from the storage partition, or 'play volume <0..100>'",
        .hint = "[file|volume <0..100>]",
        .func = &CmdPlay,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "wifi",
        .help = "the radio: status, join, forget, static address, scan, internet check",
        .hint = "[mode|join|forget|static|scan|ping|check|retry] — 'wifi help' for the forms",
        .func = &CmdWifi,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "nats",
        .help = "the bus: where it is, whether it is connected, publish and subscribe",
        .hint = "[connect|disconnect|retry|url|sub|unsub|pub] — 'nats help' for the forms",
        .func = &CmdNats,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "keys",
        .help = "this device's identity: the key it signs with, and whether it has one",
        .hint = "[selftest|forget now]",
        .func = &CmdKeys,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "register",
        .help = "register this device with the handler, using a one-time token from the host",
        .hint = "<token>",
        .func = &CmdRegister,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "forget",
        .help = "drop the registration and the pinned handler key (needs a new token after)",
        .hint = "now",
        .func = &CmdForget,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "term",
        .help = "turn line editing and up-arrow history on ('term smart'), or ask the terminal",
        .hint = "[smart|dumb]",
        .func = &CmdTerm,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "date",
        .help = "read the clock and how it is synced, set it, or sync it now",
        .hint = "[sync|set [utc] <YYYY-MM-DD> <HH:MM:SS>]",
        .func = &CmdDate,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "display",
        .help = "the panel, LVGL and the touch controller; on/off and brightness",
        .hint = "[on|off|brightness [0..100]]",
        .func = &CmdDisplay,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "reboot",
        .help = "restart the device; anything set and not saved is lost",
        .hint = nullptr,
        .func = &CmdReboot,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "poweroff",
        .help = "cut power (refused while USB is connected)",
        .hint = "now",
        .func = &CmdPowerOff,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "ls",
        .help = "list the storage partition (SPIFFS is flat — this is all of it)",
        .hint = nullptr,
        .func = &CmdLs,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "cat",
        .help = "print a file from the storage partition",
        .hint = "<path>",
        .func = &CmdCat,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
};

}  // namespace

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

    const esp_console_dev_usb_serial_jtag_config_t dev_config =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();

    esp_err_t err = esp_console_new_repl_usb_serial_jtag(&dev_config, &repl_config, &repl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "console init failed: %s", esp_err_to_name(err));
        return err;
    }

    // **History exists; the editor that reaches it is switched off, and `term`
    // is how it gets switched on.** `esp_console` already keeps the last
    // `max_history_len` lines and adds every line typed, so up-arrow costs
    // nothing to have. What disables it is `linenoiseProbe()`, run once inside
    // the call above: it asks the terminal to identify itself, and on USB
    // Serial/JTAG nobody is attached that early — the host opens the port
    // seconds later — so it times out and linenoise latches dumb mode for the
    // rest of the session, whoever attaches afterwards.
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

    ESP_LOGI(TAG, "console on USB Serial/JTAG — type 'help'");
    return ESP_OK;
}

}  // namespace console
