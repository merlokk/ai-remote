// The speaker (CLAUDE.md §10.8.1, §10.13), against a fake I²S channel and a
// real filesystem.
//
// **Most of this is the RIFF parser**, and that is where the value is. Its own
// comment names the failure it exists to prevent: encoders drop `LIST`/`INFO`
// chunks between `fmt ` and `data` whenever they feel like it, and a parser
// that trusts the canonical 44-byte offset plays metadata as audio — which
// sounds exactly like a driver bug and is an hour spent on the wrong question.
// That is one bad frame on hardware and one assertion here.
//
// The other half is what actually reaches the wire. The fake channel captures
// the bytes, so "it streamed the data and not the header" is something a test
// knows rather than something a listener guesses at.

#include <cstring>
#include <vector>

#include "es8311.h"
#include "fake_platform.h"
#include "fake_storage.h"
#include "i2c_bus.h"
#include "speaker.h"
#include "unity.h"

namespace {

constexpr uint8_t kCodecAddr = 0x18;
constexpr uint8_t kRegDacMute31 = 0x31;
constexpr uint8_t kDacMuteBits = 0x60;

constexpr audio::SpeakerPins kPins = {
    .mclk = GPIO_NUM_19,
    .bclk = GPIO_NUM_20,
    .lrck = GPIO_NUM_22,
    .data_out = GPIO_NUM_23,
};

// --- Building a WAV by hand ----------------------------------------------
// Little-endian, and assembled chunk by chunk rather than from a fixed 44-byte
// template, because the whole point of several of these tests is that the
// layout is *not* fixed.

void PushU32(std::vector<uint8_t> &out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
}

void PushU16(std::vector<uint8_t> &out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
}

void PushTag(std::vector<uint8_t> &out, const char *tag) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<uint8_t>(tag[i]));
    }
}

struct WavOptions {
    uint16_t format = 1;  // PCM
    uint16_t channels = 1;
    uint32_t sample_rate = 16000;
    uint16_t bits = 16;
    uint32_t fmt_chunk_size = 16;  // 18 or 40 for WAVE_FORMAT_EXTENSIBLE
    bool list_chunk = false;
    bool odd_chunk = false;   // a chunk with an odd size, which RIFF pads
    bool data_before_fmt = false;
    bool omit_data = false;
    // Claim more data than the payload holds — a truncated file.
    uint32_t claimed_data_bytes = 0;  // 0 means "the truth"
};

void PushFmt(std::vector<uint8_t> &wav, const WavOptions &options) {
    PushTag(wav, "fmt ");
    PushU32(wav, options.fmt_chunk_size);
    PushU16(wav, options.format);
    PushU16(wav, options.channels);
    PushU32(wav, options.sample_rate);
    PushU32(wav, options.sample_rate * options.channels * (options.bits / 8));  // byte rate
    PushU16(wav, static_cast<uint16_t>(options.channels * (options.bits / 8)));  // block align
    PushU16(wav, options.bits);
    // WAVE_FORMAT_EXTENSIBLE's tail, which the parser has to step over.
    for (uint32_t i = 16; i < options.fmt_chunk_size; ++i) {
        wav.push_back(0xAB);
    }
}

std::vector<uint8_t> MakeWav(const std::vector<uint8_t> &samples, WavOptions options = {}) {
    std::vector<uint8_t> wav;
    PushTag(wav, "RIFF");
    PushU32(wav, 0);  // size, which this parser does not read
    PushTag(wav, "WAVE");

    if (options.data_before_fmt) {
        PushTag(wav, "data");
        PushU32(wav, static_cast<uint32_t>(samples.size()));
        wav.insert(wav.end(), samples.begin(), samples.end());
        PushFmt(wav, options);
        return wav;
    }

    PushFmt(wav, options);

    if (options.list_chunk) {
        // What ffmpeg writes without `-map_metadata -1`, which
        // `working-with-code.md` tells you to pass and which somebody will
        // eventually forget.
        static const char kInfo[] = "INFOISFT\x0d\x00\x00\x00Lavf60.16.100\x00";
        PushTag(wav, "LIST");
        PushU32(wav, sizeof(kInfo) - 1);
        for (size_t i = 0; i + 1 < sizeof(kInfo); ++i) {
            wav.push_back(static_cast<uint8_t>(kInfo[i]));
        }
    }

    if (options.odd_chunk) {
        // Three bytes of payload plus one of RIFF padding. A parser that seeks
        // by the raw size lands one byte early and reads `ata\0` as a tag.
        PushTag(wav, "cue ");
        PushU32(wav, 3);
        wav.push_back(0x01);
        wav.push_back(0x02);
        wav.push_back(0x03);
        wav.push_back(0x00);  // the pad byte
    }

    if (!options.omit_data) {
        PushTag(wav, "data");
        PushU32(wav, options.claimed_data_bytes != 0 ? options.claimed_data_bytes
                                                     : static_cast<uint32_t>(samples.size()));
        wav.insert(wav.end(), samples.begin(), samples.end());
    }
    return wav;
}

// A recognisable payload: no byte of it appears in a RIFF tag, so a parser
// that streamed the header instead would be obvious rather than plausible.
std::vector<uint8_t> Samples(size_t count) {
    std::vector<uint8_t> samples;
    for (size_t i = 0; i < count; ++i) {
        samples.push_back(static_cast<uint8_t>(0xE0 + (i % 16)));
    }
    return samples;
}

void PutWav(const char *name, const std::vector<uint8_t> &wav) {
    fake::PutBinaryFile(name, wav.data(), wav.size());
}

// --- The hardware under it ------------------------------------------------

fake::Device *PutCodecOnWire() {
    fake::Device *chip = fake::AddDevice(kCodecAddr);
    chip->regs[0xFD] = 0x83;
    chip->regs[0xFE] = 0x11;
    return chip;
}

struct Rig {
    i2cbus::Bus bus;
    audio::Es8311 codec;
    audio::Speaker speaker;
};

void BringUp(Rig &rig, uint32_t sample_rate = 16000) {
    fake::MountStorage();
    TEST_ASSERT_EQUAL_INT(ESP_OK, rig.bus.Init(GPIO_NUM_7, GPIO_NUM_8));
    PutCodecOnWire();
    TEST_ASSERT_EQUAL_INT(ESP_OK, rig.codec.Init(rig.bus, sample_rate));
    TEST_ASSERT_EQUAL_INT(ESP_OK, rig.speaker.Init(rig.codec, kPins, sample_rate));
}

bool CodecMuted() {
    const fake::Device *chip = fake::DeviceAt(kCodecAddr);
    return chip != nullptr && (chip->regs[kRegDacMute31] & kDacMuteBits) == kDacMuteBits;
}

}  // namespace

// --- Coming up ------------------------------------------------------------

void test_speaker_init_opens_a_transmit_only_channel(void) {
    Rig rig;
    BringUp(rig);

    TEST_ASSERT_TRUE(rig.speaker.Ready());
    TEST_ASSERT_TRUE(fake::P().i2s.channel_open);
    TEST_ASSERT_TRUE(fake::P().i2s.enabled);

    // **`din` is unused on purpose**: the microphones have no job (§10.13), so
    // the channel is transmit-only and this is what says nobody quietly wired
    // one up.
    TEST_ASSERT_EQUAL_INT(GPIO_NUM_NC, fake::P().i2s.din);
    TEST_ASSERT_EQUAL_INT(kPins.mclk, fake::P().i2s.mclk);
    TEST_ASSERT_EQUAL_INT(kPins.data_out, fake::P().i2s.dout);
}

void test_speaker_init_matches_the_codecs_coefficient_row(void) {
    // 256×fs is what the ES8311's single coefficient row assumes (`es8311.h`),
    // and it is also ESP-IDF's default — stated in the driver rather than
    // relied upon, so stated here too.
    Rig rig;
    BringUp(rig);

    TEST_ASSERT_EQUAL_UINT32(audio::kMclkMultiple, fake::P().i2s.mclk_multiple);
    TEST_ASSERT_EQUAL_UINT32(16000, fake::P().i2s.sample_rate);
    TEST_ASSERT_EQUAL_INT(I2S_DATA_BIT_WIDTH_16BIT, fake::P().i2s.bits);
    TEST_ASSERT_EQUAL_INT(I2S_SLOT_MODE_MONO, fake::P().i2s.slots);
}

void test_speaker_auto_clears_a_starved_channel(void) {
    // Silence rather than the last buffer repeated, which is what a starved
    // channel would otherwise emit — a stutter that sounds like a broken file.
    Rig rig;
    BringUp(rig);
    TEST_ASSERT_TRUE(fake::P().i2s.auto_clear);
}

void test_speaker_refuses_to_come_up_without_a_codec(void) {
    fake::MountStorage();
    i2cbus::Bus bus;
    TEST_ASSERT_EQUAL_INT(ESP_OK, bus.Init(GPIO_NUM_7, GPIO_NUM_8));

    audio::Es8311 codec;  // never initialised — nothing on the wire
    audio::Speaker speaker;
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_STATE, speaker.Init(codec, kPins, 16000));
    TEST_ASSERT_FALSE(speaker.Ready());
    TEST_ASSERT_FALSE(fake::P().i2s.channel_open);
}

// --- Reading the header ---------------------------------------------------

void test_speaker_describes_a_plain_wav(void) {
    Rig rig;
    BringUp(rig);
    PutWav("chirp.wav", MakeWav(Samples(320)));

    audio::WavFormat format = {};
    TEST_ASSERT_EQUAL_INT(ESP_OK, rig.speaker.Describe("chirp.wav", &format));
    TEST_ASSERT_EQUAL_UINT32(16000, format.sample_rate);
    TEST_ASSERT_EQUAL_UINT16(1, format.channels);
    TEST_ASSERT_EQUAL_UINT16(16, format.bits);
    TEST_ASSERT_EQUAL_UINT32(320, format.data_bytes);
}

void test_speaker_walks_past_a_list_chunk(void) {
    // **The reason the parser walks chunks at all.** `working-with-code.md`
    // says to pass `-map_metadata -1 -fflags +bitexact` so ffmpeg does not
    // write one of these; this is what happens the day somebody forgets.
    Rig rig;
    BringUp(rig);
    WavOptions options;
    options.list_chunk = true;
    PutWav("tagged.wav", MakeWav(Samples(64), options));

    audio::WavFormat format = {};
    TEST_ASSERT_EQUAL_INT(ESP_OK, rig.speaker.Describe("tagged.wav", &format));
    TEST_ASSERT_EQUAL_UINT32(16000, format.sample_rate);
    TEST_ASSERT_EQUAL_UINT32(64, format.data_bytes);
}

void test_speaker_pads_an_odd_sized_chunk(void) {
    // RIFF rounds every chunk up to an even length, and the pad byte is not
    // counted in the size. A parser that seeks by the raw number lands one byte
    // early and reads `ata` plus a stray byte as the next tag.
    Rig rig;
    BringUp(rig);
    WavOptions options;
    options.odd_chunk = true;
    PutWav("odd.wav", MakeWav(Samples(48), options));

    audio::WavFormat format = {};
    TEST_ASSERT_EQUAL_INT(ESP_OK, rig.speaker.Describe("odd.wav", &format));
    TEST_ASSERT_EQUAL_UINT32(48, format.data_bytes);
}

void test_speaker_steps_over_an_extensible_fmt_chunk(void) {
    // WAVE_FORMAT_EXTENSIBLE writes 40 bytes of `fmt ` where the basic form
    // writes 16. The parser reads the first 16 and has to skip the rest.
    Rig rig;
    BringUp(rig);
    WavOptions options;
    options.fmt_chunk_size = 40;
    PutWav("extensible.wav", MakeWav(Samples(32), options));

    audio::WavFormat format = {};
    TEST_ASSERT_EQUAL_INT(ESP_OK, rig.speaker.Describe("extensible.wav", &format));
    TEST_ASSERT_EQUAL_UINT16(16, format.bits);
    TEST_ASSERT_EQUAL_UINT32(32, format.data_bytes);
}

void test_speaker_refuses_something_that_is_not_a_riff_wave(void) {
    Rig rig;
    BringUp(rig);
    fake::PutFile("notawav.wav", "this is a text file with a misleading name");

    audio::WavFormat format = {};
    TEST_ASSERT_EQUAL_INT(ESP_ERR_NOT_SUPPORTED, rig.speaker.Describe("notawav.wav", &format));
}

void test_speaker_names_a_compressed_wav_separately(void) {
    // The container is right and the contents are not — named apart from "not
    // a WAV" because "it *is* a .wav" is exactly what the operator will be
    // sure of. §10.7's `play` prints the difference.
    Rig rig;
    BringUp(rig);
    WavOptions options;
    options.format = 0x0011;  // IMA ADPCM
    PutWav("compressed.wav", MakeWav(Samples(64), options));

    audio::WavFormat format = {};
    TEST_ASSERT_EQUAL_INT(ESP_ERR_NOT_SUPPORTED,
                          rig.speaker.Describe("compressed.wav", &format));
}

void test_speaker_refuses_a_file_with_no_data_chunk(void) {
    Rig rig;
    BringUp(rig);
    WavOptions options;
    options.omit_data = true;
    PutWav("headeronly.wav", MakeWav({}, options));

    audio::WavFormat format = {};
    // A distinct code from "not a WAV": the format was found and the audio was
    // not, which is a differently-broken file.
    TEST_ASSERT_EQUAL_INT(ESP_ERR_NOT_FOUND, rig.speaker.Describe("headeronly.wav", &format));
}

void test_speaker_refuses_data_before_fmt(void) {
    // Legal RIFF, unplayable stream: the rate is not known when the samples
    // start. Refused rather than guessed at.
    Rig rig;
    BringUp(rig);
    WavOptions options;
    options.data_before_fmt = true;
    PutWav("backwards.wav", MakeWav(Samples(32), options));

    audio::WavFormat format = {};
    TEST_ASSERT_EQUAL_INT(ESP_ERR_NOT_SUPPORTED, rig.speaker.Describe("backwards.wav", &format));
}

void test_speaker_refuses_a_truncated_header(void) {
    Rig rig;
    BringUp(rig);
    const uint8_t stub[] = {'R', 'I', 'F', 'F', 0, 0};
    fake::PutBinaryFile("stub.wav", stub, sizeof(stub));

    audio::WavFormat format = {};
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_SIZE, rig.speaker.Describe("stub.wav", &format));
}

void test_speaker_reports_a_missing_file(void) {
    Rig rig;
    BringUp(rig);

    audio::WavFormat format = {};
    TEST_ASSERT_EQUAL_INT(ESP_ERR_NOT_FOUND, rig.speaker.Describe("nothere.wav", &format));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, rig.speaker.Describe(nullptr, &format));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, rig.speaker.Describe("chirp.wav", nullptr));
}

// --- Playing it -----------------------------------------------------------

void test_speaker_streams_the_data_and_not_the_header(void) {
    // **What the captured stream is for.** A parser that got `data_offset`
    // wrong plays the RIFF header as audio: a click at the start that nobody
    // can attribute, and that no amount of listening localises.
    Rig rig;
    BringUp(rig);
    const std::vector<uint8_t> samples = Samples(512);
    WavOptions options;
    options.list_chunk = true;  // with metadata in the way, so the offset matters
    PutWav("chirp.wav", MakeWav(samples, options));

    TEST_ASSERT_EQUAL_INT(ESP_OK, rig.speaker.PlayWav("chirp.wav"));

    TEST_ASSERT_EQUAL_UINT(samples.size(), fake::P().i2s.written_total);
    TEST_ASSERT_EQUAL_UINT(samples.size(), fake::P().i2s.captured_length);
    TEST_ASSERT_EQUAL_MEMORY(samples.data(), fake::P().i2s.captured, samples.size());
}

void test_speaker_unmutes_around_the_sound_and_mutes_after(void) {
    // The amplifier rail is up whenever the board is (§10.1), so a codec left
    // unmuted is audible hiss on a desk object.
    Rig rig;
    BringUp(rig);
    PutWav("chirp.wav", MakeWav(Samples(128)));

    TEST_ASSERT_TRUE(rig.codec.Muted());
    TEST_ASSERT_EQUAL_INT(ESP_OK, rig.speaker.PlayWav("chirp.wav"));
    TEST_ASSERT_TRUE(rig.codec.Muted());
    TEST_ASSERT_TRUE(CodecMuted());
    TEST_ASSERT_FALSE(rig.speaker.Busy());
}

void test_speaker_waits_for_the_dma_to_drain_before_muting(void) {
    // Muting the instant the last sample is handed over clips the tail, because
    // the DMA descriptors still hold audio. A cut-off chirp is a bug report
    // about the sound.
    Rig rig;
    BringUp(rig);
    PutWav("chirp.wav", MakeWav(Samples(64)));

    const uint32_t before = fake::P().delay_ms_total;
    TEST_ASSERT_EQUAL_INT(ESP_OK, rig.speaker.PlayWav("chirp.wav"));
    TEST_ASSERT_TRUE(fake::P().delay_ms_total - before >= 100);
}

void test_speaker_streams_in_more_than_one_buffer_fill(void) {
    // The streaming buffer is 4 KB; anything longer proves the loop, and the
    // captured bytes prove nothing was dropped or repeated between fills.
    Rig rig;
    BringUp(rig);
    const std::vector<uint8_t> samples = Samples(6000);
    PutWav("long.wav", MakeWav(samples));

    TEST_ASSERT_EQUAL_INT(ESP_OK, rig.speaker.PlayWav("long.wav"));
    TEST_ASSERT_TRUE(fake::P().i2s.write_calls > 1);
    TEST_ASSERT_EQUAL_UINT(samples.size(), fake::P().i2s.written_total);
    TEST_ASSERT_EQUAL_MEMORY(samples.data(), fake::P().i2s.captured, samples.size());
}

void test_speaker_retunes_the_channel_and_the_codec_together(void) {
    // A file at another rate means both halves move, or the codec decodes a
    // stream whose rate it does not share — which sounds like a broken file
    // rather than a mismatched clock.
    Rig rig;
    BringUp(rig, 16000);
    WavOptions options;
    options.sample_rate = 48000;
    PutWav("fast.wav", MakeWav(Samples(96), options));

    TEST_ASSERT_EQUAL_INT(ESP_OK, rig.speaker.PlayWav("fast.wav"));

    TEST_ASSERT_EQUAL_UINT32(48000, fake::P().i2s.sample_rate);
    TEST_ASSERT_EQUAL_UINT32(48000, rig.codec.SampleRate());
    TEST_ASSERT_EQUAL_UINT32(48000, rig.speaker.SampleRate());
    // **Stopped around the retune**, which the real driver requires — the fake
    // channel refuses a reconfigure while running, so this would fail rather
    // than pass quietly if the disable were dropped.
    TEST_ASSERT_EQUAL_UINT(1, fake::P().i2s.reconfig_count);
    TEST_ASSERT_EQUAL_UINT(1, fake::P().i2s.disable_count);
    TEST_ASSERT_TRUE(fake::P().i2s.enabled);
}

void test_speaker_does_not_retune_for_the_rate_it_is_already_at(void) {
    // Stopping and restarting the channel for no reason is a gap in the output
    // and a pop through the amplifier.
    Rig rig;
    BringUp(rig, 16000);
    PutWav("same.wav", MakeWav(Samples(64)));

    TEST_ASSERT_EQUAL_INT(ESP_OK, rig.speaker.PlayWav("same.wav"));
    TEST_ASSERT_EQUAL_UINT(0, fake::P().i2s.reconfig_count);
    TEST_ASSERT_EQUAL_UINT(0, fake::P().i2s.disable_count);
}

void test_speaker_refuses_stereo_and_says_nothing_audible(void) {
    // §10.13 gives this one job and one shape. The important half of this test
    // is the second assertion: a file that will not play must not unmute the
    // codec on the way to finding that out.
    Rig rig;
    BringUp(rig);
    WavOptions options;
    options.channels = 2;
    PutWav("stereo.wav", MakeWav(Samples(128), options));

    TEST_ASSERT_EQUAL_INT(ESP_ERR_NOT_SUPPORTED, rig.speaker.PlayWav("stereo.wav"));
    TEST_ASSERT_TRUE(rig.codec.Muted());
    TEST_ASSERT_EQUAL_UINT(0, fake::P().i2s.written_total);
}

void test_speaker_refuses_eight_bit_samples(void) {
    Rig rig;
    BringUp(rig);
    WavOptions options;
    options.bits = 8;
    PutWav("eightbit.wav", MakeWav(Samples(128), options));

    TEST_ASSERT_EQUAL_INT(ESP_ERR_NOT_SUPPORTED, rig.speaker.PlayWav("eightbit.wav"));
    TEST_ASSERT_EQUAL_UINT(0, fake::P().i2s.written_total);
}

void test_speaker_plays_what_a_truncated_file_actually_holds(void) {
    // The `data` chunk claims more than the file carries — a bad build of the
    // SPIFFS image. Play what is there and log it: failing silently would make
    // a short chirp indistinguishable from a quiet one.
    Rig rig;
    BringUp(rig);
    const std::vector<uint8_t> samples = Samples(100);
    WavOptions options;
    options.claimed_data_bytes = 4000;
    PutWav("short.wav", MakeWav(samples, options));

    TEST_ASSERT_EQUAL_INT(ESP_OK, rig.speaker.PlayWav("short.wav"));
    TEST_ASSERT_EQUAL_UINT(samples.size(), fake::P().i2s.written_total);
    TEST_ASSERT_TRUE(rig.codec.Muted());
}

void test_speaker_a_failed_write_still_mutes_and_reports(void) {
    // Leaving the codec unmuted because a write failed would turn one bad
    // playback into permanent hiss.
    Rig rig;
    BringUp(rig);
    PutWav("chirp.wav", MakeWav(Samples(256)));

    fake::FailNextI2sWrite(ESP_ERR_TIMEOUT);
    TEST_ASSERT_EQUAL_INT(ESP_ERR_TIMEOUT, rig.speaker.PlayWav("chirp.wav"));
    TEST_ASSERT_TRUE(rig.codec.Muted());
    TEST_ASSERT_TRUE(CodecMuted());
    TEST_ASSERT_FALSE(rig.speaker.Busy());
}

void test_speaker_that_never_came_up_refuses_to_play(void) {
    fake::MountStorage();
    audio::Speaker speaker;
    TEST_ASSERT_FALSE(speaker.Ready());
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_STATE, speaker.PlayWav("chirp.wav"));
}

void test_speaker_refuses_a_null_path(void) {
    Rig rig;
    BringUp(rig);
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, rig.speaker.PlayWav(nullptr));
}

void test_speaker_reports_a_missing_file_without_touching_the_codec(void) {
    Rig rig;
    BringUp(rig);
    TEST_ASSERT_EQUAL_INT(ESP_ERR_NOT_FOUND, rig.speaker.PlayWav("nothere.wav"));
    TEST_ASSERT_TRUE(rig.codec.Muted());
    TEST_ASSERT_EQUAL_UINT(0, fake::P().i2s.written_total);
}

void RegisterSpeakerTests(void) {
    RUN_TEST(test_speaker_init_opens_a_transmit_only_channel);
    RUN_TEST(test_speaker_init_matches_the_codecs_coefficient_row);
    RUN_TEST(test_speaker_auto_clears_a_starved_channel);
    RUN_TEST(test_speaker_refuses_to_come_up_without_a_codec);

    RUN_TEST(test_speaker_describes_a_plain_wav);
    RUN_TEST(test_speaker_walks_past_a_list_chunk);
    RUN_TEST(test_speaker_pads_an_odd_sized_chunk);
    RUN_TEST(test_speaker_steps_over_an_extensible_fmt_chunk);
    RUN_TEST(test_speaker_refuses_something_that_is_not_a_riff_wave);
    RUN_TEST(test_speaker_names_a_compressed_wav_separately);
    RUN_TEST(test_speaker_refuses_a_file_with_no_data_chunk);
    RUN_TEST(test_speaker_refuses_data_before_fmt);
    RUN_TEST(test_speaker_refuses_a_truncated_header);
    RUN_TEST(test_speaker_reports_a_missing_file);

    RUN_TEST(test_speaker_streams_the_data_and_not_the_header);
    RUN_TEST(test_speaker_unmutes_around_the_sound_and_mutes_after);
    RUN_TEST(test_speaker_waits_for_the_dma_to_drain_before_muting);
    RUN_TEST(test_speaker_streams_in_more_than_one_buffer_fill);
    RUN_TEST(test_speaker_retunes_the_channel_and_the_codec_together);
    RUN_TEST(test_speaker_does_not_retune_for_the_rate_it_is_already_at);
    RUN_TEST(test_speaker_refuses_stereo_and_says_nothing_audible);
    RUN_TEST(test_speaker_refuses_eight_bit_samples);
    RUN_TEST(test_speaker_plays_what_a_truncated_file_actually_holds);
    RUN_TEST(test_speaker_a_failed_write_still_mutes_and_reports);
    RUN_TEST(test_speaker_that_never_came_up_refuses_to_play);
    RUN_TEST(test_speaker_refuses_a_null_path);
    RUN_TEST(test_speaker_reports_a_missing_file_without_touching_the_codec);
}
