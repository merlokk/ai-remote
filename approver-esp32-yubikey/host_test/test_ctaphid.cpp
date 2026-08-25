// CTAPHID framing (CLAUDE.md §10.18.1), and the reason it is a component of its
// own rather than thirty lines inside the USB driver.
//
// The framing has a **sequence number**, a **length that arrives before the
// data**, and a **buffer whose fill rate is controlled by a device on the other
// end of a cable**. Three of the four ways this could be made to overrun
// something are in those two sentences, and every one of them is a case a host
// compiler can run in a microsecond and a board cannot run at all — a real key
// will never send a malformed frame, which is exactly why the malformed paths
// need a test rather than a soak.

#include <cstring>

#include "ctaphid_frames.h"
#include "unity.h"

namespace {

constexpr uint32_t kCid = 0x11223344u;

void PutBe32(uint8_t *out, uint32_t value) {
    out[0] = static_cast<uint8_t>(value >> 24);
    out[1] = static_cast<uint8_t>(value >> 16);
    out[2] = static_cast<uint8_t>(value >> 8);
    out[3] = static_cast<uint8_t>(value);
}

// A hand-built init packet, so the reader is fed bytes rather than the writer's
// own output — a round trip through one class proves nothing about the wire.
void MakeInit(uint8_t *packet, uint32_t cid, uint8_t cmd, size_t total, const uint8_t *data,
              size_t chunk) {
    std::memset(packet, 0, ctaphid::kPacketSize);
    PutBe32(packet, cid);
    packet[4] = static_cast<uint8_t>(cmd | 0x80);
    packet[5] = static_cast<uint8_t>(total >> 8);
    packet[6] = static_cast<uint8_t>(total & 0xFF);
    if (chunk > 0) {
        std::memcpy(packet + ctaphid::kInitHeader, data, chunk);
    }
}

void MakeCont(uint8_t *packet, uint32_t cid, uint8_t seq, const uint8_t *data, size_t chunk) {
    std::memset(packet, 0, ctaphid::kPacketSize);
    PutBe32(packet, cid);
    packet[4] = static_cast<uint8_t>(seq & 0x7F);
    if (chunk > 0) {
        std::memcpy(packet + ctaphid::kContHeader, data, chunk);
    }
}

// --- Writing -------------------------------------------------------------

void test_ctaphid_a_short_message_is_one_packet(void) {
    const uint8_t payload[] = {0x04};
    ctaphid::Writer w;
    w.Begin(kCid, ctaphid::kCmdCbor, payload, sizeof(payload));
    TEST_ASSERT_EQUAL_UINT32(1, w.PacketCount());

    uint8_t packet[ctaphid::kPacketSize];
    TEST_ASSERT_TRUE(w.Next(packet, sizeof(packet)));
    TEST_ASSERT_EQUAL_UINT8(0x11, packet[0]);
    TEST_ASSERT_EQUAL_UINT8(0x44, packet[3]);
    TEST_ASSERT_EQUAL_UINT8(0x80 | ctaphid::kCmdCbor, packet[4]);
    TEST_ASSERT_EQUAL_UINT8(0, packet[5]);
    TEST_ASSERT_EQUAL_UINT8(1, packet[6]);
    TEST_ASSERT_EQUAL_UINT8(0x04, packet[7]);
    // Zero-padded to the full report: a key reads 64 bytes whatever we meant.
    TEST_ASSERT_EQUAL_UINT8(0, packet[8]);
    TEST_ASSERT_FALSE(w.Next(packet, sizeof(packet)));
}

void test_ctaphid_an_empty_message_is_still_a_packet(void) {
    // `CTAPHID_CANCEL` carries nothing, and a writer that emitted no packet for
    // it would be a cancel that never left.
    ctaphid::Writer w;
    w.Begin(kCid, ctaphid::kCmdCancel, nullptr, 0);
    TEST_ASSERT_EQUAL_UINT32(1, w.PacketCount());

    uint8_t packet[ctaphid::kPacketSize];
    TEST_ASSERT_TRUE(w.Next(packet, sizeof(packet)));
    TEST_ASSERT_EQUAL_UINT8(0x80 | ctaphid::kCmdCancel, packet[4]);
    TEST_ASSERT_FALSE(w.Next(packet, sizeof(packet)));
}

void test_ctaphid_a_long_message_is_split_and_numbered_from_zero(void) {
    uint8_t payload[200];
    for (size_t i = 0; i < sizeof(payload); i++) {
        payload[i] = static_cast<uint8_t>(i);
    }
    ctaphid::Writer w;
    w.Begin(kCid, ctaphid::kCmdCbor, payload, sizeof(payload));
    // 57 in the init packet, 143 left, 59 per continuation -> 3 more.
    TEST_ASSERT_EQUAL_UINT32(4, w.PacketCount());

    uint8_t packet[ctaphid::kPacketSize];
    TEST_ASSERT_TRUE(w.Next(packet, sizeof(packet)));
    TEST_ASSERT_EQUAL_UINT8(0, packet[7]);

    for (uint8_t seq = 0; seq < 3; seq++) {
        TEST_ASSERT_TRUE(w.Next(packet, sizeof(packet)));
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(seq, packet[4], "continuations must count from zero");
        // The high bit must be clear, or a key reads a continuation as an init.
        TEST_ASSERT_EQUAL_UINT8(0, packet[4] & 0x80);
    }
    TEST_ASSERT_FALSE(w.Next(packet, sizeof(packet)));
}

void test_ctaphid_a_message_past_the_protocol_ceiling_is_refused_before_any_of_it_leaves(void) {
    // Half-sending it would leave the key holding an incomplete message and the
    // channel busy until it times out.
    static uint8_t huge[ctaphid::kProtocolMaxMessage + 1];
    ctaphid::Writer w;
    w.Begin(kCid, ctaphid::kCmdCbor, huge, sizeof(huge));
    uint8_t packet[ctaphid::kPacketSize];
    TEST_ASSERT_FALSE(w.Next(packet, sizeof(packet)));
    TEST_ASSERT_TRUE(w.Done());
}

void test_ctaphid_a_writer_refuses_a_buffer_that_is_not_a_whole_report(void) {
    const uint8_t payload[] = {1};
    ctaphid::Writer w;
    w.Begin(kCid, ctaphid::kCmdCbor, payload, sizeof(payload));
    uint8_t small[ctaphid::kPacketSize - 1];
    TEST_ASSERT_FALSE(w.Next(small, sizeof(small)));
}

// --- Reading -------------------------------------------------------------

void test_ctaphid_a_one_packet_message_is_complete_at_once(void) {
    const uint8_t body[] = {0x00, 0xA1, 0x01};
    uint8_t packet[ctaphid::kPacketSize];
    MakeInit(packet, kCid, ctaphid::kCmdCbor, sizeof(body), body, sizeof(body));

    ctaphid::Reader r;
    r.Reset();
    TEST_ASSERT_TRUE(r.Feed(packet, sizeof(packet), kCid) ==
                     ctaphid::Reader::Result::kComplete);
    TEST_ASSERT_EQUAL_UINT8(ctaphid::kCmdCbor, r.Command());
    TEST_ASSERT_EQUAL_UINT32(sizeof(body), r.Length());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(body, r.Data(), sizeof(body));
}

void test_ctaphid_a_split_message_is_reassembled(void) {
    uint8_t body[120];
    for (size_t i = 0; i < sizeof(body); i++) {
        body[i] = static_cast<uint8_t>(0xC0 + i);
    }
    uint8_t packet[ctaphid::kPacketSize];
    ctaphid::Reader r;
    r.Reset();

    MakeInit(packet, kCid, ctaphid::kCmdCbor, sizeof(body), body, ctaphid::kInitPayload);
    TEST_ASSERT_TRUE(r.Feed(packet, sizeof(packet), kCid) ==
                     ctaphid::Reader::Result::kNeedMore);

    MakeCont(packet, kCid, 0, body + ctaphid::kInitPayload, ctaphid::kContPayload);
    TEST_ASSERT_TRUE(r.Feed(packet, sizeof(packet), kCid) ==
                     ctaphid::Reader::Result::kNeedMore);

    const size_t left = sizeof(body) - ctaphid::kInitPayload - ctaphid::kContPayload;
    MakeCont(packet, kCid, 1, body + ctaphid::kInitPayload + ctaphid::kContPayload, left);
    TEST_ASSERT_TRUE(r.Feed(packet, sizeof(packet), kCid) ==
                     ctaphid::Reader::Result::kComplete);

    TEST_ASSERT_EQUAL_UINT32(sizeof(body), r.Length());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(body, r.Data(), sizeof(body));
}

void test_ctaphid_another_channel_is_ignored_and_does_not_disturb_the_message(void) {
    // A hub with a second key on it is an ordinary thing, not a fault — and the
    // one rule is that it must not corrupt what is being assembled.
    uint8_t body[80];
    std::memset(body, 0xAB, sizeof(body));
    uint8_t packet[ctaphid::kPacketSize];
    ctaphid::Reader r;
    r.Reset();

    MakeInit(packet, kCid, ctaphid::kCmdCbor, sizeof(body), body, ctaphid::kInitPayload);
    TEST_ASSERT_TRUE(r.Feed(packet, sizeof(packet), kCid) ==
                     ctaphid::Reader::Result::kNeedMore);

    uint8_t intruder[ctaphid::kPacketSize];
    const uint8_t junk[] = {0xFF};
    MakeInit(intruder, 0xDEADBEEFu, ctaphid::kCmdPing, sizeof(junk), junk, sizeof(junk));
    TEST_ASSERT_TRUE(r.Feed(intruder, sizeof(intruder), kCid) ==
                     ctaphid::Reader::Result::kIgnored);

    MakeCont(packet, kCid, 0, body + ctaphid::kInitPayload,
             sizeof(body) - ctaphid::kInitPayload);
    TEST_ASSERT_TRUE(r.Feed(packet, sizeof(packet), kCid) ==
                     ctaphid::Reader::Result::kComplete);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(body, r.Data(), sizeof(body));
}

void test_ctaphid_a_length_longer_than_the_buffer_is_refused_before_a_byte_is_copied(void) {
    // **The one line most of this component exists for.** The length is the
    // device's number; silently keeping the first two kilobytes of a signature
    // would be the worst possible failure here.
    uint8_t packet[ctaphid::kPacketSize];
    const uint8_t body[] = {1, 2, 3};
    MakeInit(packet, kCid, ctaphid::kCmdCbor, ctaphid::kMaxMessage + 1, body, sizeof(body));

    ctaphid::Reader r;
    r.Reset();
    TEST_ASSERT_TRUE(r.Feed(packet, sizeof(packet), kCid) == ctaphid::Reader::Result::kError);
    TEST_ASSERT_TRUE(r.ErrorText()[0] != '\0');
}

void test_ctaphid_a_continuation_before_an_init_is_an_error(void) {
    uint8_t packet[ctaphid::kPacketSize];
    const uint8_t body[] = {9};
    MakeCont(packet, kCid, 0, body, sizeof(body));

    ctaphid::Reader r;
    r.Reset();
    TEST_ASSERT_TRUE(r.Feed(packet, sizeof(packet), kCid) == ctaphid::Reader::Result::kError);
}

void test_ctaphid_an_out_of_order_continuation_is_an_error(void) {
    uint8_t body[120];
    std::memset(body, 0x5A, sizeof(body));
    uint8_t packet[ctaphid::kPacketSize];
    ctaphid::Reader r;
    r.Reset();

    MakeInit(packet, kCid, ctaphid::kCmdCbor, sizeof(body), body, ctaphid::kInitPayload);
    TEST_ASSERT_TRUE(r.Feed(packet, sizeof(packet), kCid) ==
                     ctaphid::Reader::Result::kNeedMore);

    // Sequence 3 where 0 was due.
    MakeCont(packet, kCid, 3, body + ctaphid::kInitPayload, ctaphid::kContPayload);
    TEST_ASSERT_TRUE(r.Feed(packet, sizeof(packet), kCid) == ctaphid::Reader::Result::kError);
}

void test_ctaphid_an_init_packet_restarts_a_message_in_progress(void) {
    // The spec says so, and the alternative would leave a channel wedged after
    // any dropped packet, recoverable only by unplugging.
    uint8_t body[120];
    std::memset(body, 0x11, sizeof(body));
    uint8_t packet[ctaphid::kPacketSize];
    ctaphid::Reader r;
    r.Reset();

    MakeInit(packet, kCid, ctaphid::kCmdCbor, sizeof(body), body, ctaphid::kInitPayload);
    TEST_ASSERT_TRUE(r.Feed(packet, sizeof(packet), kCid) ==
                     ctaphid::Reader::Result::kNeedMore);

    const uint8_t fresh[] = {0x2E};
    MakeInit(packet, kCid, ctaphid::kCmdError, sizeof(fresh), fresh, sizeof(fresh));
    TEST_ASSERT_TRUE(r.Feed(packet, sizeof(packet), kCid) ==
                     ctaphid::Reader::Result::kComplete);
    TEST_ASSERT_EQUAL_UINT8(ctaphid::kCmdError, r.Command());
    TEST_ASSERT_EQUAL_UINT32(1, r.Length());
    TEST_ASSERT_EQUAL_UINT8(0x2E, r.Data()[0]);
}

void test_ctaphid_a_short_report_is_an_error(void) {
    uint8_t packet[ctaphid::kPacketSize];
    const uint8_t body[] = {1};
    MakeInit(packet, kCid, ctaphid::kCmdCbor, sizeof(body), body, sizeof(body));

    ctaphid::Reader r;
    r.Reset();
    TEST_ASSERT_TRUE(r.Feed(packet, sizeof(packet) - 1, kCid) ==
                     ctaphid::Reader::Result::kError);
    TEST_ASSERT_TRUE(r.Feed(nullptr, sizeof(packet), kCid) == ctaphid::Reader::Result::kError);
}

void test_ctaphid_a_message_of_exactly_the_buffer_is_accepted(void) {
    // The boundary either side of the refusal above: `kMaxMessage` is allowed
    // and `kMaxMessage + 1` is not. An off-by-one here is either a refusal of
    // valid answers or an overrun.
    static uint8_t body[ctaphid::kMaxMessage];
    std::memset(body, 0x7E, sizeof(body));

    uint8_t packet[ctaphid::kPacketSize];
    ctaphid::Reader r;
    r.Reset();
    MakeInit(packet, kCid, ctaphid::kCmdCbor, sizeof(body), body, ctaphid::kInitPayload);
    ctaphid::Reader::Result result = r.Feed(packet, sizeof(packet), kCid);
    TEST_ASSERT_TRUE(result == ctaphid::Reader::Result::kNeedMore);

    size_t sent = ctaphid::kInitPayload;
    uint8_t seq = 0;
    while (sent < sizeof(body)) {
        const size_t left = sizeof(body) - sent;
        const size_t chunk = left < ctaphid::kContPayload ? left : ctaphid::kContPayload;
        MakeCont(packet, kCid, seq++, body + sent, chunk);
        result = r.Feed(packet, sizeof(packet), kCid);
        sent += chunk;
        TEST_ASSERT_FALSE(result == ctaphid::Reader::Result::kError);
    }
    TEST_ASSERT_TRUE(result == ctaphid::Reader::Result::kComplete);
    TEST_ASSERT_EQUAL_UINT32(sizeof(body), r.Length());
}

void test_ctaphid_the_writer_and_the_reader_agree(void) {
    // The round trip, last, once both halves have been checked against
    // hand-built bytes — otherwise a matching pair of mistakes would pass.
    uint8_t body[500];
    for (size_t i = 0; i < sizeof(body); i++) {
        body[i] = static_cast<uint8_t>(i * 7 + 3);
    }
    ctaphid::Writer w;
    w.Begin(kCid, ctaphid::kCmdCbor, body, sizeof(body));

    ctaphid::Reader r;
    r.Reset();
    uint8_t packet[ctaphid::kPacketSize];
    ctaphid::Reader::Result result = ctaphid::Reader::Result::kNeedMore;
    while (w.Next(packet, sizeof(packet))) {
        result = r.Feed(packet, sizeof(packet), kCid);
    }
    TEST_ASSERT_TRUE(result == ctaphid::Reader::Result::kComplete);
    TEST_ASSERT_EQUAL_UINT32(sizeof(body), r.Length());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(body, r.Data(), sizeof(body));
}

void test_ctaphid_every_error_code_has_a_name(void) {
    TEST_ASSERT_NOT_NULL(ctaphid::ErrorName(ctaphid::kErrChannelBusy));
    TEST_ASSERT_NOT_NULL(ctaphid::ErrorName(0x77));
}

}  // namespace

void RegisterCtaphidTests(void) {
    RUN_TEST(test_ctaphid_a_short_message_is_one_packet);
    RUN_TEST(test_ctaphid_an_empty_message_is_still_a_packet);
    RUN_TEST(test_ctaphid_a_long_message_is_split_and_numbered_from_zero);
    RUN_TEST(test_ctaphid_a_message_past_the_protocol_ceiling_is_refused_before_any_of_it_leaves);
    RUN_TEST(test_ctaphid_a_writer_refuses_a_buffer_that_is_not_a_whole_report);

    RUN_TEST(test_ctaphid_a_one_packet_message_is_complete_at_once);
    RUN_TEST(test_ctaphid_a_split_message_is_reassembled);
    RUN_TEST(test_ctaphid_another_channel_is_ignored_and_does_not_disturb_the_message);
    RUN_TEST(test_ctaphid_a_length_longer_than_the_buffer_is_refused_before_a_byte_is_copied);
    RUN_TEST(test_ctaphid_a_continuation_before_an_init_is_an_error);
    RUN_TEST(test_ctaphid_an_out_of_order_continuation_is_an_error);
    RUN_TEST(test_ctaphid_an_init_packet_restarts_a_message_in_progress);
    RUN_TEST(test_ctaphid_a_short_report_is_an_error);
    RUN_TEST(test_ctaphid_a_message_of_exactly_the_buffer_is_accepted);
    RUN_TEST(test_ctaphid_the_writer_and_the_reader_agree);
    RUN_TEST(test_ctaphid_every_error_code_has_a_name);
}
