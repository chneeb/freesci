/* Flash-resident OPL2 lookup tables for FreeSCI on Pico.
   Definitions live in the GENERATED fmopl_tables.c (see its header for the
   regen command). These are pure functions of EG_ENT/SIN_ENT (pow/log10/sin),
   sample-rate-independent, so they can be const .rodata (XIP flash on RP2350)
   at zero SRAM cost. SIN_TABLE holds integer OFFSETS into TL_TABLE, so a
   wavetable sample is TL_TABLE[ wavetable[idx] + env ]. */
#ifndef FMOPL_TABLES_H
#define FMOPL_TABLES_H

/* Must match the resolution baked into fmopl_tables.c (HQ). */
#define FMOPL_FLASH_ENV_BITS 16
#define FMOPL_FLASH_EG_ENT   4096
#define FMOPL_FLASH_SIN_ENT  2048
#define FMOPL_FLASH_AMS_ENT  512
#define FMOPL_FLASH_VIB_ENT  512

extern const int fmopl_TL_TABLE[16384];
extern const int fmopl_SIN_TABLE[8192];
extern const int fmopl_AMS_TABLE[1024];
extern const int fmopl_VIB_TABLE[1024];
extern const int fmopl_ENV_CURVE[8193];

#endif /* FMOPL_TABLES_H */
