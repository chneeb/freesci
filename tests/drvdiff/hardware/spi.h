#pragma once
#include <stdint.h>
void spi_set_baudrate(void *, unsigned);

typedef struct { int dummy; } spi_inst_t_stub;
#define spi1 ((void *)0)
void spi_finish(void *);
