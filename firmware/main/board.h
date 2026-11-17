#pragma once

// Распиновка платы Давида. Описание проводов: docs/WIRING.md

#include "driver/gpio.h"

// Экран ST7789 1.54" 240x240, SPI
#define PIN_LCD_SCLK GPIO_NUM_21
#define PIN_LCD_MOSI GPIO_NUM_47
#define PIN_LCD_RST  GPIO_NUM_45
#define PIN_LCD_DC   GPIO_NUM_40
#define PIN_LCD_CS   GPIO_NUM_41
#define PIN_LCD_BL   GPIO_NUM_42

// Микрофон INMP441, I2S (L/R на GND, значит левый канал)
#define PIN_MIC_SCK  GPIO_NUM_5
#define PIN_MIC_WS   GPIO_NUM_4
#define PIN_MIC_SD   GPIO_NUM_6

// Усилитель MAX98357A, I2S
#define PIN_SPK_BCLK GPIO_NUM_15
#define PIN_SPK_LRC  GPIO_NUM_16
#define PIN_SPK_DIN  GPIO_NUM_7

// На самой плате
#define PIN_LED      GPIO_NUM_48  // встроенный RGB-светодиод (WS2812)
#define PIN_BOOT_BTN GPIO_NUM_0   // кнопка BOOT, нажата = 0
