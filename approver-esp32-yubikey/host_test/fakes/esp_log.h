#pragma once

// The log macros, routed to a printf that is silent unless a test turns it on
// (`fake::P().log_enabled = true`). Silent by default because a driver's boot
// chatter buries fifteen lines of test output; kept as a real varargs call
// rather than `((void)0)` so the format strings still have to compile.

namespace fake {
void Log(const char *level, const char *tag, const char *format, ...);
}

#define ESP_LOGE(tag, ...) ::fake::Log("E", tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) ::fake::Log("W", tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) ::fake::Log("I", tag, __VA_ARGS__)
#define ESP_LOGD(tag, ...) ::fake::Log("D", tag, __VA_ARGS__)
#define ESP_LOGV(tag, ...) ::fake::Log("V", tag, __VA_ARGS__)
