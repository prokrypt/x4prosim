/*
 * ESP32-S3 LEDC (LED PWM controller), as the X4 Pro frontlight uses it.
 *
 * A register file for the 8 low-speed channels and 4 timers. A channel's
 * output level is latched when the firmware updates it (CONF0 PARA_UP, or
 * CONF1 DUTY_START): the DUTY integer part over the selected timer's
 * resolution, or the idle level while SIG_OUT_EN is off. Every change is
 * traced with the virtual time (esp32s3_ledc_duty), so a light timeline can
 * be lined up with the panel's (uc8179_frame). Hardware fades (DUTY_NUM >
 * 1), the PWM waveform and interrupts are not modeled.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/sysbus.h"
#include "trace.h"

#define TYPE_ESP32S3_LEDC "misc.esp32s3.ledc"
OBJECT_DECLARE_SIMPLE_TYPE(Esp32s3LedcState, ESP32S3_LEDC)

#define LEDC_REGS_SIZE    0x100
#define LEDC_CHANNELS     8
#define LEDC_CH_STRIDE    0x14
#define LEDC_CH_CONF0     0x00
#define LEDC_CH_DUTY      0x08
#define LEDC_CH_CONF1     0x0C
#define LEDC_CH_DUTY_R    0x10
#define LEDC_TIMER0_CONF  0xA0
#define LEDC_DATE         0xFC
#define CONF0_TIMER_SEL   0x3
#define CONF0_SIG_OUT_EN  BIT(2)
#define CONF0_IDLE_LV     BIT(3)
#define CONF0_PARA_UP     BIT(4)
#define CONF1_DUTY_START  BIT(31)
#define TIMER_DUTY_RES    0xF
#define TIMER_RST         BIT(23)
#define TIMER_PARA_UP     BIT(25)
#define DUTY_FRAC_BITS    4

struct Esp32s3LedcState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t regs[LEDC_REGS_SIZE / 4];
    uint32_t level[LEDC_CHANNELS];     /* latched output, in duty steps */
    uint32_t frac[LEDC_CHANNELS];      /* duty[3:0]: 16ths of a step (dithered over 16 cycles) */
    uint32_t max[LEDC_CHANNELS];       /* steps at 100% when latched */
};

static void ledc_latch(Esp32s3LedcState *s, int ch)
{
    uint32_t conf0 = s->regs[(ch * LEDC_CH_STRIDE + LEDC_CH_CONF0) / 4];
    uint32_t timer = s->regs[LEDC_TIMER0_CONF / 4 + 2 * (conf0 & CONF0_TIMER_SEL)];
    uint32_t res = timer & TIMER_DUTY_RES;
    uint32_t max = res ? (1u << res) : 1;
    uint32_t level, frac = 0;

    if (conf0 & CONF0_SIG_OUT_EN) {
        uint32_t duty = s->regs[(ch * LEDC_CH_STRIDE + LEDC_CH_DUTY) / 4];
        level = MIN(duty >> DUTY_FRAC_BITS, max);
        frac = level < max ? duty & ((1u << DUTY_FRAC_BITS) - 1) : 0;
    } else {
        level = conf0 & CONF0_IDLE_LV ? max : 0;
    }
    if (level != s->level[ch] || frac != s->frac[ch] || max != s->max[ch]) {
        s->level[ch] = level;
        s->frac[ch] = frac;
        s->max[ch] = max;
        trace_esp32s3_ledc_duty(qemu_clock_get_us(QEMU_CLOCK_VIRTUAL), ch, level, max, frac);
    }
}

static uint64_t ledc_read(void *opaque, hwaddr addr, unsigned int size)
{
    Esp32s3LedcState *s = opaque;
    int ch = addr / LEDC_CH_STRIDE;

    if (addr < LEDC_CHANNELS * LEDC_CH_STRIDE && addr % LEDC_CH_STRIDE == LEDC_CH_DUTY_R) {
        return s->level[ch] << DUTY_FRAC_BITS | s->frac[ch];
    }
    if (addr == LEDC_DATE) {
        return 0x2003310;   /* ledc_struct.h default */
    }
    return s->regs[addr / 4];
}

static void ledc_write(void *opaque, hwaddr addr, uint64_t value, unsigned int size)
{
    Esp32s3LedcState *s = opaque;
    int ch = addr / LEDC_CH_STRIDE;
    uint32_t v = value;
    bool latch = false;

    if (addr < LEDC_CHANNELS * LEDC_CH_STRIDE) {
        switch (addr % LEDC_CH_STRIDE) {
        case LEDC_CH_CONF0:
            latch = v & CONF0_PARA_UP;
            v &= ~CONF0_PARA_UP;            /* self-clearing */
            break;
        case LEDC_CH_CONF1:
            latch = v & CONF1_DUTY_START;
            v &= ~CONF1_DUTY_START;
            break;
        case LEDC_CH_DUTY_R:
            return;                         /* read-only */
        }
    } else if (addr >= LEDC_TIMER0_CONF && addr < LEDC_TIMER0_CONF + 4 * 8 && addr % 8 == 0) {
        v &= ~(TIMER_PARA_UP | TIMER_RST);
    }
    s->regs[addr / 4] = v;
    if (latch) {
        ledc_latch(s, ch);
    }
}

static const MemoryRegionOps ledc_ops = {
    .read = ledc_read,
    .write = ledc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

/* JSON for the board "state" property: [level, max] per channel */
static char *ledc_get_state(Object *obj, Error **errp)
{
    Esp32s3LedcState *s = ESP32S3_LEDC(obj);
    GString *j = g_string_new("[");

    for (int ch = 0; ch < LEDC_CHANNELS; ch++) {
        g_string_append_printf(j, "%s[%u,%u]", ch ? "," : "", s->level[ch], s->max[ch]);
    }
    g_string_append_c(j, ']');
    return g_string_free(j, false);
}

static void ledc_init(Object *obj)
{
    Esp32s3LedcState *s = ESP32S3_LEDC(obj);
    memory_region_init_io(&s->iomem, obj, &ledc_ops, s, TYPE_ESP32S3_LEDC, LEDC_REGS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    object_property_add_str(obj, "state", ledc_get_state, NULL);
}

static const TypeInfo ledc_info = {
    .name = TYPE_ESP32S3_LEDC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Esp32s3LedcState),
    .instance_init = ledc_init,
};

static void ledc_register_types(void)
{
    type_register_static(&ledc_info);
}

type_init(ledc_register_types)
