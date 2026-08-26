#pragma once

// **ARKG-Derive-Public-Key, on the device** (CLAUDE.md §10.18, §10.6).
//
// This is the arithmetic that lets a security key sign this device's verdicts.
// The authenticator holds one *seed* key pair; whoever has the seed **public** key
// can derive unlimited fresh public keys offline, and only that authenticator can
// reconstruct the matching private half — and only when handed the key handle the
// derivation produced. So the device derives one key, registers its public half
// through §6, and from then on every §7 signature is made inside the key.
//
// The instance is **ARKG-P256ADD-ECDH**, the one `fido2.cose.ARKG_P256` names, from
// `draft-bradleylundberg-cfrg-arkg`:
//
//     ctx'    = I2OSP(LEN(ctx), 1) || ctx
//     ctx_bl  = 'ARKG-Derive-Key-BL.'  || ctx'
//     ctx_kem = 'ARKG-Derive-Key-KEM.' || ctx'
//     (ikm_tau, kh) = KEM-Encaps(pk_kem, ikm, ctx_kem)
//     tau = BL-PRF(ikm_tau, ctx_bl)
//     pk' = pk_bl + tau * G
//
// ## Why the primitives arrive from outside
//
// Seven steps, and five of them are a hash function and some byte strings:
// `expand_message_xmd`, two HKDF expansions, an HMAC, a reduction mod n. Two need
// an elliptic curve: the ECDH inside the KEM, and the point addition that blinds
// the key. So the curve and the hash come in through `Backend`, as plain function
// pointers (§10.14.1 — no `std::function`), and **everything above them is host
// tested against numbers Python produced** (§10.11 tier 2). That is not a nicety:
// a derivation that is wrong by one byte produces a public key whose private half
// the authenticator cannot reconstruct, every reply is rejected by
// `hook.verify_reply`, and from the desk that is indistinguishable from a device
// that is not answering. Nothing logs it.
//
// `arkg_psa.cpp` supplies the real backend — PSA Crypto for the hash, the ECDH and
// the scalar multiplication, and one mbedTLS call for the addition PSA has no name
// for. It is the only file here that includes ESP-IDF, which is what keeps this one
// in the host tier.
//
// ## What it does not do
//
// * **no CBOR.** The seed key arrives as two points, and turning
//   `previewSign`'s COSE key into them is `components/fido`'s job — which owns a
//   CBOR reader already and is where the wire lives (§10.14.2);
// * **no key material of its own.** Nothing here is secret. `ikm` is entropy the
//   caller generated, and the output is a public key and a key handle, both of
//   which travel in the clear by design;
// * **no allocation, and no clock.**

#include <cstddef>
#include <cstdint>

namespace arkg {

// An uncompressed P-256 point, `04 || X || Y` — the form every backend here takes
// and returns, and the one `mbedtls_ecp_point_read_binary` and PSA both speak.
inline constexpr size_t kPointSize = 65;

// And the compressed form, `02|03 || X`, which is what §6 registers: it is exactly
// what `lib/crypto.py`'s `key_type="p256"` expects, base64 of these 33 bytes.
inline constexpr size_t kCompressedSize = 33;

inline constexpr size_t kScalarSize = 32;
inline constexpr size_t kDigestSize = 32;

// The KEM's ciphertext, which is the key handle: a 128-bit MAC tag followed by the
// ephemeral public point. This is what goes back to the authenticator inside
// COSE_Sign_Args, and it is the whole reason nothing secret has to be stored.
inline constexpr size_t kMacTagSize = 16;
inline constexpr size_t kKeyHandleSize = kMacTagSize + kPointSize;

// The draft's ceiling on `ctx`, and its floor on `ikm`. Both are enforced rather
// than assumed: a truncated `ctx` derives a key the authenticator will not
// reconstruct, and a short `ikm` gives away the derived key — the draft recommends
// 256 bits of entropy and this treats it as a requirement.
inline constexpr size_t kCtxMax = 64;
inline constexpr size_t kIkmMin = 32;
inline constexpr size_t kIkmMax = 64;

// `hash_to_field`'s output width for P-256 at a 128-bit security level:
// ceil((256 + 128) / 8). A different number here is a different scalar and a key
// nothing can sign for.
inline constexpr size_t kHashToFieldBytes = 48;

// The two domain separation tags this instance is parameterised with. Note that
// they differ — the KEM's carries its own sub-tag — and swapping them silently
// produces valid-looking, useless keys.
inline constexpr char kDstExtBl[] = "ARKG-P256";
inline constexpr char kDstExtKem[] = "ARKG-ECDH.ARKG-P256";

// The purpose label this device scopes its key to. The same string
// `approver/responder_yubikey.py` uses: the same purpose, and what tells this
// device's key apart from that responder's is the `ikm`, not this.
inline constexpr char kDeviceCtx[] = "ai-remote-approvals";

// The longest DST and info string the pipeline builds, with `ctx` at its ceiling.
// Sized here so every buffer below can be a fixed array.
inline constexpr size_t kDstMax = 128;
inline constexpr size_t kInfoMax = 160;

// --- what the curve and the hash have to provide ---------------------------

// Every function returns false on failure and **must not touch `out` when it
// does**; a half-written point that a caller treats as a key is the one failure
// mode here that is worse than no key at all.
struct Backend {
    // SHA-256, one shot.
    bool (*sha256)(const uint8_t *data, size_t length, uint8_t *out);

    // ECDH: the X coordinate of `scalar * peer`, 32 bytes — RFC 6090's compact
    // output, which is what the draft means by ECDH(pk, sk).
    bool (*ecdh)(const uint8_t *scalar, const uint8_t *peer_point, uint8_t *out);

    // `scalar * G`, as an uncompressed point.
    bool (*mul_base)(const uint8_t *scalar, uint8_t *out);

    // `a + b`, as an uncompressed point. The only operation in this file PSA has
    // no entry point for, which is why there is a backend at all.
    bool (*point_add)(const uint8_t *a, const uint8_t *b, uint8_t *out);
};

// The one the device runs on, from `arkg_psa.cpp`. Null-checked by `Derive`, so a
// build that somehow linked without it fails closed rather than deriving nonsense.
const Backend *PsaBackend();

// --- the seed key ----------------------------------------------------------

// The ARKG seed public key, as two points. `components/fido` parses these out of
// the COSE key `previewSign` returns; nothing here knows what CBOR is.
struct SeedKey {
    uint8_t blinding[kPointSize] = {};
    uint8_t kem[kPointSize] = {};
};

// --- the answer ------------------------------------------------------------

struct Derived {
    // The public key, both ways round. `compressed` is what §6 registers;
    // `point` is what a verifier imports.
    uint8_t point[kPointSize] = {};
    uint8_t compressed[kCompressedSize] = {};

    // `kh`, which the authenticator needs back to rebuild the private half.
    uint8_t key_handle[kKeyHandleSize] = {};
};

// Every intermediate, for the tests and for `arkg selftest`. **Optional**: pass
// null in production. Six of these seven numbers are produced with no elliptic
// curve at all, which is what lets the host tier localise a failure to one step
// instead of reporting "the key is wrong".
struct Trace {
    uint8_t ephemeral_scalar[kScalarSize] = {};
    uint8_t ephemeral_point[kPointSize] = {};
    uint8_t ecdh_secret[kScalarSize] = {};
    uint8_t mac_key[kDigestSize] = {};
    uint8_t mac_tag[kMacTagSize] = {};
    uint8_t shared[kDigestSize] = {};
    uint8_t tau[kScalarSize] = {};
};

// Why a derivation refused. Separate values because they need separate sentences:
// "the key this device was handed is not a point" and "this build has no curve in
// it" are the same failure to a caller and completely different problems.
enum class Status : uint8_t {
    kOk,
    kNoBackend,     // a null `Backend`, or a null function inside it
    kBadArgument,   // null out-params, an `ikm`/`ctx` outside its bounds
    kBadSeedKey,    // a point that is not an uncompressed P-256 point
    kHashFailed,    // the backend's SHA-256 said no
    kCurveFailed,   // the ECDH, the multiplication or the addition said no
    kBadScalar,     // a reduction landed on zero — a 2^-256 event, refused anyway
};

const char *StatusName(Status status);

// **The derivation.** `ctx` is a label at most `kCtxMax` bytes long (it may be
// empty, and the length prefix is what makes an empty one different from none);
// `ikm` is at least `kIkmMin` bytes of entropy the caller generated.
//
// Deterministic: the same seed key, `ikm` and `ctx` always produce the same key,
// which is what lets the device re-derive at boot and check that its registration
// still matches (§10.18.1).
Status Derive(const Backend *backend, const SeedKey &seed, const uint8_t *ikm, size_t ikm_length,
              const uint8_t *ctx, size_t ctx_length, Derived *out, Trace *trace = nullptr);

// --- the pure pieces, exposed because they are what the tests pin ----------
//
// These are here for §10.11 rather than for callers: each one is a step the vectors
// carry a number for, so a failure names the step. Using them from the firmware
// is fine and nothing does.
//
// **They need only `Backend::sha256`** — the other three may be null. That is what
// lets the host tier exercise five of the derivation's seven steps with no
// elliptic curve linked at all, and it is a deliberate part of the interface
// rather than an accident of the implementation.

// RFC 9380 §5.3.1 with SHA-256. `length` is at most 255 * 32 and in this pipeline
// is always `kHashToFieldBytes`; `dst` is at most 255 bytes.
bool ExpandMessageXmd(const Backend *backend, const uint8_t *msg, size_t msg_length,
                      const char *dst, size_t dst_length, uint8_t *out, size_t length);

// `hash_to_field(msg, 1)` over GF(n): `kHashToFieldBytes` of expansion, reduced
// modulo the order of the curve's prime-order subgroup. False when the reduction
// lands on zero, which is not a scalar.
bool HashToScalar(const Backend *backend, const uint8_t *msg, size_t msg_length, const char *dst,
                  size_t dst_length, uint8_t *out);

// HMAC-SHA256, built on the backend's hash rather than asked of it — the block
// size and the two pads are worth having under test.
bool HmacSha256(const Backend *backend, const uint8_t *key, size_t key_length,
                const uint8_t *data, size_t data_length, uint8_t *out);

// HKDF-SHA256 (RFC 5869). `Extract` uses the unset salt the draft asks for, which
// is defined as `HashLen` zero bytes — not an absent one.
bool HkdfExtract(const Backend *backend, const uint8_t *ikm, size_t ikm_length, uint8_t *prk);
bool HkdfExpand(const Backend *backend, const uint8_t *prk, const uint8_t *info,
                size_t info_length, uint8_t *out, size_t length);

// Reduces a big-endian integer modulo the subgroup order. Bit by bit, one
// conditional subtraction each — no bignum library, and 384 iterations for the
// only width this needs.
bool ReduceModOrder(const uint8_t *value, size_t length, uint8_t *out);

// `04 || X || Y` -> `02|03 || X`. False for anything that is not an uncompressed
// point, because the compressed form is what gets registered and a wrong one is a
// key nothing can ever verify against.
bool CompressPoint(const uint8_t *point, uint8_t *out);

}  // namespace arkg
