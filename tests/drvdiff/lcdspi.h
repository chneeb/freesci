#pragma once
#include <stdint.h>
#define TFT_Y_OFFSET 0
void lcd_init(void);
void lcd_spi_raise_cs(void);
void define_region_spi(int x1, int y1, int x2, int y2, int set);
void hw_send_spi(const uint8_t *buf, int len);
