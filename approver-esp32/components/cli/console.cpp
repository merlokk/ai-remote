#include "console.h"

#include <sys/time.h>

#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "board.h"
#include "buttons.h"
#include "config.h"
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
#include "qmi8658.h"
#include "speaker.h"
#include "storage.h"
#include "timezone.h"

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

int CmdDate(int argc, char **argv) {
    rtc::Pcf85063 &clock = board::Clock();
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

    printf("unknown field '%s'. settable: volume, brightness, dim, blank, nats, tz, sntp, wifi\n",
           key);
    printf("the Wi-Fi networks are a list of ssid/password pairs and are not set from here\n");
    return 1;
}

int CmdConfig(int argc, char **argv) {
    if (argc == 4 && strcmp(argv[1], "set") == 0) {
        return SetConfigField(argv[2], argv[3]);
    }

    if (argc >= 2 && strcmp(argv[1], "zones") == 0) {
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
        printf("usage: config [reload|save|restore]\n");
        printf("       config set <field> <value>\n");
        printf("       config zones [filter]\n");
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
        } else {
            printf("expected reload, save or restore; got '%s'\n", what);
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
    }

    const config::Data &c = config::Get();
    printf("source     %s%s\n", config::kPath, config::Loaded() ? "" : " (built-in defaults)");
    printf("wifi       %s, %u network(s)\n", c.wifi.active ? "on" : "off",
           static_cast<unsigned>(c.wifi.network_count));
    for (uint8_t i = 0; i < c.wifi.network_count; ++i) {
        // **The password is never printed** (§10.8.6, §10.15): it is a secret
        // from the moment it is typed, and a console dump is exactly the place
        // it must not turn up.
        printf("           %u. %s (password %s)\n", static_cast<unsigned>(i + 1),
               c.wifi.networks[i].ssid,
               c.wifi.networks[i].password[0] == '\0' ? "not set" : "set");
    }
    printf("nats       %s\n", c.nats.url);
    const int offset = tz::OffsetSeconds();
    printf("time       %s (%s), UTC%+03d:%02d%s, sntp %s\n", c.time.zone, c.time.posix,
           offset / 3600, abs(offset % 3600) / 60, tz::IsDaylightSaving() ? ", DST now" : "",
           c.time.sntp_server);
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

const esp_console_cmd_t kCommands[] = {
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
        .help = "print the parsed config, or reload / save / restore it (§10.15)",
        .hint = "[reload|save|restore]",
        .func = &CmdConfig,
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
        .help = "read the RTC, or 'date set <YYYY-MM-DD> <HH:MM:SS>' to write it",
        .hint = "[set <YYYY-MM-DD> <HH:MM:SS>]",
        .func = &CmdDate,
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
