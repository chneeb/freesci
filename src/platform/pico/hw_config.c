/* FatFS hardware configuration for PicoCalc SD card.
   SD card is on SPI0: MISO=16, MOSI=19, SCK=18, CS=17.
   No card-detect pin is wired on the PicoCalc.
*/
#include <assert.h>
#include "hw_config.h"

static spi_t spis[] = {
    {
        .hw_inst = spi0,
        .miso_gpio = 16,
        .mosi_gpio = 19,
        .sck_gpio  = 18,
        .baud_rate = 12500 * 1000,
        .DMA_IRQ_num = DMA_IRQ_0,
    }
};

static sd_card_t sd_cards[] = {
    {
        .pcName          = "0:",
        .spi             = &spis[0],
        .ss_gpio         = 17,
        .use_card_detect = false,
    }
};

size_t sd_get_num()                  { return count_of(sd_cards); }
sd_card_t *sd_get_by_num(size_t num) { return num < count_of(sd_cards) ? &sd_cards[num] : NULL; }
size_t spi_get_num()                 { return count_of(spis); }
spi_t *spi_get_by_num(size_t num)    { return num < count_of(spis) ? &spis[num] : NULL; }
