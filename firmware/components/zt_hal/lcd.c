#include "zt_hal.h"
#include <stdbool.h>
#include <string.h>
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

/* Only these two arrays hold pixels. Internal DRAM, aligned for SPI DMA. */
static DMA_ATTR uint16_t stripes[ZT_LCD_STRIPE_COUNT][ZT_LCD_STRIPE_PIXELS];
_Static_assert(sizeof(stripes) == ZT_LCD_BUFFER_BYTES, "LCD stripe budget");
_Static_assert(ZT_LCD_STRIPE_COUNT == 2 && ZT_LCD_QUEUE_DEPTH == 2, "double buffering");
enum { FREE, ACQUIRED, COPYING, QUEUED, SUBMITTED, DMA_COMPLETE, QUARANTINED };
static uint8_t ownership[ZT_LCD_STRIPE_COUNT];
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static esp_lcd_panel_io_handle_t io;
static esp_lcd_panel_handle_t panel;
static zt_lcd_done_t completed;
static void *completed_context;
static bool initialized;
/* The UI task owns submission/polling. The ISR only changes ownership to DONE.
 * Keep at most two work records, including the active DMA; no additional task. */
static zt_lcd_work_t pending[ZT_LCD_STRIPE_COUNT];
static uint8_t active = ZT_LCD_STRIPE_COUNT;
static uint8_t queued = ZT_LCD_STRIPE_COUNT;
static uint64_t submitted_us;
static zt_err_t fault;
static bool fault_reported;
/* Flash operations defer non-IRAM SPI callbacks on the C3 for hundreds of
 * milliseconds. Five seconds gives boot-time NVS activity substantial headroom
 * while still detecting a stuck transfer. Poll frequency must not shorten it. */
#define LCD_COMPLETION_TIMEOUT_US 5000000ULL

static bool dma_done(esp_lcd_panel_io_handle_t handle,
                     esp_lcd_panel_io_event_data_t *event, void *context)
{
    (void)handle; (void)event; (void)context;
    portENTER_CRITICAL_ISR(&lock);
    if (active < ZT_LCD_STRIPE_COUNT &&
        (ownership[active] == SUBMITTED || ownership[active] == QUARANTINED))
        ownership[active] = DMA_COMPLETE;
    portEXIT_CRITICAL_ISR(&lock);
    return false;
}

static void start_transfer(uint8_t index)
{
    const zt_lcd_work_t *work = &pending[index];
    submitted_us = esp_timer_get_time();
    portENTER_CRITICAL(&lock);
    active = index;
    ownership[index] = work->pixel_count ? SUBMITTED : DMA_COMPLETE;
    portEXIT_CRITICAL(&lock);
    if (!work->pixel_count) return;
    /* No prior color transfer remains outstanding when this is called. Thus
     * IDF's address-window commands cannot wait on a missing prior DMA IRQ. */
    if (esp_lcd_panel_draw_bitmap(panel, work->rect.x0, work->rect.y0,
                                 work->rect.x1, work->rect.y1, work->rgb565) != ESP_OK)
        fault = ZT_ERR_INVALID_STATE;
}

static zt_err_t poll_completions(void)
{
    uint8_t done = ZT_LCD_STRIPE_COUNT;
    portENTER_CRITICAL(&lock);
    if (active < ZT_LCD_STRIPE_COUNT && ownership[active] == DMA_COMPLETE) {
        done = active;
        ownership[done] = FREE;
        active = ZT_LCD_STRIPE_COUNT;
    }
    portEXIT_CRITICAL(&lock);
    if (done < ZT_LCD_STRIPE_COUNT && !fault_reported && completed)
        completed(pending[done].request_id, done, fault, completed_context);
    if (done < ZT_LCD_STRIPE_COUNT && fault == ZT_ERR_TIMEOUT) {
        /* The ISR has now proved the quarantined DMA finished. Its timeout was
         * already reported: do not send a second completion for this request.
         * Only lateness recovers; draw errors remain latched even after an IRQ. */
        fault = ZT_OK;
        fault_reported = false;
    }
    uint64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&lock);
    if (!fault && active < ZT_LCD_STRIPE_COUNT && ownership[active] == SUBMITTED &&
        now - submitted_us >= LCD_COMPLETION_TIMEOUT_US) fault = ZT_ERR_TIMEOUT;
    portEXIT_CRITICAL(&lock);
    if (fault && !fault_reported) {
        fault_reported = true;
        uint8_t failed = active, cancelled = queued;
        portENTER_CRITICAL(&lock);
        /* A late IRQ may still arrive. Never recycle an uncompleted DMA buffer,
         * even after notifying the UI of failure. Resume after a timeout only
         * when that IRQ is observed; a missing IRQ keeps the buffer quarantined. */
        if (failed < ZT_LCD_STRIPE_COUNT && ownership[failed] != DMA_COMPLETE)
            ownership[failed] = QUARANTINED;
        if (cancelled < ZT_LCD_STRIPE_COUNT) ownership[cancelled] = FREE;
        queued = ZT_LCD_STRIPE_COUNT;
        portEXIT_CRITICAL(&lock);
        if (completed) {
            if (failed < ZT_LCD_STRIPE_COUNT)
                completed(pending[failed].request_id, failed, fault, completed_context);
            if (cancelled < ZT_LCD_STRIPE_COUNT)
                completed(pending[cancelled].request_id, cancelled, fault, completed_context);
        }
    }
    if (!fault && active == ZT_LCD_STRIPE_COUNT && queued < ZT_LCD_STRIPE_COUNT) {
        uint8_t next = queued;
        queued = ZT_LCD_STRIPE_COUNT;
        start_transfer(next);
    }
    return fault;
}

zt_err_t zt_lcd_init(zt_lcd_done_t done, void *context)
{
    if (initialized) return ZT_ERR_INVALID_STATE;
    spi_bus_config_t bus = {
        .mosi_io_num = ZT_LCD_MOSI_GPIO, .miso_io_num = -1,
        .sclk_io_num = ZT_LCD_SCLK_GPIO, .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = ZT_LCD_STRIPE_BYTES,
    };
    if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK)
        return ZT_ERR_INVALID_STATE;
    esp_lcd_panel_io_spi_config_t config = {
        .cs_gpio_num = ZT_LCD_CS_GPIO, .dc_gpio_num = ZT_LCD_DC_GPIO,
        .spi_mode = ZT_LCD_SPI_MODE, .pclk_hz = ZT_LCD_SPI_HZ,
        .trans_queue_depth = ZT_LCD_QUEUE_DEPTH,
        .lcd_cmd_bits = 8, .lcd_param_bits = 8,
        .on_color_trans_done = dma_done,
    };
    esp_err_t err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &config, &io);
    esp_lcd_panel_dev_config_t device = {
        .reset_gpio_num = ZT_LCD_RESET_GPIO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = ZT_LCD_PIXEL_BITS,
        /* Native uint16_t stripes on the C3; ST7789 RAMCTRL handles byte order. */
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
    };
    if (err == ESP_OK) err = esp_lcd_new_panel_st7789(io, &device, &panel);
    /* Startup only: these SDK calls include the controller's reset/wake waits. */
    if (err == ESP_OK) err = esp_lcd_panel_reset(panel);
    if (err == ESP_OK) err = esp_lcd_panel_init(panel);
    if (err == ESP_OK) err = esp_lcd_panel_invert_color(panel, true);
    if (err == ESP_OK) err = esp_lcd_panel_swap_xy(panel, true);
    if (err == ESP_OK) err = esp_lcd_panel_mirror(panel, true, false);
    if (err == ESP_OK) err = esp_lcd_panel_disp_on_off(panel, true);
    if (err == ESP_OK) {
        completed = done;
        completed_context = context;
        initialized = true;
        return ZT_OK;
    }
    if (panel) { esp_lcd_panel_del(panel); panel = NULL; }
    if (io) { esp_lcd_panel_io_del(io); io = NULL; }
    spi_bus_free(SPI2_HOST);
    return ZT_ERR_INVALID_STATE;
}

zt_err_t zt_lcd_acquire(uint8_t *stripe_index, uint16_t **pixels, size_t *capacity_pixels)
{
    if (!stripe_index || !pixels || !capacity_pixels) return ZT_ERR_INVALID_ARG;
    if (!initialized) return ZT_ERR_INVALID_STATE;
    zt_err_t completion_result = poll_completions();
    if (completion_result != ZT_OK) return completion_result;
    zt_err_t result = ZT_ERR_BUSY;
    portENTER_CRITICAL(&lock);
    for (unsigned i = 0; i < ZT_LCD_STRIPE_COUNT; ++i) {
        if (ownership[i] == FREE) {
            ownership[i] = ACQUIRED;
            *stripe_index = i;
            *pixels = stripes[i];
            *capacity_pixels = ZT_LCD_STRIPE_PIXELS;
            result = ZT_OK;
            break;
        }
    }
    portEXIT_CRITICAL(&lock);
    return result;
}

zt_err_t zt_lcd_submit(const zt_lcd_work_t *work)
{
    if (!work || work->stripe_index >= ZT_LCD_STRIPE_COUNT || !work->rgb565)
        return ZT_ERR_INVALID_ARG;
    if (!initialized) return ZT_ERR_INVALID_STATE;
    if (fault) return fault;
    int width = (int)work->rect.x1 - work->rect.x0;
    int height = (int)work->rect.y1 - work->rect.y0;
    if (width <= 0 || height <= 0 || height > ZT_LCD_STRIPE_HEIGHT ||
        (size_t)width * height > ZT_LCD_STRIPE_PIXELS ||
        work->pixel_count != (size_t)width * height) return ZT_ERR_INVALID_LENGTH;
    unsigned index = work->stripe_index;
    /* The acquired stripe is the only permitted source: no borrowed lifetime or
     * copy from another in-flight stripe. Clipping compacts it in place. */
    if (work->rgb565 != stripes[index]) return ZT_ERR_INVALID_ARG;
    portENTER_CRITICAL(&lock);
    bool acquired = ownership[index] == ACQUIRED;
    if (acquired) ownership[index] = COPYING;
    portEXIT_CRITICAL(&lock);
    if (!acquired) return ZT_ERR_INVALID_STATE;
    zt_lcd_work_t clipped = *work;
    int x0 = work->rect.x0 < 0 ? 0 : work->rect.x0;
    int y0 = work->rect.y0 < 0 ? 0 : work->rect.y0;
    int x1 = work->rect.x1 > ZT_LCD_WIDTH ? ZT_LCD_WIDTH : work->rect.x1;
    int y1 = work->rect.y1 > ZT_LCD_HEIGHT ? ZT_LCD_HEIGHT : work->rect.y1;
    clipped.pixel_count = 0;
    if (x1 > x0 && y1 > y0) {
        for (int row = y0; row < y1; ++row)
            memmove(stripes[index] + (row - y0) * (x1 - x0),
                    stripes[index] + (row - work->rect.y0) * width + x0 - work->rect.x0,
                    (x1 - x0) * sizeof(uint16_t));
        clipped.rect = (zt_rect_t){x0, y0, x1, y1};
        clipped.pixel_count = (size_t)(x1 - x0) * (y1 - y0);
    }
    pending[index] = clipped;
    ownership[index] = QUEUED;
    /* There can be only one queued stripe beside the active stripe. Drawing the
     * second buffer overlaps DMA; its address window is issued after completion. */
    if (active == ZT_LCD_STRIPE_COUNT) start_transfer(index);
    else queued = index;
    return ZT_OK;
}

zt_err_t zt_lcd_release(uint8_t stripe_index)
{
    if (stripe_index >= ZT_LCD_STRIPE_COUNT) return ZT_ERR_INVALID_ARG;
    if (!initialized) return ZT_ERR_INVALID_STATE;
    portENTER_CRITICAL(&lock);
    bool acquired = ownership[stripe_index] == ACQUIRED;
    if (acquired) ownership[stripe_index] = FREE;
    portEXIT_CRITICAL(&lock);
    return acquired ? ZT_OK : ZT_ERR_INVALID_STATE;
}
