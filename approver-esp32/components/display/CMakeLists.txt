# **`ui` on this line is worth a sentence, because it points the other way from
# every other dependency here.** `components/ui` has an *empty* `REQUIRES` and
# every file in it includes `<cstdint>` and nothing else — it is a leaf of pure
# arithmetic, not a layer above this one. What the touch driver takes from it is
# `touch_cal.h`: the correction applied to every point this panel reports, which
# is a decision and therefore belongs where §10.11 can run it rather than
# duplicated here where it cannot.
#
# The rule that keeps §10.14.2 true is narrower than the dependency: **no file in
# this component may include a `ui` header that knows what an approval is.**
# `touch_cal.h` knows about pixels.
idf_component_register(SRCS "panel.cpp" "touch.cpp" "lvgl_display.cpp" "rawimage.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES i2cbus esp_lcd esp_driver_spi esp_driver_gpio storage ui
                             lvgl__lvgl espressif__esp_lcd_touch
                    PRIV_REQUIRES espressif__esp_lvgl_port
                                  espressif__esp_lcd_sh8601
                                  waveshare__esp_lcd_touch_cst9217)
