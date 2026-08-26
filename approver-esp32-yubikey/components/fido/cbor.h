#pragma once

// **As much CBOR as CTAP2 needs and not one byte more** (CLAUDE.md §10.18.4).
//
// CTAP2's wire format is CBOR (RFC 8949) in its canonical form. A general CBOR
// library is a dependency root §1 would have to sign off; what this device
// actually speaks is four request maps with integer keys and three response maps
// it reads a handful of fields out of. That is small enough to write, and
// writing it keeps the dependency list where §10.4 left it.
//
// **What is deliberately missing**, so that nobody looks for it: tags, floats,
// indefinite-length items, and the negative integers below -2^63. A request that
// would need one of them is a request this firmware does not make, and a
// *response* carrying one is skipped rather than decoded — `Skip` walks it,
// `Decode` refuses it, and a field this firmware needs is never one of them.
//
// Pure: no allocation, no ESP-IDF, no clock. §10.11's host tier runs it, which
// for a parser fed by an external device is not a nicety — every length in a
// response is a number that device chose.

#include <cstddef>
#include <cstdint>

namespace cbor {

// The major types, named as this firmware uses them. `kSimple` covers false,
// true and null, which are the only ones CTAP2 puts on the wire.
enum class Type : uint8_t {
    kUint = 0,
    kNint,
    kBytes,
    kText,
    kArray,
    kMap,
    kSimple,
    kInvalid,
};

// One decoded item. For `kBytes` / `kText`, `data` and `length` are the payload
// and point **into the caller's buffer** — nothing is copied. For `kArray` /
// `kMap`, `value` is the element count and the elements follow `header` bytes in.
// For `kUint`, `value` is the number; for `kNint`, `signed_value` is.
struct Item {
    Type type = Type::kInvalid;
    uint64_t value = 0;
    int64_t signed_value = 0;
    const uint8_t *data = nullptr;
    size_t length = 0;
    size_t header = 0;  // bytes of head, so `header + length` is a byte string's total
    bool boolean = false;
};

// --- Writing --------------------------------------------------------------

// Builds canonical CBOR into a buffer the caller owns. **Every call is checked
// and the first failure is sticky**: a caller may write a whole request and test
// `Ok()` once at the end, which is what the CTAP2 request builders do.
//
// Canonicity is the caller's job for map ordering — CTAP2 requires keys in
// ascending order and this class will not sort them. The request maps in
// `ctap2.cpp` are written in order and a test asserts their bytes.
class Writer {
   public:
    Writer(uint8_t *buffer, size_t capacity) : buffer_(buffer), capacity_(capacity) {}

    bool Uint(uint64_t value);
    bool Nint(uint64_t magnitude);  // encodes -(magnitude + 1), which is CBOR's form
    bool Int(int64_t value);
    bool Bytes(const uint8_t *data, size_t length);
    bool Text(const char *text);
    bool TextN(const char *text, size_t length);
    bool ArrayHeader(size_t count);
    bool MapHeader(size_t count);
    bool Bool(bool value);

    // Copies an already-encoded item in verbatim. For the one case where this
    // firmware carries a value it did not build — the COSE key it stored at
    // enrolment is handed back untouched.
    bool Raw(const uint8_t *data, size_t length);

    size_t Length() const { return length_; }
    bool Ok() const { return ok_; }

   private:
    bool Head(uint8_t major, uint64_t value);
    bool Put(const uint8_t *data, size_t length);

    uint8_t *buffer_;
    size_t capacity_;
    size_t length_ = 0;
    bool ok_ = true;
};

// --- Reading --------------------------------------------------------------

// Decodes the item at the start of `in`. Returns false on anything malformed,
// truncated, or of a kind this firmware does not speak.
bool Decode(const uint8_t *in, size_t size, Item *out);

// Advances `*offset` past one whole item, **including everything nested inside
// it**. This is what makes `MapFind` able to step over a value it does not want
// without understanding it. Returns false if the item is malformed or runs off
// the end.
//
// Bounded: nesting deeper than `kMaxDepth` is refused rather than recursed into,
// because the depth is a number the device on the other end chose.
inline constexpr size_t kMaxDepth = 8;
bool Skip(const uint8_t *in, size_t size, size_t *offset);

// Finds an integer-keyed entry in the map that starts at `in`. CTAP2's request
// and response maps are keyed this way; the `Text` form below is for the two
// nested maps that are not (`options`, and the credential descriptor's `type`).
//
// On success `*value` points into `in` at the start of the value and
// `*value_size` is how many bytes are left in the buffer from there — enough for
// `Decode` to work with, not the value's own length.
bool MapFind(const uint8_t *in, size_t size, int64_t key, const uint8_t **value,
             size_t *value_size);
bool MapFindText(const uint8_t *in, size_t size, const char *key, const uint8_t **value,
                 size_t *value_size);

// The two shapes this firmware reads often enough to want a one-liner for.
bool GetBytes(const uint8_t *in, size_t size, const uint8_t **data, size_t *length);
bool GetText(const uint8_t *in, size_t size, const char **text, size_t *length);
bool GetUint(const uint8_t *in, size_t size, uint64_t *value);
bool GetBool(const uint8_t *in, size_t size, bool *value);

}  // namespace cbor
