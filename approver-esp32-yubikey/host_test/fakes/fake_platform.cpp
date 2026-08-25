#include "fake_platform.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "driver/i2c_master.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "rom/ets_sys.h"

namespace fake {

namespace {

Platform platform;

// A device handle is an index into `platform.devices`, offset by one so that a
// valid handle is never null. Nothing is allocated (the rule the firmware lives
// by, kept here so the fake cannot leak either) and a stale handle from before
// a `Recover` is detectable rather than a dangling pointer.
struct Handle {
    size_t index;
    uint32_t clock_hz;
    bool open;
};

constexpr size_t kMaxHandles = Platform::kMaxDevices * 2;
Handle handles[kMaxHandles];

// Which address each handle was opened for. The real driver keeps this inside
// the handle; here it is a parallel array so the transfer functions can name
// the chip a handle reaches without the production code telling them twice.
uint8_t handle_address[kMaxHandles];

// The bus handle is a single non-null token; the driver only ever checks it
// against null and passes it back.
i2c_master_bus_t *const kBusToken = reinterpret_cast<i2c_master_bus_t *>(1);

Transfer *Record(Transfer::Kind kind, uint8_t address, uint32_t clock_hz) {
    if (platform.transfer_count >= Platform::kMaxTransfers) {
        platform.transfers_overflowed = true;
        return nullptr;
    }
    Transfer *t = &platform.transfers[platform.transfer_count++];
    *t = {};
    t->kind = kind;
    t->address = address;
    t->clock_hz = clock_hz;
    t->epoch = platform.epoch;
    return t;
}

// One shot, and cleared whether or not it matched the address — an injected
// error that silently stayed armed would poison the transfer after the one the
// test meant.
bool TakeInjectedError(uint8_t address, esp_err_t *out) {
    if (platform.next_error == ESP_OK) {
        return false;
    }
    if (platform.next_error_address != kAnyAddress &&
        platform.next_error_address != address) {
        return false;
    }
    if (platform.next_error_skip > 0) {
        --platform.next_error_skip;
        return false;
    }
    *out = platform.next_error;
    platform.next_error = ESP_OK;
    platform.next_error_address = kAnyAddress;
    platform.next_error_skip = 0;
    return true;
}

Handle *Resolve(i2c_master_dev_handle_t device) {
    if (device == nullptr) {
        return nullptr;
    }
    const size_t index = reinterpret_cast<size_t>(device) - 1;
    if (index >= kMaxHandles || !handles[index].open) {
        return nullptr;
    }
    return &handles[index];
}

}  // namespace

Platform &P() { return platform; }

void Reset() {
    platform = {};
    platform.next_error_address = kAnyAddress;
    std::memset(handles, 0, sizeof(handles));
    std::memset(handle_address, 0, sizeof(handle_address));
}

Device *AddDevice(uint8_t address) {
    if (platform.device_count >= Platform::kMaxDevices) {
        return nullptr;
    }
    Device *device = &platform.devices[platform.device_count++];
    *device = {};
    device->present = true;
    device->address = address;
    return device;
}

Device *DeviceAt(uint8_t address) {
    for (size_t i = 0; i < platform.device_count; ++i) {
        if (platform.devices[i].present && platform.devices[i].address == address) {
            return &platform.devices[i];
        }
    }
    return nullptr;
}

void FailNext(esp_err_t err, uint8_t address) { FailAfter(0, err, address); }

void FailAfter(size_t skip, esp_err_t err, uint8_t address) {
    platform.next_error = err;
    platform.next_error_address = address;
    platform.next_error_skip = skip;
}

size_t CountTransfers(Transfer::Kind kind) {
    size_t count = 0;
    for (size_t i = 0; i < platform.transfer_count; ++i) {
        if (platform.transfers[i].kind == kind) {
            ++count;
        }
    }
    return count;
}

const Transfer *LastTransfer() {
    if (platform.transfer_count == 0) {
        return nullptr;
    }
    return &platform.transfers[platform.transfer_count - 1];
}

bool OneLeaseSince(size_t first) {
    if (first >= platform.transfer_count) {
        // Nothing happened. Not an assertion this helper should make on its
        // own — a test that expected transfers checks the count itself.
        return true;
    }
    const uint32_t epoch = platform.transfers[first].epoch;
    for (size_t i = first + 1; i < platform.transfer_count; ++i) {
        if (platform.transfers[i].epoch != epoch) {
            return false;
        }
    }
    return true;
}

size_t CountLeasesSince(size_t first) {
    if (first >= platform.transfer_count) {
        return 0;
    }
    size_t leases = 1;
    uint32_t epoch = platform.transfers[first].epoch;
    for (size_t i = first + 1; i < platform.transfer_count; ++i) {
        if (platform.transfers[i].epoch != epoch) {
            epoch = platform.transfers[i].epoch;
            ++leases;
        }
    }
    return leases;
}

const Transfer *FindWrite(uint8_t address, uint8_t reg) {
    for (size_t i = platform.transfer_count; i > 0; --i) {
        const Transfer &t = platform.transfers[i - 1];
        if (t.kind == Transfer::Kind::kWrite && t.address == address &&
            t.write_length >= 2 && t.write[0] == reg) {
            return &t;
        }
    }
    return nullptr;
}

bool WroteRegister(uint8_t address, uint8_t reg, uint8_t value) {
    const Transfer *t = FindWrite(address, reg);
    return t != nullptr && t->write[1] == value;
}

void AdvanceMs(uint32_t ms) { platform.clock_us += static_cast<uint64_t>(ms) * 1000; }

void SetPinLevel(gpio_num_t pin, int level) {
    const size_t index = static_cast<size_t>(pin);
    if (pin < 0 || index >= Platform::kMaxPins) {
        return;
    }
    platform.level[index] = level != 0 ? 1 : 0;
    platform.level_forced[index] = true;
}

size_t RisingEdges(gpio_num_t pin) {
    const size_t index = static_cast<size_t>(pin);
    if (pin < 0 || index >= Platform::kMaxPins) {
        return 0;
    }
    return platform.rising_edges[index];
}

void TakeMutexFromAnotherTask() { platform.mutex_taken = true; }

void GiveMutexFromAnotherTask() { platform.mutex_taken = false; }

void Log(const char *level, const char *tag, const char *format, ...) {
    if (!platform.log_enabled) {
        return;
    }
    std::printf("%s (%s) ", level, tag);
    va_list args;
    va_start(args, format);
    std::vprintf(format, args);
    va_end(args);
    std::printf("\n");
}

}  // namespace fake

// --- esp_err ---------------------------------------------------------------

const char *esp_err_to_name(esp_err_t code) {
    switch (code) {
        case ESP_OK: return "ESP_OK";
        case ESP_FAIL: return "ESP_FAIL";
        case ESP_ERR_NO_MEM: return "ESP_ERR_NO_MEM";
        case ESP_ERR_INVALID_ARG: return "ESP_ERR_INVALID_ARG";
        case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
        case ESP_ERR_INVALID_SIZE: return "ESP_ERR_INVALID_SIZE";
        case ESP_ERR_NOT_FOUND: return "ESP_ERR_NOT_FOUND";
        case ESP_ERR_NOT_SUPPORTED: return "ESP_ERR_NOT_SUPPORTED";
        case ESP_ERR_TIMEOUT: return "ESP_ERR_TIMEOUT";
        default: return "ESP_ERR_?";
    }
}

// --- FreeRTOS --------------------------------------------------------------

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage) {
    storage->created = true;
    storage->taken = false;
    return storage;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t handle, TickType_t ticks) {
    fake::Platform &p = fake::P();
    p.last_take_ticks = ticks;
    ++p.take_calls;
    if (handle == nullptr || p.mutex_taken) {
        return pdFALSE;
    }
    p.mutex_taken = true;
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t handle) {
    fake::Platform &p = fake::P();
    ++p.give_calls;
    if (handle == nullptr) {
        return pdFALSE;
    }
    p.mutex_taken = false;
    // A new lease starts here as far as any later transfer is concerned.
    ++p.epoch;
    return pdTRUE;
}

void vTaskDelay(TickType_t ticks) {
    fake::Platform &p = fake::P();
    p.delay_ms_total += ticks;
    // A delay moves the clock. Without this, anything that polls in a loop
    // waiting for time to pass never finishes.
    p.clock_us += static_cast<uint64_t>(ticks) * 1000;
    if (p.mutex_taken) {
        // §10.14.3: a lease is held briefly and nothing sleeps under it. This
        // is the only place that rule can be observed at all — on hardware a
        // driver that breaks it still works.
        p.delay_ms_while_held += ticks;
    }
}

void esp_rom_delay_us(uint32_t us) { fake::P().clock_us += us; }

int64_t esp_timer_get_time(void) { return static_cast<int64_t>(fake::P().clock_us); }

// --- GPIO ------------------------------------------------------------------

esp_err_t gpio_config(const gpio_config_t *config) {
    fake::Platform &p = fake::P();
    for (size_t pin = 0; pin < fake::Platform::kMaxPins; ++pin) {
        if (((config->pin_bit_mask >> pin) & 1ULL) == 0) {
            continue;
        }
        p.last_mode[pin] = config->mode;
        // **The pull decides the idle level** — but only when nothing else
        // is driving the pin. A button held down shorts to ground and wins
        // over a pull-up, which is §10.15's scenario and would be silently
        // undone if configuring the pin reset it.
        if (p.level_forced[pin]) {
            continue;
        }
        if (config->pull_up_en == GPIO_PULLUP_ENABLE) {
            p.level[pin] = 1;
        } else if (config->pull_down_en == GPIO_PULLDOWN_ENABLE) {
            p.level[pin] = 0;
        }
    }
    return ESP_OK;
}

esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level) {
    fake::Platform &p = fake::P();
    const size_t index = static_cast<size_t>(pin);
    if (pin < 0 || index >= fake::Platform::kMaxPins) {
        return ESP_ERR_INVALID_ARG;
    }
    // Rising edges, not calls: what `Recover` owes the bus is nine *clocks*,
    // and counting writes would let a driver that never went low pass.
    if (p.level[index] == 0 && level != 0) {
        ++p.rising_edges[index];
    }
    p.level[index] = level != 0 ? 1 : 0;
    return ESP_OK;
}

int gpio_get_level(gpio_num_t pin) {
    const size_t index = static_cast<size_t>(pin);
    if (pin < 0 || index >= fake::Platform::kMaxPins) {
        return 0;
    }
    return fake::P().level[index];
}

// --- The I2C master driver -------------------------------------------------

esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *config,
                             i2c_master_bus_handle_t *out) {
    fake::Platform &p = fake::P();
    (void)config;
    if (p.bus_open) {
        return ESP_ERR_INVALID_STATE;
    }
    p.bus_open = true;
    ++p.bus_open_count;
    *out = fake::kBusToken;
    return ESP_OK;
}

esp_err_t i2c_del_master_bus(i2c_master_bus_handle_t bus) {
    fake::Platform &p = fake::P();
    if (bus != fake::kBusToken || !p.bus_open) {
        return ESP_ERR_INVALID_ARG;
    }
    p.bus_open = false;
    ++p.bus_delete_count;
    return ESP_OK;
}

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus,
                                    const i2c_device_config_t *config,
                                    i2c_master_dev_handle_t *out) {
    fake::Platform &p = fake::P();
    if (bus != fake::kBusToken || !p.bus_open) {
        return ESP_ERR_INVALID_STATE;
    }
    for (size_t i = 0; i < fake::kMaxHandles; ++i) {
        if (fake::handles[i].open) {
            continue;
        }
        // **The address is not checked against what is on the wire, and that
        // is right**: opening a device handle is configuration, not a
        // transaction. A real bus only finds out at the first transfer, and a
        // fake that failed earlier would hide the case where a chip is absent.
        fake::handles[i].open = true;
        fake::handles[i].index = i;
        fake::handles[i].clock_hz = config->scl_speed_hz;
        fake::handle_address[i] = static_cast<uint8_t>(config->device_address);
        ++p.open_handles;
        *out = reinterpret_cast<i2c_master_dev_handle_t>(i + 1);
        return ESP_OK;
    }
    return ESP_ERR_NO_MEM;
}

esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t device) {
    fake::Handle *handle = fake::Resolve(device);
    if (handle == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    handle->open = false;
    --fake::P().open_handles;
    return ESP_OK;
}

esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus, uint16_t address, int timeout_ms) {
    fake::Platform &p = fake::P();
    (void)timeout_ms;
    if (bus != fake::kBusToken || !p.bus_open) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t addr = static_cast<uint8_t>(address);
    fake::Record(fake::Transfer::Kind::kProbe, addr, 0);

    esp_err_t injected = ESP_OK;
    if (fake::TakeInjectedError(addr, &injected)) {
        return injected;
    }
    // An address nobody answers at: `ESP_ERR_NOT_FOUND` is what the real
    // driver returns for a probe that goes unacknowledged.
    return fake::DeviceAt(addr) != nullptr ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t device, const uint8_t *data,
                              size_t length, int timeout_ms) {
    (void)timeout_ms;
    fake::Handle *handle = fake::Resolve(device);
    if (handle == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint8_t address = fake::handle_address[handle->index];

    fake::Transfer *t = fake::Record(fake::Transfer::Kind::kWrite, address, handle->clock_hz);
    if (t != nullptr) {
        t->write_length = length;
        const size_t copy = length < sizeof(t->write) ? length : sizeof(t->write);
        std::memcpy(t->write, data, copy);
    }

    esp_err_t injected = ESP_OK;
    if (fake::TakeInjectedError(address, &injected)) {
        return injected;
    }

    fake::Device *chip = fake::DeviceAt(address);
    if (chip == nullptr) {
        return ESP_FAIL;
    }
    if (length == 0) {
        return ESP_OK;
    }

    // `{reg}` moves the cursor; `{reg, v, ...}` writes from there. The shape
    // every chip on this board uses.
    chip->cursor = data[0];
    for (size_t i = 1; i < length; ++i) {
        chip->regs[chip->cursor] = data[i];
        ++chip->cursor;
    }
    return ESP_OK;
}

esp_err_t i2c_master_receive(i2c_master_dev_handle_t device, uint8_t *out, size_t length,
                             int timeout_ms) {
    (void)timeout_ms;
    fake::Handle *handle = fake::Resolve(device);
    if (handle == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint8_t address = fake::handle_address[handle->index];

    fake::Transfer *t = fake::Record(fake::Transfer::Kind::kRead, address, handle->clock_hz);
    if (t != nullptr) {
        t->read_length = length;
    }

    esp_err_t injected = ESP_OK;
    if (fake::TakeInjectedError(address, &injected)) {
        return injected;
    }

    fake::Device *chip = fake::DeviceAt(address);
    if (chip == nullptr) {
        return ESP_FAIL;
    }
    for (size_t i = 0; i < length; ++i) {
        out[i] = chip->regs[chip->cursor];
        ++chip->cursor;
    }
    return ESP_OK;
}

esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t device, const uint8_t *write,
                                      size_t write_length, uint8_t *read, size_t read_length,
                                      int timeout_ms) {
    (void)timeout_ms;
    fake::Handle *handle = fake::Resolve(device);
    if (handle == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint8_t address = fake::handle_address[handle->index];

    fake::Transfer *t =
        fake::Record(fake::Transfer::Kind::kWriteRead, address, handle->clock_hz);
    if (t != nullptr) {
        t->write_length = write_length;
        const size_t copy = write_length < sizeof(t->write) ? write_length : sizeof(t->write);
        std::memcpy(t->write, write, copy);
        t->read_length = read_length;
    }

    esp_err_t injected = ESP_OK;
    if (fake::TakeInjectedError(address, &injected)) {
        return injected;
    }

    fake::Device *chip = fake::DeviceAt(address);
    if (chip == nullptr) {
        return ESP_FAIL;
    }
    if (write_length > 0) {
        chip->cursor = write[0];
    }
    for (size_t i = 0; i < read_length; ++i) {
        read[i] = chip->regs[chip->cursor];
        ++chip->cursor;
    }
    return ESP_OK;
}
