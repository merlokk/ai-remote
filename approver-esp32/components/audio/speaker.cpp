#include "speaker.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "storage.h"

namespace audio {

namespace {

constexpr const char *TAG = "speaker";

// The one streaming buffer, static and shared, because playback is one file at
// a time (§10.14.1 — nothing here allocates). 4 KB is 128 ms of 16 kHz mono
// audio: long enough that the file system is read in useful lumps, short enough
// that it is not a meaningful slice of the 512 KB this chip has.
constexpr size_t kStreamBufferSize = 4096;
uint8_t stream_buffer[kStreamBufferSize];

// How long a write may block before it is treated as a wedged channel rather
// than a slow one.
constexpr uint32_t kWriteTimeoutMs = 1000;

// After the last sample there is still audio inside the DMA descriptors. Muting
// immediately clips the tail; this is how long to wait first, and it is
// generous on purpose — a cut-off chirp is a bug report about the sound.
constexpr uint32_t kDrainMs = 120;

constexpr uint16_t kFormatPcm = 1;

uint32_t ReadU32(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint16_t ReadU16(const uint8_t *p) {
    return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8));
}

// Walks the RIFF chunks rather than assuming the canonical 44-byte header.
// Encoders put `LIST`/`INFO` between `fmt ` and `data` whenever they feel like
// it, and a parser that trusts the offset plays metadata as audio — which
// sounds exactly like a driver bug.
esp_err_t ParseWavHeader(FILE *file, WavFormat *out, long *data_offset) {
    uint8_t riff[12] = {};
    if (fread(riff, 1, sizeof(riff), file) != sizeof(riff)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    bool have_format = false;
    WavFormat format = {};

    for (;;) {
        uint8_t header[8] = {};
        if (fread(header, 1, sizeof(header), file) != sizeof(header)) {
            return have_format ? ESP_ERR_NOT_FOUND : ESP_ERR_INVALID_SIZE;
        }
        const uint32_t size = ReadU32(header + 4);

        if (memcmp(header, "fmt ", 4) == 0) {
            uint8_t fmt[16] = {};
            // **Sixteen bytes is the minimum, not the usual case.** A shorter
            // `fmt ` leaves the tail of this buffer zero, and a zeroed tail is
            // a believable-looking format: `bits` reads 0 and `sample_rate`
            // reads whatever fitted. Refused as a malformed file, because the
            // alternative is a `Describe` that answers with numbers nobody
            // wrote.
            if (size < sizeof(fmt)) {
                return ESP_ERR_INVALID_SIZE;
            }
            const size_t want = sizeof(fmt);
            if (fread(fmt, 1, want, file) != want) {
                return ESP_ERR_INVALID_SIZE;
            }
            if (ReadU16(fmt) != kFormatPcm) {
                // Compressed WAV: the container is right and the contents are
                // not. Named separately because "it is a .wav" is exactly what
                // the operator will be sure of.
                return ESP_ERR_NOT_SUPPORTED;
            }
            format.channels = ReadU16(fmt + 2);
            format.sample_rate = ReadU32(fmt + 4);
            format.bits = ReadU16(fmt + 14);
            have_format = true;
            if (size > want && fseek(file, static_cast<long>(size - want), SEEK_CUR) != 0) {
                return ESP_ERR_INVALID_SIZE;
            }
        } else if (memcmp(header, "data", 4) == 0) {
            if (!have_format) {
                return ESP_ERR_NOT_SUPPORTED;
            }
            if (size == 0) {
                // A header with no audio behind it — the same answer as a file
                // with no `data` chunk at all, and for the same reason: there
                // is nothing to play. It matters that this is an error rather
                // than a zero-length success, because a success unmutes the
                // codec for the length of the drain and mutes it again, which
                // is a click for nothing.
                return ESP_ERR_NOT_FOUND;
            }
            format.data_bytes = size;
            *data_offset = ftell(file);
            *out = format;
            return ESP_OK;
        } else if (fseek(file, static_cast<long>(size + (size & 1)), SEEK_CUR) != 0) {
            return ESP_ERR_INVALID_SIZE;
        }
    }
}

}  // namespace

esp_err_t Speaker::Init(Es8311 &codec, const SpeakerPins &pins, uint32_t sample_rate) {
    if (!codec.Present()) {
        return ESP_ERR_INVALID_STATE;
    }
    codec_ = &codec;
    pins_ = pins;

    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel_config.auto_clear = true;  // silence, not the last buffer, when starved
    esp_err_t err = i2s_new_channel(&channel_config, &channel_, nullptr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no I2S channel: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_MONO),
        .gpio_cfg =
            {
                .mclk = pins.mclk,
                .bclk = pins.bclk,
                .ws = pins.lrck,
                .dout = pins.data_out,
                .din = I2S_GPIO_UNUSED,
                .invert_flags =
                    {
                        .mclk_inv = false,
                        .bclk_inv = false,
                        .ws_inv = false,
                    },
            },
    };
    // 256×fs is what the codec's single coefficient row assumes (es8311.h), and
    // it is ESP-IDF's default — stated rather than relied upon.
    std_config.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    err = i2s_channel_init_std_mode(channel_, &std_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S not configured: %s", esp_err_to_name(err));
        return err;
    }

    err = i2s_channel_enable(channel_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S not enabled: %s", esp_err_to_name(err));
        return err;
    }

    sample_rate_ = sample_rate;
    ESP_LOGI(TAG, "I2S out on mclk=%d bclk=%d lrck=%d dout=%d at %" PRIu32 " Hz",
             static_cast<int>(pins.mclk), static_cast<int>(pins.bclk), static_cast<int>(pins.lrck),
             static_cast<int>(pins.data_out), sample_rate_);
    return ESP_OK;
}

esp_err_t Speaker::Retune(uint32_t sample_rate) {
    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    clk.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    return i2s_channel_reconfig_std_clock(channel_, &clk);
}

esp_err_t Speaker::Reconfigure(uint32_t sample_rate) {
    if (sample_rate == sample_rate_) {
        return ESP_OK;
    }

    // **Ask before touching anything.** The codec has five rates it can clock
    // and refuses the rest by name (`es8311.h`); finding that out from its
    // return value would mean finding it out *after* the channel has been
    // stopped for the retune, and the channel would then stay stopped — the
    // next file at a rate this speaker is already at skips the reconfigure
    // entirely, so one 22 050 Hz WAV left the speaker silent until a reboot,
    // with every `PlayWav` after it returning success or a confusing state
    // error. Same call the RTC makes about an impossible date and the PMIC
    // about a power-off over USB: refuse first, write nothing.
    if (!Es8311::RateSupported(sample_rate)) {
        ESP_LOGE(TAG, "%" PRIu32 " Hz is not a rate this codec can clock", sample_rate);
        return ESP_ERR_NOT_SUPPORTED;
    }

    // The channel has to stop before its clock can be retuned, and the codec
    // has to be told the same number or it will decode a stream whose rate it
    // does not share.
    esp_err_t err = i2s_channel_disable(channel_);
    if (err != ESP_OK) {
        return err;
    }

    err = Retune(sample_rate);
    if (err == ESP_OK) {
        err = codec_->SetSampleRate(sample_rate);
    }
    if (err != ESP_OK) {
        // Put it back the way it was and start it again. Anything that fails
        // here is a bus that was busy or a chip that did not answer — a
        // transient — and leaving a stopped channel behind would turn it into
        // a permanent one.
        ESP_LOGE(TAG, "retune to %" PRIu32 " Hz failed (%s); back to %" PRIu32 " Hz", sample_rate,
                 esp_err_to_name(err), sample_rate_);
        Retune(sample_rate_);
        i2s_channel_enable(channel_);
        return err;
    }

    err = i2s_channel_enable(channel_);
    if (err != ESP_OK) {
        return err;
    }

    sample_rate_ = sample_rate;
    return ESP_OK;
}

esp_err_t Speaker::Describe(const char *path, WavFormat *out) {
    if (path == nullptr || out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    char full[storage::kMaxPathLength];
    if (!storage::ResolvePath(path, full, sizeof(full))) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *file = fopen(full, "rb");
    if (file == nullptr) {
        return ESP_ERR_NOT_FOUND;
    }

    long offset = 0;
    const esp_err_t err = ParseWavHeader(file, out, &offset);
    fclose(file);
    return err;
}

esp_err_t Speaker::PlayWav(const char *path) {
    if (!Ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (busy_) {
        return ESP_ERR_INVALID_STATE;
    }
    if (path == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    char full[storage::kMaxPathLength];
    if (!storage::ResolvePath(path, full, sizeof(full))) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *file = fopen(full, "rb");
    if (file == nullptr) {
        return ESP_ERR_NOT_FOUND;
    }

    WavFormat format = {};
    long data_offset = 0;
    esp_err_t err = ParseWavHeader(file, &format, &data_offset);
    if (err != ESP_OK) {
        fclose(file);
        return err;
    }

    // What this driver plays, and nothing else (see the header): 16-bit PCM,
    // one channel, at a rate the codec's coefficient row covers.
    if (format.bits != 16 || format.channels != 1) {
        fclose(file);
        return ESP_ERR_NOT_SUPPORTED;
    }

    // **Seek to where the parser said the audio starts, rather than trusting
    // it to have left the position there.** It does leave it there — that is
    // why this worked without the seek — but "correct because of where a
    // function happened to stop reading" is a coupling nobody can see, and it
    // breaks the moment the parser looks ahead for a second chunk. The host
    // tests found it the honest way: hard-coding `data_offset` to the
    // canonical 44 changed nothing, which is what a value nobody reads looks
    // like.
    if (fseek(file, data_offset, SEEK_SET) != 0) {
        fclose(file);
        return ESP_ERR_INVALID_SIZE;
    }

    busy_ = true;
    err = Reconfigure(format.sample_rate);
    if (err != ESP_OK) {
        fclose(file);
        busy_ = false;
        return err;
    }

    err = codec_->Mute(false);
    if (err != ESP_OK) {
        fclose(file);
        busy_ = false;
        return err;
    }

    uint32_t remaining = format.data_bytes;
    while (remaining > 0) {
        const size_t want =
            remaining < kStreamBufferSize ? static_cast<size_t>(remaining) : kStreamBufferSize;
        const size_t got = fread(stream_buffer, 1, want, file);
        if (got == 0) {
            // The `data` chunk claimed more than the file holds. Play what
            // there was and say so: a truncated file is a bad build of the
            // image, not a reason to fail silently.
            ESP_LOGW(TAG, "%s ended %" PRIu32 " bytes early", full, remaining);
            break;
        }

        size_t written = 0;
        err = i2s_channel_write(channel_, stream_buffer, got, &written, kWriteTimeoutMs);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "write failed: %s", esp_err_to_name(err));
            break;
        }
        remaining -= static_cast<uint32_t>(got);
    }

    fclose(file);

    // The DMA still holds audio at this point; muting now would clip the tail.
    vTaskDelay(pdMS_TO_TICKS(kDrainMs));
    const esp_err_t mute_err = codec_->Mute(true);
    busy_ = false;

    return err != ESP_OK ? err : mute_err;
}

}  // namespace audio
