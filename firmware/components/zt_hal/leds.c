#include "zt_hal.h"
#include <stdbool.h>
#include "esp_timer.h"
#include "led_strip.h"

#define LED_INTERVAL_US 40000ULL
static led_strip_handle_t strip;
static bool initialized;
static uint8_t cap = ZT_LED_COMPONENT_CAP;
static uint64_t next_refresh_us;

static uint8_t clamp(uint8_t value, uint8_t limit)
{
    return value > limit ? limit : value;
}

zt_err_t zt_leds_init(void)
{
    if (initialized) return ZT_ERR_INVALID_STATE;
    led_strip_config_t config = {
        .strip_gpio_num = ZT_LED_GPIO, .max_leds = ZT_LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rmt = {
        .resolution_hz = 10000000, .mem_block_symbols = 48,
        .flags.with_dma = false,
    };
    if (led_strip_new_rmt_device(&config, &rmt, &strip) != ESP_OK)
        return ZT_ERR_INVALID_STATE;
    initialized = true;
    const zt_pixel_t off[ZT_LED_COUNT] = {0};
    return zt_leds_submit(off, ZT_LED_COUNT, ZT_LED_COMPONENT_CAP);
}

zt_err_t zt_leds_submit(const zt_pixel_t pixels[ZT_LED_COUNT], size_t count, uint8_t lower_cap)
{
    if (!pixels || count != ZT_LED_COUNT) return ZT_ERR_INVALID_ARG;
    if (!initialized) return ZT_ERR_INVALID_STATE;
    zt_pixel_t safe[ZT_LED_COUNT];
    /* Sole caller after startup is the UI task. No transport mailbox or task. */
    if (lower_cap < cap) cap = lower_cap;
    uint8_t limit = cap;
    uint64_t now = esp_timer_get_time();
    if (now < next_refresh_us) return ZT_ERR_BUSY;
    unsigned total = 0;
    for (unsigned i = 0; i < ZT_LED_COUNT; ++i) {
        safe[i] = (zt_pixel_t){clamp(pixels[i].r, limit), clamp(pixels[i].g, limit),
                              clamp(pixels[i].b, limit)};
        total += safe[i].r + safe[i].g + safe[i].b;
    }
    /* At most the aggregate of six single-color pixels at the component cap. */
    unsigned total_cap = ZT_LED_COUNT * limit;
    if (total > total_cap) {
        for (unsigned i = 0; i < ZT_LED_COUNT; ++i) {
            safe[i].r = safe[i].r * total_cap / total;
            safe[i].g = safe[i].g * total_cap / total;
            safe[i].b = safe[i].b * total_cap / total;
        }
    }
    for (unsigned i = 0; i < ZT_LED_COUNT; ++i)
        if (led_strip_set_pixel(strip, i, safe[i].r, safe[i].g, safe[i].b) != ESP_OK)
            return ZT_ERR_INVALID_STATE;
    /* Six pixels only: the approved pinned RMT refresh runs in the UI task. */
    esp_err_t result = led_strip_refresh(strip);
    next_refresh_us = (uint64_t)esp_timer_get_time() + LED_INTERVAL_US;
    return result == ESP_OK ? ZT_OK : ZT_ERR_INVALID_STATE;
}
