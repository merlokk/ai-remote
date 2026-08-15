#pragma once

#include <cstddef>
#include <cstdint>

#include "driver/gpio.h"
#include "esp_err.h"

// The I²S standard-mode surface `components/audio/speaker.cpp` uses, and
// nothing else. Struct field orders match ESP-IDF's because the production code
// fills them with designated initialisers — so a field out of place here is a
// compile error rather than a silent difference.

typedef enum { I2S_NUM_0 = 0, I2S_NUM_1 = 1 } i2s_port_t;
typedef enum { I2S_ROLE_MASTER = 0, I2S_ROLE_SLAVE = 1 } i2s_role_t;

typedef enum {
    I2S_DATA_BIT_WIDTH_8BIT = 8,
    I2S_DATA_BIT_WIDTH_16BIT = 16,
    I2S_DATA_BIT_WIDTH_24BIT = 24,
    I2S_DATA_BIT_WIDTH_32BIT = 32,
} i2s_data_bit_width_t;

typedef enum { I2S_SLOT_MODE_MONO = 1, I2S_SLOT_MODE_STEREO = 2 } i2s_slot_mode_t;

typedef enum {
    I2S_MCLK_MULTIPLE_128 = 128,
    I2S_MCLK_MULTIPLE_256 = 256,
    I2S_MCLK_MULTIPLE_384 = 384,
    I2S_MCLK_MULTIPLE_512 = 512,
} i2s_mclk_multiple_t;

typedef enum { I2S_CLK_SRC_DEFAULT = 0 } i2s_clock_src_t;

typedef enum { I2S_STD_SLOT_LEFT = 0, I2S_STD_SLOT_RIGHT = 1, I2S_STD_SLOT_BOTH = 2 } i2s_std_slot_mask_t;

#define I2S_GPIO_UNUSED GPIO_NUM_NC

typedef struct i2s_channel_obj_t *i2s_chan_handle_t;

typedef struct {
    i2s_port_t id;
    i2s_role_t role;
    uint32_t dma_desc_num;
    uint32_t dma_frame_num;
    bool auto_clear_after_cb;
    bool auto_clear_before_cb;
    bool auto_clear;
    int intr_priority;
} i2s_chan_config_t;

#define I2S_CHANNEL_DEFAULT_CONFIG(port, channel_role) \
    { .id = (port), .role = (channel_role), .dma_desc_num = 6, .dma_frame_num = 240, \
      .auto_clear_after_cb = false, .auto_clear_before_cb = false, .auto_clear = false, \
      .intr_priority = 0 }

typedef struct {
    uint32_t sample_rate_hz;
    i2s_clock_src_t clk_src;
    i2s_mclk_multiple_t mclk_multiple;
} i2s_std_clk_config_t;

#define I2S_STD_CLK_DEFAULT_CONFIG(rate) \
    { .sample_rate_hz = (rate), .clk_src = I2S_CLK_SRC_DEFAULT, \
      .mclk_multiple = I2S_MCLK_MULTIPLE_256 }

typedef struct {
    i2s_data_bit_width_t data_bit_width;
    i2s_data_bit_width_t slot_bit_width;
    i2s_slot_mode_t slot_mode;
    i2s_std_slot_mask_t slot_mask;
    uint32_t ws_width;
    bool ws_pol;
    bool bit_shift;
    bool left_align;
    bool big_endian;
    bool bit_order_lsb;
} i2s_std_slot_config_t;

#define I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bits, mono_or_stereo)                  \
    { .data_bit_width = (bits), .slot_bit_width = (bits), .slot_mode = (mono_or_stereo), \
      .slot_mask = I2S_STD_SLOT_BOTH, .ws_width = (bits), .ws_pol = false,         \
      .bit_shift = true, .left_align = false, .big_endian = false,                 \
      .bit_order_lsb = false }

typedef struct {
    gpio_num_t mclk;
    gpio_num_t bclk;
    gpio_num_t ws;
    gpio_num_t dout;
    gpio_num_t din;
    struct {
        uint32_t mclk_inv : 1;
        uint32_t bclk_inv : 1;
        uint32_t ws_inv : 1;
    } invert_flags;
} i2s_std_gpio_config_t;

typedef struct {
    i2s_std_clk_config_t clk_cfg;
    i2s_std_slot_config_t slot_cfg;
    i2s_std_gpio_config_t gpio_cfg;
} i2s_std_config_t;

esp_err_t i2s_new_channel(const i2s_chan_config_t *config, i2s_chan_handle_t *tx,
                          i2s_chan_handle_t *rx);
esp_err_t i2s_del_channel(i2s_chan_handle_t handle);
esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t handle, const i2s_std_config_t *config);
esp_err_t i2s_channel_reconfig_std_clock(i2s_chan_handle_t handle,
                                         const i2s_std_clk_config_t *config);
esp_err_t i2s_channel_enable(i2s_chan_handle_t handle);
esp_err_t i2s_channel_disable(i2s_chan_handle_t handle);
esp_err_t i2s_channel_write(i2s_chan_handle_t handle, const void *src, size_t size,
                            size_t *written, uint32_t timeout_ms);
