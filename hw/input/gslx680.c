/*
 * Silead GSLX680 touch controller on the EEGO Reader A4, I2C 0x40.
 *
 * The host uploads the controller firmware at boot: page select 0xF0, then
 * 128-byte pages written at 0x00-0x7F. Registers form one 256-byte file with
 * an auto-incrementing pointer (the first byte of a write). Faked: the
 * firmware is not run; once any page byte has arrived, the status word 0xB0
 * reads 5A 5A 5A 5A ("loaded"), and the contact frame at 0x80 (count, then
 * 4 bytes per contact) always reads zero, so no touches are reported.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/i2c/i2c.h"

#define TYPE_GSLX680 "gslx680"
OBJECT_DECLARE_SIMPLE_TYPE(Gslx680State, GSLX680)

#define GSL_FRAME   0x80    /* 0x80..0x97: contact frame */
#define GSL_STATUS  0xB0

struct Gslx680State {
    I2CSlave parent_obj;
    uint8_t regs[256];
    uint8_t ptr;
    bool ptr_next;
    bool loaded;
};

static int gslx680_event(I2CSlave *i2c, enum i2c_event event)
{
    if (event == I2C_START_SEND) {
        GSLX680(i2c)->ptr_next = true;
    }
    return 0;
}

static uint8_t gslx680_recv(I2CSlave *i2c)
{
    Gslx680State *s = GSLX680(i2c);
    uint8_t r = s->ptr++;

    if (r >= GSL_STATUS && r < GSL_STATUS + 4) {
        return s->loaded ? 0x5A : 0;
    }
    if (r >= GSL_FRAME && r < GSL_FRAME + 24) {
        return 0;
    }
    return s->regs[r];
}

static int gslx680_send(I2CSlave *i2c, uint8_t data)
{
    Gslx680State *s = GSLX680(i2c);

    if (s->ptr_next) {
        s->ptr_next = false;
        s->ptr = data;
        return 0;
    }
    if (s->ptr < GSL_FRAME) {
        s->loaded = true;
    }
    s->regs[s->ptr++] = data;
    return 0;
}

static void gslx680_class_init(ObjectClass *klass, void *data)
{
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(klass);

    sc->event = gslx680_event;
    sc->recv = gslx680_recv;
    sc->send = gslx680_send;
}

static const TypeInfo gslx680_info = {
    .name = TYPE_GSLX680,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(Gslx680State),
    .class_init = gslx680_class_init,
};

static void gslx680_register_types(void)
{
    type_register_static(&gslx680_info);
}

type_init(gslx680_register_types)
