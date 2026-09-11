> **STATUS (2026-09-11) — partially acted on. The table below is the ORIGINAL 2026-09-10 measurement and
> its "freesci-archive" column is now OUT OF DATE; read this block first.**
>
> | item | state |
> |---|---|
> | §1 core clock | **PARTLY DONE.** Mapped (Pimoroni) now defaults to **252 MHz**, device-confirmed; `psram_set_flash_timings` is passed the new target, and mapped QMI PSRAM re-derives its own divisor from `clk_sys` so it re-caps itself. **378 / 504 MHz with `vreg_set_voltage` is UNTRIED** — the recipe in §1 is still the thing to follow. On the **PIO** target 252 MHz is **RULED OUT**: the bit-banged PIO-SPI PSRAM fails its boot smoke test, and neither a scaled clkdiv nor an integer divisor of 2 rescued it; PIO stays at 133. |
> | SD clock | **DONE** — 12500 → **30000 kHz** on both targets (not in the original doc; it is the one loading lever the core clock does not touch). |
> | §2 16-bit colour | **TRIED, PARKED.** `PICO_LCD_16BIT` exists and is **OFF/incomplete**: only `flush_region` was converted while `pico_clear_screen_black` and ~66 `lcdspi.c` write sites still push 3 bytes/px, so the display shears. The doc's evidence that the panel accepts `0x65` is sound and is what justifies finishing it. |
> | §3 PIO+DMA SPI at 75 MHz | **NOT STARTED.** Untouched, and the largest remaining display win. |
>
> Also still valid and worth not re-deriving: the two ruled-out causes below (it is not the PSRAM bus, not
> resource caching/architecture), and the §3 warning that shapones' `SYS_CLK_FREQ / 4` ratio is meaningless
> at another clock — derive the divisor from a target frequency. We made exactly that class of mistake with
> the PSRAM clkdiv on PIO.
>
> Device-measured results and the build-system traps hit along the way are in CLAUDE.md under
> "Overclocking and SD speed".

# Pico performance: why frank-quest is faster, and what to copy

Written 2026-09-10 from a side-by-side comparison on the same hardware (ClockworkPi PicoCalc with a
Pimoroni Pico Plus 2). `~/Source/frank-quest` runs SCI games noticeably faster — quicker loads and a
visibly smoother picture — than this project does in its `PICO_PSRAM_MAPPED` (pimoroni) build.

The cause turned out to be mundane, and neither of the two things first suspected. Both were checked
and ruled out:

- **It is not the PSRAM bus.** Both projects use memory-mapped QMI PSRAM. This project's
  `PICO_PSRAM_MAPPED` build (`CMakeLists.txt:9`, `src/platform/pico/psram_qmi.h`) is the same
  arrangement frank-quest uses. No PIO SPI PSRAM is involved on either side.
- **It is not resource caching or engine architecture.** Nothing was found that would explain a gap
  of this size at that level.

## The measured difference

| | frank-quest | freesci-archive (pimoroni) |
|---|---|---|
| Core clock | **504 MHz** | **133 MHz** — `src/platform/pico/pico_main.c:166` |
| Core voltage | 1.65 V (`vreg_set_voltage`) | default, never raised |
| PSRAM | mapped QMI | mapped QMI |
| LCD SPI clock | 75 MHz, PIO + DMA | **25 MHz**, hardware SPI — `hw/lcdspi/lcdspi.h:7` |
| Pixel format | `0x3A = 0x65` → 2 bytes/px | **`0x3A = 0x66` → 3 bytes/px** — `hw/lcdspi/lcdspi.c:592` |

Two independent factors, both large:

- **Core clock: 3.8x.** `set_sys_clock_khz(133000, true)` is the RP2350 power-on default, called
  once, unconditionally, with no overclock path anywhere in the tree. SCI loading is dominated by
  decompression, resource decoding and the interpreter loop, all CPU-bound, so this scales almost
  directly.
- **Display bandwidth: ~4.5x.** 3x the SPI clock and 2 bytes/pixel instead of 3. A full 320x200
  frame is ~61 ms here versus ~14 ms in frank-quest.

## What to do, in order of payoff

### 1. Raise the clock (biggest win, mostly a paste)

The hard part already exists: `psram_set_flash_timings(133, 66)` is called before
`set_sys_clock_khz()` (`pico_main.c:160-166`), and `psram_qmi.h:40` notes it was adopted from
frank-snes. What is missing is raising the clock at all.

frank-quest's recipe is `src/main.c:95-115`, and the order matters — all of it before
`stdio_init_all()`, because USB must enumerate at the final clock:

1. `vreg_disable_voltage_limit()` then `vreg_set_voltage(VREG_VOLTAGE_1_65)` for 504 MHz
   (1.60 V for >=378, 1.50 V below).
2. `set_flash_timings(target_mhz)` — recompute the QMI `CLKDIV`/`RXDELAY` for the *new* clock.
3. `set_sys_clock_khz(target * 1000, false)`, with a fallback to 252 MHz if it returns false.

**The trap:** the existing `psram_set_flash_timings(133, 66)` call is parameterised for a 133 MHz
system clock. Raising `clk_sys` without recomputing the divisor for the new clock overclocks the
flash and corrupts XIP reads — which is exactly the crash `pico_main.c:160-163` already warns about
("XIP reads corrupt and it crashes here, before any serial, with TFT noise"). Pass the new target.

PSRAM re-derives its own divisor from `clk_sys` inside init in frank-quest, so it stays at its cap
independently of the core clock; check this project does the same before raising anything.

Step up rather than jumping: 252 first (no voltage change needed), then 378, then 504. Re-verify
with the SD card, the display DMA and the I2C keyboard all live.

### 2. Switch the panel to 16-bit colour (one byte)

`lcdspi.c:593` sends `0x66` (18-bit, 3 bytes/pixel). The panel accepts `0x65` — 18-bit RGB
interface, **16-bit MCU/SPI interface**, which is the half that matters. This is proven on this exact
panel by `~/Source/shapones` (`samples/v3/picocalc.cpp:208`) and by frank-quest. The ILI9488
datasheet's "SPI is 18-bit only" does not hold for this ST7365P-class panel.

Halves display bandwidth. Requires the framebuffer-to-panel conversion to emit RGB565 pairs instead
of three bytes.

### 3. Raise the SPI clock, then consider PIO

`lcdspi.h` already has `//#define LCD_SPI_SPEED 50000000` sitting commented out. 75 MHz is proven on
this panel by shapones, which drives it from a two-instruction PIO program plus DMA rather than the
hardware SPI block (`samples/v3/picocalc.pio`, `setup_pio()`). frank-quest's
`drivers/LCD_picocalc.c` is a worked example of the same approach.

Note shapones sets its rate as `SYS_CLK_FREQ / 4` with a 300 MHz system clock. That ratio is
meaningless at another clock — derive the divisor from a target frequency instead.

Steps 1 and 2 land most of the gain; 3 is a rewrite of the SPI layer for the remainder.

## Cross-references

- `~/Source/frank-quest/src/main.c:95-115` — the overclock recipe and 252 MHz fallback
- `~/Source/frank-quest/drivers/LCD_picocalc.c` — PIO+DMA SPI panel driver at 75 MHz, 16-bit
- `~/Source/frank-quest/PICOCALC_PORT.md` — display bandwidth maths and the panel findings
- `~/Source/shapones/samples/v3/picocalc.{cpp,hpp,pio}` — the reference this all came from
