// The CBOR subset of §10.18.2. Pure, bounded, and fed by a device on the other
// end of a cable — which is why every length is checked against what is actually
// in the buffer rather than against what the encoding promised.

#include "cbor.h"

#include <cstring>

namespace cbor {
namespace {

constexpr uint8_t kMajorUint = 0;
constexpr uint8_t kMajorNint = 1;
constexpr uint8_t kMajorBytes = 2;
constexpr uint8_t kMajorText = 3;
constexpr uint8_t kMajorArray = 4;
constexpr uint8_t kMajorMap = 5;
constexpr uint8_t kMajorSimple = 7;

// Decodes the head — major type and argument — returning the head's length in
// bytes, or 0 for anything malformed. Indefinite lengths (`additional == 31`)
// come back as 0 too: this firmware does not speak them, and treating one as a
// zero-length item would be a silent misparse.
size_t DecodeHead(const uint8_t *in, size_t size, uint8_t *major, uint64_t *value) {
    if (size < 1) {
        return 0;
    }
    *major = static_cast<uint8_t>(in[0] >> 5);
    const uint8_t additional = static_cast<uint8_t>(in[0] & 0x1F);

    if (additional < 24) {
        *value = additional;
        return 1;
    }

    size_t width = 0;
    switch (additional) {
        case 24:
            width = 1;
            break;
        case 25:
            width = 2;
            break;
        case 26:
            width = 4;
            break;
        case 27:
            width = 8;
            break;
        default:
            // 28..30 are reserved and 31 is an indefinite length.
            return 0;
    }
    if (size < 1 + width) {
        return 0;
    }
    uint64_t acc = 0;
    for (size_t i = 0; i < width; i++) {
        acc = (acc << 8) | in[1 + i];
    }
    *value = acc;
    return 1 + width;
}

bool SkipDepth(const uint8_t *in, size_t size, size_t *offset, size_t depth);

}  // namespace

// --- Writing --------------------------------------------------------------

bool Writer::Put(const uint8_t *data, size_t length) {
    if (!ok_) {
        return false;
    }
    if (length_ + length > capacity_) {
        ok_ = false;
        return false;
    }
    std::memcpy(buffer_ + length_, data, length);
    length_ += length;
    return true;
}

bool Writer::Head(uint8_t major, uint64_t value) {
    uint8_t head[9];
    size_t n = 0;
    // **The shortest form that fits, which is what canonical CBOR requires** —
    // and CTAP2 keys are canonical or the key rejects the request.
    if (value < 24) {
        head[0] = static_cast<uint8_t>((major << 5) | value);
        n = 1;
    } else if (value <= 0xFF) {
        head[0] = static_cast<uint8_t>((major << 5) | 24);
        head[1] = static_cast<uint8_t>(value);
        n = 2;
    } else if (value <= 0xFFFF) {
        head[0] = static_cast<uint8_t>((major << 5) | 25);
        head[1] = static_cast<uint8_t>(value >> 8);
        head[2] = static_cast<uint8_t>(value);
        n = 3;
    } else if (value <= 0xFFFFFFFFu) {
        head[0] = static_cast<uint8_t>((major << 5) | 26);
        head[1] = static_cast<uint8_t>(value >> 24);
        head[2] = static_cast<uint8_t>(value >> 16);
        head[3] = static_cast<uint8_t>(value >> 8);
        head[4] = static_cast<uint8_t>(value);
        n = 5;
    } else {
        head[0] = static_cast<uint8_t>((major << 5) | 27);
        for (int i = 0; i < 8; i++) {
            head[1 + i] = static_cast<uint8_t>(value >> (56 - 8 * i));
        }
        n = 9;
    }
    return Put(head, n);
}

bool Writer::Uint(uint64_t value) { return Head(kMajorUint, value); }

bool Writer::Nint(uint64_t magnitude) { return Head(kMajorNint, magnitude); }

bool Writer::Int(int64_t value) {
    if (value < 0) {
        // -1 is encoded as major 1 with argument 0, which is what the negation
        // below produces without ever forming -(-2^63) in a signed type.
        return Nint(static_cast<uint64_t>(-(value + 1)));
    }
    return Uint(static_cast<uint64_t>(value));
}

bool Writer::Bytes(const uint8_t *data, size_t length) {
    return Head(kMajorBytes, length) && Put(data, length);
}

bool Writer::TextN(const char *text, size_t length) {
    return Head(kMajorText, length) && Put(reinterpret_cast<const uint8_t *>(text), length);
}

bool Writer::Text(const char *text) {
    return TextN(text, text == nullptr ? 0 : std::strlen(text));
}

bool Writer::ArrayHeader(size_t count) { return Head(kMajorArray, count); }

bool Writer::MapHeader(size_t count) { return Head(kMajorMap, count); }

bool Writer::Bool(bool value) {
    const uint8_t byte = static_cast<uint8_t>((kMajorSimple << 5) | (value ? 21 : 20));
    return Put(&byte, 1);
}

bool Writer::Raw(const uint8_t *data, size_t length) { return Put(data, length); }

// --- Reading --------------------------------------------------------------

bool Decode(const uint8_t *in, size_t size, Item *out) {
    if (in == nullptr || out == nullptr) {
        return false;
    }
    *out = Item{};

    uint8_t major = 0;
    uint64_t value = 0;
    const size_t head = DecodeHead(in, size, &major, &value);
    if (head == 0) {
        return false;
    }
    out->header = head;
    out->value = value;

    switch (major) {
        case kMajorUint:
            out->type = Type::kUint;
            out->signed_value = static_cast<int64_t>(value);
            return true;
        case kMajorNint:
            out->type = Type::kNint;
            out->signed_value = -static_cast<int64_t>(value) - 1;
            return true;
        case kMajorBytes:
        case kMajorText:
            // **The length is the device's number, so it is checked against the
            // buffer we actually hold.** This one line is most of the reason
            // this parser exists rather than a borrowed one.
            if (value > size - head) {
                return false;
            }
            out->type = (major == kMajorBytes) ? Type::kBytes : Type::kText;
            out->data = in + head;
            out->length = static_cast<size_t>(value);
            return true;
        case kMajorArray:
            out->type = Type::kArray;
            return true;
        case kMajorMap:
            out->type = Type::kMap;
            return true;
        case kMajorSimple:
            if (value == 20 || value == 21) {
                out->type = Type::kSimple;
                out->boolean = (value == 21);
                return true;
            }
            if (value == 22) {  // null
                out->type = Type::kSimple;
                out->boolean = false;
                return true;
            }
            return false;
        default:
            return false;
    }
}

namespace {

bool SkipDepth(const uint8_t *in, size_t size, size_t *offset, size_t depth) {
    if (depth > kMaxDepth) {
        return false;
    }
    Item item;
    if (!Decode(in + *offset, size - *offset, &item)) {
        return false;
    }
    switch (item.type) {
        case Type::kUint:
        case Type::kNint:
        case Type::kSimple:
            *offset += item.header;
            return true;
        case Type::kBytes:
        case Type::kText:
            *offset += item.header + item.length;
            return true;
        case Type::kArray: {
            *offset += item.header;
            for (uint64_t i = 0; i < item.value; i++) {
                if (!SkipDepth(in, size, offset, depth + 1)) {
                    return false;
                }
            }
            return true;
        }
        case Type::kMap: {
            *offset += item.header;
            for (uint64_t i = 0; i < item.value; i++) {
                if (!SkipDepth(in, size, offset, depth + 1)) {
                    return false;
                }
                if (!SkipDepth(in, size, offset, depth + 1)) {
                    return false;
                }
            }
            return true;
        }
        default:
            return false;
    }
}

}  // namespace

bool Skip(const uint8_t *in, size_t size, size_t *offset) {
    if (in == nullptr || offset == nullptr || *offset > size) {
        return false;
    }
    return SkipDepth(in, size, offset, 0);
}

namespace {

// The body of both `MapFind` forms: walk the pairs, hand each key to `matches`,
// and return the first value whose key it accepts.
template <typename Match>
bool FindIn(const uint8_t *in, size_t size, Match matches, const uint8_t **value,
            size_t *value_size) {
    Item head;
    if (!Decode(in, size, &head) || head.type != Type::kMap) {
        return false;
    }
    size_t offset = head.header;
    for (uint64_t i = 0; i < head.value; i++) {
        if (offset >= size) {
            return false;
        }
        Item key;
        if (!Decode(in + offset, size - offset, &key)) {
            return false;
        }
        const size_t key_start = offset;
        if (!Skip(in, size, &offset)) {
            return false;
        }
        if (offset >= size) {
            return false;
        }
        if (matches(key, in + key_start)) {
            *value = in + offset;
            *value_size = size - offset;
            return true;
        }
        if (!Skip(in, size, &offset)) {
            return false;
        }
    }
    return false;
}

}  // namespace

bool MapFind(const uint8_t *in, size_t size, int64_t key, const uint8_t **value,
             size_t *value_size) {
    if (in == nullptr || value == nullptr || value_size == nullptr) {
        return false;
    }
    return FindIn(
        in, size,
        [key](const Item &item, const uint8_t *) {
            return (item.type == Type::kUint || item.type == Type::kNint) &&
                   item.signed_value == key;
        },
        value, value_size);
}

bool MapFindText(const uint8_t *in, size_t size, const char *key, const uint8_t **value,
                 size_t *value_size) {
    if (in == nullptr || key == nullptr || value == nullptr || value_size == nullptr) {
        return false;
    }
    const size_t key_length = std::strlen(key);
    return FindIn(
        in, size,
        [key, key_length](const Item &item, const uint8_t *) {
            return item.type == Type::kText && item.length == key_length &&
                   std::memcmp(item.data, key, key_length) == 0;
        },
        value, value_size);
}

bool GetBytes(const uint8_t *in, size_t size, const uint8_t **data, size_t *length) {
    Item item;
    if (!Decode(in, size, &item) || item.type != Type::kBytes) {
        return false;
    }
    *data = item.data;
    *length = item.length;
    return true;
}

bool GetText(const uint8_t *in, size_t size, const char **text, size_t *length) {
    Item item;
    if (!Decode(in, size, &item) || item.type != Type::kText) {
        return false;
    }
    *text = reinterpret_cast<const char *>(item.data);
    *length = item.length;
    return true;
}

bool GetUint(const uint8_t *in, size_t size, uint64_t *value) {
    Item item;
    if (!Decode(in, size, &item) || item.type != Type::kUint) {
        return false;
    }
    *value = item.value;
    return true;
}

bool GetBool(const uint8_t *in, size_t size, bool *value) {
    Item item;
    if (!Decode(in, size, &item) || item.type != Type::kSimple) {
        return false;
    }
    *value = item.boolean;
    return true;
}

}  // namespace cbor
