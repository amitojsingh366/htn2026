# Sanitized hardware map

| Peripheral | Configuration |
|---|---|
| LCD | ST7789, 320×240 RGB565, SPI2 mode 0 at 40 MHz; MOSI 10, SCLK 1, CS 2, D/C 0, reset 4 |
| LCD orientation | reset → init → invert_color(true) → swap_xy(true) → mirror(true,false) → display on; visually verify RGB/BGR and byte order during commissioning |
| Buttons | 74HC165 DATA 7, LOAD 20, CLK 21; sample before clock; active-low order A, B, Home, Down, Left, Right, Up, Aux1 |
| Start | GPIO9 active-low input; ROM-download strap; never drive |
| Aux1 | Maintained switch, debounce and latch once at boot; not an edge button |
| LEDs | Six WS2812B, GPIO3, RMT, GRB order; cap each component at 24/255 |
| LED positions | 0 upper-left, 1 upper-right, 2 middle-right, 3 bottom-right, 4 bottom-left, 5 middle-left; fill order {4,3,5,2,0,1} |
| Reserved I²C | SDA 5, SCL 6; not initialized in core |
| Accelerometer | Not initialized; official address 0x19, reported WHO_AM_I value 0x11. The probe-report address/value conflict remains unresolved and must not be copied into a driver. |
| NFC | MFRC522 address 0x26; not initialized |
| Console | Native USB Serial/JTAG; preserve GPIO18–19 |

Flash GPIO12–17 and USB GPIO18–19 are forbidden for repurposing. GPIO2 stays LCD CS. No battery ADC is verified; do not invent a percentage. AA or USB power; USB does not recharge AA cells. No security/eFuse, flash-voltage, download-access or JTAG changes are allowed.
