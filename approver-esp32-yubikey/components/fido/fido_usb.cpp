// The USB host half of §10.18.4: two tasks, one claimed interface, and a
// blocking CTAPHID exchange on top of two interrupt endpoints.
//
// The framing is `ctaphid_frames.h` and is tested without a board; what is here
// is the part that only a board can test — enumeration, claiming the right
// interface out of the three a YubiKey presents, and surviving a key being
// pulled out mid-transfer.

#include "fido_usb.h"

#include <cstring>

#include "ctaphid_frames.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/usb_host.h"

namespace fido {
namespace usb {
namespace {

// How long to wait for the key's reply to a request we just cancelled, and how
// long each read inside that budget waits. A YubiKey answers a cancel in single
// digit milliseconds; the budget is generous because the cost of getting this
// wrong is the *next* exchange reading somebody else's answer.
constexpr uint32_t kDrainMs = 250;
constexpr uint32_t kDrainReadMs = 50;


constexpr const char *TAG = "fido-usb";

// The library's own event pump, and this driver's. Both are stack-only loops;
// the sizes are what the Host Library's examples use, rounded up once after the
// first stack-overflow panic during enumeration.
constexpr uint32_t kDaemonStackBytes = 4096;
constexpr uint32_t kClientStackBytes = 4096;

// **Above the responder** (which is 4). Enumeration and transfer completion are
// both latency-sensitive in a way a signature is not: a completion callback that
// is late holds the whole client event queue behind it, and one of the things in
// that queue may be "the key was unplugged".
constexpr int kDaemonPriority = 5;
constexpr int kClientPriority = 5;

// How long a single interrupt transfer may take. A key answers an OUT in
// microseconds and an IN either immediately or when it has something to say;
// this is the ceiling on one packet, not on an exchange.
constexpr uint32_t kTransferTimeoutMs = 2000;

// How long `CTAPHID_INIT` gets. It needs no fingertip, so a key that has not
// answered in a second is a key that is not going to.
constexpr uint32_t kInitTimeoutMs = 1000;

struct Endpoint {
    uint8_t address = 0;
    uint16_t max_packet = 0;
};

struct Runtime {
    bool ready = false;

    usb_host_client_handle_t client = nullptr;
    usb_device_handle_t device = nullptr;
    uint8_t address = 0;
    bool claimed = false;
    uint8_t interface_number = 0;

    Endpoint ep_in;
    Endpoint ep_out;

    usb_transfer_t *transfer_in = nullptr;
    usb_transfer_t *transfer_out = nullptr;

    // Given by each transfer's completion callback, taken by `Exchange`.
    SemaphoreHandle_t in_done = nullptr;
    SemaphoreHandle_t out_done = nullptr;
    StaticSemaphore_t in_done_storage{};
    StaticSemaphore_t out_done_storage{};

    // One exchange at a time. The console and the responder can both want the
    // key, and a second command interleaved into a CTAPHID conversation is a
    // wedged channel.
    SemaphoreHandle_t lock = nullptr;
    StaticSemaphore_t lock_storage{};

    // The private channel `CTAPHID_INIT` handed out, or the broadcast one when
    // no conversation has happened since the key was plugged in.
    uint32_t cid = ctaphid::kBroadcastCid;

    DeviceInfo info;
    Stats stats;

    // What the client task has been asked to do. Written in the event callback,
    // which the Host Library forbids blocking in.
    volatile bool want_open = false;
    volatile bool want_close = false;
    volatile uint8_t pending_address = 0;
};

Runtime runtime;

StackType_t daemon_stack[kDaemonStackBytes / sizeof(StackType_t)];
StaticTask_t daemon_storage;
StackType_t client_stack[kClientStackBytes / sizeof(StackType_t)];
StaticTask_t client_storage;

uint32_t NowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

// UTF-16LE, as USB string descriptors are, into ASCII with everything outside it
// replaced. These reach one console line; a full decoder would be a decoder for
// a line that says "YubiKey".
void CopyStringDescriptor(const usb_str_desc_t *desc, char *out, size_t capacity) {
    out[0] = '\0';
    if (desc == nullptr || capacity < 2) {
        return;
    }
    const size_t chars = (desc->bLength >= 2) ? (desc->bLength - 2) / 2 : 0;
    size_t written = 0;
    for (size_t i = 0; i < chars && written + 1 < capacity; i++) {
        const uint16_t unit = desc->wData[i];
        out[written++] = (unit >= 0x20 && unit < 0x7F) ? static_cast<char>(unit) : '?';
    }
    out[written] = '\0';
}

// **Which of a key's interfaces is the FIDO one.** A YubiKey 5 presents three:
// an HID keyboard (the OTP slot), the FIDO HID interface, and CCID. Picking the
// wrong one means writing CTAPHID frames at a keyboard.
//
// The discriminator is the endpoint shape rather than the HID report
// descriptor's usage page, and the trade is worth writing down. The correct
// answer is usage page 0xF1D0, which needs a control transfer for the report
// descriptor and a second descriptor format to parse. The shape — **class HID,
// an interrupt IN *and* an interrupt OUT, both 64 bytes** — is unambiguous on
// every FIDO key, because a keyboard interface has no OUT endpoint and no 64-byte
// reports.
//
// And the guess is *proved* rather than assumed: the first thing this driver
// does with a claimed interface is `CTAPHID_INIT`, and an interface that is not
// CTAPHID does not answer one. A wrong guess costs a log line, not a wedged key.
bool FindFidoInterface(const usb_config_desc_t *config, uint8_t *interface_number, Endpoint *in,
                       Endpoint *out) {
    if (config == nullptr) {
        return false;
    }
    for (uint8_t number = 0; number < config->bNumInterfaces; number++) {
        int offset = 0;
        const usb_intf_desc_t *intf = usb_parse_interface_descriptor(config, number, 0, &offset);
        if (intf == nullptr || intf->bInterfaceClass != USB_CLASS_HID) {
            continue;
        }

        Endpoint found_in;
        Endpoint found_out;
        for (int i = 0; i < intf->bNumEndpoints; i++) {
            int ep_offset = offset;
            const usb_ep_desc_t *ep =
                usb_parse_endpoint_descriptor_by_index(intf, i, config->wTotalLength, &ep_offset);
            if (ep == nullptr || USB_EP_DESC_GET_XFERTYPE(ep) != USB_BM_ATTRIBUTES_XFER_INT) {
                continue;
            }
            Endpoint candidate;
            candidate.address = ep->bEndpointAddress;
            candidate.max_packet = USB_EP_DESC_GET_MPS(ep);
            if (candidate.max_packet < ctaphid::kPacketSize) {
                continue;
            }
            if (USB_EP_DESC_GET_EP_DIR(ep)) {
                found_in = candidate;
            } else {
                found_out = candidate;
            }
        }

        if (found_in.address != 0 && found_out.address != 0) {
            *interface_number = number;
            *in = found_in;
            *out = found_out;
            return true;
        }
    }
    return false;
}

// **Task context, not an ISR** — which is worth stating because the shape of
// these two functions invites the wrong assumption. The Host Library calls
// completion callbacks from inside `usb_host_client_handle_events`, i.e. on the
// client task above, so the plain `xSemaphoreGive` is correct and the `FromISR`
// form would be a wrong-context call that happens to work until it does not.
//
// The rule that does still apply is the library's own: do not block in here.
// A give does not.
void TransferInDone(usb_transfer_t *transfer) {
    (void)transfer;
    xSemaphoreGive(runtime.in_done);
}

void TransferOutDone(usb_transfer_t *transfer) {
    (void)transfer;
    xSemaphoreGive(runtime.out_done);
}

void CloseDevice() {
    if (runtime.transfer_in != nullptr) {
        usb_host_transfer_free(runtime.transfer_in);
        runtime.transfer_in = nullptr;
    }
    if (runtime.transfer_out != nullptr) {
        usb_host_transfer_free(runtime.transfer_out);
        runtime.transfer_out = nullptr;
    }
    if (runtime.claimed) {
        usb_host_interface_release(runtime.client, runtime.device, runtime.interface_number);
        runtime.claimed = false;
    }
    if (runtime.device != nullptr) {
        usb_host_device_close(runtime.client, runtime.device);
        runtime.device = nullptr;
    }
    runtime.info = DeviceInfo{};
    runtime.cid = ctaphid::kBroadcastCid;
    // **Released, so that an exchange blocked on an unplugged key returns rather
    // than holding the lock forever.** The exchange itself notices `present` went
    // false and reports `kUnplugged`.
    xSemaphoreGive(runtime.in_done);
    xSemaphoreGive(runtime.out_done);
}

void OpenDevice(uint8_t address) {
    if (runtime.device != nullptr) {
        return;
    }
    esp_err_t err = usb_host_device_open(runtime.client, address, &runtime.device);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "device_open(%u): %s", address, esp_err_to_name(err));
        runtime.device = nullptr;
        return;
    }

    usb_device_info_t device_info = {};
    const usb_device_desc_t *device_desc = nullptr;
    const usb_config_desc_t *config_desc = nullptr;
    usb_host_device_info(runtime.device, &device_info);
    usb_host_get_device_descriptor(runtime.device, &device_desc);
    usb_host_get_active_config_descriptor(runtime.device, &config_desc);

    uint8_t interface_number = 0;
    Endpoint ep_in;
    Endpoint ep_out;
    if (!FindFidoInterface(config_desc, &interface_number, &ep_in, &ep_out)) {
        runtime.stats.rejected++;
        ESP_LOGW(TAG, "device %u has no FIDO HID interface; ignoring it", address);
        usb_host_device_close(runtime.client, runtime.device);
        runtime.device = nullptr;
        return;
    }

    err = usb_host_interface_claim(runtime.client, runtime.device, interface_number, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "interface_claim(%u): %s", interface_number, esp_err_to_name(err));
        usb_host_device_close(runtime.client, runtime.device);
        runtime.device = nullptr;
        return;
    }
    runtime.claimed = true;
    runtime.interface_number = interface_number;
    runtime.ep_in = ep_in;
    runtime.ep_out = ep_out;

    // §10.18.4: the one allocation this component makes, at a device boundary,
    // freed when the key is unplugged. Never per exchange.
    if (usb_host_transfer_alloc(ctaphid::kPacketSize, 0, &runtime.transfer_in) != ESP_OK ||
        usb_host_transfer_alloc(ctaphid::kPacketSize, 0, &runtime.transfer_out) != ESP_OK) {
        ESP_LOGE(TAG, "no memory for the two 64-byte transfers");
        CloseDevice();
        return;
    }

    runtime.address = address;
    runtime.info.present = true;
    runtime.info.address = address;
    runtime.info.interface_number = interface_number;
    runtime.info.endpoint_in = ep_in.address;
    runtime.info.endpoint_out = ep_out.address;
    if (device_desc != nullptr) {
        runtime.info.vendor_id = device_desc->idVendor;
        runtime.info.product_id = device_desc->idProduct;
    }
    CopyStringDescriptor(device_info.str_desc_manufacturer, runtime.info.manufacturer,
                         sizeof(runtime.info.manufacturer));
    CopyStringDescriptor(device_info.str_desc_product, runtime.info.product,
                         sizeof(runtime.info.product));

    runtime.cid = ctaphid::kBroadcastCid;
    runtime.stats.claimed++;
    ESP_LOGI(TAG, "%04x:%04x '%s' on interface %u (in 0x%02x, out 0x%02x)",
             runtime.info.vendor_id, runtime.info.product_id, runtime.info.product,
             interface_number, ep_in.address, ep_out.address);
}

void ClientEvent(const usb_host_client_event_msg_t *msg, void *) {
    // Called from inside `usb_host_client_handle_events`. Nothing may block here.
    switch (msg->event) {
        case USB_HOST_CLIENT_EVENT_NEW_DEV:
            runtime.pending_address = msg->new_dev.address;
            runtime.want_open = true;
            runtime.stats.attached++;
            break;
        case USB_HOST_CLIENT_EVENT_DEV_GONE:
            runtime.want_close = true;
            runtime.stats.detached++;
            break;
        default:
            break;
    }
}

void DaemonTask(void *) {
    for (;;) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if ((flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) != 0) {
            usb_host_device_free_all();
        }
    }
}

void ClientTask(void *) {
    // Zero-initialised and then filled field by field, for the reason
    // `usb_host_config_t` gets the same treatment in `Init` below: this struct
    // has grown a `flags` member, and a designated initialiser that names every
    // field is a `-Werror=missing-field-initializers` failure on whichever
    // version has one more.
    usb_host_client_config_t config = {};
    config.is_synchronous = false;
    config.max_num_event_msg = 5;
    config.async.client_event_callback = ClientEvent;
    config.async.callback_arg = nullptr;
    if (usb_host_client_register(&config, &runtime.client) != ESP_OK) {
        ESP_LOGE(TAG, "client_register failed; there will be no key on this device");
        vTaskDelete(nullptr);
        return;
    }
    runtime.ready = true;

    for (;;) {
        usb_host_client_handle_events(runtime.client, pdMS_TO_TICKS(100));
        if (runtime.want_close) {
            runtime.want_close = false;
            CloseDevice();
        }
        if (runtime.want_open) {
            runtime.want_open = false;
            OpenDevice(runtime.pending_address);
        }
    }
}

// One 64-byte report out. Blocks until the completion callback fires or the
// deadline passes.
bool WritePacket(const uint8_t *packet) {
    if (!runtime.info.present || runtime.transfer_out == nullptr) {
        return false;
    }
    std::memcpy(runtime.transfer_out->data_buffer, packet, ctaphid::kPacketSize);
    runtime.transfer_out->num_bytes = ctaphid::kPacketSize;
    runtime.transfer_out->device_handle = runtime.device;
    runtime.transfer_out->bEndpointAddress = runtime.ep_out.address;
    runtime.transfer_out->callback = TransferOutDone;
    runtime.transfer_out->context = nullptr;

    xSemaphoreTake(runtime.out_done, 0);  // drain a stale give
    if (usb_host_transfer_submit(runtime.transfer_out) != ESP_OK) {
        runtime.stats.transfer_errors++;
        return false;
    }
    if (xSemaphoreTake(runtime.out_done, pdMS_TO_TICKS(kTransferTimeoutMs)) != pdTRUE) {
        runtime.stats.transfer_errors++;
        return false;
    }
    return runtime.info.present && runtime.transfer_out->status == USB_TRANSFER_STATUS_COMPLETED;
}

// One 64-byte report in. `deadline_ms` is the whole exchange's, not this
// packet's — an IN that returns nothing is retried until the exchange gives up,
// which is how a key that is waiting for a fingertip is waited on.
bool ReadPacket(uint8_t *packet, uint32_t wait_ms) {
    if (!runtime.info.present || runtime.transfer_in == nullptr) {
        return false;
    }
    runtime.transfer_in->num_bytes = ctaphid::kPacketSize;
    runtime.transfer_in->device_handle = runtime.device;
    runtime.transfer_in->bEndpointAddress = runtime.ep_in.address;
    runtime.transfer_in->callback = TransferInDone;
    runtime.transfer_in->context = nullptr;

    xSemaphoreTake(runtime.in_done, 0);
    if (usb_host_transfer_submit(runtime.transfer_in) != ESP_OK) {
        runtime.stats.transfer_errors++;
        return false;
    }
    if (xSemaphoreTake(runtime.in_done, pdMS_TO_TICKS(wait_ms)) != pdTRUE) {
        return false;
    }
    if (!runtime.info.present) {
        return false;
    }
    if (runtime.transfer_in->status != USB_TRANSFER_STATUS_COMPLETED ||
        runtime.transfer_in->actual_num_bytes < static_cast<int>(ctaphid::kPacketSize)) {
        return false;
    }
    std::memcpy(packet, runtime.transfer_in->data_buffer, ctaphid::kPacketSize);
    return true;
}

// **A cancelled request still gets an answer, and somebody has to collect it.**
// A key told `CTAPHID_CANCEL` abandons the request and replies to it with
// `CTAP2_ERR_KEEPALIVE_CANCEL` (0x2D). Left in the pipe, that reply is what the
// *next* exchange reads as its own — and the next exchange on this device is the one
// that matters: a tap on BOOT cancels the request for an `allow`, the request for a
// signed `deny` goes out immediately after, and the first thing it read back was the
// allow's `0x2D`. Fourteen milliseconds, `Gate::kCancelled`, and a red light that
// existed for seventeen (§10.18.5). The deny path had never worked, and this is why.
//
// Bounded, and a failure to drain is not reported: the exchange being abandoned has
// already failed, and the caller is being told about the cancel, not about this.
void DrainAfterCancel(uint32_t cid) {
    ctaphid::Reader reader;
    reader.Reset();
    const uint32_t deadline = NowMs() + kDrainMs;
    uint8_t packet[ctaphid::kPacketSize];
    while (static_cast<int32_t>(deadline - NowMs()) > 0) {
        if (!ReadPacket(packet, kDrainReadMs)) {
            if (!runtime.info.present) {
                return;
            }
            continue;
        }
        const ctaphid::Reader::Result result = reader.Feed(packet, sizeof(packet), cid);
        if (result == ctaphid::Reader::Result::kComplete ||
            result == ctaphid::Reader::Result::kError) {
            return;
        }
    }
    ESP_LOGW(TAG, "the cancelled request was not answered within %u ms - the next exchange "
                  "may read its reply",
             static_cast<unsigned>(kDrainMs));
}

// Sends `CTAPHID_CANCEL` and collects what it produces. Both cancel sites go
// through here so that neither can forget the second half.
void CancelAndDrain(uint32_t cid) {
    uint8_t packet[ctaphid::kPacketSize];
    ctaphid::Writer cancel;
    cancel.Begin(cid, ctaphid::kCmdCancel, nullptr, 0);
    if (cancel.Next(packet, sizeof(packet))) {
        WritePacket(packet);
    }
    DrainAfterCancel(cid);
}

// The exchange, with the lock already held and the channel already open.
esp_err_t Transact(uint32_t cid, uint8_t command, const uint8_t *request, size_t request_length,
                   uint8_t *response, size_t response_capacity, size_t *response_length,
                   uint32_t timeout_ms, KeepAlive keep_alive, void *keep_alive_context,
                   Fault *fault, uint8_t *key_error) {
    ctaphid::Writer writer;
    writer.Begin(cid, command, request, request_length);

    uint8_t packet[ctaphid::kPacketSize];
    while (writer.Next(packet, sizeof(packet))) {
        if (!WritePacket(packet)) {
            *fault = runtime.info.present ? Fault::kTimeout : Fault::kUnplugged;
            return ESP_FAIL;
        }
    }

    const uint32_t deadline = NowMs() + timeout_ms;
    ctaphid::Reader reader;
    reader.Reset();

    for (;;) {
        const int32_t left = static_cast<int32_t>(deadline - NowMs());
        if (left <= 0) {
            runtime.stats.timeouts++;
            *fault = Fault::kTimeout;
            return ESP_ERR_TIMEOUT;
        }
        // Never wait longer than a keep-alive interval, so that the caller's
        // cancel hook is offered the chance to fire even on a key that has gone
        // quiet.
        //
        // **300 ms is a floor as well as a ceiling, and that is worth knowing.**
        // Shortening it looks free and is not: `ReadPacket` submits a fresh IN
        // transfer on every call and abandons the previous one when it times out, so
        // a wait short enough to expire before the key answers re-submits a transfer
        // that is still queued. Tried at 10 ms — to poll the deny button faster —
        // this became `the key could not be reached` sixty milliseconds into every
        // exchange, with the key never lighting up. Whatever needs to be sampled
        // faster than this has to be sampled somewhere else; `buttons` grew a poller
        // of its own for exactly that reason.
        uint32_t wait = static_cast<uint32_t>(left);
        if (wait > 300) {
            wait = 300;
        }

        if (!ReadPacket(packet, wait)) {
            if (!runtime.info.present) {
                *fault = Fault::kUnplugged;
                return ESP_FAIL;
            }
            // Nothing yet. Offer the caller a chance to give up, the same as a
            // keep-alive would, so that a cancelled request stops asking.
            if (keep_alive != nullptr && !keep_alive(0, keep_alive_context)) {
                CancelAndDrain(cid);
                *fault = Fault::kCancelled;
                return ESP_FAIL;
            }
            continue;
        }

        const ctaphid::Reader::Result result = reader.Feed(packet, sizeof(packet), cid);
        if (result == ctaphid::Reader::Result::kIgnored) {
            continue;
        }
        if (result == ctaphid::Reader::Result::kError) {
            runtime.stats.protocol_errors++;
            ESP_LOGW(TAG, "framing: %s", reader.ErrorText());
            *fault = Fault::kProtocol;
            return ESP_FAIL;
        }
        if (result == ctaphid::Reader::Result::kNeedMore) {
            continue;
        }

        // A whole message.
        if (reader.Command() == ctaphid::kCmdKeepAlive) {
            const uint8_t status = reader.Length() > 0 ? reader.Data()[0] : 0;
            if (keep_alive != nullptr && !keep_alive(status, keep_alive_context)) {
                CancelAndDrain(cid);
                *fault = Fault::kCancelled;
                return ESP_FAIL;
            }
            reader.Reset();
            continue;
        }

        if (reader.Command() == ctaphid::kCmdError) {
            const uint8_t code = reader.Length() > 0 ? reader.Data()[0] : ctaphid::kErrOther;
            runtime.stats.key_errors++;
            if (key_error != nullptr) {
                *key_error = code;
            }
            ESP_LOGW(TAG, "the key answered CTAPHID_ERROR %02x (%s)", code,
                     ctaphid::ErrorName(code));
            // **One of these has a fix and it is not on this device**, so it gets its
            // own sentence rather than being filed under "the key could not be
            // reached". `CHANNEL_BUSY` means the key is still holding a transaction
            // on *another* channel — most often this board's own, from before a
            // reset: reflash while a request is waiting for a fingertip and the key
            // goes on waiting for a channel that no longer has anybody on it. It
            // frees itself when its own user-presence timeout expires, and unplugging
            // it is instant. Two runs were spent chasing this as a firmware bug
            // because the log said the key was unreachable.
            if (code == ctaphid::kErrChannelBusy) {
                ESP_LOGW(TAG, "the key is mid-transaction on another channel - it was probably "
                              "left waiting by a reset. unplug it and plug it back in, or wait "
                              "for its own timeout");
            }
            *fault = Fault::kKeyError;
            return ESP_FAIL;
        }

        if (reader.Command() != command) {
            // An answer to something else on our own channel. Not fatal, and not
            // ours: drop it and keep waiting for the one we asked for.
            reader.Reset();
            continue;
        }

        if (reader.Length() > response_capacity) {
            *fault = Fault::kTooLong;
            return ESP_ERR_INVALID_SIZE;
        }
        std::memcpy(response, reader.Data(), reader.Length());
        *response_length = reader.Length();
        *fault = Fault::kNone;
        return ESP_OK;
    }
}

// Opens a private channel. Called on the first exchange after a key appears.
bool OpenChannel(Fault *fault) {
    uint8_t nonce[ctaphid::kNonceSize];
    esp_fill_random(nonce, sizeof(nonce));

    uint8_t response[ctaphid::kInitResponseSize + 8];
    size_t length = 0;
    uint8_t key_error = 0;
    const esp_err_t err =
        Transact(ctaphid::kBroadcastCid, ctaphid::kCmdInit, nonce, sizeof(nonce), response,
                 sizeof(response), &length, kInitTimeoutMs, nullptr, nullptr, fault, &key_error);
    if (err != ESP_OK) {
        return false;
    }
    if (length < ctaphid::kInitResponseSize ||
        std::memcmp(response, nonce, ctaphid::kNonceSize) != 0) {
        // **The nonce is what makes this reply ours.** On a channel every piece
        // of software on the bus shares, a reply that does not echo our nonce is
        // somebody else's channel being handed out.
        runtime.stats.protocol_errors++;
        *fault = Fault::kProtocol;
        return false;
    }
    runtime.cid = (static_cast<uint32_t>(response[8]) << 24) |
                  (static_cast<uint32_t>(response[9]) << 16) |
                  (static_cast<uint32_t>(response[10]) << 8) | static_cast<uint32_t>(response[11]);
    const uint8_t capabilities = response[16];
    ESP_LOGI(TAG, "channel %08lx, CTAP2 %s", static_cast<unsigned long>(runtime.cid),
             (capabilities & static_cast<uint8_t>(ctaphid::kCapCbor)) != 0 ? "yes" : "no");
    return true;
}

}  // namespace

const char *FaultName(Fault fault) {
    switch (fault) {
        case Fault::kNone:
            return "ok";
        case Fault::kNoDevice:
            return "no key on the OTG port";
        case Fault::kNotClaimed:
            return "the device on the port is not a FIDO key";
        case Fault::kBusy:
            return "the key is busy with another exchange";
        case Fault::kTimeout:
            return "the key did not answer in time";
        case Fault::kCancelled:
            return "cancelled";
        case Fault::kUnplugged:
            return "the key was unplugged";
        case Fault::kProtocol:
            return "malformed CTAPHID";
        case Fault::kKeyError:
            return "the key reported an error";
        case Fault::kTooLong:
            return "the answer is longer than this firmware will hold";
    }
    return "?";
}

esp_err_t Init() {
    if (runtime.ready) {
        return ESP_OK;
    }

    runtime.in_done = xSemaphoreCreateBinaryStatic(&runtime.in_done_storage);
    runtime.out_done = xSemaphoreCreateBinaryStatic(&runtime.out_done_storage);
    runtime.lock = xSemaphoreCreateMutexStatic(&runtime.lock_storage);
    if (runtime.in_done == nullptr || runtime.out_done == nullptr || runtime.lock == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    // **Zero-initialised and then two fields set, rather than a designated
    // initialiser for all of them.** `usb_host_config_t` has grown fields
    // between ESP-IDF versions (`root_port_unpowered`, `enum_filter_cb`), and a
    // full designated initialiser is a compile error on whichever version does
    // not have the newest one. The two set here are the two that have always
    // been there and are the two this board cares about: the PHY is the chip's
    // own, and the interrupt is a low-priority one so the LED's UART and the
    // Wi-Fi driver keep their slots.
    usb_host_config_t host_config = {};
    host_config.skip_phy_setup = false;
    host_config.intr_flags = ESP_INTR_FLAG_LEVEL1;
    const esp_err_t err = usb_host_install(&host_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install: %s", esp_err_to_name(err));
        return err;
    }

    if (xTaskCreateStatic(DaemonTask, "usb-daemon",
                          sizeof(daemon_stack) / sizeof(StackType_t), nullptr, kDaemonPriority,
                          daemon_stack, &daemon_storage) == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreateStatic(ClientTask, "usb-fido", sizeof(client_stack) / sizeof(StackType_t),
                          nullptr, kClientPriority, client_stack, &client_storage) == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool Ready() { return runtime.ready; }

bool Present() { return runtime.info.present; }

DeviceInfo Device() { return runtime.info; }

Stats GetStats() { return runtime.stats; }

esp_err_t Exchange(uint8_t command, const uint8_t *request, size_t request_length,
                   uint8_t *response, size_t response_capacity, size_t *response_length,
                   uint32_t timeout_ms, KeepAlive keep_alive, void *keep_alive_context,
                   Fault *fault, uint8_t *key_error) {
    Fault local_fault = Fault::kNone;
    uint8_t local_error = 0;
    if (fault == nullptr) {
        fault = &local_fault;
    }
    if (key_error == nullptr) {
        key_error = &local_error;
    }
    *fault = Fault::kNone;
    *key_error = 0;

    if (!runtime.ready) {
        *fault = Fault::kNoDevice;
        return ESP_ERR_INVALID_STATE;
    }
    if (!runtime.info.present) {
        *fault = Fault::kNoDevice;
        return ESP_ERR_NOT_FOUND;
    }
    if (xSemaphoreTake(runtime.lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        *fault = Fault::kBusy;
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ESP_OK;
    if (runtime.cid == ctaphid::kBroadcastCid) {
        if (!OpenChannel(fault)) {
            xSemaphoreGive(runtime.lock);
            return ESP_FAIL;
        }
    }

    runtime.stats.exchanges++;
    err = Transact(runtime.cid, command, request, request_length, response, response_capacity,
                   response_length, timeout_ms, keep_alive, keep_alive_context, fault, key_error);
    xSemaphoreGive(runtime.lock);
    return err;
}

}  // namespace usb
}  // namespace fido
