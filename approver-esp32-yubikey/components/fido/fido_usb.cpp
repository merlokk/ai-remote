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
// long each read inside that budget waits. The cost of getting this wrong is the
// *next* exchange reading somebody else's answer, so it is generous — and 250 ms
// was not: a real cancel on the board ran out of it and said so.
constexpr uint32_t kDrainMs = 1000;
constexpr uint32_t kDrainReadMs = 50;

// How long to wait for a cancelled transfer's completion after flushing the
// endpoint it was queued on. The cancellation arrives the ordinary way — a
// completion callback on the client task — so this is a task handoff rather than
// anything on the bus, and 200 ms is three orders of magnitude more than it
// needs.
constexpr uint32_t kReclaimMs = 200;

// The same job on the one task that may not wait for that callback, because it
// is the task that delivers it: `CloseDevice` pumps its own event queue instead.
// Twenty rounds of 10 ms is the same ceiling expressed the other way.
constexpr int kCloseDrainRounds = 20;

// How long to leave a key that answered `CHANNEL_BUSY` before asking it again,
// and how often the caller's cancel hook is offered while waiting. §10.18.4 has
// the measurement these are sized against: the stale transaction is a
// user-presence wait and expires about 34 s after it started, so what matters is
// that the retry is cheap and cancellable rather than fast.
constexpr uint32_t kBusyRetryMs = 500;
constexpr uint32_t kBusyPollMs = 50;


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

    // **Whether the driver still owns each transfer** — the one fact this file
    // used not to track, and the one whose absence let a timed-out read poison
    // everything after it. A queued `usb_transfer_t` may not be submitted again
    // (`usb_host_transfer_submit` answers `ESP_ERR_NOT_FINISHED`), may not be read
    // out of, and may not be freed: `usb_host_transfer_free`'s contract says so in
    // one line, and the host controller writes into that buffer when the transfer
    // finally completes.
    //
    // Set by whoever submits, cleared by the completion callback, and `volatile`
    // for the same reason `want_open` below is: written on one task and read on
    // another. `Reclaim` is the only thing that ends a transfer early.
    volatile bool in_flight_in = false;
    volatile bool in_flight_out = false;

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
//
// **The flag is cleared here and given after**, in that order. A waiter that has
// taken the semaphore is then guaranteed to see the transfer as its own, which is
// what makes the flag rather than the semaphore the authority on ownership — the
// semaphore also carries `CloseDevice`'s wake-up give, which no transfer produced.
void TransferInDone(usb_transfer_t *transfer) {
    (void)transfer;
    runtime.in_flight_in = false;
    xSemaphoreGive(runtime.in_done);
}

void TransferOutDone(usb_transfer_t *transfer) {
    (void)transfer;
    runtime.in_flight_out = false;
    xSemaphoreGive(runtime.out_done);
}

// Halt an endpoint, cancel whatever is queued on it, and un-halt it. Return
// values are deliberately not logged: on a key that has just been unplugged all
// three answer `ESP_ERR_INVALID_STATE` and none of it is actionable. What is
// actionable is whether the transfer came back, which is `Reclaim`'s line.
void ResetEndpoint(uint8_t endpoint) {
    if (runtime.device == nullptr || endpoint == 0) {
        return;
    }
    usb_host_endpoint_halt(runtime.device, endpoint);
    usb_host_endpoint_flush(runtime.device, endpoint);
    usb_host_endpoint_clear(runtime.device, endpoint);
}

// **Take a transfer back from the driver so that it can be used again**, and the
// fix for the weakness this file used to only document.
//
// A read whose wait ran out returned with its IN transfer still queued. Every
// submit on that endpoint afterwards answered `ESP_ERR_NOT_FINISHED`, so
// `Transact`'s read loop had nothing left to block on and spun; `CloseDevice`,
// which an unplugged key reaches, freed a buffer the controller still owned.
//
// The documented way to end a queued transfer is through the endpoint rather than
// the transfer: halt the pipe, flush it — which cancels what is queued — and clear
// the halt. The cancellation is then delivered as an ordinary completion with
// `USB_TRANSFER_STATUS_CANCELED`, so this waits for it before calling the
// transfer ours.
//
// **A flush can race a real completion, and the caller has to care.** If the key
// answered in the moment between the wait expiring and the halt going out, the
// transfer completed normally and the packet in it is genuinely ours. So this
// reports only whether the transfer is safe to touch; the status is the caller's
// to read.
bool Reclaim(uint8_t endpoint, SemaphoreHandle_t done, volatile bool *in_flight) {
    if (!*in_flight) {
        return true;
    }
    if (runtime.device == nullptr) {
        // Nothing to halt and nobody left to answer. The library cancels a gone
        // device's transfers itself; what cannot be done from here is *prove* it,
        // so the transfer stays marked and is never touched again.
        return false;
    }

    ResetEndpoint(endpoint);
    if (xSemaphoreTake(done, pdMS_TO_TICKS(kReclaimMs)) != pdTRUE || *in_flight) {
        // Re-using this transfer now is the one thing that must not happen, so it
        // stays marked in flight — which makes this endpoint unusable until the
        // key is unplugged. Loud, because it has never been seen.
        runtime.stats.reclaims_failed++;
        ESP_LOGE(TAG, "endpoint 0x%02x did not give its transfer back", endpoint);
        return false;
    }
    runtime.stats.reclaims++;
    return true;
}

// **Nothing here may be handed back while the driver still owns it**, and that
// applies to all three things this releases. `usb_host_transfer_free` forbids an
// in-flight transfer in one line of its header; `usb_host_interface_release`
// cannot delete a pipe with URBs queued on it and answers `ESP_ERR_INVALID_STATE`
// instead; `usb_host_device_close` then fails behind it. This used to do all three
// unconditionally and ignore every return value, which on the path that reaches it
// most — a key pulled out mid-read — freed a buffer the host controller was still
// going to write into.
//
// **It runs on the client task, which is the task that delivers completion
// callbacks**, so it cannot block on a semaphore waiting for one: that would be
// waiting for itself. It pumps its own event queue instead, which is the same wait
// spelled the only way it can be spelled from here.
void CloseDevice() {
    ResetEndpoint(runtime.ep_in.address);
    ResetEndpoint(runtime.ep_out.address);
    for (int round = 0;
         round < kCloseDrainRounds && (runtime.in_flight_in || runtime.in_flight_out); round++) {
        usb_host_client_handle_events(runtime.client, pdMS_TO_TICKS(10));
    }

    // **A transfer that never came back is kept, not freed.** The pointers stay, so
    // the next `OpenDevice` re-uses these two rather than allocating a second pair —
    // §10.14.1's allowance is two 64-byte buffers at a device boundary, and handing
    // them back while the controller owns them is the one outcome worse than
    // holding on to them.
    if (runtime.in_flight_in || runtime.in_flight_out) {
        runtime.stats.reclaims_failed++;
        ESP_LOGE(TAG, "a transfer is still in flight; keeping its buffer rather than freeing "
                      "one the controller still owns");
    } else {
        usb_host_transfer_free(runtime.transfer_in);
        usb_host_transfer_free(runtime.transfer_out);
        runtime.transfer_in = nullptr;
        runtime.transfer_out = nullptr;
    }

    if (runtime.claimed) {
        const esp_err_t err =
            usb_host_interface_release(runtime.client, runtime.device, runtime.interface_number);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "interface_release: %s", esp_err_to_name(err));
        }
        runtime.claimed = false;
    }
    if (runtime.device != nullptr) {
        const esp_err_t err = usb_host_device_close(runtime.client, runtime.device);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "device_close: %s", esp_err_to_name(err));
        }
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
    // freed when the key is unplugged. Never per exchange — and **not repeated**
    // when the last close had to keep a buffer it could not safely free, which is
    // what the null checks are for.
    if ((runtime.transfer_in == nullptr &&
         usb_host_transfer_alloc(ctaphid::kPacketSize, 0, &runtime.transfer_in) != ESP_OK) ||
        (runtime.transfer_out == nullptr &&
         usb_host_transfer_alloc(ctaphid::kPacketSize, 0, &runtime.transfer_out) != ESP_OK)) {
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
    // A previous write that ran out of time has to be taken back before this one
    // can fill the same buffer in.
    if (!Reclaim(runtime.ep_out.address, runtime.out_done, &runtime.in_flight_out)) {
        runtime.stats.transfer_errors++;
        return false;
    }

    std::memcpy(runtime.transfer_out->data_buffer, packet, ctaphid::kPacketSize);
    runtime.transfer_out->num_bytes = ctaphid::kPacketSize;
    runtime.transfer_out->device_handle = runtime.device;
    runtime.transfer_out->bEndpointAddress = runtime.ep_out.address;
    runtime.transfer_out->callback = TransferOutDone;
    runtime.transfer_out->context = nullptr;

    xSemaphoreTake(runtime.out_done, 0);  // drain a stale give
    // Marked before the submit, because the callback can fire the instant after it.
    runtime.in_flight_out = true;
    if (usb_host_transfer_submit(runtime.transfer_out) != ESP_OK) {
        runtime.in_flight_out = false;
        runtime.stats.transfer_errors++;
        return false;
    }
    if (xSemaphoreTake(runtime.out_done, pdMS_TO_TICKS(kTransferTimeoutMs)) != pdTRUE) {
        runtime.stats.transfer_errors++;
        Reclaim(runtime.ep_out.address, runtime.out_done, &runtime.in_flight_out);
        return false;
    }
    return runtime.info.present && runtime.transfer_out->status == USB_TRANSFER_STATUS_COMPLETED;
}

// What one read attempt produced. **Three outcomes rather than two, and the third
// is why this type exists**: "nothing arrived, ask again" and "this endpoint cannot
// be used" used to be the same `false`, so a caller met a dead endpoint by asking
// again immediately — for the whole remaining budget of the exchange, with nothing
// left to block on. A read loop that cannot block is a busy loop.
enum class Read : uint8_t {
    kPacket,   // 64 bytes, in `packet`
    kNothing,  // the wait expired, the transfer is ours again — ask again
    kBroken,   // the endpoint is unusable; the exchange is over
};

// One 64-byte report in. `wait_ms` is this attempt's, not the exchange's — an IN
// that returns nothing is asked again until the exchange gives up, which is how a
// key waiting for a fingertip is waited on.
Read ReadPacket(uint8_t *packet, uint32_t wait_ms) {
    if (!runtime.info.present || runtime.transfer_in == nullptr) {
        return Read::kBroken;
    }
    if (!Reclaim(runtime.ep_in.address, runtime.in_done, &runtime.in_flight_in)) {
        runtime.stats.transfer_errors++;
        return Read::kBroken;
    }

    runtime.transfer_in->num_bytes = ctaphid::kPacketSize;
    runtime.transfer_in->device_handle = runtime.device;
    runtime.transfer_in->bEndpointAddress = runtime.ep_in.address;
    runtime.transfer_in->callback = TransferInDone;
    runtime.transfer_in->context = nullptr;

    xSemaphoreTake(runtime.in_done, 0);
    runtime.in_flight_in = true;
    if (usb_host_transfer_submit(runtime.transfer_in) != ESP_OK) {
        runtime.in_flight_in = false;
        runtime.stats.transfer_errors++;
        return Read::kBroken;
    }

    if (xSemaphoreTake(runtime.in_done, pdMS_TO_TICKS(wait_ms)) != pdTRUE) {
        // **The wait ran out with the transfer still queued** — the state this
        // function used to return in, and the whole reason `Reclaim` exists. Take it
        // back before anybody submits it again.
        if (!Reclaim(runtime.ep_in.address, runtime.in_done, &runtime.in_flight_in)) {
            return Read::kBroken;
        }
        // And then fall through rather than return: the flush may have raced a real
        // completion, and a key that answered in that moment has handed us a genuine
        // packet. Throwing it away here would lose a keepalive, or an assertion
        // somebody had just touched the key for. The status decides.
    }

    if (!runtime.info.present) {
        return Read::kBroken;
    }
    if (runtime.transfer_in->status != USB_TRANSFER_STATUS_COMPLETED) {
        if (runtime.transfer_in->status != USB_TRANSFER_STATUS_CANCELED) {
            // A transfer error leaves the pipe halted, so clearing it here is what
            // lets the next read happen at all. The delay is an anti-spin guard and
            // nothing else: a fault that resolved instantly and repeatedly would
            // otherwise be a read loop with no wait in it.
            runtime.stats.transfer_errors++;
            ResetEndpoint(runtime.ep_in.address);
            vTaskDelay(pdMS_TO_TICKS(2));
        }
        return Read::kNothing;
    }
    if (runtime.transfer_in->actual_num_bytes < static_cast<int>(ctaphid::kPacketSize)) {
        return Read::kNothing;
    }
    std::memcpy(packet, runtime.transfer_in->data_buffer, ctaphid::kPacketSize);
    return Read::kPacket;
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
// Bounded, and a failure to drain is not reported to the caller: the exchange being
// abandoned has already failed, and the caller is being told about the cancel. It is
// logged, though, because what it predicts is the *next* exchange going wrong.
//
// **A `CTAPHID_KEEPALIVE` is a complete message and is not the answer.** The key
// sends one every ~100 ms while it waits for a fingertip, so a drain that stopped at
// the first whole message would routinely swallow a keepalive, report success, and
// leave the reply it came for exactly where it was. This is the same mistake the
// drain exists to fix, one layer along — found by the warning below firing on a
// cancel that came from the request's deadline rather than from the button.
void DrainAfterCancel(uint32_t cid) {
    ctaphid::Reader reader;
    reader.Reset();
    const uint32_t deadline = NowMs() + kDrainMs;
    uint8_t packet[ctaphid::kPacketSize];
    while (static_cast<int32_t>(deadline - NowMs()) > 0) {
        const Read read = ReadPacket(packet, kDrainReadMs);
        if (read == Read::kBroken) {
            return;
        }
        if (read == Read::kNothing) {
            if (!runtime.info.present) {
                return;
            }
            continue;
        }
        const ctaphid::Reader::Result result = reader.Feed(packet, sizeof(packet), cid);
        if (result == ctaphid::Reader::Result::kComplete) {
            if (reader.Command() == ctaphid::kCmdKeepAlive) {
                // Still waiting on the finger it has already been told to forget.
                reader.Reset();
                continue;
            }
            return;
        }
        if (result == ctaphid::Reader::Result::kError) {
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
            // **Tell the key to forget it before walking away**, which this used not
            // to do. A key holds one transaction at a time; a request abandoned on
            // its deadline goes on holding that slot for the rest of its
            // user-presence window, so *everything after it* answers
            // `CHANNEL_BUSY` — the same wedge a reset causes (§10.18.4), except
            // self-inflicted, and this half is avoidable: the channel is still ours,
            // so there is somewhere to send the cancel.
            //
            // Found by the retry loop this went in with. A `key test` nobody touched
            // timed out, and the next command on the console spent its whole budget
            // retrying against a key that was busy because of us.
            //
            // The cancel and its drain cost up to `kDrainMs` past the deadline. That
            // is cleanup on a path that has already failed, and the alternative is a
            // key that answers nothing for half a minute.
            if (cid != ctaphid::kBroadcastCid) {
                CancelAndDrain(cid);
            }
            *fault = Fault::kTimeout;
            return ESP_ERR_TIMEOUT;
        }
        // Never wait longer than a keep-alive interval, so that the caller's
        // cancel hook is offered the chance to fire even on a key that has gone
        // quiet.
        //
        // **300 ms used to be a correctness floor and is now only a cost one**,
        // which is worth recording because the reason it was a floor is gone.
        // `ReadPacket` abandoned its IN transfer when the wait expired and submitted
        // a fresh one on the next call, so a wait short enough to expire before the
        // key answered re-submitted a transfer that was still queued. Tried at
        // 10 ms — to poll the deny button faster — that became `the key could not be
        // reached` sixty milliseconds into every exchange, with the key never
        // lighting up.
        //
        // `Reclaim` fixed that, and shortening this is now merely expensive rather
        // than wrong: every expiry costs a halt, a flush, a clear and a task handoff
        // on a pipe that was working. 300 ms it stays — and whatever needs sampling
        // faster than this is still sampled somewhere else, which is why `buttons`
        // has a poller of its own.
        uint32_t wait = static_cast<uint32_t>(left);
        if (wait > 300) {
            wait = 300;
        }

        const Read read = ReadPacket(packet, wait);
        if (read == Read::kBroken) {
            // The endpoint could not be used, or could not be taken back from the
            // driver. There is nothing left to wait for, and asking again is what
            // used to turn this loop into a busy wait.
            *fault = runtime.info.present ? Fault::kTimeout : Fault::kUnplugged;
            return ESP_FAIL;
        }
        if (read == Read::kNothing) {
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
            // **`CHANNEL_BUSY` is logged by `Exchange` and not here**, because
            // `Exchange` now waits it out: this line would otherwise be sixty
            // identical warnings at 500 ms apart while the retry does its job.
            // Everything else the key can answer is a one-off and belongs here.
            if (code != ctaphid::kErrChannelBusy) {
                ESP_LOGW(TAG, "the key answered CTAPHID_ERROR %02x (%s)", code,
                         ctaphid::ErrorName(code));
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
//
// **`key_error` is the caller's, not a local one**, and that is load-bearing: a key
// holding a stale transaction refuses `CTAPHID_INIT` with `CHANNEL_BUSY` like
// anything else, so the code has to reach `Exchange`'s retry rather than being
// swallowed here.
bool OpenChannel(Fault *fault, uint8_t *key_error) {
    uint8_t nonce[ctaphid::kNonceSize];
    esp_fill_random(nonce, sizeof(nonce));

    uint8_t response[ctaphid::kInitResponseSize + 8];
    size_t length = 0;
    const esp_err_t err =
        Transact(ctaphid::kBroadcastCid, ctaphid::kCmdInit, nonce, sizeof(nonce), response,
                 sizeof(response), &length, kInitTimeoutMs, nullptr, nullptr, fault, key_error);
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

// Wait between retries without going deaf. The caller's cancel hook is the only
// way a request that has run out of time on the bus — or a tap on BOOT — can stop
// the loop above, so it is offered at roughly the keepalive cadence rather than
// once at the end. False means the wait was cut short: cancelled, or the key left.
bool BusyWait(uint32_t ms, KeepAlive keep_alive, void *context) {
    const uint32_t until = NowMs() + ms;
    while (static_cast<int32_t>(until - NowMs()) > 0) {
        vTaskDelay(pdMS_TO_TICKS(kBusyPollMs));
        if (!runtime.info.present) {
            return false;
        }
        if (keep_alive != nullptr && !keep_alive(0, context)) {
            return false;
        }
    }
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

    runtime.stats.exchanges++;

    // **`CHANNEL_BUSY` is the one key error worth waiting out** (§10.18.4). A reset
    // taken while the key was waiting for a fingertip leaves it holding a
    // transaction for a channel that died with the previous boot, and there is
    // nothing left to send `CTAPHID_CANCEL` to. It is not permanent: the stale wait
    // is a user-presence timeout and expires about 34 s after it started. So the
    // answer is to keep asking until it does — inside the budget the caller already
    // gave us, never beyond it, because the caller's budget is a request's deadline
    // and §10.10 rule 1 says silence is the safe end of one.
    //
    // **This is the loop that used to reboot the board about one run in two**, and
    // what it was tripping over was above it rather than in it: every attempt
    // re-submitted an IN transfer the host controller still owned, because a read
    // whose wait expired left it in flight. `Reclaim` is that fix, and this loop is
    // what it was for.
    const uint32_t started = NowMs();
    const uint32_t deadline = started + timeout_ms;
    esp_err_t err = ESP_FAIL;
    bool announced = false;
    bool still_busy = false;
    for (;;) {
        const int32_t left = static_cast<int32_t>(deadline - NowMs());
        if (left <= 0) {
            // **`still_busy` is deliberately not touched here.** Reaching this means
            // the budget ran out during a wait between retries, so the last thing
            // the key said really was `CHANNEL_BUSY` and the message below should
            // say so. It looks like an oversight and is the opposite of one.
            runtime.stats.timeouts++;
            *fault = Fault::kTimeout;
            err = ESP_ERR_TIMEOUT;
            break;
        }

        // A channel of our own, opened lazily. Inside the retry rather than in
        // front of it, because a busy key refuses `CTAPHID_INIT` too.
        if (runtime.cid == ctaphid::kBroadcastCid && !OpenChannel(fault, key_error)) {
            err = ESP_FAIL;
        } else {
            err = Transact(runtime.cid, command, request, request_length, response,
                           response_capacity, response_length, static_cast<uint32_t>(left),
                           keep_alive, keep_alive_context, fault, key_error);
        }

        still_busy = err != ESP_OK && *fault == Fault::kKeyError &&
                     *key_error == ctaphid::kErrChannelBusy;
        if (!still_busy) {
            break;
        }

        runtime.stats.busy_retries++;
        if (!announced) {
            announced = true;
            ESP_LOGW(TAG, "the key is mid-transaction on another channel - it was probably left "
                          "waiting by a reset. asking again every %u ms until it expires (about "
                          "30 s from when it started); unplug it and plug it back in if you would "
                          "rather not wait",
                     static_cast<unsigned>(kBusyRetryMs));
        }
        if (!BusyWait(kBusyRetryMs, keep_alive, keep_alive_context)) {
            *fault = runtime.info.present ? Fault::kCancelled : Fault::kUnplugged;
            err = ESP_FAIL;
            break;
        }
    }

    // **Three endings, not two, and the third is the one worth being careful
    // about.** A stale transaction that expires hands the request straight on to
    // the key, which then waits for a fingertip like any other — so an exchange
    // that recovered and *then* timed out on the finger is not a key that was
    // still busy, and saying so would send somebody to unplug a key that had
    // already fixed itself.
    if (announced) {
        const unsigned waited = static_cast<unsigned>(NowMs() - started);
        if (still_busy) {
            ESP_LOGW(TAG, "the key was still holding its stale transaction after %u ms", waited);
        } else if (err == ESP_OK) {
            ESP_LOGI(TAG, "the key freed its stale transaction and answered, %u ms in", waited);
        } else {
            ESP_LOGW(TAG, "the key freed its stale transaction, then %s (%u ms in)",
                     FaultName(*fault), waited);
        }
    }

    xSemaphoreGive(runtime.lock);
    return err;
}

}  // namespace usb
}  // namespace fido
