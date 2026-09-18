/* pico_main.c — FreeSCI entry point for PicoCalc (RP2350).
   Initialises hardware, presents the SD card game chooser, then runs FreeSCI. */

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#ifdef PICO_REBOOT_BETWEEN_GAMES
#include "hardware/watchdog.h"
#endif
#include "pico_sdcard.h"
#include "kbd_input.h"
#ifdef PICO_PWM_AUDIO
#include "audio/pwm_synth.h"
#endif
#include "lcdspi.h"
#include "psram_alloc.h"
#ifdef PICO_PSRAM_MAPPED
#include "psram_qmi.h"
#include "psram_heap.h"
#else
#include "psram/psram_spi.h"
extern psram_spi_inst_t g_psram;
#endif
#include <stdio.h>
#include <string.h>
#include <malloc.h>

#ifdef PICO_PSRAM_MAPPED
/* Mapped-PSRAM build: show the SRAM heap AND the PSRAM heap (clone/node/list/hunk
   tables now live there). psheap used should climb during play while SRAM used
   stays lower than the PicoCalc build at the same point. */
#define MEMPRINT(label) do { \
    struct mallinfo _mi = mallinfo(); \
    printf("[mem] %s: free=%d arena=%d used=%d | psheap used=%u maxfree=%u\n", \
           (label), _mi.fordblks, _mi.arena, _mi.uordblks, \
           (unsigned)psram_heap_used(), (unsigned)psram_heap_free_largest()); \
} while(0)
#else
#define MEMPRINT(label) do { \
    struct mallinfo _mi = mallinfo(); \
    printf("[mem] %s: free=%d arena=%d used=%d\n", \
           (label), _mi.fordblks, _mi.arena, _mi.uordblks); \
} while(0)
#endif

/* FreeSCI's main(), renamed under HAVE_PICO */
int freesci_main(int argc, char **argv);
#ifdef PICO_PACK_VOCAB
void pico_reset_resident_vocab(void);  /* game.c */
#endif
void pico_reset_decode_scratches(void);  /* operations.c */
void pico_reset_decrypt_scratch(void);   /* decompress0.c */
void pico_reset_said_scratch(void);      /* said.c */
void census_dump_sites(void);            /* pico_mem_census.c (no-op when OFF) */

/* ---- HardFault diagnostics (RP2350 / Cortex-M33) ---------------------- */
/* The RP2350 has no MMU, so a wild pointer doesn't fault at the access — but a
   bad address into a reserved/peripheral region, an unaligned/stacking fault,
   or a stack overflow raises a HardFault.  Without a handler the chip just goes
   silent (the symptom we keep seeing when SQ3's ego lands).  This dumps the
   faulting PC/LR and the M33 fault-status registers over USB serial so a crash
   prints WHERE it died instead of vanishing.  isr_hardfault is a weak symbol in
   the pico-sdk crt0; defining it here overrides the default spin. */
/* Fault-context-safe hex writer: append "label=0xXXXXXXXX\n" to buf.
   No malloc, no printf reentrancy — just manual nibble formatting so it is
   safe to call from inside the HardFault handler. */
static char *fault_hex(char *p, const char *label, uint32_t v)
{
    static const char hx[] = "0123456789abcdef";
    while (*label) *p++ = *label++;
    *p++ = '='; *p++ = '0'; *p++ = 'x';
    for (int i = 28; i >= 0; i -= 4)
        *p++ = hx[(v >> i) & 0xf];
    *p++ = '\n';
    return p;
}

void hardfault_handler_c(uint32_t *frame)
{
    volatile uint32_t *CFSR  = (volatile uint32_t *)0xE000ED28;
    volatile uint32_t *HFSR  = (volatile uint32_t *)0xE000ED2C;
    volatile uint32_t *MMFAR = (volatile uint32_t *)0xE000ED34;
    volatile uint32_t *BFAR  = (volatile uint32_t *)0xE000ED38;

    /* USB-CDC can't transmit from fault context (TinyUSB task is starved), so
       the screen is the only reliable channel.  Build a short report with the
       manual hex writer and push it to the ILI9488 over synchronous SPI. */
    char buf[256];
    char *p = buf;
    *p++ = '\n';
    p = fault_hex(p, "HardFault PC", frame[6]);
    p = fault_hex(p, "LR  ", frame[5]);
    p = fault_hex(p, "xPSR", frame[7]);
    p = fault_hex(p, "CFSR", *CFSR);
    p = fault_hex(p, "HFSR", *HFSR);
    p = fault_hex(p, "MMFAR", *MMFAR);
    p = fault_hex(p, "BFAR", *BFAR);
    *p = '\0';

    lcd_clear();
    lcd_print_string(buf);

    /* Also try USB (harmless; the leading \n may flush, the rest likely won't) */
    printf("[FAULT]%s", buf);
    stdio_flush();
    while (1) tight_loop_contents();
}

/* Out-of-memory reporter.  PICO_MALLOC_PANIC=0 makes the pico-sdk malloc wrapper
   return NULL on exhaustion instead of panic()ing with a message we can't see; the
   sci_malloc/calloc/realloc wrappers (sci_memory.c) call this on a NULL result so
   the failing allocation's size + site + remaining free heap land on the LCD, the
   only channel that works once the heap is gone.  Same fault-safe SPI path as the
   HardFault handler; halts afterwards. */
void pico_oom_report(const char *what, unsigned long size,
                     const char *file, int line, const char *funct)
{
    struct mallinfo mi = mallinfo();

    char buf[256];
    char *p = buf;
    *p++ = '\n';
    while (*what) *p++ = *what++;
    *p++ = '\n';
    p = fault_hex(p, "size", (uint32_t)size);
    p = fault_hex(p, "free", (uint32_t)mi.fordblks);
    p = fault_hex(p, "arena", (uint32_t)mi.arena);
    p = fault_hex(p, "line", (uint32_t)line);
    /* trailing path component of file, then funct, on their own lines */
    {
        const char *base = file, *q = file;
        while (*q) { if (*q == '/') base = q + 1; q++; }
        while (*base) *p++ = *base++;
        *p++ = '\n';
        while (*funct) *p++ = *funct++;
        *p++ = '\n';
    }
    *p = '\0';

    lcd_clear();
    lcd_print_string(buf);

    printf("[OOM]%s", buf);
    stdio_flush();
    while (1) tight_loop_contents();
}

void __attribute__((naked)) isr_hardfault(void)
{
    __asm volatile(
        "movs r0, #4                   \n" /* EXC_RETURN bit2: which stack? */
        "mov  r1, lr                   \n"
        "tst  r0, r1                   \n"
        "beq  1f                       \n"
        "mrs  r0, psp                  \n" /* faulted in thread/PSP context */
        "b    2f                       \n"
        "1:                            \n"
        "mrs  r0, msp                  \n" /* faulted in handler/MSP context */
        "2:                            \n"
        "ldr  r1, =hardfault_handler_c \n"
        "bx   r1                       \n"
    );
}

int main(void)
{
    /* Core clock. SCI loading is dominated by decompression, resource decode and
       the interpreter loop -- all CPU-bound -- so this scales load times almost
       directly. Default stays the RP2350 power-on 133 MHz; PICO_SYS_CLOCK_MHZ
       raises it.

       ORDER MATTERS and all of it must precede stdio_init_all(), because USB has
       to enumerate at the FINAL clock.

       THE TRAP: psram_set_flash_timings() is parameterised BY the system clock.
       Raising clk_sys without recomputing the flash divisor for the new clock
       overclocks the flash and corrupts XIP reads -- the exact failure the
       Pimoroni bring-up hit (TFT noise, no serial, dead before any output). So
       the target is passed to BOTH calls, never hard-coded.

       PSRAM needs no equivalent here: psram_qmi_init derives its own divisor
       from clock_get_hz(clk_sys) (psram_mapped.c), so it re-caps itself at the
       new clock. That matters more on this target than it looks -- the engine's
       hot VM data lives in PSRAM, so a timing error would corrupt running
       scripts and objects, not merely graphics.
       NB the PicoCalc PIO target does NOT have that property: it passes a
       HARD-CODED clkdiv of 1.0 to psram_spi_init_clkdiv() below, i.e. the PIO
       SPI runs at full clk_sys. Raising the clock there without scaling that
       divisor overclocks the PSRAM chip. Fix that before overclocking PIO. */
#ifndef PICO_SYS_CLOCK_MHZ
#  define PICO_SYS_CLOCK_MHZ 133
#endif
#ifndef PICO_PSRAM_SM_MHZ
/* Target PIO state-machine clock for the PSRAM SPI; the SPI rate is half it.
   133 keeps the long-proven 66.5 MHz point at a 133 MHz core. Raise it WITH the
   core clock -- pico-286's device-verified PicoCalc point at 396 MHz is an SM
   clock of 198 (SPI 99 MHz), soak-tested at 0 errors. Not independently
   tunable from the core clock: the sampling phase depends on both. */
#  define PICO_PSRAM_SM_MHZ 133
#endif
#ifndef PICO_SD_SPI_KHZ
#  define PICO_SD_SPI_KHZ 12500   /* mirrors hw_config.c's default */
#endif
#ifndef PICO_FLASH_MAX_MHZ
/* Cap the flash clock regardless of the core clock. pico-286 ships its 396 MHz
   PicoCalc build as "F100", i.e. flash held at 100 MHz under a 3x core -- so
   100 is a device-proven value on this hardware. The mapped target uses 66,
   which is more conservative and already works there. */
#  define PICO_FLASH_MAX_MHZ 100
#endif
#ifdef PICO_PSRAM_MAPPED
    psram_set_flash_timings(PICO_SYS_CLOCK_MHZ, 66);
#else
    /* PIO: recompute flash timing BEFORE raising the clock, or XIP reads go
       corrupt on the way up. Omitting this is why 252 MHz was "ruled out". */
    pico_set_flash_timings(PICO_SYS_CLOCK_MHZ, PICO_FLASH_MAX_MHZ);
#endif
#if PICO_SYS_CLOCK_MHZ > 250
    /* Above ~250 MHz the core needs more than the default 1.10 V. Raise it
       BEFORE set_sys_clock_khz, and give the rail a moment to settle --
       set_sys_clock_khz simply refuses (returns false) if the PLL cannot be
       configured, but an under-volted core that DOES configure fails as random
       corruption instead, which is far harder to read. */
    vreg_set_voltage(VREG_VOLTAGE_1_30);
    sleep_ms(10);
#endif
    if (!set_sys_clock_khz(PICO_SYS_CLOCK_MHZ * 1000, false)) {
        /* Refused (bad PLL divisors, or it needs a voltage bump we do not wire
           up): fall back rather than run on at whatever clk_sys happens to be.
           Deliberately do NOT recompute the flash timings for 133 -- leaving
           them as computed for the higher target only makes flash SLOWER than
           it needs to be here, never faster than its cap, which is the safe
           direction. (Adopted from frank-quest's fallback.) */
        set_sys_clock_khz(133000, true);
    }

    /* PIN clk_peri -- the SDK ties it to clk_sys UNDIVIDED
       ("CLK PERI = clk_sys. Used as reference clock for UART and SPI serial",
       runtime_init_clocks.c), so raising the core to 396 MHz also clocks the
       UART and BOTH SPI peripherals -- SD card and LCD -- at 396 MHz, far past
       spec. Device-observed at 396: serial died immediately after the [clk]
       print, and SQ3 then HardFaulted on launch, which is what corrupt SD reads
       feeding the resource loader look like.
       Pinning it at 133 MHz keeps every peripheral at exactly the reference the
       proven 133 MHz build uses, so the existing PICO_SD_SPI_KHZ /
       PICO_LCD_SPI_KHZ divisors stay valid. MUST precede stdio_init_all (UART)
       and the lcd/sd init below. */
    if (clock_get_hz(clk_sys) > 133000000u)
        clock_configure(clk_peri, 0,
                        CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS,
                        clock_get_hz(clk_sys), 133000000u);

    stdio_init_all();
    /* Give the USB host time to enumerate the CDC device before we print */
    sleep_ms(2000);
    printf("[clk] sys_clk = %u Hz (requested %d MHz)\n",
           (unsigned)clock_get_hz(clk_sys), PICO_SYS_CLOCK_MHZ);
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

#ifdef PICO_PWM_AUDIO
    /* PWM audio on GPIO 26/27 (PicoCalc standard) */
    pwm_synth_init(26);
    MEMPRINT("after pwm_init");
#endif

#ifdef PICO_PSRAM_MAPPED
    /* Memory-mapped QMI PSRAM (Pimoroni Pico Plus 2, APS6404 on CS1 GPIO 47). */
    {
        size_t psram_bytes = psram_qmi_init(PSRAM_CS_PIN);
        printf("[psram] mapped QMI init: %u bytes detected\n", (unsigned)psram_bytes);
        if (psram_bytes == 0) {
            lcd_clear();
            lcd_print_string("PSRAM not detected!\n(QMI CS1)");
            while (1) tight_loop_contents();
        }
    }
#else
    /* PSRAM on PIO1 (CS=20, SCK=21, MOSI=2, MISO=3) */
    {
        /* The PIO state machine runs at clk_sys/clkdiv, so a hard-coded 1.0
           makes the PSRAM SPI rate scale with the core clock. At 133 MHz that is
           the proven rate; overclocking with 1.0 pushes the PSRAM past it and the
           boot smoke test below fails ("PSRAM test failed!") -- observed on the
           PicoCalc at 252 MHz.

           NB the driver header's "clkdiv >1.0 needed above 280 MHz" is RP2040
           guidance and does NOT transfer to the RP2350 + this PCB/PSRAM.

           So derive the divisor to hold the SPI at its 133 MHz-equivalent rate.
           Consequence worth knowing: PSRAM does NOT get faster with the
           overclock -- only CPU-bound work does. */
        /* TARGET AN SM CLOCK, DO NOT HOLD THE SPI RATE CONSTANT.
           Holding the SPI rate was the obvious move and it FAILED at 396 MHz
           (clkdiv 3 -> SPI 66 MHz, the same rate that works at 133): the boot
           smoke test still reported "PSRAM test FAILED".

           Why holding the rate is not enough: the PIO input synchronizer is
           clocked by clk_sys, NOT the SM clock, so its 2-cycle latency is ~15ns
           at 133 MHz but ~5ns at 396 MHz. MISO therefore arrives ~10ns earlier
           relative to the sampling edge -- most of a bit period at 66 MHz SPI.
           The SAMPLING PHASE moves with the system clock even at a fixed SPI
           rate, which is exactly why pico-286 reports this failing at both
           faster AND slower settings and ships a sweep to find the point.

           So the divisor is derived from a target SM clock instead:
             133 MHz sys, target 133 -> clkdiv 1 -> SPI 66.5  (our proven point)
             396 MHz sys, target 198 -> clkdiv 2 -> SPI 99    (pico-286's, which
                                       is device-verified on PicoCalc hardware
                                       at 396 MHz: soak-tested 0 errors, ~5 MB/s)
           Round to NEAREST so the SM clock lands near the target rather than
           systematically under it; integer only, because the PIO fractional
           divider stretches individual cycles and a bit-banged protocol with
           tight setup/hold does not tolerate the jitter. */
        /* Derive from the ACHIEVED clock, never the requested one. If
           set_sys_clock_khz refused the target it silently fell back to 133,
           and a divisor computed for 396 would then put the SPI at 44/2 =
           22 MHz -- far below the proven point and on the wrong side of the
           fudge cycle, i.e. a dead bus that looks exactly like an overclock
           failure. This is the trap the [clk] line exists to expose ("trust
           this, not CMakeCache"), and the mapped target already avoids it by
           deriving from clock_get_hz. */
        int achieved_mhz = (int)(clock_get_hz(clk_sys) / 1000000u);
        int psram_div_i = (achieved_mhz + PICO_PSRAM_SM_MHZ / 2) / PICO_PSRAM_SM_MHZ;
        float psram_clkdiv;
        if (psram_div_i < 1)
            psram_div_i = 1;
        psram_clkdiv = (float)psram_div_i;
        printf("[psram] PIO clkdiv %.1f from achieved %d MHz -> SPI %.1f MHz\n",
               (double)psram_clkdiv, achieved_mhz,
               (double)achieved_mhz / psram_clkdiv / 2.0);
        g_psram = psram_spi_init_clkdiv(pio1, -1, psram_clkdiv, true);
    }
#endif
    MEMPRINT("after psram_init");

    /* PSRAM smoke test.  A wrong PSRAM setup can pass a tiny read-back yet corrupt
       under load, so sweep a patterned block across several offsets that cross
       chunk (PIO) and 1024-byte page (QMI) boundaries and exercise high address
       bits — without a big permanent buffer (256B on the stack, freed after; the
       shared code must not cost the memory-tight PicoCalc any .bss). */
    {
        enum { SMOKE_LEN = 256 };
        static const uint32_t offsets[] = { 0u, 900u, 0x100000u };  /* .rodata */
        uint8_t buf[SMOKE_LEN];
        int ok = 1;
        for (unsigned o = 0; o < sizeof(offsets)/sizeof(offsets[0]) && ok; o++) {
            for (int i = 0; i < SMOKE_LEN; i++)
                buf[i] = (uint8_t)(i * 31 + 7 + offsets[o]);   /* offset-tagged */
            psram_store(offsets[o], buf, SMOKE_LEN);
        }
        for (unsigned o = 0; o < sizeof(offsets)/sizeof(offsets[0]) && ok; o++) {
            memset(buf, 0, SMOKE_LEN);
            psram_load(offsets[o], buf, SMOKE_LEN);
            for (int i = 0; i < SMOKE_LEN && ok; i++)
                if (buf[i] != (uint8_t)(i * 31 + 7 + offsets[o])) ok = 0;
        }
        if (ok) {
            printf("[psram] OK (pattern sweep across %u offsets)\n",
                   (unsigned)(sizeof(offsets)/sizeof(offsets[0])));
        } else {
            printf("[psram] FAIL: pattern mismatch\n");
            lcd_clear();
            lcd_print_string("PSRAM test FAILED!");
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

        /* Tell the user what's happening while resources load -- this is the
           long, screen-blank phase. The message survives until graphics init
           clears the LCD and the first room draws (no LCD writes happen during
           resource loading). */
        {
            const char *name = game_dir, *q = game_dir;
            while (*q) { if (*q == '/') name = q + 1; q++; }
            char msg[96];
            snprintf(msg, sizeof(msg), "Loading %s...\nPlease wait.", name);
            lcd_clear();
            lcd_print_string(msg);
        }

        /* Use short options: -d gamedir, -g graphics, -q no-sound.
           HAVE_GETOPT_LONG is not enabled for the pico build so
           main.c falls back to plain getopt which needs short forms.
           When PICO_PWM_AUDIO is ON, drop -q so the SCI sound pipeline
           runs and feeds the PWM PCM device. */
#ifdef PICO_PWM_AUDIO
        /* One spare slot: the chooser's [S] toggle may append -q below, and a
           bare initialiser list would size this exactly, making that append a
           one-past-the-end write. */
        char *argv[7] = {
            "freesci",
            "-d", game_dir,
            "-g", "pico",
            NULL
        };
        int argc = 5;
#else
        char *argv[] = {
            "freesci",
            "-d", game_dir,
            "-g", "pico",
            "-q",
            NULL
        };
        int argc = 6;
#endif
#ifdef PICO_PWM_AUDIO
        /* Sound builds normally omit -q; the chooser's [S] toggle puts it back
           so one uf2 serves games that fit with sound and games that do not. */
        {
            extern int pico_sound_enabled;

            if (!pico_sound_enabled) {
                argv[argc++] = "-q";
                argv[argc] = NULL;
            }
            printf("[snd] launching with sound %s\n",
                   pico_sound_enabled ? "ON" : "OFF");
        }
#endif

        {
            MEMPRINT("pre-launch");
        }
        /* Report the achieved clock HERE, not just at boot: with uf2loader in
           the picture the serial cable cannot be attached early enough to see
           anything printed before the chooser. clock_get_hz is the ACHIEVED
           value, so this also reveals a silent fallback when
           set_sys_clock_khz() refused the requested frequency. */
        printf("[clk] clk_peri = %u Hz (pinned; SD/LCD SPI + UART reference)\n",
           (unsigned)clock_get_hz(clk_peri));
    printf("[clk] sys_clk = %u Hz (requested %d MHz)%s\n",
               (unsigned)clock_get_hz(clk_sys), PICO_SYS_CLOCK_MHZ,
               (clock_get_hz(clk_sys) / 1000000u) == (unsigned)PICO_SYS_CLOCK_MHZ
                 ? "" : "  <-- FELL BACK, requested clock not achieved");
        printf("[clk] SD SPI = %d kHz\n", PICO_SD_SPI_KHZ);
#ifdef PICO_STATIC_COMPOSED
        {
            extern int pico_composed_enabled;
            printf("[gfx] composed surface %s\n",
                   pico_composed_enabled ? "ON" : "OFF");
        }
#endif
#if (defined(PICO_STATIC_VIEW_PRIORITY) || defined(PICO_STATIC_VIEW_BAKE)) \
    && !defined(PICO_WORKING_PRIORITY)
        {
            extern int pico_static_view_priority_enabled;
            printf("[gfx] static view priority %s\n",
                   pico_static_view_priority_enabled ? "ON" : "OFF");
        }
#endif
        printf("Launching freesci_main\n");
        freesci_main(argc, argv);
        printf("freesci_main returned\n");
#ifdef PICO_PACK_VOCAB
        /* Drop the resident packed vocab so the next (possibly different) game
           re-packs its own; kept resident only across in-game restores. */
        pico_reset_resident_vocab();
#endif
        /* Reset the arena before the next game.  The prior game ratcheted the
           picolibc break to the physical ceiling (it never returns sbrk'd memory
           on its own), so without this the next game inherits a maxed, fragmented
           arena and OOMs on a routine alloc despite plenty of total free space
           (e.g. loading PQ2 right after SQ3).  Free the permanent decode scratches
           (which otherwise pin the break high), then malloc_trim releases the now-
           free top of the heap via sbrk, so the next game grows from a low arena —
           a cold-boot heap without the power cycle.

           Measured (FSCI_PROBE_MEM, SQ3 -> chooser): the trim DOES work
           (arena 413,324 -> 167,564), but 22,508 bytes stayed live where a cold
           boot has 4 — and malloc_trim can only release the top free chunk, so
           whatever survives sets the next game's floor.  The two lazy
           "allocate once, never free" scratches below are ~20.9KB of that:
           decrypt1's LZW token tables (16,384) and said's parse scratch
           (~4,512).  Both are genuinely game-independent WITHIN a game, which
           is why they were never freed; across the chooser they are just
           inherited ballast. */
        pico_reset_decode_scratches();
        pico_reset_decrypt_scratch();
        pico_reset_said_scratch();
        malloc_trim(0);
        MEMPRINT("post-trim");
        /* Whatever `used` still reports here is the residual cross-game floor.
           On a census build, name it rather than guess: the histogram gives the
           size classes and the SITES line the call sites. */
        census_dump_sites();
#ifdef PICO_REBOOT_BETWEEN_GAMES
        /* The trim above is best-effort and structurally cannot guarantee a low
           floor: it only releases the TOP free chunk, so one surviving block in
           an unlucky spot pins the arena regardless of how small it is.  Reboot
           instead — the chooser is where this loop was headed anyway, so the
           destination is unchanged and the next game starts on a genuinely cold
           heap.  (This is the power-cycle workaround, automated.)  The resets
           above are deliberately kept: they are correct in their own right, and
           they are what the OFF path relies on. */
        printf("[mem] rebooting into the chooser for a cold heap\n");
        stdio_flush();
        watchdog_reboot(0, 0, 0);
        for (;;)
            ;
#endif
        /* After the game exits, loop back to the chooser */
    }
}
