# SCI01 VGA on the PicoCalc (branch `pico-sci1`)

Goal: Jones in the Fast Lane (VGA floppy, interpreter 1.000.060, `SCI_VERSION_01_VGA`) on the **PIO** PicoCalc
build. Desktop plays it since 2026-09-30 (see `pico-engine-fixes.md`). The Pimoroni target would be easier but
needs the board swapped into the case, so the PIO build comes first.

## Why it can fit (2026-09-30 estimate)

- Script/object/locals footprint (desktop probe) is at or below the SCI0 games that already fit: Jones worst room
  39 KB scripts + 15 KB object vars + 3 KB locals, vs KQ4 42-46 KB scripts, SQ3 40-50 KB, PQ2 45-51 KB.
- No text parser ("Assuming that this game does not use a parser"): none of the parser's SRAM (resident vocab,
  the ~45 KB GNF build peak per command).
- The problem is resource SIZE, not count: largest pic 55,611 B decompressed (54,871 compressed, method 1), largest
  view 34,693 B, largest sound 46,715 B (method 2). The SCI0 games top out at ~12 KB pics and 13-21 KB views, which
  is what the 16 KB decompress scratch was sized for. KQ4's measured contiguous margin in play is ~41 KB.
- Resource methods in Jones: pics are all method 1 (Huffman); everything else is method 0 or 2 (LZW, `decrypt3`).
  No reorder methods (3/4).

## Step 1 -- `visdiff` decodes VGA pictures as the device does

`tests/picodiff/visdiff` hardcoded version 0 and no static palette, so a first run "agreed" on Jones by decoding its
VGA pictures as SCI0 on both sides (blank pictures). Now: a game with a palette 999 resource is treated as
`SCI_VERSION_01_VGA` (the resource map alone says SCI0 for Jones -- the game learns version 3 from the interpreter
hash in main.c), the static palette is passed, the EGA dither is skipped, the priority map is diffed, and the Pico
side decodes with `resource == NULL` through the PSRAM stream cache exactly as on the device (one pic per child
process; `RAWPTR=1` passes the raw pointer; `DUMP=dir` writes PPMs; all-blank runs fail).

Found at once: the VGA `SET_PALETTE` opcode and the equidistant priority table read the resource pointer directly
(NULL on the Pico) -- every Jones picture would have faulted. Both page through PSRAM now. Harness note: the decoder's
`sci1` argument must be 0 for SCI0 pictures (it is a flag, not a version); passing `SCI_VERSION_0` (=1) crashed the
desktop reference.

## Step 2 -- picture memory

A VGA picture is mostly one embedded bitmap (pic 11, the town board: first cel 33,259 B). The existing Pico branch
malloc'd the whole cel, decoded it into a full-size pixmap and translated that -- three large SRAM blocks -- to copy
rows into the visual map. `_pico_draw_embedded_cel1` (`sci_pic_0.c`) decodes the RLE stream straight from PSRAM into
`visual_map->index_data`, reproducing the desktop blit exactly (stride 320, no clipping, zeroed cel, the "titlebar
hack"). Overlaid pictures keep the buffered path: desktop's `view_transparentize` indexes the picture with the view's
width, which a direct decode cannot reproduce.

The load: VGA resources go through `decompress01`, which had none of decompress0's Pico treatment (flat input
buffer + flat output, ~110 KB for the board) -- and a latent bug: with `PICO_STREAM_METHODS` bit 2 compiled in,
`decrypt2` takes a *stream descriptor*, but decompress01's method 1 passed it a plain buffer, so every SCI01 Huffman
resource would have decoded garbage on the device. Now:
- `pico_stream_decompress01` (`decompress0.c`) runs SCI01 methods 0 and 1 through the 4 KB window (SCI01 method 1 is
  the same Huffman coder as SCI0 method 2).
- `pico_decompress_alloc` is shared and gains a one-load lent buffer (`g_pico_decompress_borrow`):
  `gfxr_interpreter_calculate_pic` lends the resident `visual[0]` (already borrowed as the decode buffer) while a
  VGA pic loads; the data goes to PSRAM and is evicted, then the loan ends and the decode reuses the buffer. Every
  res->data free site treats the lent buffer like the scratch.
- On the Pico, VGA pics take the Pico (PSRAM) branch of `calculate_pic` with the version passed to the decoder.
- `gfxop_new_pic` lets `SCI_VERSION_01_VGA` through; later versions still halt legibly.

Verified offline: `decompdiff` gained an SCI01 mode (stock `decompress01` vs the Pico stream, bytes + return code +
fd position): Jones 266 resources, 0 differ, and the SCI0 corpus unchanged (3,471 resources across 5 games).
`visdiff`: all 7 Jones pictures identical through the direct decoder, SCI0 games 0 differ. PIO `.bss` 30,840 (+8:
the lent-buffer pointer and size).

Not done yet: method 2 (`decrypt3`) still reads a flat input (Jones: 240 resources, up to 27.5 KB), the palette is
still the EGA mapping on the LCD (step 3), VGA views have no PSRAM offload (step 4). Nothing flashed yet.

## Step 3 -- 256-colour palette on the LCD

The driver already works in 256 palette slots (`palette[256][3]`, `pal565` for the 16-bit LCD path) and `visual[0]`
holds slot numbers; SCI0 fills the slots from the 256 dithered EGA combinations (`pico_setup_sci0_palette`). For VGA
games `gfxop_new_pic` now calls `pico_setup_vga_palette` with the picture's own 256 colours (its `SET_PALETTE`
opcode; `gfxr_read_pal1` always returns all 256 entries), or the static palette 999 when it sets none -- what
desktop draws the picture with. Nothing else needed: `pico_blit_indexed` already uses the identity LUT for
256-colour pixmaps (VGA cels index the system palette, as in Sierra's interpreter), resolves text/window colours to
the nearest slot, and takes transparency from each pixmap's own `color_key` (the VGA cel header). `kPalette`
subfunction 3 (Jones calls it) is unimplemented on desktop too. The picture colours are the ones `visdiff DUMP`
renders (same pic-palette mapping), which match desktop. `.bss` unchanged (30,840).

Known risk before a device run: VGA views already get the Pico's cel-to-PSRAM offload (it runs after the version
switch), but a view is decoded completely first -- all cels in SRAM at once, plus the decompressed resource, plus
decrypt3's flat compressed input. For the largest Jones view (view.711: 27.5 KB compressed, 34.7 KB decompressed)
that is well past the heap margin; an OOM halts legibly. That is step 4.
