#include "responder.h"

#include <cstring>

#include "approval.h"
#include "device_key.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nats_link.h"
#include "registrar.h"
#include "registration.h"
#include "screens.h"
#include "signing.h"

namespace responder {
namespace {

constexpr const char *TAG = "responder";

StackType_t g_stack[kTaskStackBytes / sizeof(StackType_t)];
StaticTask_t g_tcb;

StaticSemaphore_t g_lock_storage;
SemaphoreHandle_t g_lock = nullptr;

StaticSemaphore_t g_work_storage;
SemaphoreHandle_t g_work = nullptr;

Status g_status;
bool g_started = false;

// **A decision waiting to be signed.** The request is 2.3 KB, so these are static
// and there are two of them — §10.14.1's "fixed capacity makes full a state that
// must be designed", and the design is §10.10's: over capacity is a drop.
struct Pending {
    bool used = false;
    ui::Request request;
    ui::Verdict verdict = ui::Verdict::kDeny;
    // Which connection the press belonged to. If the bus has reconnected since,
    // the inbox this would answer into is gone and publishing is worse than
    // silence — it looks like an answer to something nobody is waiting for.
    uint16_t connects = 0;
};

Pending g_pending[kPendingDecisions];

// Where a delivery is parsed. On the **bus task**, whose stack is 8 KB with about
// 5 KB in use — a 2.3 KB `ui::Request` as a local there is how a stack protection
// fault gets found on a Tuesday.
ui::Request g_incoming;

// What was subscribed, and when. `Bus::Close` forgets every subscription, so a
// reconnect leaves this component subscribed to nothing while believing it is —
// the connection counter is what makes that visible without asking the library.
bool g_subscribed = false;
uint16_t g_subscribed_at = 0;

void Lock() { xSemaphoreTake(g_lock, portMAX_DELAY); }
void Unlock() { xSemaphoreGive(g_lock); }

// --- off the wire ----------------------------------------------------------

// Runs on the bus task. Parses, and hands the card straight to the screen: both
// are cheap and neither blocks, so there is no reason to bounce this through
// another task and 2.3 KB of queue to do it.
void OnRequest(const nats::Message &message, void *) {
    Lock();
    ++g_status.received;
    Unlock();

    const protocol::RequestStatus status = protocol::ParseApprovalRequest(
        message.data, message.size, message.reply, &g_incoming);
    if (status != protocol::RequestStatus::kOk) {
        // §10.10, and this is the whole of it: one log line, no reply, and the
        // hook falls back to its own prompt. Junk on an open subject must cost
        // exactly this much.
        ESP_LOGW(TAG, "dropped a message on %s: %s", message.subject,
                 protocol::RequestStatusText(status));
        Lock();
        ++g_status.refused;
        Unlock();
        return;
    }

    if (!screens::Inject(g_incoming)) {
        // The card queue is full, or the payload did not fit its bounds. Same
        // answer: nothing goes back.
        ESP_LOGW(TAG, "%s in %s was not shown - the card queue refused it", g_incoming.tool_name,
                 g_incoming.cwd);
        Lock();
        ++g_status.refused;
        Unlock();
        return;
    }

    ESP_LOGI(TAG, "%s in %s is on the screen", g_incoming.tool_name, g_incoming.cwd);
    Lock();
    ++g_status.queued;
    Unlock();
}

// --- and back onto it ------------------------------------------------------

// Runs on the **screen task**, the moment a press decides a card. Copy and
// signal; nothing here allocates, waits or signs.
void OnDecision(const ui::Request &request, ui::Verdict verdict, void *) {
    const uint16_t connects = nats::Get().connects;

    Lock();
    Pending *slot = nullptr;
    for (Pending &candidate : g_pending) {
        if (!candidate.used) {
            slot = &candidate;
            break;
        }
    }
    if (slot == nullptr) {
        ++g_status.overflowed;
        Unlock();
        // A decision a human made that will never be heard. It is the fail-safe
        // working — the hook times out and asks again — but it is the one counter
        // on this component worth watching.
        ESP_LOGE(TAG, "no room to sign a decision - it will not be sent");
        return;
    }
    slot->used = true;
    slot->request = request;
    slot->verdict = verdict;
    slot->connects = connects;
    Unlock();

    xSemaphoreGive(g_work);
}

// Signs one decision and publishes it. On this component's own task, which is the
// only one with the stack for it.
void Answer(const Pending &pending) {
    const char *behavior =
        pending.verdict == ui::Verdict::kAllow ? protocol::kBehaviorAllow : protocol::kBehaviorDeny;

    // **The connection first, before any work.** If the socket has been round the
    // houses since the press, the reply-to inbox is gone; §10.10 says discard it
    // rather than publish into a dead one.
    const nats::Status bus = nats::Get();
    if (bus.state != nats::State::kConnected || bus.connects != pending.connects) {
        Lock();
        ++g_status.stale_dropped;
        Unlock();
        screens::SetReceiptNote("decided, not sent - the bus had gone");
        ESP_LOGW(TAG, "the connection moved between the press and the reply - nothing sent");
        return;
    }

    protocol::Decision decision;
    decision.v = pending.request.v;
    decision.ts = pending.request.ts;
    decision.session_id = pending.request.session_id;
    decision.nonce = pending.request.nonce;
    decision.tool_name = pending.request.tool_name;
    decision.input_sha256 = pending.request.input_sha256;
    decision.behavior = behavior;

    char message[protocol::kSigningBytesMax];
    const size_t length = protocol::DecisionSigningBytes(decision, message, sizeof message);

    uint8_t signature[crypto::kSignatureSize];
    char signature_b64[crypto::kSignatureB64Size];
    char payload[protocol::kDecisionReplyMax];

    const bool signed_ok =
        length != 0 &&
        crypto::Sign(reinterpret_cast<const uint8_t *>(message), length, signature) == ESP_OK &&
        crypto::Base64Encode(signature, sizeof signature, signature_b64, sizeof signature_b64);

    if (!signed_ok || protocol::BuildDecisionReply(pending.request, behavior, protocol::kKeyId,
                                                   signature_b64, payload, sizeof payload) == 0) {
        Lock();
        ++g_status.sign_failed;
        Unlock();
        screens::SetReceiptNote("decided, not sent - could not sign it");
        ESP_LOGE(TAG, "could not sign the decision - nothing sent");
        return;
    }

    const esp_err_t err = nats::Publish(pending.request.reply, payload, nullptr);
    if (err != ESP_OK) {
        Lock();
        ++g_status.publish_failed;
        Unlock();
        screens::SetReceiptNote("decided, not sent - the bus refused it");
        ESP_LOGE(TAG, "publish to %s failed: %s", pending.request.reply, esp_err_to_name(err));
        return;
    }

    // **The bookkeeping goes here, before the flush, and that ordering is a
    // finding rather than a preference.**
    //
    // It was the other way round first, and on the board the reply reached the
    // hook — verified, trusted, acted on — while `request` still said `sent 0`
    // and the glass still said nothing had left, for **minutes**. `Flush` waits
    // on a mutex the library holds across its own socket reads, so it can stall
    // far longer than the two seconds it is asked for, and everything after it
    // stalled with it.
    //
    // §4 already draws this line: published means the bytes are gone, and for a
    // decision that *is* the delivery — the hook is inside a request-reply and
    // has the answer the moment the server does. So the counters and the receipt
    // are true as soon as `Publish` returns, and the flush below is confirmation
    // rather than the thing being waited for.
    Lock();
    ++g_status.replied;
    if (pending.verdict == ui::Verdict::kAllow) {
        ++g_status.allowed;
    } else {
        ++g_status.denied;
    }
    Unlock();

    // The receipt of §10.8.4: what actually left the device, with a fingerprint of
    // the signature, because neither the press nor the countdown tells the
    // operator that. `responder_yubikey.print_decision` exists for exactly this.
    char note[48];
    snprintf(note, sizeof note, "%s sent, sig %.8s", behavior, signature_b64);
    screens::SetReceiptNote(note);
    ESP_LOGI(TAG, "%s -> %s, sig %.8s", behavior, pending.request.reply, signature_b64);

    // And now the confirmation. A flush that does not come back is worth a log
    // line and nothing else: it does not unsend the reply, and the operator has
    // already been told the truth.
    if (!nats::Flush(2000)) {
        ESP_LOGW(TAG, "the server did not confirm the reply within 2 s");
    }
}

// --- the subscription ------------------------------------------------------

Blocker WhyNot(const nats::Status &bus) {
    if (!crypto::Ready()) {
        return Blocker::kNoKey;
    }
    if (!registration::Registered()) {
        return Blocker::kNotRegistered;
    }
    if (bus.state != nats::State::kConnected) {
        return Blocker::kNoBus;
    }
    return Blocker::kNone;
}

void MaintainSubscription() {
    const nats::Status bus = nats::Get();
    const Blocker blocker = WhyNot(bus);

    // A reconnect drops every subscription with the client (`Bus::Close`), so
    // "subscribed" is only true for the connection it was made on.
    if (g_subscribed && (blocker != Blocker::kNone || bus.connects != g_subscribed_at)) {
        if (blocker == Blocker::kNone || bus.state == nats::State::kConnected) {
            nats::Unsubscribe(protocol::kApprovalsSubject);
        }
        g_subscribed = false;
        ESP_LOGW(TAG, "no longer answering %s: %s", protocol::kApprovalsSubject,
                 BlockerText(blocker));
    }

    if (!g_subscribed && blocker == Blocker::kNone) {
        const esp_err_t err = nats::Subscribe(protocol::kApprovalsSubject,
                                              protocol::kApproversQueue, &OnRequest, nullptr);
        if (err == ESP_OK) {
            g_subscribed = true;
            g_subscribed_at = bus.connects;
            ESP_LOGI(TAG, "answering %s in the group %s", protocol::kApprovalsSubject,
                     protocol::kApproversQueue);
        }
    }

    Lock();
    g_status.subscribed = g_subscribed;
    g_status.blocked_by = blocker;
    g_status.stack_low_water = uxTaskGetStackHighWaterMark(nullptr);
    Unlock();
}

void Task(void *) {
    for (;;) {
        MaintainSubscription();

        // Drain whatever was decided. Taken one at a time and copied out under the
        // lock, so the screen task can add another while this one is being signed.
        for (;;) {
            Pending taken;
            bool found = false;
            Lock();
            for (Pending &slot : g_pending) {
                if (slot.used) {
                    taken = slot;
                    slot.used = false;
                    found = true;
                    break;
                }
            }
            Unlock();
            if (!found) {
                break;
            }
            Answer(taken);
        }

        // Woken by a press, or by the tick — whichever comes first. A decision is
        // never left waiting for the next half-second.
        xSemaphoreTake(g_work, pdMS_TO_TICKS(kTickMs));
    }
}

}  // namespace

const char *BlockerText(Blocker blocker) {
    switch (blocker) {
        case Blocker::kNone:
            return "nothing";
        case Blocker::kNoKey:
            return "this device has no key - see 'keys'";
        case Blocker::kNotRegistered:
            return "this device is not registered - see 'register'";
        case Blocker::kNoBus:
            return "not connected to the bus - see 'nats'";
    }
    return "unknown";
}

esp_err_t Init() {
    if (g_started) {
        return ESP_OK;
    }
    g_lock = xSemaphoreCreateMutexStatic(&g_lock_storage);
    g_work = xSemaphoreCreateBinaryStatic(&g_work_storage);
    if (g_lock == nullptr || g_work == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    // Registered before the task starts, so a press cannot arrive at a null hook
    // in the window between the two.
    screens::OnDecision(&OnDecision, nullptr);
    screens::SetReceiptNote("decided, not sent - not on the bus");

    if (xTaskCreateStatic(&Task, "responder", sizeof g_stack / sizeof *g_stack, nullptr,
                          kTaskPriority, g_stack, &g_tcb) == nullptr) {
        return ESP_FAIL;
    }

    g_started = true;
    Lock();
    g_status.ready = true;
    Unlock();
    return ESP_OK;
}

bool Ready() { return g_started; }

Status Get() {
    if (!g_started) {
        return Status{};
    }
    Lock();
    const Status copy = g_status;
    Unlock();
    return copy;
}

}  // namespace responder
