#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "ili9341_8080.pio.h"
#include "lvgl.h"
#include "demos/widgets/lv_demo_widgets.h"

//
// RP2040 GPIO → ILI9341 pin map (8-bit 8080 parallel)
//
//  D0-D7  GP0-7   Pico 1,2,4,5,6,7,9,10    PIO OUT
//  WR      GP8     Pico 11                   PIO SET (strobe)
//  RD      GP9     Pico 12                   GPIO pull-high
//  DC      GP10    Pico 14                   GPIO (1=data,0=cmd)
//  CS      GP11    Pico 15                   GPIO held low
//  RST     GP12    Pico 16                   GPIO

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

// ---------- globals ----------

static uint32_t swap_buf[LV_HOR_RES_MAX * 10];  // 2400 words = 9600 B (packs 2 pixels/word)
static lv_disp_drv_t *disp_drv_ref;
static uint program_offset;
static dma_channel_config dma_cfg;

// ---------- helpers ----------

static inline void pio_put(uint8_t d) {
    pio_sm_put_blocking(DISPLAY_PIO, DISPLAY_SM, d);
}

static void write_cmd_sync(uint8_t cmd) {
    gpio_put(DC_PIN, 0);
    pio_put(cmd);
    while (pio_sm_get_tx_fifo_level(DISPLAY_PIO, DISPLAY_SM) > 0);
}

static void write_data_sync(uint8_t data) {
    gpio_put(DC_PIN, 1);
    pio_put(data);
}

static void set_addr_win(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    write_cmd_sync(0x2A);
    write_data_sync(x0 >> 8); write_data_sync(x0 & 0xFF);
    write_data_sync(x1 >> 8); write_data_sync(x1 & 0xFF);

    write_cmd_sync(0x2B);
    write_data_sync(y0 >> 8); write_data_sync(y0 & 0xFF);
    write_data_sync(y1 >> 8); write_data_sync(y1 & 0xFF);

    write_cmd_sync(0x2C);
    gpio_put(DC_PIN, 1);
}

// ---------- DMA ISR ----------

static void dma_irq_handler(void) {
    if (dma_hw->ints0 & (1u << DMA_CHAN)) {
        dma_hw->ints0 = (1u << DMA_CHAN);
        gpio_put(25, 1);
        while (pio_sm_get_tx_fifo_level(DISPLAY_PIO, DISPLAY_SM) > 0)
            tight_loop_contents();
        for (volatile int i = 0; i < 8; i++)
            tight_loop_contents();
        ili9341_8080_switch_byte(DISPLAY_PIO, DISPLAY_SM, program_offset);
        lv_disp_flush_ready(disp_drv_ref);
        gpio_put(25, 0);
    }
}

// ---------- ILI9341 init ----------

static void ili9341_init(void) {
    gpio_put(RST_PIN, 0); sleep_ms(10);
    gpio_put(RST_PIN, 1); sleep_ms(120);

    write_cmd_sync(0x01); sleep_ms(120);
    write_cmd_sync(0x28);

    write_cmd_sync(0xCB); write_data_sync(0x39); write_data_sync(0x2C);
                           write_data_sync(0x00); write_data_sync(0x34);
                           write_data_sync(0x02);
    write_cmd_sync(0xCF); write_data_sync(0x00); write_data_sync(0xC1);
                           write_data_sync(0x30);
    write_cmd_sync(0xE8); write_data_sync(0x85); write_data_sync(0x00);
                           write_data_sync(0x78);
    write_cmd_sync(0xEA); write_data_sync(0x00); write_data_sync(0x00);
    write_cmd_sync(0xED); write_data_sync(0x64); write_data_sync(0x03);
                           write_data_sync(0x12); write_data_sync(0x81);
    write_cmd_sync(0xF7); write_data_sync(0x20);
    write_cmd_sync(0xC0); write_data_sync(0x23);
    write_cmd_sync(0xC1); write_data_sync(0x10);
    write_cmd_sync(0xC5); write_data_sync(0x3E); write_data_sync(0x28);
    write_cmd_sync(0xC7); write_data_sync(0x86);

    write_cmd_sync(0x36); write_data_sync(0x48);
    write_cmd_sync(0x3A); write_data_sync(0x55);
    write_cmd_sync(0xB1); write_data_sync(0x00); write_data_sync(0x18);
    write_cmd_sync(0xB6); write_data_sync(0x08); write_data_sync(0x82);
                           write_data_sync(0x27);

    write_cmd_sync(0xF2); write_data_sync(0x00);
    write_cmd_sync(0x26); write_data_sync(0x01);
    write_cmd_sync(0xE0); write_data_sync(0x0F); write_data_sync(0x31);
                           write_data_sync(0x2B); write_data_sync(0x0C);
                           write_data_sync(0x0E); write_data_sync(0x08);
                           write_data_sync(0x4E); write_data_sync(0xF1);
                           write_data_sync(0x37); write_data_sync(0x07);
                           write_data_sync(0x10); write_data_sync(0x03);
                           write_data_sync(0x0E); write_data_sync(0x09);
                           write_data_sync(0x00);
    write_cmd_sync(0xE1); write_data_sync(0x00); write_data_sync(0x0E);
                           write_data_sync(0x14); write_data_sync(0x03);
                           write_data_sync(0x11); write_data_sync(0x07);
                           write_data_sync(0x31); write_data_sync(0xC1);
                           write_data_sync(0x48); write_data_sync(0x08);
                           write_data_sync(0x0F); write_data_sync(0x0C);
                           write_data_sync(0x31); write_data_sync(0x36);
                           write_data_sync(0x0F);

    write_cmd_sync(0x11); sleep_ms(120);
    write_cmd_sync(0x29); sleep_ms(50);
}

// ---------- LVGL display driver ----------

static lv_color_t buf[LV_HOR_RES_MAX * 10];

static void disp_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p) {
    uint32_t w = lv_area_get_width(area);
    uint32_t h = lv_area_get_height(area);
    uint32_t n = w * h;

    set_addr_win(area->x1, area->y1, area->x2, area->y2);

    // Pack 2 pixels per uint32_t with bytes in HI0,LO0,HI1,LO1 order.
    // PIO shift_right: out pins,8 outputs bits[7:0] first = HI0
    uint8_t *b = (uint8_t *)color_p;
    uint32_t pairs = n / 2;
    uint32_t i;
    for (i = 0; i < pairs; i++) {
        swap_buf[i] = b[i * 4 + 1]                     // HI0
                    | ((uint32_t)b[i * 4 + 0] << 8)     // LO0
                    | ((uint32_t)b[i * 4 + 3] << 16)    // HI1
                    | ((uint32_t)b[i * 4 + 2] << 24);   // LO1
    }

    // Odd pixel: pack a partial word; ILI9341 ignores extra writes past window
    uint32_t dma_words = pairs;
    if (n & 1) {
        swap_buf[i] = b[n * 2 - 1] | ((uint32_t)b[n * 2 - 2] << 8);
        dma_words = pairs + 1;
    }

    disp_drv_ref = drv;

    ili9341_8080_switch_dma(DISPLAY_PIO, DISPLAY_SM, program_offset);
    dma_channel_abort(DMA_CHAN);
    dma_channel_configure(
        DMA_CHAN, &dma_cfg,
        &pio0_hw->txf[DISPLAY_SM],
        swap_buf, dma_words, true
    );
}

// ---------- LVGL 5 ms tick ----------

static struct repeating_timer lvgl_tick;

static bool tick_cb(struct repeating_timer *t) {
    lv_tick_inc(5);
    return true;
}

// ---------- main ----------

int main(void) {
    stdio_init_all();
    sleep_ms(100);

    // ---- PIO ----
    program_offset = pio_add_program(DISPLAY_PIO, &ili9341_8080_program);
    ili9341_8080_program_init(DISPLAY_PIO, DISPLAY_SM, program_offset, DATA_BASE, WR_PIN);
    gpio_put(WR_PIN, 1);
    pio_sm_set_enabled(DISPLAY_PIO, DISPLAY_SM, true);

    // ---- GPIO control ----
    gpio_init(RD_PIN);   gpio_set_dir(RD_PIN, GPIO_OUT);   gpio_put(RD_PIN, 1);
    gpio_init(DC_PIN);   gpio_set_dir(DC_PIN, GPIO_OUT);   gpio_put(DC_PIN, 0);
    gpio_init(CS_PIN);   gpio_set_dir(CS_PIN, GPIO_OUT);   gpio_put(CS_PIN, 0);
    gpio_init(RST_PIN);  gpio_set_dir(RST_PIN, GPIO_OUT);  gpio_put(RST_PIN, 1);
    gpio_init(25);       gpio_set_dir(25, GPIO_OUT);       gpio_put(25, 0);

    // ---- DMA ----
    dma_cfg = dma_channel_get_default_config(DMA_CHAN);
    channel_config_set_transfer_data_size(&dma_cfg, DMA_SIZE_32);
    channel_config_set_read_increment(&dma_cfg, true);
    channel_config_set_write_increment(&dma_cfg, false);
    channel_config_set_dreq(&dma_cfg, pio_get_dreq(DISPLAY_PIO, DISPLAY_SM, true));

    dma_channel_configure(
        DMA_CHAN, &dma_cfg,
        &pio0_hw->txf[DISPLAY_SM],
        NULL, 0, false
    );

    dma_hw->ints0 = (1u << DMA_CHAN);
    dma_channel_set_irq0_enabled(DMA_CHAN, true);
    irq_set_exclusive_handler(DMA_IRQ_0, dma_irq_handler);
    irq_set_enabled(DMA_IRQ_0, true);

    // ---- Display ----
    ili9341_init();

    // ---- Test: CPU-driven rectangle ----
    {
        gpio_put(DC_PIN, 0);
        pio_put(0x2A); while (pio_sm_get_tx_fifo_level(DISPLAY_PIO, DISPLAY_SM) > 0);
        gpio_put(DC_PIN, 1);
        pio_put(0); while (pio_sm_get_tx_fifo_level(DISPLAY_PIO, DISPLAY_SM) > 0);
        pio_put(0); while (pio_sm_get_tx_fifo_level(DISPLAY_PIO, DISPLAY_SM) > 0);
        pio_put(0); while (pio_sm_get_tx_fifo_level(DISPLAY_PIO, DISPLAY_SM) > 0);
        pio_put(99); while (pio_sm_get_tx_fifo_level(DISPLAY_PIO, DISPLAY_SM) > 0);
        gpio_put(DC_PIN, 0);
        pio_put(0x2B); while (pio_sm_get_tx_fifo_level(DISPLAY_PIO, DISPLAY_SM) > 0);
        gpio_put(DC_PIN, 1);
        pio_put(0); while (pio_sm_get_tx_fifo_level(DISPLAY_PIO, DISPLAY_SM) > 0);
        pio_put(0); while (pio_sm_get_tx_fifo_level(DISPLAY_PIO, DISPLAY_SM) > 0);
        pio_put(0); while (pio_sm_get_tx_fifo_level(DISPLAY_PIO, DISPLAY_SM) > 0);
        pio_put(99); while (pio_sm_get_tx_fifo_level(DISPLAY_PIO, DISPLAY_SM) > 0);
        gpio_put(DC_PIN, 0);
        pio_put(0x2C); while (pio_sm_get_tx_fifo_level(DISPLAY_PIO, DISPLAY_SM) > 0);
        gpio_put(DC_PIN, 1);
        for (int px = 0; px < 100 * 100; px++) {
            pio_put(0xF8);
            pio_put(0x00);
        }
        while (pio_sm_get_tx_fifo_level(DISPLAY_PIO, DISPLAY_SM) > 0);
    }

    // ---- LVGL ----
    lv_init();

    lv_disp_drv_t drv;
    lv_disp_drv_init(&drv);
    drv.hor_res  = TFT_W;
    drv.ver_res  = TFT_H;
    drv.flush_cb = disp_flush;
    static lv_disp_draw_buf_t draw_buf;
    lv_disp_draw_buf_init(&draw_buf, buf, NULL, sizeof(buf) / sizeof(lv_color_t));
    drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&drv);

    add_repeating_timer_ms(5, tick_cb, NULL, &lvgl_tick);

    lv_demo_widgets();

    for (;;) {
        lv_timer_handler();
        sleep_ms(5);
    }
}
