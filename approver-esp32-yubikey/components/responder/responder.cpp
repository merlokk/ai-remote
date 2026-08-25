// The loop of §7 on this board (CLAUDE.md §10.18): two tasks, one queue, one
// gate, and the four rules `responder.h` states.

#include "responder.h"

#include <cstdio>
#include <cstring>

#include "approval.h"
#include "board.h"
#include "config.h"
#include "device_key.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fido.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "indicator.h"
#include "psa/crypto.h"
#include "nats_link.h"
#include "registrar.h"
#include "registration.h"
#include "request_card.h"
#include "signing.h"
#include "watcher.h"

namespace responder {
namespace {

constexpr const char *TAG = "responder";

StackType_t g_stack[kTaskStackBytes / sizeof(StackType_t)];
StaticTask_t g_tcb;
StackType_t g_gate_stack[kGateStackBytes / sizeof(StackType_t)];
StaticTask_t g_gate_tcb;

StaticSemaphore_t g_lock_storage;
SemaphoreHandle_t g_lock = nullptr;

StaticSemaphore_t g_work_storage;
SemaphoreHandle_t g_work = nullptr;

Status g_status;
bool g_started = false;
TaskHandle_t g_gate_task = nullptr;

// **The queue, and it is the sibling board's screen queue with the screen taken
// off it.** `ui::RequestCard` owns which request is in front, how long it has
// left, and what happens when it runs out — all of which is the same on a device
// with one LED as on a device with a panel, because none of it was ever about
// pixels (§10.8.4's logic, §10.11's host tests).
ui::RequestCard g_card;

// Where a delivery is parsed. On the **bus task**, whose stack is 8 KB with about
// 5 KB in use — a 2.3 KB `ui::Request` as a local there is how a stack protection
// fault gets found on a Tuesday.
ui::Request g_incoming;

// The request the gate task is working on. Copied out of the queue under the
// lock, so that a delivery arriving mid-gate cannot move it.
ui::Request g_under_gate;
volatile bool g_gate_busy = false;

// **Set only while `Answer` is signing**, which is what `Busy()` reports. Not
// "the gate is running": the gate runs for the whole time a request is on the
// desk, and reporting that as `signing` painted the LED blue for thirty seconds
// where it should have been white and flashing (§10.17). Found by typing
// `request test` and looking at the light.
volatile bool g_signing = false;

// **The request the gate has given up on**, by nonce.
//
// This exists because of a spin found on the board: with no key on the port the
// gate returned in microseconds, the request stayed at the head of the queue
// because nothing had decided it, and the task took it again — several hundred
// times a second, each with a log line, until the TTL ran out. A fail-safe that
// fails loudly enough to drown the console is not a fail-safe.
//
// So a request that has been given up on is remembered and not re-gated. It stays
// on the queue and expires there, which is §10.10's outcome exactly: no reply,
// the hook times out, the question goes back to its own terminal.
char g_abandoned_nonce[ui::kNonceSize] = {};

// **A decision waiting to be signed.** The request is 2.3 KB, so these are static
// and there are two of them — §10.14.1's "fixed capacity makes full a state that
// must be designed", and the design is §10.10's: over capacity is a drop.
struct Pending {
    bool used = false;
    ui::Request request;
    ui::Verdict verdict = ui::Verdict::kDeny;
    // Which connection the decision belonged to. If the bus has reconnected
    // since, the inbox this would answer into is gone and publishing is worse
    // than silence — it looks like an answer to something nobody is waiting for.
    uint16_t connects = 0;
};

Pending g_pending[kPendingDecisions];

// What was subscribed, and when. `Bus::Close` forgets every subscription, so a
// reconnect leaves this component subscribed to nothing while believing it is —
// the connection counter is what makes that visible without asking the library.
bool g_subscribed = false;
uint16_t g_subscribed_at = 0;

// The last thing that happened, in words, for `request` on the console. This is
// the receipt the C6 board puts on the glass; here it is a string and a log line.
char g_receipt[64] = "nothing yet";

void Lock() { xSemaphoreTake(g_lock, portMAX_DELAY); }
void Unlock() { xSemaphoreGive(g_lock); }

uint32_t NowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

void SetReceipt(const char *text) {
    Lock();
    snprintf(g_receipt, sizeof g_receipt, "%s", text);
    Unlock();
}

// --- off the wire ----------------------------------------------------------

// Runs on the bus task. Parses and queues; the gate task is what picks it up.
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

    Lock();
    const bool accepted = g_card.Arrived(g_incoming, NowMs());
    if (accepted) {
        ++g_status.queued;
    } else {
        ++g_status.refused;
    }
    Unlock();

    if (!accepted) {
        ESP_LOGW(TAG, "%s in %s was not queued - the queue refused it", g_incoming.tool_name,
                 g_incoming.cwd);
        return;
    }

    ESP_LOGI(TAG, "%s in %s is waiting for a fingertip", g_incoming.tool_name, g_incoming.cwd);
    // The light, now rather than on the next tick: a request that takes half a
    // second to become visible is half a second of a deadline spent on nothing.
    indicator::Poke();
    if (g_gate_task != nullptr) {
        xTaskAbortDelay(g_gate_task);
    }
}

// --- the gate --------------------------------------------------------------

// What the keep-alive hook is watching while the key waits to be touched.
struct GateWatch {
    uint32_t deadline_ms = 0;
    bool button_denied = false;
    bool bus_gone = false;
    uint16_t connects = 0;
};

// **Called roughly twice a second while a fingertip is owed**, and on every
// read timeout in between. It is the only thing running during a gate, so it is
// where the three ways a gate should stop early live.
//
// Returning false cancels the exchange: `CTAPHID_CANCEL` goes to the key and
// `fido::RequireTouch` comes back `kCancelled`.
bool GateKeepAlive(uint8_t, void *context) {
    GateWatch *watch = static_cast<GateWatch *>(context);

    // 1. **A short press on BOOT is a deny**, and it stops the key being asked
    //    for something nobody wants any more. This is the whole reason the gate
    //    has a hook rather than a plain timeout.
    if (config::Get().approval.deny_button) {
        board::Buttons().Poll(board::button::kBootIndex);
        if (board::Buttons().Pressed(board::button::kBootIndex)) {
            watch->button_denied = true;
            return false;
        }
    }

    // 2. The bus went round the houses. Whatever the operator does now, the
    //    reply-to inbox is gone (§10.10) — better to stop asking than to collect
    //    a touch that cannot be used.
    const nats::Status bus = nats::Get();
    if (bus.state != nats::State::kConnected || bus.connects != watch->connects) {
        watch->bus_gone = true;
        return false;
    }

    // 3. The request itself ran out. `fido` has its own deadline, but the
    //    request's TTL is usually the shorter of the two and it is the one that
    //    matters — past it there is nobody listening.
    if (static_cast<int32_t>(NowMs() - watch->deadline_ms) >= 0) {
        return false;
    }

    return true;
}

// The development mode of §10.18: no key required, so the button carries both
// verdicts and the awkward one is `allow`. Returns true when a verdict was made.
bool ButtonOnlyDecision(GateWatch *watch, ui::Verdict *out) {
    buttons::PressLength press(kButtonAllowHoldMs);
    for (;;) {
        if (static_cast<int32_t>(NowMs() - watch->deadline_ms) >= 0) {
            return false;
        }
        const nats::Status bus = nats::Get();
        if (bus.state != nats::State::kConnected || bus.connects != watch->connects) {
            watch->bus_gone = true;
            return false;
        }

        board::Buttons().Poll(board::button::kBootIndex);
        const buttons::Press result =
            press.Update(board::Buttons().Pressed(board::button::kBootIndex), NowMs());
        if (result == buttons::Press::kLong) {
            *out = ui::Verdict::kAllow;
            return true;
        }
        if (result == buttons::Press::kShort) {
            *out = ui::Verdict::kDeny;
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(buttons::kPollIntervalMs));
    }
}

// Hands a decided request to the signing task.
void Decided(const ui::Request &request, ui::Verdict verdict, uint16_t connects) {
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

// Polls the button, the bus and the clock while there is nothing else to do.
// Returns true when a key is present and enrolled; false when the caller should
// stop waiting, with `*deny` saying whether that is because somebody refused.
//
// **This is what stops the gate spinning.** Without it, a request arriving with
// no key in the port was answered — with nothing — in microseconds, and taken
// again immediately.
bool WaitForKey(GateWatch *watch, bool *deny) {
    *deny = false;
    for (;;) {
        if (fido::Present() && fido::Enrolled()) {
            return true;
        }
        if (config::Get().approval.deny_button) {
            board::Buttons().Poll(board::button::kBootIndex);
            if (board::Buttons().Pressed(board::button::kBootIndex)) {
                *deny = true;
                return false;
            }
        }
        const nats::Status bus = nats::Get();
        if (bus.state != nats::State::kConnected || bus.connects != watch->connects) {
            watch->bus_gone = true;
            return false;
        }
        if (static_cast<int32_t>(NowMs() - watch->deadline_ms) >= 0) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(buttons::kPollIntervalMs * 10));
    }
}

// **The gate itself.** Runs on its own task, blocks for as long as the request
// has left, and produces one of three things: an allow, a deny, or nothing.
//
// Returns **true when it answered** — allow or deny. False means nothing was
// decided and nothing will be published (§10.10); the caller then remembers this
// request and lets it expire on the queue rather than gating it again, which is
// the fix for a spin found on the board and described at `g_abandoned_nonce`.
bool RunGate(const ui::Request &request) {
    const config::Data &settings = config::Get();
    const nats::Status bus = nats::Get();

    GateWatch watch;
    watch.connects = bus.connects;
    watch.deadline_ms = NowMs() + request.ttl_ms;
    {
        // The operator's own ceiling, when it is the shorter of the two. Its
        // whole job is to stop asking before the hook has given up (§10.18).
        const uint32_t configured = settings.approval.touch_timeout_seconds * 1000u;
        if (configured != 0 && configured < request.ttl_ms) {
            watch.deadline_ms = NowMs() + configured;
        }
    }

    Lock();
    ++g_status.gate_asked;
    Unlock();

    ui::Verdict verdict = ui::Verdict::kDeny;
    bool decided = false;

    if (!settings.approval.require_key) {
        // §10.18's development mode. Logged every single time, because a device
        // that can approve without a key must never be one somebody forgot they
        // configured.
        ESP_LOGW(TAG, "approval.requireKey is false - the button alone decides this");
        decided = ButtonOnlyDecision(&watch, &verdict);
        if (!decided) {
            SetReceipt(watch.bus_gone ? "no reply - the bus had gone" : "no reply - nobody pressed");
            Lock();
            ++g_status.gate_declined;
            Unlock();
        }
    } else {
        // **First, wait for a key.** A request arriving with nothing in the OTG
        // port is the ordinary case, not an edge one: the operator sees the
        // light, reaches into a pocket and plugs one in. So the device sits on
        // `pending` — white, fast (§10.17) — until a key appears, somebody taps
        // BOOT, the bus goes, or the request runs out.
        //
        // Refusing at the door instead is what the first version did, and it
        // collapsed the whole gate to a microsecond: nothing was decided, the
        // request stayed at the head of the queue, and the task took it again
        // several hundred times a second.
        bool button_denied = false;
        if (!WaitForKey(&watch, &button_denied)) {
            if (button_denied) {
                Lock();
                ++g_status.button_denied;
                Unlock();
                verdict = ui::Verdict::kDeny;
                decided = true;
            } else {
                const char *why = watch.bus_gone ? "the bus had gone"
                                                 : "nobody plugged a key in";
                char note[64];
                snprintf(note, sizeof note, "no reply - %s", why);
                SetReceipt(note);
                ESP_LOGW(TAG, "%s: %s", request.tool_name, why);
                Lock();
                ++g_status.gate_declined;
                Unlock();
                return false;
            }
        }

        if (!decided) {
            // **The challenge, and it is the whole design in four lines.**
            //
            // What the key signs over is the SHA-256 of the *exact bytes this
            // device would sign for an `allow`* — session, nonce, tool, input
            // hash, verdict and timestamp. Three things follow:
            //
            //   * the touch is bound to this request and cannot be replayed onto
            //     another one;
            //   * the key can only ever authorise an **allow**, because the bytes
            //     it committed to say `allow`. A deny needs no key and cannot be
            //     manufactured out of one;
            //   * if the bytes could not be assembled, there is nothing to
            //     authorise and nothing is asked for. §10.10 reaching this far
            //     down.
            protocol::Decision decision;
            decision.v = request.v;
            decision.ts = request.ts;
            decision.session_id = request.session_id;
            decision.nonce = request.nonce;
            decision.tool_name = request.tool_name;
            decision.input_sha256 = request.input_sha256;
            decision.behavior = protocol::kBehaviorAllow;

            char message[protocol::kSigningBytesMax];
            const size_t length =
                protocol::DecisionSigningBytes(decision, message, sizeof message);
            if (length == 0) {
                ESP_LOGE(TAG, "the signing bytes would not assemble - the key is not asked");
                SetReceipt("no reply - the request would not assemble");
                Lock();
                ++g_status.gate_declined;
                Unlock();
                return false;
            }

            // PSA rather than `mbedtls_sha256`, for the reason `fido.cpp` states
            // at its own hash: on ESP-IDF v6 the classic mbedTLS entry points
            // live under `mbedtls/private/`, and this one is too important to
            // reach for that way.
            uint8_t challenge[32];
            size_t produced = 0;
            if (psa_hash_compute(PSA_ALG_SHA_256, reinterpret_cast<const uint8_t *>(message),
                                 length, challenge, sizeof challenge, &produced) != PSA_SUCCESS ||
                produced != sizeof challenge) {
                ESP_LOGE(TAG, "could not hash the signing bytes - the key is not asked");
                SetReceipt("no reply - the challenge would not hash");
                Lock();
                ++g_status.gate_declined;
                Unlock();
                return false;
            }

            // The budget left *now*, not the one this started with: a request
            // that spent twenty of its thirty seconds waiting for a key gets ten
            // to be touched in, not another thirty it does not have.
            const int32_t remaining = static_cast<int32_t>(watch.deadline_ms - NowMs());
            if (remaining <= 0) {
                SetReceipt("no reply - time ran out");
                Lock();
                ++g_status.gate_declined;
                Unlock();
                return false;
            }

            fido::usb::Fault fault = fido::usb::Fault::kNone;
            uint8_t ctap_status = 0;
            const fido::Gate gate =
                fido::RequireTouch(challenge, static_cast<uint32_t>(remaining), &GateKeepAlive,
                                   &watch, &fault, &ctap_status);
            if (gate == fido::Gate::kApproved) {
                Lock();
                ++g_status.gate_approved;
                Unlock();
                verdict = ui::Verdict::kAllow;
                decided = true;
            } else if (watch.button_denied) {
                Lock();
                ++g_status.button_denied;
                Unlock();
                verdict = ui::Verdict::kDeny;
                decided = true;
            } else {
                // **Not a deny.** A gate that timed out or failed says nothing at
                // all — see the fourth rule in `responder.h`.
                char note[64];
                snprintf(note, sizeof note, "no reply - %s", fido::GateName(gate));
                SetReceipt(note);
                ESP_LOGW(TAG, "%s: %s (%s)", request.tool_name, fido::GateText(gate),
                         fido::usb::FaultName(fault));
                Lock();
                ++g_status.gate_declined;
                Unlock();
                return false;
            }
        }
    }

    if (!decided) {
        return false;
    }

    // Take it off the queue. `Press` is the sibling board's call and its guard —
    // ignore a decision made in the first 300 ms of a request being presented —
    // applies here for the same reason it does there: a finger that was already
    // down when the request appeared did not decide this one.
    ui::Request taken;
    Lock();
    const bool removed = g_card.Press(verdict, NowMs(), &taken);
    Unlock();

    if (!removed) {
        ESP_LOGW(TAG, "the decision arrived too early for this request - ignored");
        return false;
    }
    if (watch.bus_gone) {
        Lock();
        ++g_status.stale_dropped;
        Unlock();
        SetReceipt("decided, not sent - the bus had gone");
        return true;
    }
    // **Nothing is published for a test request** (`request test` on the console).
    // It carries `kTestReplySubject` and is stopped here, at the last step before
    // the wire — everything up to this point is the real path, which is the whole
    // reason the test is worth running.
    if (std::strcmp(taken.reply, kTestReplySubject) == 0) {
        SetReceipt(verdict == ui::Verdict::kAllow ? "allow (test - nothing sent)"
                                                  : "deny (test - nothing sent)");
        ESP_LOGI(TAG, "test request decided %s; nothing was sent",
                 verdict == ui::Verdict::kAllow ? "allow" : "deny");
        indicator::ShowVerdict(verdict == ui::Verdict::kAllow);
        return true;
    }

    Decided(taken, verdict, watch.connects);
    return true;
}

void GateTask(void *) {
    for (;;) {
        bool have = false;
        Lock();
        g_card.Tick(NowMs());
        const ui::Request *front = g_card.Front();
        // **Not the one already given up on.** See `g_abandoned_nonce` — without
        // this check a request nothing can decide is re-gated as fast as the CPU
        // allows, which on the board looked like a crash and was one log line
        // repeating a few hundred times a second.
        if (front != nullptr && !g_gate_busy &&
            std::strcmp(front->nonce, g_abandoned_nonce) != 0) {
            g_under_gate = *front;
            have = true;
        }
        Unlock();

        if (have) {
            g_gate_busy = true;
            indicator::Poke();
            const bool answered = RunGate(g_under_gate);
            if (!answered) {
                Lock();
                snprintf(g_abandoned_nonce, sizeof g_abandoned_nonce, "%s", g_under_gate.nonce);
                Unlock();
            }
            g_gate_busy = false;
            indicator::Poke();
            continue;
        }

        Lock();
        g_status.gate_stack_low_water = uxTaskGetStackHighWaterMark(nullptr);
        Unlock();
        vTaskDelay(pdMS_TO_TICKS(kGateIdleMs));
    }
}

// --- and back onto it ------------------------------------------------------

// Signs one decision and publishes it. On this component's own task, which is the
// only one with the stack for it.
void Answer(const Pending &pending) {
    const char *behavior =
        pending.verdict == ui::Verdict::kAllow ? protocol::kBehaviorAllow : protocol::kBehaviorDeny;

    // **The connection first, before any work.** If the socket has been round the
    // houses since the touch, the reply-to inbox is gone; §10.10 says discard it
    // rather than publish into a dead one.
    const nats::Status bus = nats::Get();
    if (bus.state != nats::State::kConnected || bus.connects != pending.connects) {
        Lock();
        ++g_status.stale_dropped;
        Unlock();
        SetReceipt("decided, not sent - the bus had gone");
        ESP_LOGW(TAG, "the connection moved between the touch and the reply - nothing sent");
        g_signing = false;
        return;
    }

    // From here to the publish is what `Busy()` reports and what the LED shows
    // as `signing` (§10.17). Tens of milliseconds when it works; the colour is
    // there for when it does not.
    g_signing = true;

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
        SetReceipt("decided, not sent - could not sign it");
        g_signing = false;
        ESP_LOGE(TAG, "could not sign the decision - nothing sent");
        return;
    }

    const esp_err_t err = nats::Publish(pending.request.reply, payload, nullptr);
    if (err != ESP_OK) {
        Lock();
        ++g_status.publish_failed;
        Unlock();
        SetReceipt("decided, not sent - the bus refused it");
        g_signing = false;
        ESP_LOGE(TAG, "publish to %s failed: %s", pending.request.reply, esp_err_to_name(err));
        return;
    }

    // **The bookkeeping goes here, before the flush, and that ordering is a
    // finding rather than a preference** — carried over from the sibling board,
    // where a reply reached the hook while the device's own counters still said
    // nothing had left, for minutes. `Flush` waits on a mutex the library holds
    // across its own socket reads, so it can stall far longer than the two
    // seconds it is asked for, and everything after it stalls with it.
    //
    // §4 already draws this line: published means the bytes are gone, and for a
    // decision that *is* the delivery — the hook is inside a request-reply and
    // has the answer the moment the server does.
    Lock();
    ++g_status.replied;
    if (pending.verdict == ui::Verdict::kAllow) {
        ++g_status.allowed;
    } else {
        ++g_status.denied;
    }
    Unlock();

    g_signing = false;

    char note[64];
    snprintf(note, sizeof note, "%s sent, sig %.8s", behavior, signature_b64);
    SetReceipt(note);
    ESP_LOGI(TAG, "%s -> %s, sig %.8s", behavior, pending.request.reply, signature_b64);

    // The one thing the operator sees without a console (§10.17): green for an
    // allow, red for a deny, and then back to whatever the device is.
    indicator::ShowVerdict(pending.verdict == ui::Verdict::kAllow);

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
    // **The first rule of this component, and the one it broke on its first real
    // registration** (see `Blocker::kNotEnrolled`): a device that cannot produce
    // an `allow` must not be on a queue group that takes requests away from
    // devices that can.
    if (config::Get().approval.require_key && !fido::Enrolled()) {
        return Blocker::kNotEnrolled;
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

        // **Somebody else's subscription, on this task's clock.** The `status`
        // watcher of §9.7 has no task of its own — it has nothing to do between
        // deliveries — and a second task ticking twice a second to notice a
        // reconnect would be 3 KB of stack for a boolean. This is a scheduling
        // favour and not a dependency: nothing in `watcher` is on this
        // component's `REQUIRES` line the other way round, which is what keeps
        // "delete the watcher and the responder still works" true.
        watcher::Maintain();

        // Drain whatever was decided. Taken one at a time and copied out under the
        // lock, so the gate task can add another while this one is being signed.
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

        // Woken by a decision, or by the tick — whichever comes first. A decision
        // is never left waiting for the next half-second.
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
        case Blocker::kNotEnrolled:
            return "no security key enrolled - run 'key enrol'";
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

    if (xTaskCreateStatic(&Task, "responder", sizeof g_stack / sizeof *g_stack, nullptr,
                          kTaskPriority, g_stack, &g_tcb) == nullptr) {
        return ESP_FAIL;
    }
    g_gate_task = xTaskCreateStatic(&GateTask, "gate",
                                    sizeof g_gate_stack / sizeof *g_gate_stack, nullptr,
                                    kGatePriority, g_gate_stack, &g_gate_tcb);
    if (g_gate_task == nullptr) {
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

bool RequestPending() {
    if (!g_started) {
        return false;
    }
    Lock();
    const bool pending = g_card.Front() != nullptr;
    Unlock();
    return pending;
}

bool Busy() { return g_signing; }

PendingView Front() {
    PendingView view;
    if (!g_started) {
        return view;
    }
    Lock();
    const ui::Request *front = g_card.Front();
    if (front != nullptr) {
        view.present = true;
        snprintf(view.tool_name, sizeof view.tool_name, "%s", front->tool_name);
        snprintf(view.cwd, sizeof view.cwd, "%s", front->cwd);
        view.remaining_ms = g_card.RemainingMs(NowMs());
        view.waiting = g_card.Waiting();
    }
    Unlock();
    return view;
}

bool InjectTestRequest(const char *tool_name, const char *text, uint32_t ttl_ms) {
    if (!g_started) {
        return false;
    }
    ui::Request request;
    request.v = protocol::kVersion;
    request.ts = 0;
    snprintf(request.session_id, sizeof request.session_id, "console-test");
    snprintf(request.nonce, sizeof request.nonce, "console-test-nonce");
    snprintf(request.input_sha256, sizeof request.input_sha256,
             "0000000000000000000000000000000000000000000000000000000000000000");
    snprintf(request.tool_name, sizeof request.tool_name, "%s",
             tool_name != nullptr ? tool_name : "Bash");
    snprintf(request.cwd, sizeof request.cwd, "console");
    snprintf(request.tool_input, sizeof request.tool_input, "%s", text != nullptr ? text : "");
    // The sentinel of `kTestReplySubject` - see the header for why it is a
    // subject rather than an empty string.
    snprintf(request.reply, sizeof request.reply, "%s", kTestReplySubject);
    request.ttl_ms = ttl_ms;

    Lock();
    const bool accepted = g_card.Arrived(request, NowMs());
    Unlock();
    if (accepted) {
        indicator::Poke();
        if (g_gate_task != nullptr) {
            xTaskAbortDelay(g_gate_task);
        }
    }
    return accepted;
}

}  // namespace responder
