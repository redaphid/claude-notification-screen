// The leader's screen: 412x412 round SPD2010 over QSPI.
//
// Pins and reset behaviour come from Waveshare's own driver for this board
// (waveshareteam/ESP32-S3-Touch-LCD-1.46, Display_SPD2010.h), not from guesses.
#pragma once

#include <stdint.h>

// QSPI display -- ESP_PANEL_LCD_SPI_IO_* in Waveshare's header.
#define LCD_PIN_SCK 40
#define LCD_PIN_D0 46
#define LCD_PIN_D1 45
#define LCD_PIN_D2 42
#define LCD_PIN_D3 41
#define LCD_PIN_CS 21
#define LCD_PIN_TE 18
#define LCD_PIN_BACKLIGHT 5
#define LCD_SPI_MODE 3
#define LCD_SPI_HZ (40 * 1000 * 1000)
#define LCD_W 412
#define LCD_H 412

// Reset is NOT a GPIO on this board: it hangs off a TCA9554 I2C expander, which
// is why the panel stays dark and silent if you only drive pins. Expander pin 2
// (bit 1 of the output register), low then high.
#define EXPANDER_I2C_ADDR 0x20
#define EXPANDER_I2C_SDA 11
#define EXPANDER_I2C_SCL 10
#define EXPANDER_OUTPUT_REG 0x01
#define EXPANDER_CONFIG_REG 0x03
#define EXPANDER_LCD_RESET_PIN 2

// Brings up I2C, releases the panel from reset, starts the display, and clears
// it. Returns false if the expander never acknowledges -- which is the tell-tale
// for "this is not the 1.46" rather than "the display is broken".
bool conductorDisplayInit();

// Publishes what this board is currently hearing. Returns immediately: the
// actual drawing happens on the other core, so the audio path never waits on
// the panel. Safe to call at whatever rate the analysis loop runs.
// How long the last frame actually took to transform and push. The panel is
// 412x412 over QSPI and the effects render at 240x240, so the scale-up is not
// free -- and this loop also has to feed a microphone.
uint32_t conductorDisplayLastDrawUs();
uint32_t conductorDisplayFps();

void conductorDisplayDraw(const float features[4], uint8_t beat, float beatEnv,
                          uint32_t txCount, uint32_t analysisFps);
