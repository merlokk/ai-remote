// The CBOR subset (CLAUDE.md §10.18.4), and the reason it is worth a test file
// of its own: **every length in a CTAP2 response is a number the key chose.**
//
// A parser fed by a device on the other end of a cable is the same class of
// thing as a parser fed by a network, and §10.10's rule about the bus applies
// here unchanged. What this file pins is that a malformed, truncated or
// hostile-shaped item is *refused* rather than read past the end of the buffer.
//
// The writer is here too, and for a different reason: CTAP2 requires canonical
// CBOR — shortest-form lengths, map keys in ascending order — and a key rejects a
// request that is not. A canonicity bug looks like a key that mysteriously will
// not talk to this device.

#include <cstring>

#include "cbor.h"
#include "unity.h"

namespace {

// --- Writing -------------------------------------------------------------

void test_cbor_small_integers_are_one_byte(void) {
    uint8_t buffer[8];
    cbor::Writer w(buffer, sizeof(buffer));
    TEST_ASSERT_TRUE(w.Uint(0));
    TEST_ASSERT_TRUE(w.Uint(23));
    TEST_ASSERT_EQUAL_UINT32(2, w.Length());
    TEST_ASSERT_EQUAL_UINT8(0x00, buffer[0]);
    TEST_ASSERT_EQUAL_UINT8(0x17, buffer[1]);
}

void test_cbor_lengths_use_the_shortest_form(void) {
    // **Canonical CBOR, which the key checks.** A writer that used a two-byte
    // length for 24 would produce requests a YubiKey answers with
    // `CTAP2_ERR_INVALID_CBOR`, and nothing about that error names the cause.
    uint8_t buffer[16];
    cbor::Writer w(buffer, sizeof(buffer));
    w.Uint(24);
    w.Uint(255);
    w.Uint(256);
    w.Uint(65536);
    TEST_ASSERT_TRUE(w.Ok());
    TEST_ASSERT_EQUAL_UINT8(0x18, buffer[0]);  // one extra byte
    TEST_ASSERT_EQUAL_UINT8(24, buffer[1]);
    TEST_ASSERT_EQUAL_UINT8(0x18, buffer[2]);
    TEST_ASSERT_EQUAL_UINT8(255, buffer[3]);
    TEST_ASSERT_EQUAL_UINT8(0x19, buffer[4]);  // two extra bytes
    TEST_ASSERT_EQUAL_UINT8(0x1A, buffer[7]);  // four extra bytes
}

void test_cbor_negative_integers_use_the_major_type_one_form(void) {
    // ES256 is `-7`, which is major type 1 with argument 6. Getting this wrong
    // means a `pubKeyCredParams` the key does not recognise.
    uint8_t buffer[4];
    cbor::Writer w(buffer, sizeof(buffer));
    TEST_ASSERT_TRUE(w.Int(-7));
    TEST_ASSERT_EQUAL_UINT32(1, w.Length());
    TEST_ASSERT_EQUAL_UINT8(0x26, buffer[0]);

    cbor::Writer w2(buffer, sizeof(buffer));
    TEST_ASSERT_TRUE(w2.Int(-1));
    TEST_ASSERT_EQUAL_UINT8(0x20, buffer[0]);
}

void test_cbor_a_writer_that_runs_out_of_room_stays_failed(void) {
    // Sticky, so a caller can build a whole request and test `Ok()` once — which
    // is what the CTAP2 request builders do.
    uint8_t buffer[2];
    cbor::Writer w(buffer, sizeof(buffer));
    TEST_ASSERT_TRUE(w.Uint(1));
    TEST_ASSERT_FALSE(w.Uint(70000));
    TEST_ASSERT_FALSE(w.Ok());
    TEST_ASSERT_FALSE(w.Uint(1));  // and it does not recover
}

void test_cbor_text_and_bytes_carry_their_length(void) {
    uint8_t buffer[32];
    cbor::Writer w(buffer, sizeof(buffer));
    TEST_ASSERT_TRUE(w.Text("id"));
    TEST_ASSERT_EQUAL_UINT8(0x62, buffer[0]);  // text, length 2
    TEST_ASSERT_EQUAL_UINT8('i', buffer[1]);

    const uint8_t raw[] = {0xDE, 0xAD};
    TEST_ASSERT_TRUE(w.Bytes(raw, sizeof(raw)));
    TEST_ASSERT_EQUAL_UINT8(0x42, buffer[3]);  // bytes, length 2
}

void test_cbor_booleans_are_the_simple_values(void) {
    uint8_t buffer[2];
    cbor::Writer w(buffer, sizeof(buffer));
    w.Bool(true);
    w.Bool(false);
    TEST_ASSERT_EQUAL_UINT8(0xF5, buffer[0]);
    TEST_ASSERT_EQUAL_UINT8(0xF4, buffer[1]);
}

// --- Reading -------------------------------------------------------------

void test_cbor_decodes_the_five_kinds_this_firmware_speaks(void) {
    cbor::Item item;

    const uint8_t small[] = {0x0A};
    TEST_ASSERT_TRUE(cbor::Decode(small, sizeof(small), &item));
    TEST_ASSERT_TRUE(item.type == cbor::Type::kUint);
    TEST_ASSERT_EQUAL_INT64(10, item.signed_value);

    const uint8_t negative[] = {0x26};
    TEST_ASSERT_TRUE(cbor::Decode(negative, sizeof(negative), &item));
    TEST_ASSERT_TRUE(item.type == cbor::Type::kNint);
    TEST_ASSERT_EQUAL_INT64(-7, item.signed_value);

    const uint8_t text[] = {0x62, 'h', 'i'};
    TEST_ASSERT_TRUE(cbor::Decode(text, sizeof(text), &item));
    TEST_ASSERT_TRUE(item.type == cbor::Type::kText);
    TEST_ASSERT_EQUAL_UINT32(2, item.length);

    const uint8_t truth[] = {0xF5};
    TEST_ASSERT_TRUE(cbor::Decode(truth, sizeof(truth), &item));
    TEST_ASSERT_TRUE(item.type == cbor::Type::kSimple);
    TEST_ASSERT_TRUE(item.boolean);

    const uint8_t array[] = {0x83};
    TEST_ASSERT_TRUE(cbor::Decode(array, sizeof(array), &item));
    TEST_ASSERT_TRUE(item.type == cbor::Type::kArray);
    TEST_ASSERT_EQUAL_UINT32(3, item.value);
}

void test_cbor_a_byte_string_longer_than_the_buffer_is_refused(void) {
    // **The one check most of this parser exists for.** The length is the key's
    // number; a decoder that trusted it would hand a caller a pointer past the
    // end of a 64-byte USB report.
    const uint8_t lying[] = {0x58, 0x40, 0x01, 0x02};  // "64 bytes follow", two do
    cbor::Item item;
    TEST_ASSERT_FALSE(cbor::Decode(lying, sizeof(lying), &item));
}

void test_cbor_a_truncated_head_is_refused(void) {
    const uint8_t cut[] = {0x19, 0x01};  // two-byte length, one byte present
    cbor::Item item;
    TEST_ASSERT_FALSE(cbor::Decode(cut, sizeof(cut), &item));
    TEST_ASSERT_FALSE(cbor::Decode(cut, 0, &item));
}

void test_cbor_an_indefinite_length_is_refused_rather_than_read_as_empty(void) {
    // `0x5F` is an indefinite-length byte string. This firmware does not speak
    // it, and a decoder that treated the head as a zero-length item would parse
    // the *contents* as the next field — a silent misparse rather than an error.
    const uint8_t indefinite[] = {0x5F, 0x41, 0x01, 0xFF};
    cbor::Item item;
    TEST_ASSERT_FALSE(cbor::Decode(indefinite, sizeof(indefinite), &item));
}

void test_cbor_a_tag_is_refused(void) {
    const uint8_t tagged[] = {0xC0, 0x01};  // major type 6
    cbor::Item item;
    TEST_ASSERT_FALSE(cbor::Decode(tagged, sizeof(tagged), &item));
}

void test_cbor_skip_steps_over_a_whole_nested_item(void) {
    // `{"a": [1, 2], "b": 3}` — skipping the first value has to walk the array.
    const uint8_t map[] = {0xA2, 0x61, 'a', 0x82, 0x01, 0x02, 0x61, 'b', 0x03};
    size_t offset = 1;                       // past the map head
    TEST_ASSERT_TRUE(cbor::Skip(map, sizeof(map), &offset));  // the key "a"
    TEST_ASSERT_EQUAL_UINT32(3, offset);
    TEST_ASSERT_TRUE(cbor::Skip(map, sizeof(map), &offset));  // the array
    TEST_ASSERT_EQUAL_UINT32(6, offset);
}

void test_cbor_skip_refuses_a_nesting_depth_it_will_not_follow(void) {
    // The depth is a number the device on the other end chose, so it is bounded
    // rather than recursed into.
    uint8_t deep[32];
    for (size_t i = 0; i < sizeof(deep) - 1; i++) {
        deep[i] = 0x81;  // an array of one, over and over
    }
    deep[sizeof(deep) - 1] = 0x01;
    size_t offset = 0;
    TEST_ASSERT_FALSE(cbor::Skip(deep, sizeof(deep), &offset));
}

void test_cbor_mapfind_reads_an_integer_key(void) {
    // A CTAP2 response map: {1: "ok", 2: h'AB'}
    const uint8_t map[] = {0xA2, 0x01, 0x62, 'o', 'k', 0x02, 0x41, 0xAB};
    const uint8_t *value = nullptr;
    size_t left = 0;

    TEST_ASSERT_TRUE(cbor::MapFind(map, sizeof(map), 1, &value, &left));
    const char *text = nullptr;
    size_t length = 0;
    TEST_ASSERT_TRUE(cbor::GetText(value, left, &text, &length));
    TEST_ASSERT_EQUAL_UINT32(2, length);
    TEST_ASSERT_EQUAL_UINT8('o', text[0]);

    TEST_ASSERT_TRUE(cbor::MapFind(map, sizeof(map), 2, &value, &left));
    const uint8_t *bytes = nullptr;
    TEST_ASSERT_TRUE(cbor::GetBytes(value, left, &bytes, &length));
    TEST_ASSERT_EQUAL_UINT32(1, length);
    TEST_ASSERT_EQUAL_UINT8(0xAB, bytes[0]);
}

void test_cbor_mapfind_skips_a_value_it_does_not_understand_the_shape_of(void) {
    // {1: [1,2,3], 3: 7} — finding key 3 means stepping over the array.
    const uint8_t map[] = {0xA2, 0x01, 0x83, 0x01, 0x02, 0x03, 0x03, 0x07};
    const uint8_t *value = nullptr;
    size_t left = 0;
    TEST_ASSERT_TRUE(cbor::MapFind(map, sizeof(map), 3, &value, &left));
    uint64_t number = 0;
    TEST_ASSERT_TRUE(cbor::GetUint(value, left, &number));
    TEST_ASSERT_EQUAL_UINT64(7, number);
}

void test_cbor_mapfind_answers_false_for_a_key_that_is_not_there(void) {
    const uint8_t map[] = {0xA1, 0x01, 0x01};
    const uint8_t *value = nullptr;
    size_t left = 0;
    TEST_ASSERT_FALSE(cbor::MapFind(map, sizeof(map), 9, &value, &left));
}

void test_cbor_mapfind_on_something_that_is_not_a_map_is_false(void) {
    const uint8_t array[] = {0x82, 0x01, 0x02};
    const uint8_t *value = nullptr;
    size_t left = 0;
    TEST_ASSERT_FALSE(cbor::MapFind(array, sizeof(array), 1, &value, &left));
}

void test_cbor_mapfind_on_a_map_that_lies_about_its_size_is_false(void) {
    // "three pairs follow", one does. A parser that walked off the end here
    // would be reading whatever is next in a 2 KB static buffer.
    const uint8_t map[] = {0xA3, 0x01, 0x01};
    const uint8_t *value = nullptr;
    size_t left = 0;
    TEST_ASSERT_FALSE(cbor::MapFind(map, sizeof(map), 7, &value, &left));
}

void test_cbor_mapfind_by_text_reads_a_named_key(void) {
    // The nested maps CTAP2 keys by string: `options`, and the credential
    // descriptor's `id` / `type`.
    const uint8_t map[] = {0xA1, 0x62, 'u', 'p', 0xF5};
    const uint8_t *value = nullptr;
    size_t left = 0;
    TEST_ASSERT_TRUE(cbor::MapFindText(map, sizeof(map), "up", &value, &left));
    bool flag = false;
    TEST_ASSERT_TRUE(cbor::GetBool(value, left, &flag));
    TEST_ASSERT_TRUE(flag);

    TEST_ASSERT_FALSE(cbor::MapFindText(map, sizeof(map), "uv", &value, &left));
    // And a prefix must not match: "u" is not "up".
    TEST_ASSERT_FALSE(cbor::MapFindText(map, sizeof(map), "u", &value, &left));
}

void test_cbor_the_typed_getters_refuse_the_wrong_type(void) {
    const uint8_t number[] = {0x07};
    const uint8_t *bytes = nullptr;
    size_t length = 0;
    TEST_ASSERT_FALSE(cbor::GetBytes(number, sizeof(number), &bytes, &length));

    const char *text = nullptr;
    TEST_ASSERT_FALSE(cbor::GetText(number, sizeof(number), &text, &length));

    bool flag = false;
    TEST_ASSERT_FALSE(cbor::GetBool(number, sizeof(number), &flag));

    const uint8_t truth[] = {0xF5};
    uint64_t value = 0;
    TEST_ASSERT_FALSE(cbor::GetUint(truth, sizeof(truth), &value));
}

}  // namespace

void RegisterCborTests(void) {
    RUN_TEST(test_cbor_small_integers_are_one_byte);
    RUN_TEST(test_cbor_lengths_use_the_shortest_form);
    RUN_TEST(test_cbor_negative_integers_use_the_major_type_one_form);
    RUN_TEST(test_cbor_a_writer_that_runs_out_of_room_stays_failed);
    RUN_TEST(test_cbor_text_and_bytes_carry_their_length);
    RUN_TEST(test_cbor_booleans_are_the_simple_values);

    RUN_TEST(test_cbor_decodes_the_five_kinds_this_firmware_speaks);
    RUN_TEST(test_cbor_a_byte_string_longer_than_the_buffer_is_refused);
    RUN_TEST(test_cbor_a_truncated_head_is_refused);
    RUN_TEST(test_cbor_an_indefinite_length_is_refused_rather_than_read_as_empty);
    RUN_TEST(test_cbor_a_tag_is_refused);
    RUN_TEST(test_cbor_skip_steps_over_a_whole_nested_item);
    RUN_TEST(test_cbor_skip_refuses_a_nesting_depth_it_will_not_follow);
    RUN_TEST(test_cbor_mapfind_reads_an_integer_key);
    RUN_TEST(test_cbor_mapfind_skips_a_value_it_does_not_understand_the_shape_of);
    RUN_TEST(test_cbor_mapfind_answers_false_for_a_key_that_is_not_there);
    RUN_TEST(test_cbor_mapfind_on_something_that_is_not_a_map_is_false);
    RUN_TEST(test_cbor_mapfind_on_a_map_that_lies_about_its_size_is_false);
    RUN_TEST(test_cbor_mapfind_by_text_reads_a_named_key);
    RUN_TEST(test_cbor_the_typed_getters_refuse_the_wrong_type);
}
