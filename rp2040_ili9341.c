#include <stdio.h>
#include <string.h>
#include <math.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "ili9341_8080.pio.h"

//
// RP2040 GPIO → ILI9341 pin map (8-bit 8080 parallel)
//
#define DATA_BASE   0
#define WR_PIN      8
#define RD_PIN      9
#define DC_PIN      10
#define CS_PIN      11
#define RST_PIN     12

#define DISPLAY_PIO   pio0
#define DISPLAY_SM    0

#define TFT_W   240
#define TFT_H   320

#define DMA_CHAN    0

// ---------- framebuffer ----------

static uint8_t fb[TFT_W * TFT_H * 2];   // (HI, LO) per pixel for 8-bit interface

static uint program_offset;
static dma_channel_config dma_cfg;

// ---------- display helpers ----------

static inline void pio_put(uint8_t d) {
    pio_sm_put_blocking(DISPLAY_PIO, DISPLAY_SM, d);
}

static void write_cmd(uint8_t cmd) {
    gpio_put(DC_PIN, 0);
    pio_put(cmd);
    while (pio_sm_get_tx_fifo_level(DISPLAY_PIO, DISPLAY_SM) > 0);
}

static void write_data(uint8_t data) {
    gpio_put(DC_PIN, 1);
    pio_put(data);
}

static void set_addr_win(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    write_cmd(0x2A);
    write_data(x0 >> 8); write_data(x0 & 0xFF);
    write_data(x1 >> 8); write_data(x1 & 0xFF);
    write_cmd(0x2B);
    write_data(y0 >> 8); write_data(y0 & 0xFF);
    write_data(y1 >> 8); write_data(y1 & 0xFF);
    write_cmd(0x2C);
    gpio_put(DC_PIN, 1);
}

static void flush_fb(void) {
    set_addr_win(0, 0, TFT_W - 1, TFT_H - 1);
    dma_channel_configure(
        DMA_CHAN, &dma_cfg,
        &pio0_hw->txf[DISPLAY_SM],
        fb, sizeof(fb), true
    );
    while (dma_channel_is_busy(DMA_CHAN))
        tight_loop_contents();
    while (pio_sm_get_tx_fifo_level(DISPLAY_PIO, DISPLAY_SM) > 0)
        tight_loop_contents();
}

static inline void set_px(int x, int y, uint16_t c) {
    int i = (y * TFT_W + x) * 2;
    fb[i]   = c >> 8;
    fb[i+1] = c & 0xFF;
}

// ---------- ILI9341 init ----------

static void ili9341_init(void) {
    gpio_put(RST_PIN, 0); sleep_ms(10);
    gpio_put(RST_PIN, 1); sleep_ms(120);

    write_cmd(0x01); sleep_ms(120);
    write_cmd(0x28);

    write_cmd(0xCB); write_data(0x39); write_data(0x2C);
    write_data(0x00); write_data(0x34); write_data(0x02);
    write_cmd(0xCF); write_data(0x00); write_data(0xC1); write_data(0x30);
    write_cmd(0xE8); write_data(0x85); write_data(0x00); write_data(0x78);
    write_cmd(0xEA); write_data(0x00); write_data(0x00);
    write_cmd(0xED); write_data(0x64); write_data(0x03);
    write_data(0x12); write_data(0x81);
    write_cmd(0xF7); write_data(0x20);
    write_cmd(0xC0); write_data(0x23);
    write_cmd(0xC1); write_data(0x10);
    write_cmd(0xC5); write_data(0x3E); write_data(0x28);
    write_cmd(0xC7); write_data(0x86);

    write_cmd(0x36); write_data(0x48);
    write_cmd(0x3A); write_data(0x55);
    write_cmd(0xB1); write_data(0x00); write_data(0x18);
    write_cmd(0xB6); write_data(0x08); write_data(0x82); write_data(0x27);

    write_cmd(0xF2); write_data(0x00);
    write_cmd(0x26); write_data(0x01);
    write_cmd(0xE0); write_data(0x0F); write_data(0x31);
    write_data(0x2B); write_data(0x0C); write_data(0x0E);
    write_data(0x08); write_data(0x4E); write_data(0xF1);
    write_data(0x37); write_data(0x07); write_data(0x10);
    write_data(0x03); write_data(0x0E); write_data(0x09);
    write_data(0x00);
    write_cmd(0xE1); write_data(0x00); write_data(0x0E);
    write_data(0x14); write_data(0x03); write_data(0x11);
    write_data(0x07); write_data(0x31); write_data(0xC1);
    write_data(0x48); write_data(0x08); write_data(0x0F);
    write_data(0x0C); write_data(0x31); write_data(0x36);
    write_data(0x0F);

    write_cmd(0x11); sleep_ms(120);
    write_cmd(0x29); sleep_ms(50);
}

// ---------- 3D cube ----------

// ---- 3D rotating cube --------------------------------------------------
#define CUBE_SCALE 130

static const int8_t cube_verts[8][3] = {
    {-1,-1,-1},{ 1,-1,-1},{ 1, 1,-1},{-1, 1,-1},
    {-1,-1, 1},{ 1,-1, 1},{ 1, 1, 1},{-1, 1, 1},
};

static const uint8_t cube_faces[6][4] = {
    {0,1,2,3},{5,4,7,6},{4,5,1,0},
    {3,2,6,7},{4,0,3,7},{1,5,6,2},
};

static const uint16_t face_colors[6] = {
    0xF800,0x001F,0x07E0,0xFFE0,0x07FF,0xF81F,
};

static const int8_t face_norms[6][3] = {
    {0,0,-1},{0,0,1},{0,-1,0},
    {0,1,0},{-1,0,0},{1,0,0},
};

// Q16.16 fixed-point trig
#define TRIG_BITS 8
#define TRIG_SIZE (1 << TRIG_BITS)
static int32_t trig_tab[TRIG_SIZE];

static void init_trig(void) {
    for (int i = 0; i < TRIG_SIZE; i++) {
        double a = 6.283185307179586 * i / TRIG_SIZE;
        trig_tab[i] = (int32_t)(sin(a) * 65536.0);
    }
}

static inline int32_t fp_mul(int32_t a, int32_t b) {
    return (int32_t)(((int64_t)a * b) >> 16);
}

// Per-frame state
static int16_t sx[8], sy[8], sz[8];
static int16_t face_rz[6];   // face center Z for depth sort
static uint8_t face_vis[6];  // visible flag
static uint8_t face_bright[6];
static int32_t ang_x, ang_y;

#define CX (TFT_W/2)
#define CY (TFT_H/2) + 10

// ---- draw a horizontal span into fb ----
static void fill_span(int y, int x0, int x1, uint16_t c) {
    if (y < 0 || y >= TFT_H) return;
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (x0 < 0) x0 = 0;
    if (x1 >= TFT_W) x1 = TFT_W - 1;
    if (x0 > x1) return;
    uint8_t hi = c >> 8, lo = c & 0xFF;
    int base = (y * TFT_W + x0) * 2;
    for (int x = x0; x <= x1; x++) {
        fb[base] = hi; fb[base+1] = lo; base += 2;
    }
}

// ---- rasterize a single triangle ----
static void fill_tri(int vx[3], int vy[3], uint16_t col) {
    // Sort by Y (bubble)
    for (int k = 0; k < 2; k++)
        for (int j = 0; j < 2 - k; j++)
            if (vy[j] > vy[j+1]) {
                int t = vx[j]; vx[j] = vx[j+1]; vx[j+1] = t;
                t = vy[j]; vy[j] = vy[j+1]; vy[j+1] = t;
            }

    int y0 = vy[0], y1 = vy[1], y2 = vy[2];
    if (y0 >= y2) return;

    // Clip Y to screen
    int ys = y0 < 0 ? 0 : y0;
    int ye = y2 >= TFT_H ? TFT_H - 1 : y2;
    if (ys > ye) return;

    int cross = (vx[1]-vx[0])*(vy[2]-vy[0]) - (vy[1]-vy[0])*(vx[2]-vx[0]);

    // ---- flat-top ----
    if (y0 == y1) {
        int xl = vx[0], xr = vx[1];
        if (xl > xr) { int t = xl; xl = xr; xr = t; }
        int dxl = (vx[2] - xl) * 65536 / (y2 - y0);
        int dxr = (vx[2] - xr) * 65536 / (y2 - y0);
        int fl = xl * 65536 + dxl * (ys - y0);
        int fr = xr * 65536 + dxr * (ys - y0);
        for (int y = ys; y <= ye; y++) {
            fill_span(y, fl >> 16, fr >> 16, col);
            fl += dxl; fr += dxr;
        }
        return;
    }

    // ---- flat-bottom ----
    if (y1 == y2) {
        int xl = vx[1], xr = vx[2];
        if (xl > xr) { int t = xl; xl = xr; xr = t; }
        int dxl = (xl - vx[0]) * 65536 / (y1 - y0);
        int dxr = (xr - vx[0]) * 65536 / (y1 - y0);
        int fl = vx[0] * 65536 + dxl * (ys - y0);
        int fr = vx[0] * 65536 + dxr * (ys - y0);
        for (int y = ys; y <= ye; y++) {
            fill_span(y, fl >> 16, fr >> 16, col);
            fl += dxl; fr += dxr;
        }
        return;
    }

    // ---- general case ----
    int dx_long = (vx[2] - vx[0]) * 65536 / (y2 - y0);
    int dx_short_top, dx_short_bot;

    if (cross >= 0) {
        // middle LEFT → left=short(v0→v1→v2), right=long(v0→v2)
        dx_short_top = (vx[1] - vx[0]) * 65536 / (y1 - y0);
        dx_short_bot = (vx[2] - vx[1]) * 65536 / (y2 - y1);

        // Top half
        int y_top_end = y1 < ye ? y1 : ye + 1;
        int fl = vx[0] * 65536 + dx_short_top * (ys - y0);
        int fr = vx[0] * 65536 + dx_long * (ys - y0);
        for (int y = ys; y < y_top_end; y++) {
            fill_span(y, fl >> 16, fr >> 16, col);
            fl += dx_short_top; fr += dx_long;
        }

        // Bottom half
        int yb = y1 > ys ? y1 : ys;
        if (yb <= ye) {
            fl = vx[1] * 65536 + dx_short_bot * (yb - y1);
            fr = vx[0] * 65536 + dx_long * (yb - y0);
            for (int y = yb; y <= ye; y++) {
                fill_span(y, fl >> 16, fr >> 16, col);
                fl += dx_short_bot; fr += dx_long;
            }
        }
    } else {
        // middle RIGHT → left=long(v0→v2), right=short(v0→v1→v2)
        dx_short_top = (vx[1] - vx[0]) * 65536 / (y1 - y0);
        dx_short_bot = (vx[2] - vx[1]) * 65536 / (y2 - y1);

        // Top half
        int y_top_end = y1 < ye ? y1 : ye + 1;
        int fl = vx[0] * 65536 + dx_long * (ys - y0);
        int fr = vx[0] * 65536 + dx_short_top * (ys - y0);
        for (int y = ys; y < y_top_end; y++) {
            fill_span(y, fl >> 16, fr >> 16, col);
            fl += dx_long; fr += dx_short_top;
        }

        // Bottom half
        int yb = y1 > ys ? y1 : ys;
        if (yb <= ye) {
            fl = vx[0] * 65536 + dx_long * (yb - y0);
            fr = vx[1] * 65536 + dx_short_bot * (yb - y1);
            for (int y = yb; y <= ye; y++) {
                fill_span(y, fl >> 16, fr >> 16, col);
                fl += dx_long; fr += dx_short_bot;
            }
        }
    }
}

static void draw_face(int f, uint16_t col) {
    int px[4], py[4];
    for (int i = 0; i < 4; i++) {
        int vi = cube_faces[f][i];
        px[i] = sx[vi]; py[i] = sy[vi];
    }
    // Split quad → two triangles (0,1,2) and (0,2,3)
    int vx[3], vy[3];
    vx[0]=px[0]; vy[0]=py[0]; vx[1]=px[1]; vy[1]=py[1]; vx[2]=px[2]; vy[2]=py[2];
    fill_tri(vx, vy, col);
    vx[0]=px[0]; vy[0]=py[0]; vx[1]=px[2]; vy[1]=py[2]; vx[2]=px[3]; vy[2]=py[3];
    fill_tri(vx, vy, col);
}

static void render_frame(void) {
    memset(fb, 0, sizeof(fb));

    // Trig indices
    int ai = (ang_x >> (16-TRIG_BITS)) & (TRIG_SIZE-1);
    int aj = (ai + TRIG_SIZE/4) & (TRIG_SIZE-1);
    int32_t cos_x = trig_tab[ai], sin_x = trig_tab[aj];
    ai = (ang_y >> (16-TRIG_BITS)) & (TRIG_SIZE-1);
    aj = (ai + TRIG_SIZE/4) & (TRIG_SIZE-1);
    int32_t cos_y = trig_tab[ai], sin_y = trig_tab[aj];

    int32_t sc = CUBE_SCALE * 65536;

    // Rotate all 8 vertices
    for (int i = 0; i < 8; i++) {
        int32_t x = cube_verts[i][0] * sc;
        int32_t y = cube_verts[i][1] * sc;
        int32_t z = cube_verts[i][2] * sc;

        int32_t rx = fp_mul(x, cos_y) - fp_mul(z, sin_y);
        int32_t rz = fp_mul(x, sin_y) + fp_mul(z, cos_y);
        x = rx; z = rz;
        int32_t ry = fp_mul(y, cos_x) - fp_mul(z, sin_x);
        rz = fp_mul(y, sin_x) + fp_mul(z, cos_x);
        y = ry; z = rz;

        sx[i] = CX + (int16_t)(x >> 16);
        sy[i] = CY + (int16_t)(y >> 16);
        sz[i] = (int16_t)(z >> 16);
    }

    // Collect visible faces with brightness and depth
    int nf = 0;
    int vis_f[6], vis_b[6];
    int16_t vis_z[6];

    for (int f = 0; f < 6; f++) {
        int32_t nx = face_norms[f][0] * 65536;
        int32_t ny = face_norms[f][1] * 65536;
        int32_t nz = face_norms[f][2] * 65536;

        int32_t rnx = fp_mul(nx, cos_y) - fp_mul(nz, sin_y);
        int32_t rnz = fp_mul(nx, sin_y) + fp_mul(nz, cos_y);
        nx = rnx; nz = rnz;
        int32_t rny = fp_mul(ny, cos_x) - fp_mul(nz, sin_x);
        rnz = fp_mul(ny, sin_x) + fp_mul(nz, cos_x);
        ny = rny; nz = rnz;

        // Back-face cull
        if (nz <= 0) continue;

        // Diffuse shading
        int32_t dot = fp_mul(nx, 32768) + fp_mul(ny, 32768) + nz;
        if (dot < 0) dot = 0;
        if (dot > 65536) dot = 65536;

        vis_f[nf] = f;
        vis_b[nf] = (int)(dot >> 8);
        // Face center Z (average of 4 vertices), larger = further
        vis_z[nf] = (sz[cube_faces[f][0]]+sz[cube_faces[f][1]]+
                     sz[cube_faces[f][2]]+sz[cube_faces[f][3]]);
        nf++;
    }

    // Sort visible faces by Z descending (painter's algorithm)
    for (int i = 0; i < nf-1; i++)
        for (int j = 0; j < nf-1-i; j++)
            if (vis_z[j] < vis_z[j+1]) {
                int16_t tz = vis_z[j]; vis_z[j] = vis_z[j+1]; vis_z[j+1] = tz;
                int tf = vis_f[j]; vis_f[j] = vis_f[j+1]; vis_f[j+1] = tf;
                int tb = vis_b[j]; vis_b[j] = vis_b[j+1]; vis_b[j+1] = tb;
            }

    // Draw faces back-to-front
    for (int fi = 0; fi < nf; fi++) {
        int f = vis_f[fi];
        int b = vis_b[fi] * 255 / 256;  // 0..255
        if (b < 8) continue;

        uint16_t bc = face_colors[f];
        int r = ((bc>>11)&0x1F) * b / 255;
        int g = ((bc>>5)&0x3F) * b / 255;
        int bl = (bc&0x1F) * b / 255;
        uint16_t col = ((r>>3)<<11)|((g>>2)<<5)|(bl>>3);

        draw_face(f, col);
    }
}

// ---------- main ----------

int main(void) {
    stdio_init_all();
    sleep_ms(100);

    // PIO
    program_offset = pio_add_program(DISPLAY_PIO, &ili9341_8080_program);
    ili9341_8080_program_init(DISPLAY_PIO, DISPLAY_SM, program_offset, DATA_BASE, WR_PIN);
    gpio_put(WR_PIN, 1);
    pio_sm_set_enabled(DISPLAY_PIO, DISPLAY_SM, true);

    // GPIO
    gpio_init(RD_PIN);   gpio_set_dir(RD_PIN, GPIO_OUT);   gpio_put(RD_PIN, 1);
    gpio_init(DC_PIN);   gpio_set_dir(DC_PIN, GPIO_OUT);   gpio_put(DC_PIN, 0);
    gpio_init(CS_PIN);   gpio_set_dir(CS_PIN, GPIO_OUT);   gpio_put(CS_PIN, 0);
    gpio_init(RST_PIN);  gpio_set_dir(RST_PIN, GPIO_OUT);  gpio_put(RST_PIN, 1);

    // DMA
    dma_cfg = dma_channel_get_default_config(DMA_CHAN);
    channel_config_set_transfer_data_size(&dma_cfg, DMA_SIZE_8);
    channel_config_set_read_increment(&dma_cfg, true);
    channel_config_set_write_increment(&dma_cfg, false);
    channel_config_set_dreq(&dma_cfg, pio_get_dreq(DISPLAY_PIO, DISPLAY_SM, true));
    dma_channel_configure(DMA_CHAN, &dma_cfg, &pio0_hw->txf[DISPLAY_SM], NULL, 0, false);

    // Display
    ili9341_init();
    init_trig();

    // Show CPU-driven test rect
    {
        gpio_put(DC_PIN, 0);
        pio_put(0x2A); while (pio_sm_get_tx_fifo_level(DISPLAY_PIO, DISPLAY_SM) > 0);
        gpio_put(DC_PIN, 1);
        for (int i = 0; i < 4; i++) { pio_put(0); while (pio_sm_get_tx_fifo_level(DISPLAY_PIO, DISPLAY_SM) > 0); }
        pio_put(99); while (pio_sm_get_tx_fifo_level(DISPLAY_PIO, DISPLAY_SM) > 0);
        gpio_put(DC_PIN, 0);
        pio_put(0x2B); while (pio_sm_get_tx_fifo_level(DISPLAY_PIO, DISPLAY_SM) > 0);
        gpio_put(DC_PIN, 1);
        for (int i = 0; i < 4; i++) { pio_put(0); while (pio_sm_get_tx_fifo_level(DISPLAY_PIO, DISPLAY_SM) > 0); }
        pio_put(99); while (pio_sm_get_tx_fifo_level(DISPLAY_PIO, DISPLAY_SM) > 0);
        gpio_put(DC_PIN, 0);
        pio_put(0x2C); while (pio_sm_get_tx_fifo_level(DISPLAY_PIO, DISPLAY_SM) > 0);
        gpio_put(DC_PIN, 1);
        for (int px = 0; px < 100 * 100; px++) {
            pio_put(0xF8); pio_put(0x00);
        }
        while (pio_sm_get_tx_fifo_level(DISPLAY_PIO, DISPLAY_SM) > 0);
    }

    ang_x = 0;
    ang_y = 0;

    for (;;) {
        render_frame();
        flush_fb();
        ang_x += 786;  // ~0.012 rad per frame in Q16.16
        ang_y += 524;
        gpio_put(25, 1);
        gpio_put(25, 0);
    }
}
