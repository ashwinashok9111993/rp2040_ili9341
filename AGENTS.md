# AGENTS.md — RP2040 ILI9341 LVGL DMA

## Purpose
Boost framerate on RP2040 Pico driving ILI9341 (MAR2406) display via 8-bit parallel 8080 interface using PIO + DMA.

## Status
- PIO byte-mode works (test rectangle visible via CPU-driven writes).
- DMA pixel transfer NOT WORKING yet — grey screen after init with test rect but no LVGL content.

## Current Architecture
- PIO0 SM0 runs `ili9341_8080.pio` program with two sections:
  - **byte_mode** (wrap 0-3): `pull block → out 8 → WR strobe`. Used for init/commands.
  - **dma_mode** (4-16): `out 8 → WR strobe` × 4 per autopulled word. Used for pixel data.
- Mode switching via `pio_sm_restart` + `pio_sm_exec(jmp)`.
- DMA channel 0 transfers from `swap_buf[]` to PIO TX FIFO.
- DMA ISR drains FIFO, switches back to byte mode, calls `lv_disp_flush_ready`.

## Key Files
- `ili9341_8080.pio` — PIO program (byte + dma modes), init & switch helpers
- `rp2040_ili9341.c` — main: ILI9341 init, LVGL glue, DMA setup, ISR
- `CMakeLists.txt` — builds LVGL v8.4.0 with widget demo
- `lv_conf.h` — LVGL config

## Debugging
- Red 100×100 rect drawn via CPU byte-mode BEFORE LVGL starts → confirms byte-mode PIO works.
- LVGL flush calls `printf("  flush ...")` — check USB serial.
- Suspect: DMA never starts, ISR never fires → `flush_ready` never called → LVGL hangs.

## Build
```bash
cd build && cmake .. && make -j4
```
Output: `rp2040_ili9341.uf2`

## Pin Map
| Function | GPIO | Pico Pin |
|----------|------|----------|
| D0-D7    | 0-7  | 1,2,4,5,6,7,9,10 |
| WR       | 8    | 11 |
| RD       | 9    | 12 |
| DC       | 10   | 14 |
| CS       | 11   | 15 |
| RST      | 12   | 16 |

## Display
LCD Wiki MAR2406 (QD243701 panel + TSC2046 touch). Touch not used.