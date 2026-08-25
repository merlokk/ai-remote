#pragma once

// **CTAPHID framing, and nothing else** (CLAUDE.md §10.18.1).
//
// A FIDO key on USB is an HID device with one 64-byte report in each direction
// and a chunking protocol on top: the first packet of a message carries a
// command and a length, every packet after it carries a sequence number, and the
// receiver puts them back together. That is all this file does. It does not know
// what a request is, what CBOR is, or that USB exists — it turns a message into
// packets and packets back into a message.
//
// Carving it out is worth a sentence, because the alternative is tempting: the
// framing is thirty lines and it would fit inside the driver. But it is thirty
// lines with a **sequence number**, a **length that arrives before the data**,
// and a **buffer that a malicious or broken device controls the fill rate of** —
// which is three of the four ways this component could be made to overrun
// something. §10.11's host tier can run every one of those cases in a
// millisecond and a board cannot run them at all.
//
// Reference: FIDO CTAP 2.1, §11.2 (USB HID transport).

#include <cstddef>
#include <cstdint>

namespace ctaphid {

// One HID report. Fixed at 64 for full-speed devices, which every FIDO key on a
// USB-A or USB-C plug is; the spec allows other sizes over BLE and NFC and this
// firmware speaks neither.
inline constexpr size_t kPacketSize = 64;

// CID(4) + CMD(1) + BCNTH(1) + BCNTL(1).
inline constexpr size_t kInitHeader = 7;
// CID(4) + SEQ(1).
inline constexpr size_t kContHeader = 5;

inline constexpr size_t kInitPayload = kPacketSize - kInitHeader;  // 57
inline constexpr size_t kContPayload = kPacketSize - kContHeader;  // 59

// The sequence number is seven bits and starts at zero, so a message is at most
// one init packet and 128 continuations. The spec's own ceiling.
inline constexpr size_t kMaxSequence = 128;
inline constexpr size_t kProtocolMaxMessage = kInitPayload + kMaxSequence * kContPayload;  // 7609

// **What this firmware will actually hold, which is far less** (§10.14.1: it is
// a static buffer, so its size is a permanent cost). The two responses this
// device asks for are `authenticatorGetInfo` — a few hundred bytes on every key
// tested — and `authenticatorGetAssertion`, which is a signature, a credential
// id and an authenticator data blob. Two kilobytes is comfortably past both and
// is 3.5 KB less RAM than the protocol ceiling would cost.
//
// A message longer than this is a **refusal**, not a truncation: `Reader` reports
// an error and the caller drops the exchange. Silently keeping the first 2 KB of
// a signature would be the worst possible failure here.
inline constexpr size_t kMaxMessage = 2048;

// The command byte, with the high bit that marks an init packet already stripped.
enum Cmd : uint8_t {
    kCmdPing = 0x01,
    kCmdMsg = 0x03,   // CTAP1/U2F, which this device does not speak
    kCmdLock = 0x04,
    kCmdInit = 0x06,
    kCmdWink = 0x08,
    kCmdCbor = 0x10,  // CTAP2 — the one that matters here
    kCmdCancel = 0x11,
    kCmdKeepAlive = 0x3B,
    kCmdError = 0x3F,
};

// The channel every conversation starts on: a key is asked for a private one and
// answers with it.
inline constexpr uint32_t kBroadcastCid = 0xFFFFFFFFu;

// `CTAPHID_INIT` carries an 8-byte nonce out and echoes it back, which is how a
// reply is matched to a request on a channel shared with other software.
inline constexpr size_t kNonceSize = 8;

// What `CTAPHID_INIT` answers with: nonce(8) cid(4) protocol(1) major(1) minor(1)
// build(1) capabilities(1).
inline constexpr size_t kInitResponseSize = 17;

// The capability bits of that last byte.
inline constexpr uint8_t kCapWink = 0x01;
inline constexpr uint8_t kCapCbor = 0x04;  // the device speaks CTAP2
inline constexpr uint8_t kCapNmsg = 0x08;  // …and *only* CTAP2

// The error codes a key can answer with in a `kCmdError` message. Only the ones
// this firmware can act on differently are named; the rest are reported as a
// number, which is what an operator would search for anyway.
enum Error : uint8_t {
    kErrNone = 0x00,
    kErrInvalidCmd = 0x01,
    kErrInvalidPar = 0x02,
    kErrInvalidLen = 0x03,
    kErrInvalidSeq = 0x04,
    kErrMsgTimeout = 0x05,
    kErrChannelBusy = 0x06,
    kErrLockRequired = 0x0A,
    kErrInvalidChannel = 0x0B,
    kErrOther = 0x7F,
};

const char *ErrorName(uint8_t code);

// The one byte that says "still waiting", sent by the key roughly twice a second
// while it is holding a request open for a fingertip.
enum KeepAlive : uint8_t {
    kKeepAliveProcessing = 1,
    kKeepAliveUpNeeded = 2,  // **the operator has not touched it yet**
};

// --- Out ------------------------------------------------------------------

// Splits a message into packets, one at a time. **Pull-based** so that the
// driver can submit each packet and wait, rather than this class needing a
// buffer for all 128 of them.
class Writer {
   public:
    // `data` is borrowed and must outlive the writer. `length` may be 0 — a
    // `CTAPHID_CANCEL` is exactly that.
    void Begin(uint32_t cid, uint8_t cmd, const uint8_t *data, size_t length);

    // Fills `out` with the next 64-byte packet, zero-padded. Returns false once
    // the message is finished.
    bool Next(uint8_t *out, size_t capacity);

    // How many packets `Begin` implies, for a driver that wants to size a wait.
    size_t PacketCount() const;

    bool Done() const { return done_; }

   private:
    uint32_t cid_ = 0;
    uint8_t cmd_ = 0;
    const uint8_t *data_ = nullptr;
    size_t length_ = 0;
    size_t sent_ = 0;
    int seq_ = -1;  // -1 means the init packet has not gone yet
    bool done_ = true;
};

// --- In -------------------------------------------------------------------

// Reassembles packets into a message. Every field a device controls — the
// length, the sequence, the channel — is checked before it is used.
class Reader {
   public:
    enum class Result : uint8_t {
        kNeedMore = 0,  // a valid packet, and the message is not finished
        kComplete,      // the message is whole; `Command`/`Data`/`Length` are good
        kIgnored,       // a valid packet for somebody else's channel
        kError,         // malformed, out of sequence, or longer than `kMaxMessage`
    };

    void Reset();

    // Feeds one 64-byte report. `expect_cid` is the channel this exchange owns;
    // packets for any other channel are `kIgnored` rather than an error, because
    // a hub with two keys on it is an ordinary thing and not a fault.
    Result Feed(const uint8_t *packet, size_t size, uint32_t expect_cid);

    uint8_t Command() const { return cmd_; }
    size_t Length() const { return length_; }
    const uint8_t *Data() const { return buffer_; }

    // Why the last `kError` happened, in words. One string, because the caller
    // logs it and nothing branches on it.
    const char *ErrorText() const { return error_; }

   private:
    uint8_t buffer_[kMaxMessage] = {};
    size_t length_ = 0;    // what the init packet promised
    size_t filled_ = 0;    // what has arrived
    uint8_t cmd_ = 0;
    int seq_ = -1;
    bool started_ = false;
    const char *error_ = "";
};

}  // namespace ctaphid
