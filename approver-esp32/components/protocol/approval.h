#pragma once

// **§7 on the wire**: what arrives on `approvals.*`, and what goes back into the
// subject it arrived with (CLAUDE.md §10.2, §10.8.4).
//
// `signing.h` next door is the bytes a decision is signed over; this is the JSON
// either side of that signature. Same rules: cJSON and `<cstdint>` only, so all
// of it is host-tested, and every refusal writes nothing.
//
// **It fills a `ui::Request`, and that is deliberate rather than a layering
// slip.** That struct is §7's field list verbatim — `request_card.h` says so
// where it defines it — and the alternative was a second struct here plus a
// conversion between them, which is two places for a field to be forgotten. The
// dependency is free: `components/ui` includes `<cstdint>` and nothing else, so
// this component stays compilable by a bare host compiler.
//
// Everything here treats its input as attacker-shaped (§10.10). The subject is
// open on the LAN (§10.3), so a payload that is not JSON, one missing fields, one
// with a `tool_name` full of control characters and one four megabytes long are
// all ordinary traffic — each is dropped with one log line and no reply, which is
// §7's fail-safe rather than an error path.

#include <cstddef>
#include <cstdint>

#include "request_card.h"

namespace protocol {

// **The subscription of §6 and §10.2**, and the queue group is not optional: it
// is what makes each request reach exactly one responder. A device that
// subscribed without it would answer requests the YubiKey responder was also
// answering, and both replies would race into the same inbox.
inline constexpr char kApprovalsSubject[] = "approvals.*";
inline constexpr char kApproversQueue[] = "approvers";

// §7 has two, and the reply carries the one the human pressed.
inline constexpr char kReasonNone[] = "";

// Enough for every field at its bound plus the signature and the punctuation
// around them. Sized here so the caller's buffer is a fixed array (§10.14.1).
inline constexpr size_t kDecisionReplyMax = 768;

// Why a delivery is not a card. Each is its own value because each is its own log
// line, and "somebody sent junk" and "somebody sent a request this device is too
// small for" are different facts about the network.
enum class RequestStatus : uint8_t {
    kOk,
    kNotJson,
    kBadVersion,
    kMissingField,
    // A field longer than this device will hold. **Refused, never truncated**
    // (§10.8.4): a card whose command has been shortened is exactly the card
    // people approve by reflex.
    kTooLong,
    // No subject to answer into. §10.10: a press on it could never reach anybody,
    // so it must not become a card.
    kNoReply,
    kBadTimestamp,
};

const char *RequestStatusText(RequestStatus status);

// Parses one delivery. `reply_subject` is the `reply` field of the NATS message —
// empty or null is `kNoReply`.
//
// `out` is filled only on `kOk`, and `ttl_ms` is left at zero: **the hook does not
// put its timeout on the wire**, so the card's own default is what expires it
// (`ui::RequestCard::kDefaultTtlMs`, chosen below any plausible hook timeout for
// the reason §10.8.4 gives).
RequestStatus ParseApprovalRequest(const char *json, size_t length, const char *reply_subject,
                                   ui::Request *out);

// The reply §7 expects: the six echoed fields, the verdict, an empty `reason`,
// this device's `key_id` and the signature.
//
// **No `updated_input`, ever** (§10.2) — it is the only field a responder
// originates and carrying it would force this firmware to reproduce Python's
// canonical JSON and hash it identically. Its absence is what keeps
// `updated_input_sha256` empty in the signed bytes, and the two have to agree.
//
// Returns the length, or 0 and writes nothing.
size_t BuildDecisionReply(const ui::Request &request, const char *behavior, const char *key_id,
                          const char *signature_b64, char *out, size_t out_size);

}  // namespace protocol
