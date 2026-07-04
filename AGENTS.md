# AGENTS.md — RP2040 ILI9341 LVGL DMA

## Purpose
Boost framerate on RP2040 Pico driving ILI9341 (MAR2406) display via 8-bit parallel 8080 interface using PIO + DMA.

## Status
- PIO byte-mode works (test rectangle visible via CPU-driven writes).
- **Synchronous DMA works** — `disp_flush` packs 2 pixels/uint32_t into `swap_buf[]`, starts DMA, busy-waits for completion, drains FIFO+OSR, switches to byte mode, calls `flush_ready`. No ISR.

## Current Architecture
- PIO0 SM0 runs `ili9341_8080.pio` program with two sections:
  - **byte_mode** (wrap 0-3): `pull block → out 8 → WR strobe`. Used for init/commands.
  - **dma_mode** (4-16): `out 8 → WR strobe` × 4 per autopulled word. Used for pixel data.
- Mode switching via `pio_sm_restart` + `pio_sm_exec(jmp)`.
- DMA channel 0 transfers from `swap_buf[]` to PIO TX FIFO (synchronous: busy-wait in `disp_flush`).
- No ISR. Flush is synchronous: DMA starts → busy-wait → drain → switch byte → `flush_ready`.
- Swap buffer packs 2× RGB565 pixels per uint32_t in HI0,LO0,HI1,LO1 byte order.

## Key Files
- `ili9341_8080.pio` — PIO program (byte + dma modes), init & switch helpers
- `rp2040_ili9341.c` — main: ILI9341 init, LVGL glue, DMA setup, synchronous flush
- `CMakeLists.txt` — builds LVGL v8.4.0 with widget demo
- `lv_conf.h` — LVGL config

## Debugging
- Red 100×100 rect drawn via CPU byte-mode BEFORE LVGL starts → confirms byte-mode PIO works.
- LVGL flush calls synchronous DMA (busy-wait) — works correctly.
- Earlier async DMA (ISR-based) caused horizontal stripes. Root cause: `dma_channel_configure` clears IRQ enable (`INTE`) on each call, AND async `flush_ready` from ISR races with LVGL single-buffer render.
- Fix: synchronous DMA — no ISR, poll `dma_channel_is_busy` in `disp_flush`, drain, switch byte, `flush_ready`.

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