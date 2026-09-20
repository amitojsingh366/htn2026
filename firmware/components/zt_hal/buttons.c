#include "zt_hal.h"
#include <stdbool.h>
#include <string.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"

/* Owned by the 100 Hz input task. The sink must be a nonblocking copy. */
static struct {
    zt_button_sink_t sink;
    void *context;
    zt_button_edge_t queue[ZT_INPUT_QUEUE_CAPACITY];
    uint8_t count, candidate[9], samples[9], stable[9];
    uint64_t edge_at[9], repeat_at[9], next_poll;
    uint32_t repeat_drops, edge_overflows;
    bool initialized, polled, switch_ready;
    uint8_t boot_switch, changed_live;
} input;
static portMUX_TYPE switch_lock = portMUX_INITIALIZER_UNLOCKED;

static bool direction(unsigned button)
{
    return button >= ZT_BUTTON_DOWN && button <= ZT_BUTTON_UP;
}

static void remove_edge(unsigned index)
{
    for (unsigned i = index + 1; i < input.count; ++i)
        input.queue[i - 1] = input.queue[i];
    --input.count;
}

static zt_err_t drain(void)
{
    /* A stalled consumer cannot cause unbounded work in the input task. */
    for (unsigned i = 0; i < ZT_INPUT_QUEUE_CAPACITY && input.count; ++i) {
        zt_err_t result = input.sink(&input.queue[0], input.context);
        if (result != ZT_OK) return result;
        remove_edge(0);
    }
    return ZT_OK;
}

static zt_err_t enqueue(zt_button_edge_t edge)
{
    if (edge.kind == ZT_EDGE_REPEAT) {
        for (unsigned i = 0; i < input.count; ++i) {
            if (input.queue[i].kind == ZT_EDGE_REPEAT && input.queue[i].button == edge.button) {
                ++input.repeat_drops;
                return ZT_OK;
            }
        }
    } else {
        /* A release supersedes queued repeats of its own key. */
        for (unsigned i = 0; i < input.count;) {
            if (input.queue[i].kind == ZT_EDGE_REPEAT && input.queue[i].button == edge.button)
                remove_edge(i);
            else ++i;
        }
    }
    if (input.count == ZT_INPUT_QUEUE_CAPACITY) {
        if (edge.kind == ZT_EDGE_REPEAT) {
            ++input.repeat_drops;
            return ZT_OK;
        }
        for (unsigned i = 0; i < input.count; ++i) {
            if (input.queue[i].kind == ZT_EDGE_REPEAT) {
                remove_edge(i);
                ++input.repeat_drops;
                break;
            }
        }
    }
    if (input.count == ZT_INPUT_QUEUE_CAPACITY) {
        /* Never evict an A press (or another real edge). Leave the transition
         * pending in the debouncer and expose backpressure to the input owner. */
        ++input.edge_overflows;
        return ZT_ERR_NO_SPACE;
    }
    input.queue[input.count++] = edge;
    return ZT_OK;
}

zt_err_t zt_buttons_init(zt_button_sink_t sink, void *context)
{
    if (!sink) return ZT_ERR_INVALID_ARG;
    if (input.initialized) return ZT_ERR_INVALID_STATE;
    gpio_config_t outputs = {
        .pin_bit_mask = (1ULL << ZT_BUTTON_LOAD_GPIO) | (1ULL << ZT_BUTTON_CLK_GPIO),
        .mode = GPIO_MODE_OUTPUT, .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config_t inputs = {
        .pin_bit_mask = (1ULL << ZT_BUTTON_DATA_GPIO) | (1ULL << ZT_START_GPIO),
        .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&inputs) != ESP_OK || gpio_config(&outputs) != ESP_OK ||
        gpio_set_level(ZT_BUTTON_LOAD_GPIO, 1) != ESP_OK ||
        gpio_set_level(ZT_BUTTON_CLK_GPIO, 0) != ESP_OK) return ZT_ERR_INVALID_STATE;
    memset(&input, 0, sizeof(input));
    input.sink = sink;
    input.context = context;
    input.initialized = true;
    return ZT_OK;
}

zt_err_t zt_buttons_poll(uint64_t now_us)
{
    if (!input.initialized) return ZT_ERR_INVALID_STATE;
    if (input.polled && now_us < input.next_poll) return ZT_OK;
    bool continuous = !input.polled || now_us < input.next_poll + ZT_BUTTON_POLL_MS * 1000ULL;
    input.polled = true;
    input.next_poll = now_us + ZT_BUTTON_POLL_MS * 1000ULL;
    if (!continuous) memset(input.samples, 0, sizeof(input.samples));
    (void)drain();
    uint8_t raw[9];
    gpio_set_level(ZT_BUTTON_CLK_GPIO, 0);
    gpio_set_level(ZT_BUTTON_LOAD_GPIO, 0);
    gpio_set_level(ZT_BUTTON_LOAD_GPIO, 1);
    for (unsigned i = 0; i < ZT_BUTTON_SHIFT_BITS; ++i) {
        /* Q7 already holds the first parallel bit after LOAD is released. */
        raw[i] = gpio_get_level(ZT_BUTTON_DATA_GPIO) == ZT_BUTTON_ACTIVE_LEVEL;
        gpio_set_level(ZT_BUTTON_CLK_GPIO, 1);
        gpio_set_level(ZT_BUTTON_CLK_GPIO, 0);
    }
    raw[ZT_BUTTON_START] = gpio_get_level(ZT_START_GPIO) == ZT_BUTTON_ACTIVE_LEVEL;
    zt_err_t result = ZT_OK;
    for (unsigned i = 0; i < 9; ++i) {
        if (raw[i] != input.candidate[i] || !input.samples[i]) {
            input.candidate[i] = raw[i];
            input.samples[i] = 1;
        } else if (input.samples[i] < ZT_BUTTON_STABLE_SAMPLES) {
            if (++input.samples[i] == ZT_BUTTON_STABLE_SAMPLES) input.edge_at[i] = now_us;
        }
        if (input.samples[i] < ZT_BUTTON_STABLE_SAMPLES) continue;
        if (i == ZT_BUTTON_AUX1) {
            portENTER_CRITICAL(&switch_lock);
            if (!input.switch_ready) {
                input.boot_switch = raw[i];
                input.switch_ready = true;
            } else if (raw[i] != input.boot_switch) {
                input.changed_live = 1;
            }
            portEXIT_CRITICAL(&switch_lock);
            continue;
        }
        if (raw[i] != input.stable[i]) {
            zt_button_edge_t edge = {input.edge_at[i], (zt_button_t)i,
                                    raw[i] ? ZT_EDGE_PRESS : ZT_EDGE_RELEASE};
            if (enqueue(edge) != ZT_OK) { result = ZT_ERR_NO_SPACE; continue; }
            input.stable[i] = raw[i];
            input.repeat_at[i] = now_us + ZT_BUTTON_REPEAT_DELAY_MS * 1000ULL;
        } else if (raw[i] && direction(i) && now_us >= input.repeat_at[i]) {
            (void)enqueue((zt_button_edge_t){now_us, (zt_button_t)i, ZT_EDGE_REPEAT});
            input.repeat_at[i] = now_us + ZT_BUTTON_REPEAT_PERIOD_MS * 1000ULL;
        }
    }
    zt_err_t sink_result = drain();
    return result != ZT_OK ? result : sink_result;
}

zt_err_t zt_buttons_boot_host_switch(uint8_t *host_selected, uint8_t *changed_live)
{
    if (!host_selected || !changed_live) return ZT_ERR_INVALID_ARG;
    portENTER_CRITICAL(&switch_lock);
    bool ready = input.switch_ready;
    if (ready) {
        *host_selected = input.boot_switch;
        *changed_live = input.changed_live;
    }
    portEXIT_CRITICAL(&switch_lock);
    return ready ? ZT_OK : ZT_ERR_BUSY;
}
