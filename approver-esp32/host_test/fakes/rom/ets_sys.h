#pragma once

#include <cstdint>

// The busy-wait `Bus::Recover` uses between SCL edges. Counted, not spent.
void esp_rom_delay_us(uint32_t us);
