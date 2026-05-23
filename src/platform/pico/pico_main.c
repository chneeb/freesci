/* pico_main.c — FreeSCI entry point for PicoCalc (RP2350).
   Initialises hardware, presents the SD card game chooser, then runs FreeSCI. */

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "pico_sdcard.h"
#include "kbd_input.h"
#include "audio/pwm_synth.h"
#include "lcdspi.h"
#include "psram_alloc.h"
#include "psram/psram_spi.h"

extern psram_spi_inst_t g_psram;
#include <stdio.h>
#include <string.h>
#include <malloc.h>

#define MEMPRINT(label) do { \
    struct mallinfo _mi = mallinfo(); \
    printf("[mem] %s: free=%d arena=%d used=%d\n", \
           (label), _mi.fordblks, _mi.arena, _mi.uordblks); \
} while(0)

/* FreeSCI's main(), renamed under HAVE_PICO */
int freesci_main(int argc, char **argv);

int main(void)
{
    set_sys_clock_khz(133000, true);
    stdio_init_all();
    /* Give the USB host time to enumerate the CDC device before we print */
    sleep_ms(2000);
    MEMPRINT("after stdio_init");

    /* Initialise display and keyboard unconditionally */
    lcd_init();
    MEMPRINT("after lcd_init");
    kbd_input_init();
    MEMPRINT("after kbd_init");

    if (!pico_sd_card_init()) {
        lcd_clear();
        lcd_print_string("SD card init failed!\nHalting.");
        printf("SD card init FAILED\n");
        while (1) tight_loop_contents();
    }

    MEMPRINT("after sd_init");

    /* PWM audio on GPIO 26/27 (PicoCalc standard) */
    pwm_synth_init(26);
    MEMPRINT("after pwm_init");

    /* PSRAM on PIO1 (CS=20, SCK=21, MOSI=2, MISO=3) */
    g_psram = psram_spi_init_clkdiv(pio1, -1, 1.0f, true);
    MEMPRINT("after psram_init");

    /* PSRAM smoke test: write a pattern and read it back */
    {
        static const uint8_t wr[8] = {0xDE,0xAD,0xBE,0xEF,0x01,0x23,0x45,0x67};
        uint8_t rd[8] = {0};
        psram_store(0, wr, 8);
        psram_load(0, rd, 8);
        if (memcmp(wr, rd, 8) == 0) {
            printf("[psram] OK\n");
        } else {
            printf("[psram] FAIL: got %02x %02x %02x %02x %02x %02x %02x %02x\n",
                   rd[0],rd[1],rd[2],rd[3],rd[4],rd[5],rd[6],rd[7]);
            lcd_clear();
            lcd_print_string("PSRAM test FAILED!\nCheck SPI wiring.");
            while (1) tight_loop_contents();
        }
    }

    while (1) {
        char game_dir[64];

        MEMPRINT("before chooser");
        if (!pico_show_dir_chooser(game_dir, sizeof(game_dir)))
            continue;  /* ESC pressed — re-show chooser */

        MEMPRINT("after chooser");
        printf("Selected: %s\n", game_dir);

        /* Use short options: -d gamedir, -g graphics, -q no-sound.
           HAVE_GETOPT_LONG is not enabled for the pico build so
           main.c falls back to plain getopt which needs short forms. */
        char *argv[] = {
            "freesci",
            "-d", game_dir,
            "-g", "pico",
            "-q",
            NULL
        };
        int argc = 6;

        {
            MEMPRINT("pre-launch");
        }
        printf("Launching freesci_main\n");
        freesci_main(argc, argv);
        printf("freesci_main returned\n");
        /* After the game exits, loop back to the chooser */
    }
}
