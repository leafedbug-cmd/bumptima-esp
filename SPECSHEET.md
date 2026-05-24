# Lonely Binary Gold Edition — ESP32-WROOM-32UE

## Overview

| Property | Value |
|---|---|
| Module | ESP32-WROOM-32UE |
| SoC | Espressif ESP32 (Xtensa LX6, dual-core, 240 MHz) |
| Flash | 4 MB (internal) |
| PSRAM | None (standard WROOM-32UE) |
| Wi-Fi | 802.11 b/g/n 2.4 GHz |
| Bluetooth | BT 4.2 + BLE |
| Antenna | External U.FL connector (UE variant) |
| USB | USB-C (onboard CP210x or CH340 — check silkscreen) |
| Voltage | 3.3 V logic / 5 V input via USB-C or 5V pin |
| Buttons | EN (reset), BOOT (GPIO0) |

---

## Onboard LED

| Feature | GPIO | Notes |
|---|---|---|
| **WS2812B RGB LED** | **GPIO2** | Labeled "RGB 102" on PCB silkscreen. Single addressable NeoPixel. Logic high = 3.3 V compatible. |

> **Note:** GPIO2 is also ADC2_0, TOUCH2, and RTC-capable. Keep this in mind if you use deep sleep or ADC2 alongside the LED. GPIO2 must be LOW or floating at boot to enter flash mode.

---

## Pin Reference

### Left Header (top → bottom, board facing component side)

| Pin Label | GPIO | Alternate Functions |
|---|---|---|
| 5V | — | 5 V power output/input |
| 11 | GPIO11 | SD_CMD, SPI_CS1 |
| 10 | GPIO10 | SD_DATA3, SPI_WP |
| 9 | GPIO9 | SD_DATA2, SPI_HD |
| 13 | GPIO13 | ADC2_4, TOUCH4, RTC_GPIO14, MTCK |
| GND | — | Ground |
| 12 | GPIO12 | ADC2_5, TOUCH5, RTC_GPIO15, MTDI |
| 14 | GPIO14 | ADC2_6, TOUCH6, RTC_GPIO16, DAC_2 (via label) |
| 27 | GPIO27 | ADC2_7, TOUCH7, RTC_GPIO17, DAC_1 (via label) |
| 26 | GPIO26 | ADC2_9, DAC_2, RTC_GPIO7 |
| 25 | GPIO25 | ADC2_8, DAC_1, RTC_GPIO6 |
| 33 | GPIO33 | ADC1_5, TOUCH8, RTC_GPIO8 |
| 32 | GPIO32 | ADC1_4, TOUCH9, RTC_GPIO9 |
| 35 | GPIO35 | ADC1_7, RTC_GPIO5 — **input only** |
| 34 | GPIO34 | ADC1_6, RTC_GPIO4 — **input only** |
| 39 | GPIO39 | ADC1_3, RTC_GPIO3, VN — **input only** |
| 36 | GPIO36 | ADC1_0, RTC_GPIO0, VP — **input only** |
| EN | EN | Active-low chip enable / reset |
| 3V | — | 3.3 V output (from onboard LDO) |

### Right Header (top → bottom, board facing component side)

| Pin Label | GPIO | Alternate Functions |
|---|---|---|
| 6 | GPIO6 | SD_CLK — **avoid in most designs** |
| 7 | GPIO7 | SD_DATA0, D0 |
| 8 | GPIO8 | SD_DATA1, D1 |
| 15 | GPIO15 | ADC2_3, TOUCH3, RTC_GPIO13, MTDO |
| 2 | **GPIO2** | **WS2812B LED**, ADC2_2, TOUCH2, RTC_GPIO12 |
| 0 | GPIO0 | BOOT button, ADC2_1, TOUCH1, RTC_GPIO11 |
| 4 | GPIO4 | ADC2_0, TOUCH0, RTC_GPIO10 |
| 16 | GPIO16 | U2RXD, OD/IE |
| 17 | GPIO17 | U2TXD, OD/IE |
| 5 | GPIO5 | VSPI_SS, SDIO |
| 18 | GPIO18 | VSPI_SCK, OD/IE |
| 19 | GPIO19 | VSPI_MISO, OD/IE |
| GND | — | Ground |
| 21 | GPIO21 | I2C SDA (WIRE_SDA), OD/IE/WPU |
| RX | GPIO3 | U0RXD, SERIAL_RX, OD/IE/WPU |
| TX | GPIO1 | U0TXD, SERIAL_TX, OD/IE |
| 22 | GPIO22 | I2C SCL (WIRE_SCL), VSPI_MOSI, OD/IE |
| 23 | GPIO23 | VSPI_MOSI, SPI_MOSI, OD/IE |
| GND | — | Ground |

---

## Key Functional Groups

| Bus / Function | Pins |
|---|---|
| I2C (default) | SDA = GPIO21, SCL = GPIO22 |
| SPI (VSPI) | MOSI = GPIO23, MISO = GPIO19, SCK = GPIO18, SS = GPIO5 |
| UART0 (monitor/flash) | TX = GPIO1, RX = GPIO3 |
| UART2 | TX = GPIO17, RX = GPIO16 |
| DAC | DAC1 = GPIO25, DAC2 = GPIO26 |
| Touch | GPIO0, GPIO2, GPIO4, GPIO12, GPIO13, GPIO14, GPIO15, GPIO27, GPIO32, GPIO33 |
| ADC1 | GPIO32–GPIO39 (safe to use with Wi-Fi) |
| ADC2 | GPIO0, GPIO2, GPIO4, GPIO12–GPIO15, GPIO25–GPIO27 — **unavailable when Wi-Fi active** |
| Input-only | GPIO34, GPIO35, GPIO36, GPIO39 — no internal pull-up/down |
| Avoid (SPI flash) | GPIO6–GPIO11 |

---

## Boot Constraints

| GPIO | Boot requirement |
|---|---|
| GPIO0 | Must be HIGH (or floating with pull-up) for normal boot; hold LOW for flash mode |
| GPIO2 | Must be LOW or floating during flash mode (conflicts with WS2812 if driven high at boot) |
| GPIO12 | Must be LOW at boot (strapping pin — affects flash voltage) |
| GPIO15 | Must be HIGH at boot (enables boot log on UART0) |

---

## Development Environment

- **Framework:** Arduino (ESP32 Arduino Core) or ESP-IDF
- **Board target:** `esp32` → `ESP32 Dev Module` (or Lonely Binary if available in BSP)
- **Upload speed:** 921600
- **Flash frequency:** 80 MHz
- **Flash mode:** DIO
- **Partition scheme:** Default 4MB

---

## WS2812B Quick-Start (Arduino)

```cpp
#include <Adafruit_NeoPixel.h>

#define LED_PIN   2
#define LED_COUNT 1

Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

void setup() {
  strip.begin();
  strip.show(); // initialize to off
}

void loop() {
  strip.setPixelColor(0, strip.Color(0, 150, 0)); // green
  strip.show();
  delay(500);
  strip.clear();
  strip.show();
  delay(500);
}
```

> Or use the FastLED library with `FastLED.addLeds<WS2812B, 2, GRB>(leds, 1);`
