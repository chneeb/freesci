/* Memory-mapped PSRAM backend for the Pimoroni Pico Plus 2 (RP2350B) target.
 *
 * This replaces the PicoCalc's PIO-SPI PSRAM driver (psram_alloc.c +
 * psram/psram_spi.c).  It implements the SAME four-function offset-based API
 * (psram_alloc/reset/store/load) that every offload site already calls, so the
 * rest of the port is unchanged — the only difference is that here the PSRAM is
 * addressable, so store/load are memcpy against the CS1 XIP window instead of
 * PIO DMA transfers.
 *
 * Selected by CMake -DPICO_PSRAM_MAPPED=ON (with -DPICO_BOARD=
 * pimoroni_pico_plus2_rp2350).  The PicoCalc target does not compile this file.
 *
 * psram_qmi_init() is the standard RP2350 APS6404 bring-up sequence: direct-mode
 * ID read (size) -> enter QPI -> program CS1 read/write formats + timing -> leave
 * direct mode -> enable writes to the M1 window.  Every register/field name is
 * grounded in this pico-sdk's headers (2.2.0).
 *
 * The TIMING block (clock divisor, rxdelay, MAX_SELECT, MIN_DESELECT, COOLDOWN)
 * and the direct-mode ordering are adopted verbatim from a DEVICE-TESTED
 * reference running the same Pimoroni Pico Plus 2 + APS6404 at ~133 MHz
 * (~/Source/frank-snes drivers/psram_init.c).  Those constants are read-sampling
 * critical — do not "simplify" them.  Still device-unverified IN THIS project:
 * the ID-read size detection and the memcpy backend are ours, so the boot smoke
 * test (patterned multi-offset sweep in pico_main.c) remains the gate on first
 * flash, and running a game is the real load test.
 */
#if defined(HAVE_PICO) && defined(PICO_PSRAM_MAPPED)

#include "psram_qmi.h"
#include "psram_alloc.h"          /* the shared 4-function API signatures */
#include <string.h>

#include "hardware/regs/qmi.h"
#include "hardware/regs/xip.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/xip.h"
#include "hardware/structs/io_bank0.h"   /* GPIO_FUNC_XIP_CS1 */
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "pico/platform.h"        /* __no_inline_not_in_flash_func */

/* ---- bump allocator over the CS1 window (identical model to psram_alloc.c) ---- */
static uint32_t s_psram_offset = 0;
static size_t   s_psram_size   = 0;   /* set by psram_qmi_init(); 0 until then */

uint32_t
psram_alloc(size_t bytes)
{
    uint32_t addr = s_psram_offset;
    s_psram_offset += (uint32_t)bytes;
    return addr;
}

void
psram_reset(void)
{
    s_psram_offset = 0;
}

/* Addressable PSRAM: store/load are plain memcpy against the mapped window.
   addr is an offset from PSRAM_XIP_BASE (same offsets psram_alloc() hands out). */
void
psram_store(uint32_t addr, const uint8_t *src, size_t len)
{
    memcpy((void *)(uintptr_t)(PSRAM_XIP_BASE + addr), src, len);
}

void
psram_load(uint32_t addr, uint8_t *dst, size_t len)
{
    memcpy(dst, (const void *)(uintptr_t)(PSRAM_XIP_BASE + addr), len);
}

/* ---- Flash (CS0) timing --------------------------------------------------------
   Must run BEFORE set_sys_clock_khz() and from SRAM (it rewrites the flash QMI
   timing the executing code is fetched through).  Adopted verbatim from frank-snes
   set_flash_timings() — proven on this board.  The pico2-default flash timing works
   on a Pico 2's flash at 133 MHz but NOT on the Pimoroni Plus 2's 16 MB chip once
   the flash clock scales with the system clock. */
void
__no_inline_not_in_flash_func(psram_set_flash_timings)(int cpu_mhz, int flash_max_mhz)
{
    const int clock_hz = cpu_mhz * 1000000;
    const int max_flash_freq = flash_max_mhz * 1000000;

    int divisor = (clock_hz + max_flash_freq - (max_flash_freq >> 4) - 1) / max_flash_freq;
    if (divisor == 1 && clock_hz >= 166000000) divisor = 2;

    int rxdelay = divisor;
    if (clock_hz / divisor > 100000000 && clock_hz >= 166000000) rxdelay += 1;

    qmi_hw->m[0].timing = 0x60007000u
                          | ((uint32_t)rxdelay << QMI_M0_TIMING_RXDELAY_LSB)
                          | ((uint32_t)divisor << QMI_M0_TIMING_CLKDIV_LSB);
}

/* ---- QMI bring-up ------------------------------------------------------------
   Runs from SRAM: direct mode takes over the shared QMI, so flash fetches would
   stall mid-routine if any of this executed from XIP flash. */

static size_t
__no_inline_not_in_flash_func(psram_qmi_init_inner)(unsigned cs_pin)
{
    /* Read the system clock NOW, while XIP flash still works.  Direct mode below
       takes over the whole QMI (flash CS0 included), so from that point until we
       leave direct mode we must NOT touch flash — no flash-resident function calls
       (clock_get_hz lives in flash) and no .rodata reads — or the CPU fetches
       garbage and crashes.  This is why the timing math uses this captured value
       instead of calling clock_get_hz() mid-sequence (matches frank-snes). */
    const int clock_hz = (int)clock_get_hz(clk_sys);

    /* Compute ALL timing values here, before direct mode, while flash is alive.
       The 64-bit division below pulls in a libgcc helper (__aeabi_ldivmod) that
       normally lives in flash; doing the math now means the only thing left to do
       inside direct mode is plain hardware-register writes (no flash touch).
       Values adopted from a device-tested reference on this exact board
       (~/Source/frank-snes drivers/psram_init.c, APS6404 @ ~133 MHz) — rxdelay and
       min_deselect are read-sampling-critical; do NOT "simplify" them. */
    const int max_psram_hz = 133 * 1000000;
    int divisor = (clock_hz + max_psram_hz - 1) / max_psram_hz;
    if (divisor == 1 && clock_hz > 100000000) divisor = 2;
    int rxdelay = divisor;
    if (clock_hz / divisor > 100000000) rxdelay += 1;
    const int clock_period_fs = 1000000000000000ll / clock_hz;
    const int max_select   = (125 * 1000000) / clock_period_fs;          /* ~8us tCEM */
    const int min_deselect = (18 * 1000000 + (clock_period_fs - 1)) / clock_period_fs
                             - (divisor + 1) / 2;                        /* ~18ns tCPH */

    /* Route the chosen GPIO to the QMI second chip-select. */
    gpio_set_function(cs_pin, GPIO_FUNC_XIP_CS1);

    uint32_t intr = save_and_disable_interrupts();

    /* Direct mode with HARDWARE-managed chip-select (AUTO_CS1N) — the device-tested
       frank-snes sequence (~/Source/frank-snes drivers/psram_init.c), which is what
       actually works on this exact board.  The previous manual-ASSERT_CS1N toggling
       + a custom 0x9F ID-read left the QMI/PSRAM in a bad state once a real chip
       responded (TFT noise on device); dropped entirely.  No runtime size probe —
       the Pimoroni Plus 2 is known 8 MB and the boot smoke test is the real
       presence/integrity check. */
    qmi_hw->direct_csr = 10u << QMI_DIRECT_CSR_CLKDIV_LSB
                         | QMI_DIRECT_CSR_EN_BITS
                         | QMI_DIRECT_CSR_AUTO_CS1N_BITS;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {}

    /* Exit QPI (0xF5, quad) first, in case a prior firmware left the chip in QPI:
       a BOOTSEL/watchdog reset does NOT power-cycle the PSRAM, and our dev workflow
       reflashes repeatedly, so the chip can survive in QPI across a reset.  Harmless
       if it is already in SPI.  NOPUSH = no read phase; OE + quad IWIDTH so the
       command reaches a chip that IS in QPI.  (frank omits this — it assumes a cold
       start — but we keep it for reflash robustness.)  Then enter QPI (0x35). */
    qmi_hw->direct_tx = QMI_DIRECT_TX_NOPUSH_BITS
                        | QMI_DIRECT_TX_OE_BITS
                        | (QMI_DIRECT_TX_IWIDTH_VALUE_Q << QMI_DIRECT_TX_IWIDTH_LSB)
                        | 0xf5u;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {}

    qmi_hw->direct_tx = QMI_DIRECT_TX_NOPUSH_BITS | 0x35u;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {}

    /* Program the CS1 formats/timing (all values precomputed above the direct-mode
       entry — only register writes here, no flash access), then drop out of direct
       mode (direct_csr = 0) so XIP drives CS1 — matches the frank-snes ordering. */
    qmi_hw->m[1].timing =
        (1u                                  << QMI_M1_TIMING_COOLDOWN_LSB) |
        (QMI_M1_TIMING_PAGEBREAK_VALUE_1024  << QMI_M1_TIMING_PAGEBREAK_LSB) |
        ((uint32_t)max_select                << QMI_M1_TIMING_MAX_SELECT_LSB) |
        ((uint32_t)min_deselect              << QMI_M1_TIMING_MIN_DESELECT_LSB) |
        ((uint32_t)rxdelay                   << QMI_M1_TIMING_RXDELAY_LSB) |
        ((uint32_t)divisor                   << QMI_M1_TIMING_CLKDIV_LSB);

    /* Read: 0xEB fast quad read, quad addr/data, 24 dummy bits (= 6 quad cyc). */
    qmi_hw->m[1].rfmt =
        (QMI_M0_RFMT_PREFIX_WIDTH_VALUE_Q << QMI_M0_RFMT_PREFIX_WIDTH_LSB) |
        (QMI_M0_RFMT_ADDR_WIDTH_VALUE_Q   << QMI_M0_RFMT_ADDR_WIDTH_LSB) |
        (QMI_M0_RFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M0_RFMT_SUFFIX_WIDTH_LSB) |
        (QMI_M0_RFMT_DUMMY_WIDTH_VALUE_Q  << QMI_M0_RFMT_DUMMY_WIDTH_LSB) |
        (QMI_M0_RFMT_DUMMY_LEN_VALUE_24   << QMI_M0_RFMT_DUMMY_LEN_LSB) |
        (QMI_M0_RFMT_DATA_WIDTH_VALUE_Q   << QMI_M0_RFMT_DATA_WIDTH_LSB) |
        (QMI_M0_RFMT_PREFIX_LEN_VALUE_8   << QMI_M0_RFMT_PREFIX_LEN_LSB);
    qmi_hw->m[1].rcmd = 0xebu;

    /* Write: 0x38 quad write, quad addr/data, no dummy. */
    qmi_hw->m[1].wfmt =
        (QMI_M0_WFMT_PREFIX_WIDTH_VALUE_Q << QMI_M0_WFMT_PREFIX_WIDTH_LSB) |
        (QMI_M0_WFMT_ADDR_WIDTH_VALUE_Q   << QMI_M0_WFMT_ADDR_WIDTH_LSB) |
        (QMI_M0_WFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M0_WFMT_SUFFIX_WIDTH_LSB) |
        (QMI_M0_WFMT_DUMMY_WIDTH_VALUE_Q  << QMI_M0_WFMT_DUMMY_WIDTH_LSB) |
        (QMI_M0_WFMT_DATA_WIDTH_VALUE_Q   << QMI_M0_WFMT_DATA_WIDTH_LSB) |
        (QMI_M0_WFMT_PREFIX_LEN_VALUE_8   << QMI_M0_WFMT_PREFIX_LEN_LSB);
    qmi_hw->m[1].wcmd = 0x38u;

    /* Leave direct mode; XIP now drives CS1 with the formats above. */
    qmi_hw->direct_csr = 0;

    /* Allow the CPU to write to the M1 (CS1) XIP window — required or store()
       silently drops writes. */
    hw_set_bits(&xip_ctrl_hw->ctrl, XIP_CTRL_WRITABLE_M1_BITS);

    restore_interrupts(intr);
    return 0x800000u;   /* Pimoroni Pico Plus 2 = 8 MiB (no runtime probe) */
}

size_t
psram_qmi_init(unsigned cs_pin)
{
    s_psram_size = psram_qmi_init_inner(cs_pin);
    s_psram_offset = 0;
    return s_psram_size;
}

#endif /* HAVE_PICO && PICO_PSRAM_MAPPED */
