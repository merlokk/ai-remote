// CTAPHID framing (CLAUDE.md §10.18.1). No USB, no allocation, no clock —
// §10.11's host tier runs every branch here, including the malformed ones a real
// key would never send and a broken one might.

#include "ctaphid_frames.h"

#include <cstring>

namespace ctaphid {
namespace {

void PutBe32(uint8_t *out, uint32_t value) {
    out[0] = static_cast<uint8_t>(value >> 24);
    out[1] = static_cast<uint8_t>(value >> 16);
    out[2] = static_cast<uint8_t>(value >> 8);
    out[3] = static_cast<uint8_t>(value);
}

uint32_t GetBe32(const uint8_t *in) {
    return (static_cast<uint32_t>(in[0]) << 24) | (static_cast<uint32_t>(in[1]) << 16) |
           (static_cast<uint32_t>(in[2]) << 8) | static_cast<uint32_t>(in[3]);
}

}  // namespace

const char *ErrorName(uint8_t code) {
    switch (code) {
        case kErrNone:
            return "none";
        case kErrInvalidCmd:
            return "invalid command";
        case kErrInvalidPar:
            return "invalid parameter";
        case kErrInvalidLen:
            return "invalid length";
        case kErrInvalidSeq:
            return "invalid sequence";
        case kErrMsgTimeout:
            return "message timeout";
        case kErrChannelBusy:
            return "channel busy";
        case kErrLockRequired:
            return "channel lock required";
        case kErrInvalidChannel:
            return "invalid channel";
        case kErrOther:
            return "unspecified error";
        default:
            return "unknown error";
    }
}

void Writer::Begin(uint32_t cid, uint8_t cmd, const uint8_t *data, size_t length) {
    cid_ = cid;
    cmd_ = cmd;
    data_ = data;
    length_ = length;
    sent_ = 0;
    seq_ = -1;
    // A message longer than the protocol allows is refused *here*, at the point
    // it is described, rather than half-sent and then abandoned — a key left
    // holding an incomplete message keeps the channel busy until it times out.
    done_ = length > kProtocolMaxMessage;
}

size_t Writer::PacketCount() const {
    if (length_ <= kInitPayload) {
        return 1;
    }
    const size_t rest = length_ - kInitPayload;
    return 1 + (rest + kContPayload - 1) / kContPayload;
}

bool Writer::Next(uint8_t *out, size_t capacity) {
    if (done_ || out == nullptr || capacity < kPacketSize) {
        return false;
    }
    std::memset(out, 0, kPacketSize);
    PutBe32(out, cid_);

    if (seq_ < 0) {
        out[4] = static_cast<uint8_t>(cmd_ | 0x80);
        out[5] = static_cast<uint8_t>(length_ >> 8);
        out[6] = static_cast<uint8_t>(length_ & 0xFF);
        const size_t chunk = length_ < kInitPayload ? length_ : kInitPayload;
        if (chunk > 0) {
            std::memcpy(out + kInitHeader, data_, chunk);
        }
        sent_ = chunk;
        seq_ = 0;
    } else {
        out[4] = static_cast<uint8_t>(seq_ & 0x7F);
        const size_t left = length_ - sent_;
        const size_t chunk = left < kContPayload ? left : kContPayload;
        std::memcpy(out + kContHeader, data_ + sent_, chunk);
        sent_ += chunk;
        seq_++;
    }

    if (sent_ >= length_) {
        done_ = true;
    }
    return true;
}

void Reader::Reset() {
    length_ = 0;
    filled_ = 0;
    cmd_ = 0;
    seq_ = -1;
    started_ = false;
    error_ = "";
}

Reader::Result Reader::Feed(const uint8_t *packet, size_t size, uint32_t expect_cid) {
    if (packet == nullptr || size < kPacketSize) {
        error_ = "short report";
        return Result::kError;
    }

    const uint32_t cid = GetBe32(packet);
    if (cid != expect_cid) {
        // Somebody else's channel. Not a fault: a hub with a second key on it,
        // or another process on a shared bus, is an ordinary thing. The one rule
        // is that it must not disturb the message being assembled.
        return Result::kIgnored;
    }

    const bool is_init = (packet[4] & 0x80) != 0;

    if (is_init) {
        // **An init packet always restarts the message**, even mid-assembly. The
        // spec says so, and the alternative — refusing it — would leave a channel
        // wedged after any dropped packet, recoverable only by unplugging.
        const size_t promised =
            (static_cast<size_t>(packet[5]) << 8) | static_cast<size_t>(packet[6]);
        if (promised > kMaxMessage) {
            Reset();
            error_ = "message longer than this firmware will hold";
            return Result::kError;
        }
        cmd_ = static_cast<uint8_t>(packet[4] & 0x7F);
        length_ = promised;
        filled_ = 0;
        seq_ = 0;
        started_ = true;
        error_ = "";

        const size_t chunk = length_ < kInitPayload ? length_ : kInitPayload;
        if (chunk > 0) {
            std::memcpy(buffer_, packet + kInitHeader, chunk);
            filled_ = chunk;
        }
        return filled_ >= length_ ? Result::kComplete : Result::kNeedMore;
    }

    if (!started_) {
        error_ = "continuation before an init packet";
        return Result::kError;
    }

    const uint8_t seq = static_cast<uint8_t>(packet[4] & 0x7F);
    if (seq != static_cast<uint8_t>(seq_)) {
        Reset();
        error_ = "out of sequence";
        return Result::kError;
    }
    seq_++;

    const size_t left = length_ - filled_;
    const size_t chunk = left < kContPayload ? left : kContPayload;
    // `filled_ + chunk <= length_ <= kMaxMessage` holds by construction — the
    // init packet's length was capped before a byte of it was copied, and
    // `chunk` never exceeds what is left. The assertion is the reason this class
    // exists apart from the driver.
    std::memcpy(buffer_ + filled_, packet + kContHeader, chunk);
    filled_ += chunk;

    return filled_ >= length_ ? Result::kComplete : Result::kNeedMore;
}

}  // namespace ctaphid
