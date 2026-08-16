#pragma once

// **The bus, as a class** — CLAUDE.md §10.5's call surface and nothing else.
//
// Underneath it is `debsahu/espidf-nats` (§10.4): header-only C++, MIT, and
// approved for exactly this job. What this class is for is the other half of
// that decision — §10.5 says "the wrapper exists to keep the call surface
// small", and small means these verbs:
//
//     connect · disconnect · subscribe (with a queue group) · publish (into a
//     reply subject) · flush · process
//
// JetStream, key-value, the object store, NKey and JWT auth, message headers
// and the WebSocket transport are all in the library and **none of them is
// reachable from here**. That is §10.13's rule applied to a dependency: on a
// board, unused is not the same as absent, and a call surface nobody can widen
// by accident is the cheapest way to keep it that way.
//
// Three properties this class adds that the library does not have:
//
//   * **no allocation of ours** (§10.14.1). The library allocates — that is
//     its business, and the heap low-water mark is what says whether it
//     mattered — but the client object itself lives in a static arena in the
//     .cpp rather than behind a `new`, and the subscription table is fixed;
//   * **one place decides when to reconnect.** The library has its own backoff
//     and it is switched off here, because `link_policy.h` is the half that
//     can be tested and two things deciding is one too many;
//   * **nothing is buffered while offline.** The library will happily queue a
//     hundred messages to send when the socket comes back; for a responder
//     that is precisely wrong (§10.10 — a decision that arrives late is worse
//     than one that never arrives), so the queue is switched off too.
//
// Library layer (§10.14.2): it knows about subjects and payloads, and has
// never heard of an approval.

#include <cstddef>
#include <cstdint>

#include "endpoint.h"
#include "esp_err.h"

// The client, forward-declared at global scope where it lives. Its header is
// 7,000 lines of header-only C++ and stays inside `nats_bus.cpp`; nothing that
// merely wants to publish should pay to compile it.
class NATS;

namespace nats {

// Enough for what §10.2 needs (`approvals.*` and `status`), the private inbox
// of §10.7, and a couple the console can take for a look around.
inline constexpr size_t kMaxSubscriptions = 6;

// A NATS subject is bounded by the protocol at far more than this; what this
// bounds is what *we* will hold, and `approvals.<session_id>` is the longest
// this device has any business subscribing to.
inline constexpr size_t kSubjectSize = 96;

// One delivery, pointing into the library's receive buffer. **Valid only for
// the duration of the handler** — copy anything that has to outlive it, into
// something with a bound on it (§10.10: everything off the bus is
// attacker-shaped).
struct Message {
    const char *subject;
    // The subject to answer into, and the whole of request-reply: there is no
    // correlation to invent. Empty when the publisher wants no answer.
    const char *reply;
    const char *data;
    size_t size;
};

// A plain function pointer and a `void *`, which is the shape §10.14.1 asks
// for and the shape every IDF callback already has.
using Handler = void (*)(const Message &message, void *user);

// What the library counts, for the console. Not a health check — "the socket
// is up" is `Connected()`, and these are how much has gone through it.
struct Counters {
    uint64_t messages_in = 0;
    uint64_t messages_out = 0;
    uint64_t bytes_in = 0;
    uint64_t bytes_out = 0;
};

namespace detail {

// How a delivery gets from the library's context-free callback back to the
// `Bus` that asked for it. Not for callers; it is in the header only because
// the class has to name it as a friend.
void Deliver(int sid, const Message &message);

}  // namespace detail

// One subscription, for a readout. Deliberately three facts and not four:
// the library counts messages per subscription and offers no way to read the
// count, and a field that is always zero is worse than no field.
struct SubscriptionInfo {
    const char *subject;
    const char *queue;  // "" when it is not in a group
    int sid;
};

class Bus {
   public:
    Bus() = default;
    ~Bus();
    Bus(const Bus &) = delete;
    Bus &operator=(const Bus &) = delete;

    // Builds the client for one address. **No I/O**: a constructor that
    // touched the network would be a boot crash naming the wrong thing
    // (§10.14.1), so this is the two-phase half and `Connect` is the other.
    //
    // The library takes its endpoint at construction and offers no way to
    // change it, so pointing the device somewhere else means building another
    // client — which is what makes this a separate call rather than a field.
    //
    // **There is one arena**, so a second `Bus` asking for a client while the
    // first still holds it is refused with `ESP_ERR_NO_MEM` rather than served
    // from a heap this firmware does not use.
    esp_err_t Open(const Endpoint &endpoint);

    // Disconnects if connected, destroys the client, forgets every
    // subscription. Safe to call when nothing is open.
    void Close();

    bool Opened() const { return client_ != nullptr; }
    const Endpoint &Where() const { return endpoint_; }

    // Blocks for up to `timeout_ms` — the TCP connect, the server's `INFO` and
    // our `CONNECT`. Called only from the task that also calls `Process`.
    esp_err_t Connect(uint32_t timeout_ms);
    void Disconnect();
    bool Connected() const;

    // §6's queue group is not optional, so it is an argument rather than an
    // option: `nullptr` or "" is a plain subscription, anything else joins
    // that group. Re-subscribing to the same subject replaces the handler
    // rather than taking a second slot.
    esp_err_t Subscribe(const char *subject, const char *queue, Handler handler, void *user);
    esp_err_t Unsubscribe(const char *subject);

    size_t SubscriptionCount() const { return used_; }
    bool SubscriptionAt(size_t index, SubscriptionInfo *out) const;

    // `reply` may be `nullptr`: a decision answers into the subject the
    // request arrived with, and a plain publish has nowhere to answer.
    //
    // **Publishing is not delivery** (§4): this hands the bytes to the socket
    // and returns. `Flush` is what waits for the server to have them.
    esp_err_t Publish(const char *subject, const char *payload, const char *reply);

    bool Flush(uint32_t timeout_ms);

    // The pump: read whatever has arrived, dispatch it, answer a `PING`.
    // Non-blocking. One task calls this and no other.
    void Process();

    Counters Counts() const;

   private:
    struct Slot {
        bool used;
        int sid;
        Handler handler;
        void *user;
        char subject[kSubjectSize];
        char queue[kSubjectSize];
    };

    // Where an incoming message finds its handler. The library's callback type
    // carries no context, so the trampoline in the .cpp reaches the one open
    // `Bus` through a file static — and the arena above is what makes "the one
    // open Bus" a fact rather than an assumption.
    void Dispatch(int sid, const Message &message);
    friend void detail::Deliver(int sid, const Message &message);

    Slot *Find(const char *subject);

    NATS *client_ = nullptr;
    Endpoint endpoint_ = {};
    Slot slots_[kMaxSubscriptions] = {};
    size_t used_ = 0;
};

}  // namespace nats
