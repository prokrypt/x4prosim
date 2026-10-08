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
 * refresh (0x12), which holds BUSY_N low for "busy-ms" and then updates a
 * per-pixel ink level (KW mode, plane bit 1 = white):
 *  - PSR (0x00) REG=0, OTP waveform (bodies not dumped, so idealized): every
 *    pixel goes to DTM2, except inside PTIN/PTOUT (0x91/0x92, whole panel; no
 *    0x90 window) where pixels with OLD == NEW are not driven and hold.
 *  - REG=1, register LUTs (0x20 VCOM, 0x21 WW, 0x22 KW, 0x23 WK, 0x24 KK; 6-byte
 *    groups [levels, TP_A..TP_D, RP]): the row chosen by each pixel's OLD/NEW
 *    bits runs frame by frame. Drive = source - VCOM (VDH -> black, VDL ->
 *    white); each frame moves the ink 1/"swing-frames" of a full swing,
 *    clamped at black and white, so DC-balanced rows still land on a level
 *    and short ones leave gray. VDHR (11) on a source row is not modeled.
 *  - CDI (0x50) N2OCP: NEW is copied to OLD after the refresh.
 * Other commands (power, booster, PLL, VCOM, temperature setting) are
 * accepted and ignored.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
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
#define LUT_ROWS 5
#define LUT_LEN 60              /* 10 groups; the X4 Pro driver writes 7 */
#define LUT_GROUPS (LUT_LEN / 6)
#define PSR_REG 0x20
#define CDI_N2OCP 0x08

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
    uint32_t pos;           /* byte index into the plane or LUT being written, or read index */
    uint8_t psr;            /* PSR byte 0 */
    uint8_t cdi;            /* CDI byte 0 */
    bool partial;           /* between PTIN and PTOUT */
    uint8_t lut[LUT_ROWS][LUT_LEN];
    uint8_t swing;          /* frames of one-way drive for a full black <-> white swing */

    /* bit-banged SPI on GPIO */
    bool sclk;
    bool sda_in;
    uint8_t shift;
    int bits;
    const uint8_t *rd;      /* read data for the current read command */
    uint32_t rd_len;

    uint8_t otp[OTP_SIZE];
    uint8_t ram[2][H_ADDR][WB];
    uint8_t ink[H][W];      /* shown level: 0 black .. swing white */
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

/* Walks one LUT row frame by frame. */
typedef struct LutCursor {
    const uint8_t *row;
    int group, rep, phase, frame;
} LutCursor;

static bool lut_next(LutCursor *c, int *level)
{
    while (c->group < LUT_GROUPS) {
        const uint8_t *g = c->row + c->group * 6;
        if (c->rep >= g[5]) {
            c->group++;
            c->rep = c->phase = c->frame = 0;
        } else if (c->phase >= 4) {
            c->rep++;
            c->phase = c->frame = 0;
        } else if (c->frame >= g[1 + c->phase]) {
            c->phase++;
            c->frame = 0;
        } else {
            c->frame++;
            *level = (g[0] >> (6 - 2 * c->phase)) & 3;
            return true;
        }
    }
    return false;
}

/* What a LUT row does to each ink level 0..swing: map[level] = level after. */
static void uc8179_row_map(Uc8179State *s, int r, uint8_t *map)
{
    static const int drive[4] = { 0, 1, -1, 0 };    /* GND, VDH, VDL, VDHR/float */
    LutCursor src = { .row = s->lut[r] }, com = { .row = s->lut[0] };
    bool vdhr = false;

    for (int i = 0; i <= s->swing; i++) {
        map[i] = i;
    }
    for (;;) {
        int ls = 0, lc = 0;
        bool more_s = lut_next(&src, &ls), more_c = lut_next(&com, &lc);
        if (!more_s && !more_c) {
            break;
        }
        vdhr |= ls == 3;
        int d = drive[ls] - drive[lc];     /* > 0 toward black */
        if (d) {
            for (int i = 0; i <= s->swing; i++) {
                map[i] = MIN(s->swing, MAX(0, map[i] - d));
            }
        }
    }
    if (vdhr) {
        qemu_log_mask(LOG_UNIMP, "uc8179: VDHR in LUT row 0x%x not modeled\n", 0x20 + r);
    }
}

static void uc8179_refresh(Uc8179State *s)
{
    /* LUT row per pixel, indexed by OLD << 1 | NEW: KK, KW, WK, WW */
    static const int row_of[4] = { 4, 2, 3, 1 };
    bool reg = s->psr & PSR_REG;
    uint8_t map[LUT_ROWS][UINT8_MAX + 1];

    if (reg) {
        for (int r = 1; r < LUT_ROWS; r++) {
            uc8179_row_map(s, r, map[r]);
        }
    }
    /* The driver streams framebuffer row h-1-i into RAM row i. */
    for (int y = 0; y < H; y++) {
        int r = H - 1 - y;
        for (int x = 0; x < W; x++) {
            int o = (s->ram[PLANE_OLD][r][x / 8] >> (7 - x % 8)) & 1;
            int n = (s->ram[PLANE_NEW][r][x / 8] >> (7 - x % 8)) & 1;
            uint8_t *w = &s->ink[y][x];
            if (reg) {
                *w = map[row_of[o << 1 | n]][*w];
            } else if (!s->partial || o != n) {
                *w = n ? s->swing : 0;
            }
        }
    }
    if (s->cdi & CDI_N2OCP) {
        memcpy(s->ram[PLANE_OLD], s->ram[PLANE_NEW], sizeof(s->ram[PLANE_NEW]));
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
    case 0x91:  /* PTIN */
        s->partial = true;
        break;
    case 0x92:  /* PTOUT */
        s->partial = false;
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
    case 0x00:
        if (s->pos++ == 0) {
            s->psr = v;
        }
        break;
    case 0x07:
        if (v == 0xA5) s->asleep = true;
        break;
    case 0x20 ... 0x24:
        if (s->pos < LUT_LEN) {
            s->lut[s->cmd - 0x20][s->pos++] = v;
        }
        break;
    case 0x50:
        if (s->pos++ == 0) {
            s->cdi = v;
        }
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

static void uc8179_reset_regs(Uc8179State *s)
{
    s->psr = 0x0F;      /* REG=0: OTP waveforms */
    s->cdi = 0x31;      /* N2OCP off */
    s->partial = false;
    memset(s->lut, 0, sizeof(s->lut));
}

static void uc8179_set_rst(void *opaque, int n, int level)
{
    Uc8179State *s = UC8179(opaque);
    if (!level) {
        s->in_reset = true;
    } else if (s->in_reset) {
        s->in_reset = false;
        s->asleep = false;
        uc8179_reset_regs(s);
        s->cmd = 0;
        s->bits = 0;
        s->rd = NULL;
    }
}

static void uc8179_update_display(void *opaque)
{
    Uc8179State *s = opaque;
    DisplaySurface *surface = qemu_console_surface(s->con);

    if (!s->redraw) {
        return;
    }
    s->redraw = false;
    uint32_t *d = (uint32_t *)surface_data(surface);
    int stride = surface_stride(surface) / 4;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint8_t g = 0x10 + s->ink[y][x] * 0xe0 / s->swing;
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
    if (!s->swing) {
        error_setg(errp, "uc8179: swing-frames must be at least 1");
        return;
    }
    memset(s->ink, s->swing, sizeof(s->ink));
    uc8179_reset_regs(s);
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
    /* 6: the vendor gray LUT's black + 2 / + 4 white frames read 1/3 and 2/3 */
    DEFINE_PROP_UINT8("swing-frames", Uc8179State, swing, 6),
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
