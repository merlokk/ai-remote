#pragma once

// **The cable** (CLAUDE.md §10.18.3): the ESP32-S3's own USB peripheral running
// as a *host*, one FIDO key on the other end of it, and a CTAPHID exchange that
// blocks until the key answers or the operator gives up.
//
// This is the layer that knows about USB and knows nothing else. It does not
// know what CTAP2 is (`ctap2.h`), what an approval is (`fido.h`), or what a
// verdict is (`responder.h`). What it exposes is a request-response: hand it a
// command byte and a payload, get a payload back.
//
// ## Three things about this board that shape the whole component
//
//   * **the OTG port is the *second* USB-C on the board, and the console is on
//     the first** (§10.1). The YD-ESP32-S3 has a CH343P bridge on one connector
//     and the S3's native USB on the other, which is the only reason a device
//     that is a USB *host* can still be talked to over USB. On the C6 board of
//     the sibling folder the two were the same port and this design would have
//     been impossible;
//   * **VBUS comes from the board, not from this firmware.** There is no
//     switchable 5 V supply here — the OTG connector's VBUS is tied to the 5 V
//     rail, so a key gets power whenever the board does. What follows is that
//     this component cannot power-cycle a key that has wedged, and `keyreset` on
//     the console says so rather than pretending;
//   * **a key can be unplugged at any moment, including mid-exchange.** That is
//     not an error path to be tidy about, it is the ordinary case: the operator
//     carries the thing. Every wait here has a deadline and a disconnect
//     cancels rather than hangs.
//
// ## What it costs, and the one rule it bends
//
// §10.14.1 forbids dynamic memory. The USB Host Library allocates its transfer
// buffers through `usb_host_transfer_alloc`, and there is no static form of it.
// That is **two 64-byte transfers, allocated when a key is plugged in and freed
// when it is unplugged** — a one-shot path at a device boundary, which is the
// same allowance `config.cpp` takes for cJSON and states in the same words. It
// is not a per-exchange allocation and it must never become one.

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace fido {
namespace usb {

// What a plugged-in key says it is. Strings are copied out of the device's own
// descriptors and truncated to fit — they reach a console line and nothing else.
struct DeviceInfo {
    bool present = false;
    uint16_t vendor_id = 0;
    uint16_t product_id = 0;
    uint8_t address = 0;
    char manufacturer[32] = {};
    char product[48] = {};

    // The interface this firmware claimed and the two endpoints on it, for a
    // console readout — and, when a key is not recognised, for the one line that
    // says what was seen instead.
    uint8_t interface_number = 0;
    uint8_t endpoint_in = 0;
    uint8_t endpoint_out = 0;
};

// Why an exchange did not happen, in words. Kept apart from `esp_err_t` because
// "no key plugged in" and "the key said no" are both `ESP_FAIL` to a caller that
// only has an error code, and they are the two things an operator most needs
// told apart.
enum class Fault : uint8_t {
    kNone = 0,
    kNoDevice,      // nothing on the port
    kNotClaimed,    // something on the port, and it is not a FIDO key
    kBusy,          // another exchange is running
    kTimeout,       // the key never finished
    kCancelled,     // the operator or the caller gave up
    kUnplugged,     // it was there when we started
    kProtocol,      // a malformed CTAPHID frame
    kKeyError,      // the key answered with CTAPHID_ERROR
    kTooLong,       // the answer is longer than this firmware will hold
};

const char *FaultName(Fault fault);

// Called while a key is holding an exchange open waiting to be touched, roughly
// twice a second. The device uses it to keep the LED honest — a key that is
// waiting for a finger and a key that has stopped answering look identical
// without it.
//
// Returning **false cancels the exchange**: a `CTAPHID_CANCEL` goes out and the
// call returns `kCancelled`. That is how a request that timed out on the bus
// stops asking the operator for a fingertip nobody is waiting for any more.
using KeepAlive = bool (*)(uint8_t status, void *context);

// Brings up the USB Host Library and starts the two tasks it needs — a daemon
// for the library and a client for this driver. **Not fatal if it fails**
// (§10.10): a device whose USB host will not start is a device that cannot
// approve, which is the safe direction.
esp_err_t Init();
bool Ready();

// Is there a claimed FIDO interface on the port right now. This is what
// `indicator` asks every tick, so it is a plain read of a flag and takes no lock.
bool Present();

DeviceInfo Device();

// One CTAPHID exchange on this device's private channel, blocking until the key
// answers, the deadline passes, or the key is unplugged.
//
// `command` is a `ctaphid::Cmd`. The channel is opened lazily with
// `CTAPHID_INIT` on the first exchange after a key is plugged in, and the
// caller never sees that.
//
// On a `CTAPHID_ERROR` from the key the result is `kKeyError` and `*key_error`
// carries the code; on every other failure `*key_error` is zero.
esp_err_t Exchange(uint8_t command, const uint8_t *request, size_t request_length,
                   uint8_t *response, size_t response_capacity, size_t *response_length,
                   uint32_t timeout_ms, KeepAlive keep_alive, void *keep_alive_context,
                   Fault *fault, uint8_t *key_error);

// Counters for the console. Every one of them is a thing that has gone wrong
// with a cable, which is the class of fault hardest to reason about from a log.
struct Stats {
    uint32_t attached = 0;
    uint32_t detached = 0;
    uint32_t claimed = 0;
    uint32_t rejected = 0;  // a device on the port with no FIDO interface
    uint32_t exchanges = 0;
    uint32_t timeouts = 0;
    uint32_t protocol_errors = 0;
    uint32_t key_errors = 0;
    uint32_t transfer_errors = 0;
};

Stats GetStats();

}  // namespace usb
}  // namespace fido
