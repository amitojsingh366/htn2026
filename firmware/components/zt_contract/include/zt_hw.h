#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZT_LCD_CONTROLLER "ST7789"
#define ZT_LCD_WIDTH 320
#define ZT_LCD_HEIGHT 240
#define ZT_LCD_PIXEL_BITS 16
#define ZT_LCD_PIXEL_BYTES 2
#define ZT_LCD_SPI_HOST 2
#define ZT_LCD_SPI_MODE 0
#define ZT_LCD_SPI_HZ 40000000
#define ZT_LCD_MOSI_GPIO 10
#define ZT_LCD_SCLK_GPIO 1
#define ZT_LCD_CS_GPIO 2
#define ZT_LCD_DC_GPIO 0
#define ZT_LCD_RESET_GPIO 4
/* Mandatory ordered calls: reset, init, invert_color, swap_xy, mirror, display on.
 * Commissioning must visually verify RGB/BGR and byte order. */
typedef enum {
 ZT_LCD_ORIENT_RESET=0, ZT_LCD_ORIENT_INIT=1, ZT_LCD_ORIENT_INVERT=2,
 ZT_LCD_ORIENT_SWAP_XY=3, ZT_LCD_ORIENT_MIRROR=4, ZT_LCD_ORIENT_DISPLAY_ON=5
} zt_lcd_orientation_step_t;
#define ZT_LCD_INVERT_COLOR 1
#define ZT_LCD_SWAP_XY 1
#define ZT_LCD_MIRROR_X 1
#define ZT_LCD_MIRROR_Y 0
#define ZT_LCD_DISPLAY_ON 1
#define ZT_BUTTON_DATA_GPIO 7
#define ZT_BUTTON_LOAD_GPIO 20
#define ZT_BUTTON_CLK_GPIO 21
#define ZT_BUTTON_SHIFT_BITS 8
#define ZT_BUTTON_ACTIVE_LEVEL 0
/* 74HC165: sample before each clock pulse. Logical active-low shift order. */
#define ZT_BUTTON_SHIFT_A 0
#define ZT_BUTTON_SHIFT_B 1
#define ZT_BUTTON_SHIFT_HOME 2
#define ZT_BUTTON_SHIFT_DOWN 3
#define ZT_BUTTON_SHIFT_LEFT 4
#define ZT_BUTTON_SHIFT_RIGHT 5
#define ZT_BUTTON_SHIFT_UP 6
#define ZT_BUTTON_SHIFT_AUX1 7
#define ZT_START_GPIO 9 /* input only: also ROM-download strap; never driven */
#define ZT_LED_COUNT 6
#define ZT_LED_GPIO 3
#define ZT_LED_MODEL "WS2812B"
#define ZT_LED_COLOR_ORDER "GRB"
#define ZT_LED_USE_RMT 1
#define ZT_LED_COMPONENT_CAP 24
#define ZT_LED_UPPER_LEFT 0
#define ZT_LED_UPPER_RIGHT 1
#define ZT_LED_MIDDLE_RIGHT 2
#define ZT_LED_BOTTOM_RIGHT 3
#define ZT_LED_BOTTOM_LEFT 4
#define ZT_LED_MIDDLE_LEFT 5
#define ZT_LED_FILL_SEQUENCE {4, 3, 5, 2, 0, 1}
#define ZT_I2C_RESERVED_SDA_GPIO 5 /* reserved, not initialized in core */
#define ZT_I2C_RESERVED_SCL_GPIO 6 /* reserved, not initialized in core */
#define ZT_ACCEL_UNINITIALIZED_ADDRESS 0x19
#define ZT_ACCEL_REPORTED_WHO_AM_I_VALUE 0x11
/* Unresolved probe-report address 0x19 vs WHO_AM_I value 0x11 conflict:
 * these are not interchangeable; do not copy the claim into a driver. */
#define ZT_NFC_UNINITIALIZED_ADDRESS 0x26 /* MFRC522, not initialized */
#define ZT_FLASH_GPIO_FORBIDDEN_FIRST 12
#define ZT_FLASH_GPIO_FORBIDDEN_LAST 17
#define ZT_USB_GPIO_FORBIDDEN_FIRST 18
#define ZT_USB_GPIO_FORBIDDEN_LAST 19

#ifdef __cplusplus
}
#endif
