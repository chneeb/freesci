#pragma once
#if defined(HAVE_PICO) && defined(PICO_PSRAM_MAPPED)

#include <stdint.h>
#include <stddef.h>

/* Memory-mapped PSRAM target (Pimoroni Pico Plus 2, RP2350B).  The onboard
   APS6404 PSRAM sits on the QMI second chip-select (CS1) and is addressable
   through the cached XIP window at 0x11000000 — unlike the PicoCalc's PIO-SPI
   PSRAM, which is store/load only.  psram_qmi_init() brings up the QMI for CS1
   and returns the detected size in bytes (0 if no PSRAM answered). */

#define PSRAM_XIP_BASE  0x11000000u

/* PSRAM chip-select GPIO on the Pimoroni Pico Plus 2 (= the board header's
   PIMORONI_PICO_PLUS2_PSRAM_CS_PIN, but we build as pico2 so that define is not
   available — GPIO 47 exists on the RP2350B silicon regardless of the pico2
   compile-time config, exactly as frank-snes relies on). */
#define PSRAM_CS_PIN  47u

/* cs_pin = PSRAM_CS_PIN (47). Returns detected size in bytes, or 0. */
size_t psram_qmi_init(unsigned cs_pin);

/* Reprogram the FLASH (CS0) QMI timing for the target CPU clock BEFORE raising the
   system clock.  The Pimoroni Pico Plus 2's 16 MB flash chip does not tolerate the
   pico2-default timing once the flash clock is scaled up to sys/divisor at 133 MHz
   (XIP reads corrupt -> early crash, TFT noise, no serial).  Call this immediately
   before set_sys_clock_khz().  Adopted from frank-snes set_flash_timings(). */
void psram_set_flash_timings(int cpu_mhz, int flash_max_mhz);

#endif /* HAVE_PICO && PICO_PSRAM_MAPPED */
