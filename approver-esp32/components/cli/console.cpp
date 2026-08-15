#include "console.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "board.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_console.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "storage.h"

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
        printf("%u file(s), %u bytes; partition %u of %u bytes used\n",
               static_cast<unsigned>(count), static_cast<unsigned>(listed_bytes),
               static_cast<unsigned>(used), static_cast<unsigned>(total));
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
    // powers the amplifier (§10.1). Both start off.
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

    const esp_console_dev_usb_serial_jtag_config_t dev_config =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();

    esp_err_t err = esp_console_new_repl_usb_serial_jtag(&dev_config, &repl_config, &repl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "console init failed: %s", esp_err_to_name(err));
        return err;
    }

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
