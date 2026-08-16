#include "nats_bus.h"

#include <cstdio>
#include <cstring>
#include <new>

#include "esp_log.h"

// **The WebSocket transport is compiled out here, and it has to be done in
// this order.** §10.4's rule is that what can be turned off is turned off:
// this device speaks TCP to a server on the LAN (§10.3) and `nats_bus.h` has
// no way to name any other transport.
//
// Undefining it is the only lever that works, and the reason is a genuine
// contradiction inside the dependency. The library's manifest requires
// `espressif/esp_websocket_client` unconditionally, so the component is always
// downloaded and always in `BUILD_COMPONENTS`; its CMakeLists therefore puts
// `-DCONFIG_ESP_WEBSOCKET_CLIENT_ENABLE=1` on the command line of everything
// that includes it. But that same `REQUIRES` never reaches ESP-IDF's
// requirement pass — it is computed from `BUILD_COMPONENTS`, which is not
// populated yet when that pass runs — so the include directory does *not*
// propagate. The result is a define saying the transport is available and an
// include path saying it is not, and `#include <esp_websocket_client.h>`
// failing. The header's own `__has_include` fallback gets it right; this is
// what lets it decide.
#undef CONFIG_ESP_WEBSOCKET_CLIENT_ENABLE

// The client of §10.4, and the only file in this firmware that includes it.
#include "espidf_nats.h"

namespace nats {

namespace {

constexpr const char *TAG = "nats";

// **The client lives here** — one object, in static storage, for the life of
// the device (§10.14.1). Not a `new`, and not a member either: the library's
// header is 7,000 lines and holding a `NATS` by value in `nats_bus.h` would
// put all of it into everything that merely wants to publish.
//
// Placement new into an arena is what pays for that separation, and it costs
// two things worth writing down: exactly one `Bus` can be open at a time, and
// pointing the device at a different server means destroying this object and
// building another one — because the library takes its endpoint at
// construction and offers no way to change it afterwards.
alignas(NATS) uint8_t arena[sizeof(NATS)];
Bus *arena_owner = nullptr;

// The library's callback type carries no `void *`, so every subscription is
// handed the same function and the `sid` is what says which one it was.
// `arena_owner` is how it gets back to a `Bus` at all — legitimate here for
// exactly one reason, which is that the arena above makes "the open Bus" a
// singular thing rather than a hopeful assumption.
void OnMessage(nats_msg_t raw) {
    Message message;
    message.subject = raw.subject != nullptr ? raw.subject : "";
    // The whole of request-reply is in this field (§10.5): it is the subject a
    // decision is published into, and there is no correlation to invent.
    message.reply = raw.reply != nullptr ? raw.reply : "";
    message.data = raw.data != nullptr ? raw.data : "";
    message.size = raw.size > 0 ? static_cast<size_t>(raw.size) : 0u;
    detail::Deliver(raw.sid, message);
}

}  // namespace

namespace detail {

void Deliver(int sid, const Message &message) {
    if (arena_owner != nullptr) {
        arena_owner->Dispatch(sid, message);
    }
}

}  // namespace detail

Bus::~Bus() { Close(); }

esp_err_t Bus::Open(const Endpoint &endpoint) {
    if (client_ != nullptr) {
        if (Same(endpoint_, endpoint)) {
            return ESP_OK;
        }
        // A different address is a different client. Nothing survives it, not
        // even the subscriptions — they belonged to the other server.
        Close();
    }
    if (arena_owner != nullptr) {
        ESP_LOGE(TAG, "the client is already open elsewhere");
        return ESP_ERR_NO_MEM;
    }

    // Copied first: the library keeps the hostname **by pointer** and reads it
    // at every connect, so what it points at has to outlive it. `endpoint_` is
    // a member of this object, which does.
    endpoint_ = endpoint;
    client_ = new (arena) NATS(endpoint_.host, static_cast<int>(endpoint_.port));
    arena_owner = this;

    // **Two of the library's own opinions, switched off** — see the header.
    // Reconnection belongs to `link_policy.h`, which is the half that can be
    // tested; and a responder that publishes a decision the socket comes back
    // to find is answering a request that timed out minutes ago (§10.10).
    client_->max_reconnect_attempts = 0;
    client_->message_buffering_enabled = false;

    ESP_LOGI(TAG, "client for %s:%u", endpoint_.host, static_cast<unsigned>(endpoint_.port));
    return ESP_OK;
}

void Bus::Close() {
    if (client_ == nullptr) {
        return;
    }
    // The destructor disconnects, frees every `Sub` and deletes the mutexes.
    client_->~NATS();
    client_ = nullptr;
    arena_owner = nullptr;
    std::memset(slots_, 0, sizeof(slots_));
    used_ = 0;
}

esp_err_t Bus::Connect(uint32_t timeout_ms) {
    if (client_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (client_->connected) {
        return ESP_OK;
    }
    if (!client_->connect(timeout_ms)) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

void Bus::Disconnect() {
    if (client_ != nullptr) {
        client_->disconnect();
    }
}

bool Bus::Connected() const { return client_ != nullptr && client_->connected; }

Bus::Slot *Bus::Find(const char *subject) {
    for (Slot &slot : slots_) {
        if (slot.used && std::strcmp(slot.subject, subject) == 0) {
            return &slot;
        }
    }
    return nullptr;
}

esp_err_t Bus::Subscribe(const char *subject, const char *queue, Handler handler, void *user) {
    if (client_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (subject == nullptr || subject[0] == '\0' || handler == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (std::strlen(subject) >= kSubjectSize ||
        (queue != nullptr && std::strlen(queue) >= kSubjectSize)) {
        // Refused rather than truncated — `config::CopyString` states the
        // reason: half a subject subscribes to something else entirely.
        return ESP_ERR_INVALID_SIZE;
    }
    // The library refuses this too, and for the same reason: `SUB` is a line
    // on the wire, and there is no wire yet.
    if (!client_->connected) {
        return ESP_ERR_INVALID_STATE;
    }

    // An existing subject is replaced rather than doubled: two subscriptions
    // to `approvals.*` would deliver every request twice.
    if (Find(subject) != nullptr) {
        Unsubscribe(subject);
    }
    Slot *slot = nullptr;
    for (Slot &candidate : slots_) {
        if (!candidate.used) {
            slot = &candidate;
            break;
        }
    }
    if (slot == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    const bool grouped = queue != nullptr && queue[0] != '\0';
    const int sid = client_->subscribe(subject, &OnMessage, grouped ? queue : nullptr);
    if (sid < 0) {
        return ESP_FAIL;
    }

    slot->used = true;
    slot->sid = sid;
    slot->handler = handler;
    slot->user = user;
    snprintf(slot->subject, sizeof(slot->subject), "%s", subject);
    snprintf(slot->queue, sizeof(slot->queue), "%s", grouped ? queue : "");
    ++used_;

    ESP_LOGI(TAG, "subscribed to %s%s%s (sid %d)", subject, grouped ? " in group " : "",
             grouped ? queue : "", sid);
    return ESP_OK;
}

esp_err_t Bus::Unsubscribe(const char *subject) {
    if (client_ == nullptr || subject == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    Slot *slot = Find(subject);
    if (slot == nullptr) {
        return ESP_ERR_NOT_FOUND;
    }
    client_->unsubscribe(slot->sid);
    *slot = {};
    if (used_ > 0) {
        --used_;
    }
    return ESP_OK;
}

bool Bus::SubscriptionAt(size_t index, SubscriptionInfo *out) const {
    if (out == nullptr || index >= kMaxSubscriptions || !slots_[index].used) {
        return false;
    }
    out->subject = slots_[index].subject;
    out->queue = slots_[index].queue;
    out->sid = slots_[index].sid;
    return true;
}

esp_err_t Bus::Publish(const char *subject, const char *payload, const char *reply) {
    if (client_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (subject == nullptr || subject[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!client_->connected) {
        // **Not queued.** With buffering off the library would drop this
        // silently; saying so is the honest answer, and the caller is the one
        // that knows whether a late send would still have meant anything.
        return ESP_ERR_INVALID_STATE;
    }
    client_->publish(subject, payload, reply);
    return ESP_OK;
}

bool Bus::Flush(uint32_t timeout_ms) {
    return client_ != nullptr && client_->connected && client_->flush(timeout_ms);
}

void Bus::Process() {
    if (client_ != nullptr) {
        client_->process();
    }
}

Counters Bus::Counts() const {
    Counters counters;
    if (client_ == nullptr) {
        return counters;
    }
    const nats_connection_metrics_t metrics = client_->get_metrics();
    counters.messages_in = metrics.msgs_received;
    counters.messages_out = metrics.msgs_sent;
    counters.bytes_in = metrics.bytes_received;
    counters.bytes_out = metrics.bytes_sent;
    return counters;
}

void Bus::Dispatch(int sid, const Message &message) {
    for (const Slot &slot : slots_) {
        if (slot.used && slot.sid == sid) {
            slot.handler(message, slot.user);
            return;
        }
    }
    // A delivery for a subscription we no longer have is not an error: an
    // `UNSUB` and a message can cross on the wire. One line, and nothing else
    // happens — §10.10's rule about junk on an open subject.
    ESP_LOGD(TAG, "message for sid %d, which is nobody's", sid);
}

}  // namespace nats
