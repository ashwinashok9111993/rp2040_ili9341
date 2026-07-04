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

// Cube data ----------------------------------------------------------------
// 8 vertices of a ±1 cube scaled by 120 for orthographic projection
#define CUBE_SCALE 130
static const int8_t cube_verts[8][3] = {
    {-1,-1,-1}, { 1,-1,-1}, { 1, 1,-1}, {-1, 1,-1},
    {-1,-1, 1}, { 1,-1, 1}, { 1, 1, 1}, {-1, 1, 1},
};

// 6 quad faces (vertex indices), base colors
static const int8_t cube_faces[6][4] = {
    {0,1,2,3},  {5,4,7,6},  {4,5,1,0},
    {3,2,6,7},  {4,0,3,7},  {1,5,6,2},
};
static const uint16_t face_colors[6] = {
    0xF800, 0x001F, 0x07E0, 0xFFE0, 0x07FF, 0xF81F,
};

// Face normals (pre-projection)
static const int8_t face_norms[6][3] = {
    { 0, 0,-1}, { 0, 0, 1}, { 0,-1, 0},
    { 0, 1, 0}, {-1, 0, 0}, { 1, 0, 0},
};

// Q16.16 fixed-point trig ------------------------------------------------
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

// State --------------------------------------------------------------------
static int16_t sx[8], sy[8];
static int8_t face_bright[6];
static int32_t ang_x, ang_y;

#define CX (TFT_W / 2)
#define CY (TFT_H / 2) + 10

static void transform_cube(void) {
    int ai = (ang_x >> (16 - TRIG_BITS)) & (TRIG_SIZE - 1);
    int aj = (ai + TRIG_SIZE / 4) & (TRIG_SIZE - 1);
    int32_t c_x = trig_tab[ai], s_x = trig_tab[aj];
    ai = (ang_y >> (16 - TRIG_BITS)) & (TRIG_SIZE - 1);
    aj = (ai + TRIG_SIZE / 4) & (TRIG_SIZE - 1);
    int32_t c_y = trig_tab[ai], s_y = trig_tab[aj];

    int32_t sc = CUBE_SCALE * 65536;

    for (int i = 0; i < 8; i++) {
        int32_t x = cube_verts[i][0] * sc;
        int32_t y = cube_verts[i][1] * sc;
        int32_t z = cube_verts[i][2] * sc;

        // Rotate Y
        int32_t rx = fp_mul(x, c_y) - fp_mul(z, s_y);
        int32_t rz = fp_mul(x, s_y) + fp_mul(z, c_y);
        x = rx; z = rz;
        // Rotate X
        int32_t ry = fp_mul(y, c_x) - fp_mul(z, s_x);
        rz = fp_mul(y, s_x) + fp_mul(z, c_x);
        y = ry; z = rz;

        // Orthographic projection to screen
        sx[i] = CX + (int16_t)(x >> 16);
        sy[i] = CY + (int16_t)(y >> 16);
    }

    for (int f = 0; f < 6; f++) {
        int32_t nx = face_norms[f][0] * 65536;
        int32_t ny = face_norms[f][1] * 65536;
        int32_t nz = face_norms[f][2] * 65536;

        int32_t rnx = fp_mul(nx, c_y) - fp_mul(nz, s_y);
        int32_t rnz = fp_mul(nx, s_y) + fp_mul(nz, c_y);
        nx = rnx; nz = rnz;
        int32_t rny = fp_mul(ny, c_x) - fp_mul(nz, s_x);
        rnz = fp_mul(ny, s_x) + fp_mul(nz, c_x);
        ny = rny; nz = rnz;

        // light from above-right
        int32_t dot = fp_mul(nx, 32768) + fp_mul(ny, 32768) + nz;
        if (dot < 0) dot = 0;
        if (dot > 65536) dot = 65536;
        face_bright[f] = (int8_t)(dot >> 8);
    }
}

static void draw_span(int y, int x0, int x1, int r, int g, int b) {
    if (y < 0 || y >= TFT_H) return;
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (x0 < 0) x0 = 0;
    if (x1 >= TFT_W) x1 = TFT_W - 1;
    if (x0 > x1) return;

    uint16_t c = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    uint8_t hi = c >> 8, lo = c & 0xFF;
    int base = y * TFT_W * 2 + x0 * 2;
    for (int x = x0; x <= x1; x++) {
        fb[base]     = hi;
        fb[base + 1] = lo;
        base += 2;
    }
}

static void draw_face(int f) {
    int b = face_bright[f];
    if (b <= 0) return;

    uint16_t base = face_colors[f];
    int r = ((base >> 11) & 0x1F) * b / 255;
    int g = ((base >> 5)  & 0x3F) * b / 255;
    int bv = (base & 0x1F) * b / 255;

    // Get screen coords of 4 vertices
    int px[4], py[4];
    for (int i = 0; i < 4; i++) {
        int vi = cube_faces[f][i];
        px[i] = sx[vi];
        py[i] = sy[vi];
    }

    // Split quad into 2 triangles: (0,1,2) and (0,2,3)
    for (int tri = 0; tri < 2; tri++) {
        int v0i, v1i, v2i;
        if (tri == 0) { v0i = 0; v1i = 1; v2i = 2; }
        else           { v0i = 0; v1i = 2; v2i = 3; }

        int vx[3] = {px[v0i], px[v1i], px[v2i]};
        int vy[3] = {py[v0i], py[v1i], py[v2i]};

        // Sort vertices by Y (bubble sort 3 elements)
        for (int k = 0; k < 2; k++) {
            for (int j = 0; j < 2 - k; j++) {
                if (vy[j] > vy[j+1]) {
                    int t = vx[j]; vx[j] = vx[j+1]; vx[j+1] = t;
                    t = vy[j]; vy[j] = vy[j+1]; vy[j+1] = t;
                }
            }
        }

        int y0 = vy[0], y1 = vy[1], y2 = vy[2];
        if (y0 == y2) return; // degenerate
        if (y0 < 0) y0 = 0;
        if (y2 >= TFT_H) y2 = TFT_H - 1;
        if (y0 > y2) return;

        // Scanline fill using fixed-point edge walking
        int dx_l = (vx[2] - vx[0]) * 65536 / (vy[2] - vy[0] + 1);
        int x_l  = vx[0] * 65536;

        for (int y = y0; y <= y2; y++) {
            int x_r;
            if (y <= y1 && vy[1] > vy[0]) {
                int dx_r = (vx[1] - vx[0]) * 65536 / (vy[1] - vy[0]);
                x_r = vx[0] * 65536 + dx_r * (y - vy[0]);
            } else if (vy[2] > vy[1]) {
                int dx_r = (vx[2] - vx[1]) * 65536 / (vy[2] - vy[1]);
                x_r = vx[1] * 65536 + dx_r * (y - vy[1]);
            } else {
                x_r = vx[2] * 65536;
            }
            draw_span(y, x_l >> 16, x_r >> 16, r, g, bv);
            x_l += dx_l;
        }
    }
}

static void render_frame(void) {
    memset(fb, 0, sizeof(fb));          // clear to black
    transform_cube();
    for (int f = 0; f < 6; f++)
        draw_face(f);
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
