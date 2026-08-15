#pragma once

// The playback side of the audio hardware: an I²S transmit channel feeding the
// ES8311, and enough WAV parsing to get a file off SPIFFS into it.
//
// **Uncompressed PCM only, and that is a decision.** §10.13 gives this hardware
// one job — a short chirp on a new request (§10.8.1) — and a decoder is the
// wrong price for it: an MP3 decoder is a new dependency under root §1, tens of
// kilobytes of code and a chunk of the 512 KB this chip has (§10.1). A 16 kHz
// mono WAV of a three-second sound is ~100 KB of a 10.9 MB partition, which is
// the resource this board has in abundance. The conversion happens on the host
// and the command is in `working-with-code.md`.
//
// **It plays; it does not decode, mix, or record.** One file at a time, from
// the calling task, blocking until the sound is over.
//
// Library layer (§10.14.2): it knows about a bus, a chip and a file, and
// nothing about approvals.

#include <cstdint>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "es8311.h"
#include "esp_err.h"

namespace audio {

// The four wires the codec's playback path needs. `din` is deliberately absent:
// the microphones have no job (§10.13), so the channel is transmit-only.
struct SpeakerPins {
    gpio_num_t mclk = GPIO_NUM_NC;
    gpio_num_t bclk = GPIO_NUM_NC;
    gpio_num_t lrck = GPIO_NUM_NC;  // word select
    gpio_num_t data_out = GPIO_NUM_NC;
};

// What a WAV has to be to play here. Anything else is refused by name rather
// than approximated — a file played at the wrong rate sounds like broken
// hardware, and that is an hour lost to the wrong question.
struct WavFormat {
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits;
    uint32_t data_bytes;
};

class Speaker {
   public:
    Speaker() = default;
    Speaker(const Speaker &) = delete;
    Speaker &operator=(const Speaker &) = delete;

    // Trivial constructor, separate Init (§10.14.1). The codec must already be
    // initialised; the caller owns it, and owns the amplifier rail as well
    // (§10.1 — it is the PMIC's ALDO2, which is board knowledge, not this
    // layer's).
    esp_err_t Init(Es8311 &codec, const SpeakerPins &pins, uint32_t sample_rate);
    bool Ready() const { return channel_ != nullptr; }

    // Reads the header without playing anything — what the console prints
    // before deciding a file is worth sending to the codec.
    esp_err_t Describe(const char *path, WavFormat *out);

    // Plays a WAV from the storage partition, blocking until it has finished.
    // Unmutes around the sound and mutes again after, because the amplifier is
    // powered whenever the board is and a codec left unmuted is audible hiss.
    esp_err_t PlayWav(const char *path);

    // Playback is one-at-a-time: a second caller gets ESP_ERR_INVALID_STATE
    // rather than two sounds interleaved sample by sample.
    bool Busy() const { return busy_; }

    uint32_t SampleRate() const { return sample_rate_; }

   private:
    esp_err_t Reconfigure(uint32_t sample_rate);

    Es8311 *codec_ = nullptr;
    i2s_chan_handle_t channel_ = nullptr;
    SpeakerPins pins_ = {};
    uint32_t sample_rate_ = 0;
    bool busy_ = false;
};

}  // namespace audio
