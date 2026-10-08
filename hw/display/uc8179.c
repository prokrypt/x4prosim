/*
 * UltraChip UC8179 e-paper controller (800x480 visible, 800x600 addressed), as
 * wired on the Xteink X4 Pro: 3-wire SPI where SDA (MOSI) doubles as the
 * read line, DC/RST on GPIO, BUSY_N on GPIO (low = busy).
 *
 * Writes arrive two ways: bytes from the SPI controller (SSI bus), and
 * bit-banged SCL/SDA edges on GPIO, which the firmware uses for its
 * controller probe and for register reads. Reads answer the identity the
 * real X4 Pro panel gives (REV 0x70 = 00 00 01 FF FF, FLG 0x71 = 0x13 idle),
 * a fixed 25 C temperature (0x40), and the OTP (0xA2) of that panel: its
 * bank 0 head and per-temperature-range headers (VCOM_DC) as dumped from the
 * device; the waveform bodies, which were not dumped, read as 0x00.
 *
 * Models DTM1 (0x10, old plane) and DTM2 (0x13, new plane) and display
 * refresh (0x12), which holds BUSY_N low for "busy-ms" and then shows the
 * panel. Waveforms are not simulated: with no LUT upload the panel shows
 * DTM2; after a LUT upload (0x20..0x24) it shows DTM1/DTM2 as 4 gray levels.
 * Other commands (power, booster, PLL, VCOM, temperature setting) are
 * accepted and ignored.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/ssi/ssi.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "ui/console.h"
#include "ui/pixel_ops.h"

#define TYPE_UC8179 "uc8179"
OBJECT_DECLARE_SIMPLE_TYPE(Uc8179State, UC8179)

#define W 800
#define H 480
#define H_ADDR 600
#define WB (W / 8)
#define OTP_SIZE 0x1000
#define OTP_TR(n) (0x49 + (n) * 0xF7)

enum { PLANE_OLD, PLANE_NEW };

struct Uc8179State {
    SSIPeripheral parent_obj;
    QemuConsole *con;
    qemu_irq busy_n;
    qemu_irq sda_out;
    QEMUTimer busy_timer;
    uint32_t busy_ms;
    bool portrait;

    bool dc;
    bool in_reset;
    bool asleep;
    bool busy;
    uint8_t cmd;
    uint32_t pos;           /* byte index into the plane being written, or read index */
    bool custom_lut;

    /* bit-banged SPI on GPIO */
    bool sclk;
    bool sda_in;
    uint8_t shift;
    int bits;
    const uint8_t *rd;      /* read data for the current read command */
    uint32_t rd_len;

    uint8_t otp[OTP_SIZE];
    uint8_t ram[2][H_ADDR][WB];
    uint8_t shown[H][WB * 2];
    bool redraw;
};

static const uint8_t uc8179_rev[] = { 0x00, 0x00, 0x01, 0xff, 0xff };
static const uint8_t uc8179_temp[] = { 25, 0x00 };

/* OTP bank 0 as read from an X4 Pro panel: 0x000..0x01E, then TR headers at OTP_TR(n). */
static const uint8_t uc8179_otp_head[] = {
    0xA5, 0x05, 0x0A, 0x0F, 0x14, 0x50, 0x5A, 0x64, 0x7F, 0xFF, 0xFF, 0xFF, 0xA5, 0x1F, 0x27, 0x27,
    0x36, 0x17, 0x00, 0x29, 0x07, 0x22, 0x64, 0x02, 0x58, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00,
};
static const uint8_t uc8179_otp_vcom[8] = { 0x2E, 0x2E, 0x2E, 0x22, 0x22, 0x1A, 0x22, 0x26 };

static void uc8179_set_busy(Uc8179State *s, uint32_t ms)
{
    s->busy = true;
    qemu_set_irq(s->busy_n, 0);
    timer_mod(&s->busy_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + ms);
}

static void uc8179_busy_done(void *opaque)
{
    Uc8179State *s = opaque;
    s->busy = false;
    qemu_set_irq(s->busy_n, 1);
}

static void uc8179_refresh(Uc8179State *s)
{
    /* The driver streams framebuffer row h-1-i into RAM row i. */
    for (int y = 0; y < H; y++) {
        int r = H - 1 - y;
        for (int xb = 0; xb < WB; xb++) {
            uint8_t o = s->ram[PLANE_OLD][r][xb], n = s->ram[PLANE_NEW][r][xb];
            uint16_t px = 0;
            for (int b = 7; b >= 0; b--) {
                int lvl = s->custom_lut ? (((n >> b) & 1) << 1) | ((o >> b) & 1)
                                        : (((n >> b) & 1) ? 3 : 0);
                px = (px << 2) | lvl;
            }
            s->shown[y][xb * 2] = px >> 8;
            s->shown[y][xb * 2 + 1] = px & 0xff;
        }
    }
    s->redraw = true;
}

static void uc8179_command(Uc8179State *s, uint8_t c)
{
    s->cmd = c;
    s->pos = 0;
    s->rd = NULL;
    s->rd_len = 0;

    switch (c) {
    case 0x02:  /* POF */
    case 0x04:  /* PON */
        uc8179_set_busy(s, 2);
        break;
    case 0x07:  /* DSLP; the check code 0xA5 follows as data */
        break;
    case 0x12:  /* DRF */
        uc8179_refresh(s);
        uc8179_set_busy(s, s->busy_ms);
        break;
    case 0x20 ... 0x24:
        s->custom_lut = true;
        break;
    case 0x40:  /* TSC: BUSY while sensing, then the reading is clocked out */
        uc8179_set_busy(s, 5);
        s->rd = uc8179_temp;
        s->rd_len = sizeof(uc8179_temp);
        break;
    case 0x70:
        s->rd = uc8179_rev;
        s->rd_len = sizeof(uc8179_rev);
        break;
    case 0xA2:
        s->rd = s->otp;
        s->rd_len = OTP_SIZE;
        break;
    }
}

static void uc8179_data(Uc8179State *s, uint8_t v)
{
    switch (s->cmd) {
    case 0x07:
        if (v == 0xA5) s->asleep = true;
        break;
    case 0x10:
    case 0x13:
        if (s->pos < H_ADDR * WB) {
            s->ram[s->cmd == 0x10 ? PLANE_OLD : PLANE_NEW][s->pos / WB][s->pos % WB] = v;
            s->pos++;
        }
        break;
    }
}

static void uc8179_byte(Uc8179State *s, uint8_t v)
{
    if (s->in_reset || s->asleep) {
        return;
    }
    if (!s->dc) {
        uc8179_command(s, v);
    } else {
        uc8179_data(s, v);
    }
}

static uint32_t uc8179_transfer(SSIPeripheral *dev, uint32_t data)
{
    uc8179_byte(UC8179(dev), data);
    return 0xff;
}

/* Next bit the controller presents on SDA during a register read. */
static uint8_t uc8179_read_byte(Uc8179State *s)
{
    if (s->cmd == 0x71) {
        return s->busy ? 0x12 : 0x13;
    }
    if (!s->rd) {
        return 0xff;
    }
    return s->pos < s->rd_len ? s->rd[s->pos] : s->rd[s->rd_len - 1];
}

static bool uc8179_reading(Uc8179State *s)
{
    return s->dc && !s->in_reset && (s->cmd == 0x71 || s->rd);
}

static void uc8179_present_bit(Uc8179State *s)
{
    if (uc8179_reading(s)) {
        qemu_set_irq(s->sda_out, (uc8179_read_byte(s) >> (7 - s->bits)) & 1);
    } else {
        qemu_set_irq(s->sda_out, 1);
    }
}

static void uc8179_set_sclk(void *opaque, int n, int level)
{
    Uc8179State *s = UC8179(opaque);
    bool rise = level && !s->sclk;

    s->sclk = level;
    if (!rise || s->parent_obj.cs) {     /* CS is active low */
        return;
    }
    if (uc8179_reading(s)) {
        /* The host sampled the presented bit while SCL was low. */
        if (++s->bits == 8) {
            s->bits = 0;
            s->pos++;
        }
    } else {
        s->shift = (s->shift << 1) | s->sda_in;
        if (++s->bits == 8) {
            s->bits = 0;
            uc8179_byte(s, s->shift);
        }
    }
    uc8179_present_bit(s);
}

static void uc8179_set_sda(void *opaque, int n, int level)
{
    UC8179(opaque)->sda_in = level != 0;
}

static void uc8179_set_dc(void *opaque, int n, int level)
{
    Uc8179State *s = UC8179(opaque);
    s->dc = level != 0;
    s->bits = 0;
    uc8179_present_bit(s);
}

static int uc8179_set_cs(SSIPeripheral *dev, bool level)
{
    Uc8179State *s = UC8179(dev);
    s->bits = 0;
    if (level) {
        qemu_set_irq(s->sda_out, 1);
    }
    return 0;
}

static void uc8179_set_rst(void *opaque, int n, int level)
{
    Uc8179State *s = UC8179(opaque);
    if (!level) {
        s->in_reset = true;
    } else if (s->in_reset) {
        s->in_reset = false;
        s->asleep = false;
        s->custom_lut = false;
        s->cmd = 0;
        s->bits = 0;
        s->rd = NULL;
    }
}

static void uc8179_update_display(void *opaque)
{
    Uc8179State *s = opaque;
    static const uint8_t shade[4] = { 0x10, 0x60, 0xa8, 0xf0 };
    DisplaySurface *surface = qemu_console_surface(s->con);

    if (!s->redraw) {
        return;
    }
    s->redraw = false;
    uint32_t *d = (uint32_t *)surface_data(surface);
    int stride = surface_stride(surface) / 4;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint8_t byte = s->shown[y][x / 4];
            uint8_t g = shade[(byte >> (6 - 2 * (x % 4))) & 3];
            int dx = s->portrait ? H - 1 - y : x, dy = s->portrait ? x : y;
            d[dy * stride + dx] = rgb_to_pixel32(g, g, g);
        }
    }
    dpy_gfx_update(s->con, 0, 0, surface_width(surface), surface_height(surface));
}

static void uc8179_invalidate(void *opaque)
{
    UC8179(opaque)->redraw = true;
}

static const GraphicHwOps uc8179_ops = {
    .invalidate = uc8179_invalidate,
    .gfx_update = uc8179_update_display,
};

static void uc8179_realize(SSIPeripheral *d, Error **errp)
{
    DeviceState *dev = DEVICE(d);
    Uc8179State *s = UC8179(d);

    memset(s->otp, 0x00, OTP_TR(12));
    memset(s->otp + OTP_TR(12), 0xff, OTP_SIZE - OTP_TR(12));
    memcpy(s->otp, uc8179_otp_head, sizeof(uc8179_otp_head));
    for (int n = 0; n < 12; n++) {
        static const uint8_t tr[] = { 0x67, 0xBF, 0x3F, 0x0D, 0x00, 0x00, 0x00 };
        memcpy(s->otp + OTP_TR(n), n < 8 ? tr : (const uint8_t[7]){ 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff }, 7);
        if (n < 8) {
            s->otp[OTP_TR(n) + 5] = uc8179_otp_vcom[n];
        }
    }
    memset(s->ram, 0xff, sizeof(s->ram));
    memset(s->shown, 0xff, sizeof(s->shown));
    s->redraw = true;
    s->con = graphic_console_init(dev, 0, &uc8179_ops, s);
    qemu_console_resize(s->con, s->portrait ? H : W, s->portrait ? W : H);
    timer_init_ms(&s->busy_timer, QEMU_CLOCK_VIRTUAL, uc8179_busy_done, s);
    qdev_init_gpio_in_named(dev, uc8179_set_dc, "dc", 1);
    qdev_init_gpio_in_named(dev, uc8179_set_rst, "rst", 1);
    qdev_init_gpio_in_named(dev, uc8179_set_sclk, "sclk", 1);
    qdev_init_gpio_in_named(dev, uc8179_set_sda, "sda", 1);
    qdev_init_gpio_out_named(dev, &s->busy_n, "busy", 1);
    qdev_init_gpio_out_named(dev, &s->sda_out, "sda-out", 1);
}

static Property uc8179_properties[] = {
    DEFINE_PROP_UINT32("busy-ms", Uc8179State, busy_ms, 300),
    DEFINE_PROP_BOOL("portrait", Uc8179State, portrait, true),
    DEFINE_PROP_END_OF_LIST(),
};

static void uc8179_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SSIPeripheralClass *k = SSI_PERIPHERAL_CLASS(klass);

    k->realize = uc8179_realize;
    k->transfer = uc8179_transfer;
    k->set_cs = uc8179_set_cs;
    k->cs_polarity = SSI_CS_LOW;
    device_class_set_props(dc, uc8179_properties);
}

static const TypeInfo uc8179_info = {
    .name = TYPE_UC8179,
    .parent = TYPE_SSI_PERIPHERAL,
    .instance_size = sizeof(Uc8179State),
    .class_init = uc8179_class_init,
};

static void uc8179_register_types(void)
{
    type_register_static(&uc8179_info);
}

type_init(uc8179_register_types)
