#pragma once
#include <stddef.h>
#include <stdint.h>
#include "zt_common.h"
#include "zt_hw.h"
#ifdef __cplusplus
extern "C" {
#endif
#define ZT_BUTTON_POLL_MS 10
#define ZT_BUTTON_STABLE_SAMPLES 3
#define ZT_BUTTON_REPEAT_DELAY_MS 400
#define ZT_BUTTON_REPEAT_PERIOD_MS 150
#define ZT_HOME_HOLD_MS 2000
#define ZT_LCD_STRIPE_HEIGHT 4
#define ZT_LCD_STRIPE_COUNT 2
#define ZT_LCD_STRIPE_PIXELS (ZT_LCD_WIDTH * ZT_LCD_STRIPE_HEIGHT)
#define ZT_LCD_STRIPE_BYTES (ZT_LCD_STRIPE_PIXELS * ZT_LCD_PIXEL_BYTES)
#define ZT_LCD_QUEUE_DEPTH 2
/* Aux1 is a maintained switch, latched once at boot after debounce, never an
 * edge button. Live change is only a reboot notice. A never repeats. */
typedef enum {
    ZT_BUTTON_A=0, ZT_BUTTON_B=1, ZT_BUTTON_HOME=2, ZT_BUTTON_DOWN=3,
    ZT_BUTTON_LEFT=4, ZT_BUTTON_RIGHT=5, ZT_BUTTON_UP=6, ZT_BUTTON_AUX1=7,
    ZT_BUTTON_START=8
} zt_button_t;
typedef enum { ZT_EDGE_RELEASE=0, ZT_EDGE_PRESS=1, ZT_EDGE_REPEAT=2 } zt_edge_kind_t;
typedef struct { uint64_t at_us; zt_button_t button; zt_edge_kind_t kind; } zt_button_edge_t;
typedef zt_err_t (*zt_button_sink_t)(const zt_button_edge_t *edge, void *context);
/* Sink copies accepted edge before return; coalesce direction repeats before
 * dropping press/release. Called in input task, never an ISR. */
zt_err_t zt_buttons_init(zt_button_sink_t sink, void *context);
zt_err_t zt_buttons_poll(uint64_t now_us);
zt_err_t zt_buttons_boot_host_switch(uint8_t *host_selected, uint8_t *changed_live);
/* Exclusive end coordinates; clipped integer rectangles, no full-screen buffer. */
typedef struct { int16_t x0, y0, x1, y1; } zt_rect_t;
typedef struct {
    zt_rect_t rect;
    const uint16_t *rgb565;
    size_t pixel_count;
    uint8_t stripe_index;
    uint32_t request_id;
} zt_lcd_work_t;
typedef void (*zt_lcd_done_t)(uint32_t request_id, uint8_t stripe_index, zt_err_t result, void *context);
/* Exactly two internal-DMA 320x16 RGB565 buffers (20480 bytes). Acquire grants
 * exclusive use until submit; submitted stripes cannot be reused until DMA done.
 * Completion only signals ownership, never mutates gameplay. Queue depth two. */
zt_err_t zt_lcd_init(zt_lcd_done_t done, void *context);
zt_err_t zt_lcd_acquire(uint8_t *stripe_index, uint16_t **pixels, size_t *capacity_pixels);
zt_err_t zt_lcd_submit(const zt_lcd_work_t *work);
zt_err_t zt_lcd_release(uint8_t stripe_index);
typedef struct { uint8_t r, g, b; } zt_pixel_t;
zt_err_t zt_leds_init(void);
/* Count must equal six; HAL clamps every component to <=24, including debug UI. */
zt_err_t zt_leds_submit(const zt_pixel_t pixels[ZT_LED_COUNT], size_t count, uint8_t lower_cap);
#ifdef __cplusplus
}
#endif
