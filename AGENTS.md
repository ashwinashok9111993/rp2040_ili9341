# AGENTS.md — RP2040 ILI9341 PIO DMA

## Purpose
Drive ILI9341 (MAR2406) display via 8-bit parallel 8080 interface using PIO + DMA. Current demo: rolling dice with physics animation.

## Architecture
- PIO0 SM0: single byte-mode program (pull block → out pins,8 → WR strobe). No dma_mode, no autopull.
- DMA channel 0: `DMA_SIZE_8` with TX DREQ, reads directly from framebuffer `fb[]`.
- Synchronous flush: `dma_channel_configure` → busy-wait → FIFO drain → done.
- LVGL not linked in current builds (cube/dice demos use raw framebuffer).

## Branches
- `master` — LVGL widget demo baseline.
- `cube-3d` — flat-shaded rotating cube (orthographic, Q16.16 fixed-point).
- `dice-demo` (current) — rolling 3D die with dot faces, 3-state animation.

## Dice Demo
- 3 states: `ROLL` (fast spin, ~80 frames), `SETTLE` (decelerate), `SHOW` (~150 frames).
- Random initial velocity with perturbation and damping.
- Standard die dot patterns (1–6), bilinear interpolated on projected face quads.
- Result digit drawn at bottom of screen when settled (5×7 font, 8x scaled).
- Body: cream white with Lambertian shading; dots: black.

## Key Files
- `ili9341_8080.pio` — single byte-mode PIO program
- `rp2040_ili9341.c` — main: ILI9341 init, DMA setup, dice renderer
- `CMakeLists.txt` — builds with LVGL v8.4.0 (unused in dice build)
- `lv_conf.h` — LVGL config

## PIO timing (125 MHz)
- 4 instructions, `[1]` delay on WR edges (16ns meets ILI9341 15ns min).
- 6 cycles/byte = 48ns. Full frame flush: 153600 bytes × 48ns ≈ 7.4ms.

## Build
```bash
cd build && cmake .. && make -j4
```
Output: `rp2040_ili9341.uf2`

## Pin Map
| Function | GPIO | Pico Pin |
|----------|------|----------|
| D0-D7    | 0-7  | 1,2,4,6,7,9,10,11 |
| WR       | 8    | 12 |
| RD       | 9    | 14 |
| DC       | 10   | 15 |
| CS       | 11   | 16 |
| RST      | 12   | 17 |

## Display
LCD Wiki MAR2406 (QD243701 panel + TSC2046 touch). Touch not used.