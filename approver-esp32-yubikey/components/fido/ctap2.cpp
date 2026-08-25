// CTAP2 requests and responses (CLAUDE.md §10.18.2). Bytes in, bytes out — no
// USB, no key material, no verdict. §10.11's host tier runs all of it against
// captured frames.

#include "ctap2.h"

#include <cstring>

#include "cbor.h"

namespace ctap2 {
namespace {

// COSE_Key labels, from RFC 8152.
constexpr int64_t kCoseKty = 1;
constexpr int64_t kCoseAlg = 3;
constexpr int64_t kCoseCrv = -1;
constexpr int64_t kCoseX = -2;
constexpr int64_t kCoseY = -3;
constexpr int64_t kCoseKtyEc2 = 2;
constexpr int64_t kCoseCrvP256 = 1;

uint32_t GetBe32(const uint8_t *in) {
    return (static_cast<uint32_t>(in[0]) << 24) | (static_cast<uint32_t>(in[1]) << 16) |
           (static_cast<uint32_t>(in[2]) << 8) | static_cast<uint32_t>(in[3]);
}

bool TextEquals(const uint8_t *in, size_t size, const char *want) {
    const char *text = nullptr;
    size_t length = 0;
    if (!cbor::GetText(in, size, &text, &length)) {
        return false;
    }
    const size_t want_length = std::strlen(want);
    return length == want_length && std::memcmp(text, want, want_length) == 0;
}

// Walks the `versions` array of a `getInfo` response.
void ReadVersions(const uint8_t *in, size_t size, Info *out) {
    cbor::Item array;
    if (!cbor::Decode(in, size, &array) || array.type != cbor::Type::kArray) {
        return;
    }
    size_t offset = array.header;
    for (uint64_t i = 0; i < array.value; i++) {
        if (offset >= size) {
            return;
        }
        if (TextEquals(in + offset, size - offset, "FIDO_2_0")) {
            out->fido2 = true;
        } else if (TextEquals(in + offset, size - offset, "FIDO_2_1")) {
            out->fido2 = true;
            out->fido21 = true;
        } else if (TextEquals(in + offset, size - offset, "FIDO_2_1_PRE")) {
            out->fido2 = true;
        } else if (TextEquals(in + offset, size - offset, "U2F_V2")) {
            out->u2f = true;
        }
        if (!cbor::Skip(in, size, &offset)) {
            return;
        }
    }
}

void ReadOption(const uint8_t *options, size_t size, const char *name, bool *out) {
    const uint8_t *value = nullptr;
    size_t left = 0;
    if (cbor::MapFindText(options, size, name, &value, &left)) {
        bool flag = false;
        if (cbor::GetBool(value, left, &flag)) {
            *out = flag;
        }
    }
}

// Every response body is `status || CBOR`. This is the one place that peels the
// status off, so no parser below can forget to.
bool SplitStatus(const uint8_t *response, size_t size, uint8_t *status, const uint8_t **body,
                 size_t *body_size) {
    if (response == nullptr || size < 1) {
        if (status != nullptr) {
            *status = kErrInvalidLength;
        }
        return false;
    }
    if (status != nullptr) {
        *status = response[0];
    }
    if (response[0] != kOk) {
        return false;
    }
    *body = response + 1;
    *body_size = size - 1;
    return true;
}

}  // namespace

const char *StatusName(uint8_t status) {
    switch (status) {
        case kOk:
            return "ok";
        case kErrInvalidCommand:
            return "invalid command";
        case kErrInvalidParameter:
            return "invalid parameter";
        case kErrInvalidLength:
            return "invalid length";
        case kErrCborUnexpectedType:
            return "unexpected CBOR type";
        case kErrInvalidCbor:
            return "invalid CBOR";
        case kErrMissingParameter:
            return "missing parameter";
        case kErrCredentialExcluded:
            return "credential already enrolled on this key";
        case kErrUnsupportedAlgorithm:
            return "unsupported algorithm";
        case kErrOperationDenied:
            return "operation denied";
        case kErrUserActionPending:
            return "waiting for the operator";
        case kErrKeepaliveCancel:
            return "cancelled";
        case kErrNoCredentials:
            return "no credential on this key for this device";
        case kErrPinAuthInvalid:
            return "PIN auth invalid";
        case kErrPinRequired:
            return "this key requires a PIN, which this device cannot supply";
        case kErrUpRequired:
            return "user presence required";
        case kErrActionTimeout:
            return "the key timed out waiting to be touched";
        default:
            return "unknown status";
    }
}

// --- Requests -------------------------------------------------------------

bool BuildGetInfo(uint8_t *out, size_t capacity, size_t *length) {
    if (out == nullptr || capacity < 1) {
        return false;
    }
    out[0] = kGetInfo;
    *length = 1;
    return true;
}

bool BuildMakeCredential(const uint8_t *client_data_hash, const uint8_t *user_id, uint8_t *out,
                         size_t capacity, size_t *length) {
    if (out == nullptr || capacity < 2 || client_data_hash == nullptr || user_id == nullptr) {
        return false;
    }
    out[0] = kMakeCredential;
    cbor::Writer w(out + 1, capacity - 1);

    // Keys 1..4, ascending — canonical CBOR, which the key will check.
    w.MapHeader(4);

    w.Int(1);
    w.Bytes(client_data_hash, kClientDataHashSize);

    w.Int(2);  // rp
    w.MapHeader(2);
    w.Text("id");
    w.Text(kRelyingPartyId);
    w.Text("name");
    w.Text(kRelyingPartyName);

    w.Int(3);  // user
    w.MapHeader(3);
    w.Text("id");
    w.Bytes(user_id, kUserIdSize);
    w.Text("name");
    w.Text(kUserName);
    w.Text("displayName");
    w.Text(kUserDisplayName);

    w.Int(4);  // pubKeyCredParams
    w.ArrayHeader(1);
    w.MapHeader(2);
    w.Text("alg");
    w.Int(kAlgEs256);
    w.Text("type");
    w.Text("public-key");

    // **No key 7 (`options`), on purpose.** The defaults are `rk: false` and
    // `uv: false`, which is exactly what this device wants: a non-discoverable
    // credential that costs the key none of its resident slots, and no user
    // verification it would need a PIN pad to satisfy. Sending the defaults
    // explicitly would be three more bytes and one more thing to get wrong.
    //
    // Presence is **not** an option here — `makeCredential` always requires a
    // touch, which is why enrolment is something the operator can feel happening.

    if (!w.Ok()) {
        return false;
    }
    *length = 1 + w.Length();
    return true;
}

bool BuildGetAssertion(const uint8_t *client_data_hash, const uint8_t *credential_id,
                       size_t credential_id_length, uint8_t *out, size_t capacity,
                       size_t *length) {
    if (out == nullptr || capacity < 2 || client_data_hash == nullptr ||
        credential_id == nullptr || credential_id_length == 0) {
        return false;
    }
    out[0] = kGetAssertion;
    cbor::Writer w(out + 1, capacity - 1);

    w.MapHeader(4);

    w.Int(1);  // rpId
    w.Text(kRelyingPartyId);

    w.Int(2);  // clientDataHash
    w.Bytes(client_data_hash, kClientDataHashSize);

    w.Int(3);  // allowList
    w.ArrayHeader(1);
    w.MapHeader(2);
    w.Text("id");
    w.Bytes(credential_id, credential_id_length);
    w.Text("type");
    w.Text("public-key");

    // **`up: true`, said out loud.** It is the default, and it is written anyway
    // because it is the single most important byte this firmware sends: it is
    // what makes the key refuse to answer until a human touches it, and it is
    // the whole reason this gate is worth having. A default that silently
    // changed would be a device that approved things by itself.
    w.Int(5);  // options
    w.MapHeader(1);
    w.Text("up");
    w.Bool(true);

    if (!w.Ok()) {
        return false;
    }
    *length = 1 + w.Length();
    return true;
}

bool BuildGetAssertionAny(const uint8_t *client_data_hash, uint8_t *out, size_t capacity,
                          size_t *length) {
    if (out == nullptr || capacity < 2 || client_data_hash == nullptr) {
        return false;
    }
    out[0] = kGetAssertion;
    cbor::Writer w(out + 1, capacity - 1);

    w.MapHeader(3);
    w.Int(1);
    w.Text(kRelyingPartyId);
    w.Int(2);
    w.Bytes(client_data_hash, kClientDataHashSize);
    w.Int(5);
    w.MapHeader(1);
    w.Text("up");
    w.Bool(true);

    if (!w.Ok()) {
        return false;
    }
    *length = 1 + w.Length();
    return true;
}

// --- Responses ------------------------------------------------------------

bool ParseInfo(const uint8_t *response, size_t size, Info *out, uint8_t *status) {
    const uint8_t *body = nullptr;
    size_t body_size = 0;
    if (!SplitStatus(response, size, status, &body, &body_size)) {
        return false;
    }
    *out = Info{};

    const uint8_t *value = nullptr;
    size_t left = 0;

    if (cbor::MapFind(body, body_size, 1, &value, &left)) {
        ReadVersions(value, left, out);
    }
    if (cbor::MapFind(body, body_size, 3, &value, &left)) {
        const uint8_t *aaguid = nullptr;
        size_t aaguid_length = 0;
        if (cbor::GetBytes(value, left, &aaguid, &aaguid_length) && aaguid_length == 16) {
            out->aaguid = aaguid;
        }
    }
    if (cbor::MapFind(body, body_size, 4, &value, &left)) {
        ReadOption(value, left, "rk", &out->option_rk);
        ReadOption(value, left, "up", &out->option_up);
        ReadOption(value, left, "uv", &out->option_uv);
        ReadOption(value, left, "clientPin", &out->client_pin_set);
    }
    if (cbor::MapFind(body, body_size, 5, &value, &left)) {
        cbor::GetUint(value, left, &out->max_message_size);
    }
    return true;
}

bool ParseAuthData(const uint8_t *data, size_t size, AuthData *out) {
    if (data == nullptr || out == nullptr || size < kAuthDataMinSize) {
        return false;
    }
    *out = AuthData{};
    out->rp_id_hash = data;
    out->flags = data[kRpIdHashSize];
    out->sign_count = GetBe32(data + kRpIdHashSize + 1);

    if ((out->flags & kFlagAttestedData) == 0) {
        return true;
    }

    // aaguid(16) || credentialIdLength(2, big-endian) || credentialId || COSE key
    size_t offset = kAuthDataMinSize;
    if (size < offset + 18) {
        return false;
    }
    const uint8_t *aaguid = data + offset;
    offset += 16;
    const size_t id_length =
        (static_cast<size_t>(data[offset]) << 8) | static_cast<size_t>(data[offset + 1]);
    offset += 2;
    // The length is the key's number. Checked against the buffer, not trusted.
    if (id_length == 0 || size < offset + id_length) {
        return false;
    }
    out->credential_id = data + offset;
    out->credential_id_length = id_length;
    offset += id_length;

    // Whatever is left is the COSE key, followed by extensions this firmware
    // requests none of. `cbor::Skip` is what finds where the key ends — the
    // alternative would be assuming the key is the rest of the buffer, which is
    // true today and would silently become wrong the day an extension appears.
    size_t key_end = 0;
    if (!cbor::Skip(data + offset, size - offset, &key_end) || key_end == 0) {
        return false;
    }
    out->cose_key = data + offset;
    out->cose_key_length = key_end;
    // `aaguid` is not part of the struct's contract for assertions, so it is only
    // set on the path that actually has one.
    (void)aaguid;
    return true;
}

bool ParseMakeCredential(const uint8_t *response, size_t size, Credential *out, uint8_t *status) {
    const uint8_t *body = nullptr;
    size_t body_size = 0;
    if (!SplitStatus(response, size, status, &body, &body_size)) {
        return false;
    }
    *out = Credential{};

    const uint8_t *value = nullptr;
    size_t left = 0;
    // Key 2 is authData. **Key 1 (`fmt`) and key 3 (`attStmt`) are deliberately
    // not read**: this device does not verify attestation. It has no root store,
    // no way to be told which manufacturers are acceptable, and no operator to
    // ask — and a check that always passes is worse than an absent one, because
    // it reads as a check. What the enrolment binds is *this credential to this
    // device*, which is authData's job and not the attestation statement's.
    if (!cbor::MapFind(body, body_size, 2, &value, &left)) {
        return false;
    }
    const uint8_t *auth_data = nullptr;
    size_t auth_data_length = 0;
    if (!cbor::GetBytes(value, left, &auth_data, &auth_data_length)) {
        return false;
    }

    AuthData parsed;
    if (!ParseAuthData(auth_data, auth_data_length, &parsed)) {
        return false;
    }
    if ((parsed.flags & kFlagAttestedData) == 0 || parsed.credential_id == nullptr) {
        return false;
    }
    if (parsed.credential_id_length > kMaxCredentialIdSize ||
        parsed.cose_key_length > kMaxCoseKeySize) {
        return false;
    }

    out->id = parsed.credential_id;
    out->id_length = parsed.credential_id_length;
    out->cose_key = parsed.cose_key;
    out->cose_key_length = parsed.cose_key_length;
    out->flags = parsed.flags;
    out->aaguid = auth_data + kAuthDataMinSize;
    return true;
}

bool ParseAssertion(const uint8_t *response, size_t size, Assertion *out, uint8_t *status) {
    const uint8_t *body = nullptr;
    size_t body_size = 0;
    if (!SplitStatus(response, size, status, &body, &body_size)) {
        return false;
    }
    *out = Assertion{};

    const uint8_t *value = nullptr;
    size_t left = 0;

    // Key 1 is the credential descriptor, and it is optional when the request
    // carried an allow list of one — some keys omit it. Read when present so
    // that the caller can check it is the credential it asked for.
    if (cbor::MapFind(body, body_size, 1, &value, &left)) {
        const uint8_t *id = nullptr;
        size_t id_left = 0;
        if (cbor::MapFindText(value, left, "id", &id, &id_left)) {
            cbor::GetBytes(id, id_left, &out->credential_id, &out->credential_id_length);
        }
    }

    if (!cbor::MapFind(body, body_size, 2, &value, &left) ||
        !cbor::GetBytes(value, left, &out->auth_data, &out->auth_data_length)) {
        return false;
    }
    if (!cbor::MapFind(body, body_size, 3, &value, &left) ||
        !cbor::GetBytes(value, left, &out->signature, &out->signature_length)) {
        return false;
    }
    if (cbor::MapFind(body, body_size, 5, &value, &left)) {
        cbor::GetUint(value, left, &out->number_of_credentials);
    }
    return true;
}

bool CoseToP256Point(const uint8_t *cose_key, size_t size, uint8_t *out, size_t capacity) {
    if (cose_key == nullptr || out == nullptr || capacity < kP256PointSize) {
        return false;
    }

    const uint8_t *value = nullptr;
    size_t left = 0;
    cbor::Item item;

    // **Every one of these three checks is load-bearing.** A COSE key that is not
    // an EC2 P-256 ES256 key must be refused *here*, before its coordinates are
    // handed to a verifier that would interpret them as if it were one.
    if (!cbor::MapFind(cose_key, size, kCoseKty, &value, &left) ||
        !cbor::Decode(value, left, &item) || item.signed_value != kCoseKtyEc2) {
        return false;
    }
    if (!cbor::MapFind(cose_key, size, kCoseAlg, &value, &left) ||
        !cbor::Decode(value, left, &item) || item.signed_value != kAlgEs256) {
        return false;
    }
    if (!cbor::MapFind(cose_key, size, kCoseCrv, &value, &left) ||
        !cbor::Decode(value, left, &item) || item.signed_value != kCoseCrvP256) {
        return false;
    }

    const uint8_t *x = nullptr;
    const uint8_t *y = nullptr;
    size_t x_length = 0;
    size_t y_length = 0;
    if (!cbor::MapFind(cose_key, size, kCoseX, &value, &left) ||
        !cbor::GetBytes(value, left, &x, &x_length) || x_length != 32) {
        return false;
    }
    if (!cbor::MapFind(cose_key, size, kCoseY, &value, &left) ||
        !cbor::GetBytes(value, left, &y, &y_length) || y_length != 32) {
        return false;
    }

    out[0] = 0x04;  // uncompressed point
    std::memcpy(out + 1, x, 32);
    std::memcpy(out + 33, y, 32);
    return true;
}

}  // namespace ctap2
