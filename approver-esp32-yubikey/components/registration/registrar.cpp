#include "registrar.h"

#include <cstdio>
#include <cstring>
#include <ctime>

#include "cJSON.h"
#include "crypto.h"
#include "esp_log.h"
#include "fido.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nats_link.h"
#include "registration.h"
#include "storage.h"

namespace registration {
namespace {

constexpr const char *TAG = "registration";

// 32 random bytes, base64 — the same shape the three existing responders use.
constexpr size_t kNonceBytes = 32;

// What is on the filesystem, and it is small on purpose: two strings and a
// timestamp. Anything else about this device belongs in `config.json`, which is
// the file a restore is allowed to touch (§10.15).
struct Record {
    bool present = false;
    int64_t ts = 0;
    char key_id[protocol::kKeyIdMax + 1] = {};
    char server_key[protocol::kB64_32Max + 1] = {};

    // **Which key was registered** (§10.18.1). The handler's allowlist entry names
    // one public key, and on this device that key comes from the enrolment — so a
    // re-enrolment silently invalidates the registration. Recording it here turns
    // that from "every approval is quietly rejected" into a refusal to subscribe,
    // with a sentence saying which of the two files to fix.
    char pubkey[protocol::kPubkeyMax + 1] = {};
};

Record g_record;

// --- the in-flight exchange -------------------------------------------------
// One at a time, and the console is the only caller, so this is a static rather
// than something passed around (§10.14.1). `g_waiting` is what the bus task
// checks before touching any of it.
StaticSemaphore_t g_reply_storage;
SemaphoreHandle_t g_reply = nullptr;

volatile bool g_waiting = false;
char g_reply_json[kMaxReplyBytes];
size_t g_reply_length = 0;
bool g_reply_overlong = false;

char g_inbox[40];

// **The verifier handed to the parser** (§10.7). Base64 and Ed25519 are
// libsodium's, and this is the whole of what `components/protocol` is not allowed
// to know — the seam falls exactly on the layer boundary.
bool VerifyReply(const char *public_key_b64, const char *message, size_t length,
                 const char *signature_b64, void *) {
    uint8_t public_key[crypto::kPublicKeySize];
    uint8_t signature[crypto::kSignatureSize];

    // **Exactly the right number of bytes, or no.** A key that decoded to 31
    // bytes would be read past by the verify, and a base64 string that decodes to
    // something shorter than a key is not a key with a typo in it — it is not a
    // key.
    if (crypto::Base64Decode(public_key_b64, public_key, sizeof public_key) !=
        crypto::kPublicKeySize) {
        return false;
    }
    if (crypto::Base64Decode(signature_b64, signature, sizeof signature) !=
        crypto::kSignatureSize) {
        return false;
    }
    return crypto::Verify(public_key, reinterpret_cast<const uint8_t *>(message), length, signature);
}

// Runs on the bus task. Copies and signals; every decision happens on the
// console task, which is the one that can afford the stack a verify needs.
void OnReply(const nats::Message &message, void *) {
    if (!g_waiting) {
        return;
    }
    // §10.10: everything off the bus is attacker-shaped. A reply too big for the
    // buffer is dropped **and the fact recorded**, so the console can say "too
    // long" instead of "timed out" — which are different problems.
    if (message.size == 0 || message.size >= sizeof g_reply_json) {
        g_reply_overlong = message.size != 0;
        g_reply_length = 0;
    } else {
        std::memcpy(g_reply_json, message.data, message.size);
        g_reply_json[message.size] = '\0';
        g_reply_length = message.size;
        g_reply_overlong = false;
    }
    g_waiting = false;
    xSemaphoreGive(g_reply);
}

void MakeInbox(char *out, size_t capacity) {
    // `_INBOX.<32 hex>` (§10.5). The randomness matters less here than in the
    // nonce — an inbox nobody can guess is a nicety, a nonce nobody can guess is
    // the replay protection — but it comes from the same source and costs
    // nothing.
    uint8_t bytes[16];
    esp_fill_random(bytes, sizeof bytes);
    size_t at = static_cast<size_t>(std::snprintf(out, capacity, "_INBOX."));
    for (size_t i = 0; i < sizeof bytes && at + 2 < capacity; ++i) {
        static const char kHex[] = "0123456789abcdef";
        out[at++] = kHex[bytes[i] >> 4];
        out[at++] = kHex[bytes[i] & 0x0f];
    }
    out[at] = '\0';
}

esp_err_t Save(const Record &record) {
    char buffer[256];
    cJSON *root = cJSON_CreateObject();
    if (root == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddNumberToObject(root, "v", 1);
    cJSON_AddStringToObject(root, "key_id", record.key_id);
    cJSON_AddStringToObject(root, "server_key", record.server_key);
    cJSON_AddStringToObject(root, "pubkey", record.pubkey);
    cJSON_AddNumberToObject(root, "registered_ts", static_cast<double>(record.ts));
    const cJSON_bool printed =
        cJSON_PrintPreallocated(root, buffer, static_cast<int>(sizeof buffer), 1);
    cJSON_Delete(root);
    if (!printed) {
        return ESP_ERR_INVALID_SIZE;
    }
    // The same write `config.json` gets, and for a sharper reason: a truncated
    // `server_key` that happened to still be valid base64 would pin this device
    // to a key nobody has, and only a `forget` would get it back.
    return storage::WriteFileAtomically(kPath, kTempPath, buffer, std::strlen(buffer));
}

bool Load(Record *out) {
    char buffer[256];
    size_t length = 0;
    if (storage::ReadFile(kPath, buffer, sizeof buffer, &length) != ESP_OK) {
        return false;
    }

    cJSON *root = cJSON_ParseWithLength(buffer, length);
    if (root == nullptr) {
        return false;
    }
    struct Guard {
        cJSON *root;
        ~Guard() { cJSON_Delete(root); }
    } guard{root};

    if (!cJSON_IsObject(root)) {
        return false;
    }
    const cJSON *key_id = cJSON_GetObjectItemCaseSensitive(root, "key_id");
    const cJSON *server_key = cJSON_GetObjectItemCaseSensitive(root, "server_key");
    if (!cJSON_IsString(key_id) || !cJSON_IsString(server_key) ||
        std::strlen(key_id->valuestring) > protocol::kKeyIdMax ||
        std::strlen(server_key->valuestring) != protocol::kB64_32Max) {
        // **A half-read registration is worse than none.** A device that came up
        // pinned to a truncated key would refuse every future registration with
        // "signed by a different key", which is a sentence that sends somebody
        // hunting an attacker who is not there.
        return false;
    }

    std::snprintf(out->key_id, sizeof out->key_id, "%s", key_id->valuestring);
    std::snprintf(out->server_key, sizeof out->server_key, "%s", server_key->valuestring);

    // The registered public key. **Absent is not tolerated**: a record with no
    // `pubkey` was written by a build that registered a different kind of key
    // (§10.18), and treating it as "probably still fine" is exactly the silent
    // failure this field exists to prevent.
    const cJSON *pubkey = cJSON_GetObjectItemCaseSensitive(root, "pubkey");
    if (!cJSON_IsString(pubkey) || std::strlen(pubkey->valuestring) > protocol::kPubkeyMax ||
        pubkey->valuestring[0] == 0) {
        return false;
    }
    std::snprintf(out->pubkey, sizeof out->pubkey, "%s", pubkey->valuestring);
    const cJSON *ts = cJSON_GetObjectItemCaseSensitive(root, "registered_ts");
    out->ts = cJSON_IsNumber(ts) ? static_cast<int64_t>(ts->valuedouble) : 0;
    out->present = true;
    return true;
}

}  // namespace

esp_err_t Init() {
    if (g_reply == nullptr) {
        g_reply = xSemaphoreCreateBinaryStatic(&g_reply_storage);
    }

    // Before the read, the same way `config::Init` does it: a crash in the rename
    // window left a complete file with no name, and finishing that is the
    // recovery (§10.15).
    storage::RecoverInterruptedWrite(kPath, kTempPath);

    Record loaded;
    if (Load(&loaded)) {
        g_record = loaded;
        ESP_LOGI(TAG, "registered as %s, handler key %s", g_record.key_id, g_record.server_key);
        // **The staleness check is not made here**, and that is the whole point of
        // `ReportKeyBinding` existing. It needs the enrolment, and `fido::Init()`
        // runs after this — after the bus, for the heap reason `main.cpp` gives — so
        // asking `Registered()` at this moment compares against an empty key and
        // calls *every* registration stale. It did, on the first boot after a good
        // registration, and the line it printed sends an operator to spend a
        // one-time token on a registration that is fine.
    } else if (storage::Exists(kPath)) {
        // The file is there and is not one. Said out loud rather than treated as
        // "not registered", because those need different things from an operator.
        ESP_LOGW(TAG, "%s is unreadable - this device is not registered", kPath);
    } else {
        ESP_LOGI(TAG, "not registered");
    }
    return ESP_OK;
}

// **Registered means "the handler knows the key this device is holding now"**, and
// the second half is what makes this more than a file check: the allowlist entry
// names one public key, and on this device that key belongs to the enrolment. A
// `key enrol` produces a new one, so the entry becomes worthless the moment it runs.
//
// §10.10 rule 5 is why this is enforced rather than logged: a device that subscribed
// with a stale registration would take requests out of a shared queue group and
// answer them with signatures the hook rejects — which is worse than not answering,
// because it is invisible.
bool Registered() {
    if (!g_record.present) {
        return false;
    }
    const char *current = fido::PublicKeyBase64();
    return current[0] != 0 && std::strcmp(current, g_record.pubkey) == 0;
}

void ReportKeyBinding() {
    if (!g_record.present || !fido::Enrolled()) {
        return;
    }
    if (Registered()) {
        return;
    }
    ESP_LOGW(TAG,
             "the registration is for another key (%s) - this device signs as %s now. "
             "Run `register <token>` with a fresh token.",
             g_record.pubkey, fido::PublicKeyBase64());
}

// Was there a registration at all, whatever it names. The console needs the
// difference so it can say "stale" instead of "none" (`commands.md`).
bool RegistrationPresent() { return g_record.present; }

const char *RegisteredPublicKey() { return g_record.present ? g_record.pubkey : ""; }
const char *KeyId() { return g_record.present ? g_record.key_id : ""; }
const char *ServerKey() { return g_record.present ? g_record.server_key : ""; }
int64_t RegisteredTs() { return g_record.present ? g_record.ts : 0; }

esp_err_t Register(const char *token, char *detail, size_t detail_size) {
    if (token == nullptr || detail == nullptr || detail_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    detail[0] = '\0';

    // **The argument before the world.** What was typed is checkable for free and
    // is certain; whether there is a network is state that changes under you. An
    // operator with a mistyped token and a radio that is off should be told about
    // the token — it is the half they can fix without going anywhere, and it is
    // wrong whatever the network does. Found by doing it the other way round on
    // the board and being told about the bus while holding a token with no dot in
    // it.
    char key_id[protocol::kKeyIdMax + 1];
    if (!protocol::ParseToken(token, key_id, sizeof key_id)) {
        std::snprintf(detail, detail_size, "that is not a token: it must be '<key id>.<secret>'");
        return ESP_ERR_INVALID_ARG;
    }
    if (std::strcmp(key_id, protocol::kKeyId) != 0) {
        // The handler binds the token to the name in its prefix, so a token for
        // another slot would be refused there anyway — with "key_id mismatch",
        // several seconds later, over the network. Saying it here is the same
        // answer sooner and in words.
        std::snprintf(detail, detail_size, "that token is for '%s'; this device is '%s'", key_id,
                      protocol::kKeyId);
        return ESP_ERR_INVALID_ARG;
    }

    // Then the two things about the world somebody can act on, named rather than
    // folded into one failure. A device with no key has nothing to register; a
    // device with no bus has nobody to register with.
    // **Enrolment comes first on this device**, which is the one ordering §10.18
    // changed: the key being registered is derived from a security key, so with
    // nothing enrolled there is no key to register — not a weaker one, none.
    if (!fido::Enrolled() || fido::PublicKeyBase64()[0] == 0) {
        std::snprintf(detail, detail_size,
                      "this device has no signing key yet - plug a security key in and run "
                      "`key enrol` first");
        return ESP_ERR_INVALID_STATE;
    }
    if (nats::Get().state != nats::State::kConnected) {
        std::snprintf(detail, detail_size, "not connected to the bus - 'nats' says why");
        return ESP_ERR_INVALID_STATE;
    }

    // **The nonce, and it is here rather than earlier** (§10.7): the bus is
    // connected, so the radio is up, so `esp_fill_random` is a true random source
    // rather than a PRNG. A predictable nonce is a reply somebody can replay.
    uint8_t nonce_bytes[kNonceBytes];
    esp_fill_random(nonce_bytes, sizeof nonce_bytes);
    char nonce[protocol::kB64_32Max + 1];
    if (!crypto::Base64Encode(nonce_bytes, sizeof nonce_bytes, nonce, sizeof nonce)) {
        std::snprintf(detail, detail_size, "could not build a nonce");
        return ESP_FAIL;
    }

    protocol::RegistrationRequest request;
    request.ts = static_cast<int64_t>(std::time(nullptr));
    request.token = token;
    request.key_id = key_id;
    // The ARKG-derived public key (§10.18), base64 of its compressed point — which
    // is exactly what `lib/crypto.py` verifies a `key_type: "p256"` signature with.
    request.pubkey = fido::PublicKeyBase64();
    request.nonce = nonce;

    char payload[protocol::kRequestMax];
    if (protocol::BuildRegistrationRequest(request, payload, sizeof payload) == 0) {
        std::snprintf(detail, detail_size, "the token is too long to send");
        return ESP_ERR_INVALID_SIZE;
    }

    MakeInbox(g_inbox, sizeof g_inbox);
    g_reply_length = 0;
    g_reply_overlong = false;
    g_waiting = true;
    // Drain anything a previous attempt left behind, so a stale give cannot make
    // this one return instantly with nothing.
    xSemaphoreTake(g_reply, 0);

    esp_err_t err = nats::Subscribe(g_inbox, nullptr, &OnReply, nullptr);
    if (err != ESP_OK) {
        g_waiting = false;
        std::snprintf(detail, detail_size, "could not open a reply inbox: %s",
                      esp_err_to_name(err));
        return err;
    }

    err = nats::Publish(protocol::kRegistrationSubject, payload, g_inbox);
    if (err != ESP_OK) {
        g_waiting = false;
        nats::Unsubscribe(g_inbox);
        std::snprintf(detail, detail_size, "could not send the request: %s", esp_err_to_name(err));
        return err;
    }

    const bool answered = xSemaphoreTake(g_reply, pdMS_TO_TICKS(kReplyTimeoutMs)) == pdTRUE;
    g_waiting = false;
    nats::Unsubscribe(g_inbox);

    if (!answered) {
        std::snprintf(detail, detail_size,
                      "no answer in %u seconds - is the registration handler running?",
                      static_cast<unsigned>(kReplyTimeoutMs / 1000));
        return ESP_ERR_TIMEOUT;
    }
    if (g_reply_length == 0) {
        std::snprintf(detail, detail_size,
                      g_reply_overlong ? "the answer was too long to read"
                                       : "the answer was empty");
        return ESP_ERR_INVALID_SIZE;
    }

    protocol::RegistrationReply reply;
    const protocol::ReplyStatus status = protocol::ParseRegistrationReply(
        g_reply_json, g_reply_length, nonce, g_record.present ? g_record.server_key : nullptr,
        &VerifyReply, nullptr, &reply);
    if (status != protocol::ReplyStatus::kVerified) {
        // **Never "registration failed".** `registrations` is open, so a reply
        // that does not verify is somebody else answering — a different problem
        // from the handler saying no, and the operator has to be able to tell.
        std::snprintf(detail, detail_size, "the answer could not be trusted: %s",
                      protocol::ReplyStatusText(status));
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (!reply.ok) {
        // A rejection changes nothing on the filesystem, which is what makes a
        // second attempt with a good token safe.
        std::snprintf(detail, detail_size, "the handler refused: %s",
                      reply.error[0] != '\0' ? reply.error : "no reason given");
        return ESP_FAIL;
    }

    Record record;
    record.present = true;
    record.ts = reply.ts;
    std::snprintf(record.key_id, sizeof record.key_id, "%s",
                  reply.key_id[0] != '\0' ? reply.key_id : key_id);
    std::snprintf(record.server_key, sizeof record.server_key, "%s", reply.server_key);
    // **The key this registration is for**, recorded at the moment it was accepted.
    // Read back at every boot and compared with the enrolment's; see `Registered`.
    std::snprintf(record.pubkey, sizeof record.pubkey, "%s", request.pubkey);

    err = Save(record);
    if (err != ESP_OK) {
        // Registered on the handler and not on the device: the honest thing to
        // say, because the token is spent and a retry needs a new one.
        std::snprintf(detail, detail_size,
                      "the handler accepted it and this device could not save it (%s) - the "
                      "token is spent, mint another",
                      esp_err_to_name(err));
        return err;
    }

    g_record = record;
    std::snprintf(detail, detail_size, "registered as %s, handler key %s", record.key_id,
                  record.server_key);
    ESP_LOGI(TAG, "%s", detail);
    return ESP_OK;
}

esp_err_t Forget() {
    const esp_err_t err = storage::Remove(kPath);
    if (err != ESP_OK) {
        return err;
    }
    // The temp too: a crash could have left one, and leaving it would mean the
    // next boot's recovery quietly restoring the registration that was just
    // dropped.
    storage::Remove(kTempPath);
    g_record = Record{};
    return ESP_OK;
}

}  // namespace registration
