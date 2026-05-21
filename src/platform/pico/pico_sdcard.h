#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

bool    pico_sd_card_init(void);
bool    pico_show_dir_chooser(char *out_path, size_t len);

uint8_t *pico_sd_read_file(const char *path, size_t *out_size);
void     pico_sd_free_file(uint8_t *buf);
size_t   pico_sd_read_file_at(const char *path, size_t offset, void *buf, size_t len);

bool    pico_sd_save_open(const char *path, bool write);
bool    pico_sd_save_write(const void *buf, size_t len);
bool    pico_sd_save_read(void *buf, size_t len);
void    pico_sd_save_close(void);
