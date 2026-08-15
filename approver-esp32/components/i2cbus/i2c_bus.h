#pragma once

// The one I²C bus, leased rather than shared (CLAUDE.md §10.14.3).
//
// Five chips hang off two wires — CST9220 touch, QMI8658 IMU, PCF85063 RTC,
// AXP2101 PMIC, ES8311/ES7210 codecs — and several tasks want them at once on a
// single core. So there is exactly one owner, and everybody else borrows:
//
//     if (auto lease = bus.Acquire()) {
//         lease.WriteRegister(kAddr, 0x30, value);   // a sequence, uninterrupted
//         lease.ReadRegister(kAddr, 0x34, buf, 2);
//     }                                              // released here, on every path
//
// **The lease exists to make a sequence atomic**, not a single transfer. One
// transaction needs no help; a read-modify-write on the PMIC does. §10.14.3
// argues this against the house firmware's per-call locking, whose cost is
// visible there as bespoke "do several things at once" methods.
//
// A failed acquire is a logged skip, never a block: `Held()` is false and every
// transfer returns ESP_ERR_INVALID_STATE, and the caller decides what a miss
// means. A task waiting forever on a wedged bus is a watchdog panic with a
// confusing name.
//
// Library layer: it knows about wires, and nothing about approvals (§10.14.2).

#include <cstddef>
#include <cstdint>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace i2cbus {

// The default clock. **Speed is per device**, not one number for the wire:
// `driver/i2c_master.h` puts `scl_speed_hz` in the *device* config, and the
// vendor's own driver for this board talks to the AXP2101 at 100 kHz while
// nothing else has to slow down for it. §10.14.3 used to say the opposite; it
// now says this, and the argument is recorded there.
//
// A device that says nothing gets this.
inline constexpr uint32_t kClockHz = 400000;

// Long enough that a slow chip finishes, short enough that a wedged one does
// not take the frame with it.
inline constexpr uint32_t kDefaultAcquireMs = 50;
inline constexpr int kTransferTimeoutMs = 100;

// Device handles are cached per address in a fixed table — no heap of ours
// (§10.14.1), and this board has five chips on the bus.
inline constexpr size_t kMaxDevices = 8;

class Bus;

// The scope guard. Non-copyable, movable, and releasing is the destructor —
// "release" is not a line anyone can forget to write.
class Lease {
   public:
    explicit Lease(Bus *bus) : bus_(bus) {}
    ~Lease();

    Lease(const Lease &) = delete;
    Lease &operator=(const Lease &) = delete;
    Lease(Lease &&other) noexcept : bus_(other.bus_) { other.bus_ = nullptr; }
    Lease &operator=(Lease &&) = delete;

    bool Held() const { return bus_ != nullptr; }
    explicit operator bool() const { return Held(); }

    esp_err_t Probe(uint8_t address);
    esp_err_t Write(uint8_t address, const uint8_t *data, size_t length);
    esp_err_t Read(uint8_t address, uint8_t *out, size_t length);
    esp_err_t WriteRead(uint8_t address, const uint8_t *write, size_t write_length,
                        uint8_t *read, size_t read_length);

    // The two shapes every chip on this board actually uses.
    esp_err_t ReadRegister(uint8_t address, uint8_t reg, uint8_t *out, size_t length);
    esp_err_t WriteRegister(uint8_t address, uint8_t reg, uint8_t value);

   private:
    Bus *bus_;
};

class Bus {
   public:
    Bus() = default;
    Bus(const Bus &) = delete;
    Bus &operator=(const Bus &) = delete;

    // Trivial constructor, separate Init (§10.14.1): a static object whose
    // constructor touched I²C would be a boot crash naming the wrong thing.
    //
    // The pins are arguments, not an include of `board.h`: this layer knows
    // about wires, not about which board they are on (§10.14.2).
    esp_err_t Init(gpio_num_t scl, gpio_num_t sda, uint32_t clock_hz = kClockHz);
    bool Ready() const { return handle_ != nullptr; }

    // Declares the clock a particular chip is talked to at. Call it before the
    // first transfer to that address — and **not** while holding a lease, since
    // it takes the bus itself. A chip that never calls this runs at kClockHz.
    esp_err_t AddDevice(uint8_t address, uint32_t clock_hz);

    Lease Acquire(uint32_t timeout_ms = kDefaultAcquireMs);

    // The raw bus handle, for a third-party driver that has to open its own
    // device on this bus. `esp_lcd_touch` is the one that does, and it is the
    // reason this exists.
    //
    // **It is not permission to skip the lease.** Whoever calls such a driver
    // takes the lease around the call, so the driver's transfers land inside a
    // critical section it has never heard of — `display::Touch::Read` is the
    // worked example, and `touch.h` argues why the short path was refused.
    i2c_master_bus_handle_t Handle() const { return handle_; }

    // A slave holding SDA low is a known failure with a known fix: clock it out
    // until it lets go, then re-init. Handled once, here, rather than five
    // times in five drivers. Bounded, and one log line.
    esp_err_t Recover();

   private:
    friend class Lease;

    void Release();
    esp_err_t DeviceFor(uint8_t address, i2c_master_dev_handle_t *out);
    esp_err_t OpenDevice(uint8_t address, uint32_t clock_hz, i2c_master_dev_handle_t *out);
    void ForgetDevices();

    struct DeviceSlot {
        uint8_t address;
        bool used;
        uint32_t clock_hz;
        i2c_master_dev_handle_t handle;
    };

    i2c_master_bus_handle_t handle_ = nullptr;
    gpio_num_t scl_ = GPIO_NUM_NC;
    gpio_num_t sda_ = GPIO_NUM_NC;
    uint32_t clock_hz_ = kClockHz;
    SemaphoreHandle_t mutex_ = nullptr;
    StaticSemaphore_t mutex_storage_ = {};
    DeviceSlot devices_[kMaxDevices] = {};
};

}  // namespace i2cbus
