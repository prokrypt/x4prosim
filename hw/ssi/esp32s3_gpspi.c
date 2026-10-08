/*
 * ESP32-S3 general-purpose SPI master (SPI2/SPI3), CPU-driven mode only.
 *
 * Setting CMD.USR shifts MS_DLEN+1 bits out of W0..W15 (byte 0 = W0 bits 7:0)
 * onto the SSI bus and stores what comes back in the same buffer, then raises
 * TRANS_DONE. CMD.UPDATE self-clears. DMA, address/command/dummy phases and
 * hardware CS are not modeled: the X4 Pro drives CS and DC from GPIO.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/ssi/ssi.h"

#define TYPE_ESP32S3_GPSPI "ssi.esp32s3.gpspi"
OBJECT_DECLARE_SIMPLE_TYPE(Esp32s3GpspiState, ESP32S3_GPSPI)

#define R_CMD           0x00
#define R_MS_DLEN       0x1C
#define R_DMA_INT_ENA   0x34
#define R_DMA_INT_CLR   0x38
#define R_DMA_INT_RAW   0x3C
#define R_DMA_INT_ST    0x40
#define R_DMA_INT_SET   0x44
#define R_W0            0x98
#define R_W15           0xD4
#define R_DATE          0xF0
#define REGS_SIZE       0x100

#define CMD_UPDATE      BIT(23)
#define CMD_USR         BIT(24)
#define INT_TRANS_DONE  BIT(12)

struct Esp32s3GpspiState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    SSIBus *bus;
    uint32_t regs[REGS_SIZE / 4];
    uint32_t int_raw;
    uint32_t int_ena;
};

static void gpspi_update_irq(Esp32s3GpspiState *s)
{
    qemu_set_irq(s->irq, (s->int_raw & s->int_ena) != 0);
}

static void gpspi_transfer(Esp32s3GpspiState *s)
{
    uint8_t *buf = (uint8_t *)&s->regs[R_W0 / 4];
    uint32_t bytes = ((s->regs[R_MS_DLEN / 4] & 0x3ffff) + 1 + 7) / 8;

    if (bytes > 64) {
        bytes = 64;
    }
    for (uint32_t i = 0; i < bytes; i++) {
        /* W registers hold bytes little-endian; the host is assumed LE too. */
        buf[i] = ssi_transfer(s->bus, buf[i]);
    }
    s->int_raw |= INT_TRANS_DONE;
}

static uint64_t gpspi_read(void *opaque, hwaddr addr, unsigned int size)
{
    Esp32s3GpspiState *s = opaque;
    switch (addr) {
    case R_DMA_INT_RAW: return s->int_raw;
    case R_DMA_INT_ST:  return s->int_raw & s->int_ena;
    case R_DMA_INT_ENA: return s->int_ena;
    case R_DATE:        return 0x2101190;
    }
    return s->regs[addr / 4];
}

static void gpspi_write(void *opaque, hwaddr addr, uint64_t value, unsigned int size)
{
    Esp32s3GpspiState *s = opaque;

    switch (addr) {
    case R_CMD:
        s->regs[0] = value & ~(CMD_UPDATE | CMD_USR);
        if (value & CMD_USR) {
            gpspi_transfer(s);
        }
        break;
    case R_DMA_INT_ENA: s->int_ena = value; break;
    case R_DMA_INT_CLR: s->int_raw &= ~value; break;
    case R_DMA_INT_SET: s->int_raw |= value; break;
    default:
        s->regs[addr / 4] = value;
        break;
    }
    gpspi_update_irq(s);
}

static const MemoryRegionOps gpspi_ops = {
    .read = gpspi_read,
    .write = gpspi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void gpspi_init(Object *obj)
{
    Esp32s3GpspiState *s = ESP32S3_GPSPI(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &gpspi_ops, s, TYPE_ESP32S3_GPSPI, REGS_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    s->bus = ssi_create_bus(DEVICE(obj), "spi");
}

static const TypeInfo gpspi_info = {
    .name = TYPE_ESP32S3_GPSPI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Esp32s3GpspiState),
    .instance_init = gpspi_init,
};

static void gpspi_register_types(void)
{
    type_register_static(&gpspi_info);
}

type_init(gpspi_register_types)
