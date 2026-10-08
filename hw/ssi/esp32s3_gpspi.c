/*
 * ESP32-S3 general-purpose SPI master (SPI2), CPU and GDMA modes.
 *
 * Setting CMD.USR runs one transaction on the SSI bus: the command phase
 * (USER2 value, low byte first), the address phase (ADDR from bit 31 down),
 * dummy cycles (USER1 cyclelen / 8 bytes of 0xFF), then the data phase of
 * MS_DLEN+1 bits. Data comes from W0..W15 (byte 0 = W0 bits 7:0), or from the
 * GDMA out channel bound to SPI2 when DMA_TX_ENA is set; received bytes go to
 * W0..W15 or the GDMA in channel (DMA_RX_ENA). MOSI-only and MISO-only
 * (half-duplex) and full-duplex transfers work. The "cs0" output goes low for
 * the transaction unless CS0_DIS, and stays low with CS_KEEP_ACTIVE. Phases
 * are rounded up to whole bytes and dual/quad/octal modes are sent as 1-bit.
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
#include "hw/qdev-properties.h"
#include "hw/dma/esp_gdma.h"

#define TYPE_ESP32S3_GPSPI "ssi.esp32s3.gpspi"
OBJECT_DECLARE_SIMPLE_TYPE(Esp32s3GpspiState, ESP32S3_GPSPI)

#define R_CMD           0x00
#define R_ADDR          0x04
#define R_USER          0x10
#define R_USER1         0x14
#define R_USER2         0x18
#define R_MS_DLEN       0x1C
#define R_MISC          0x20
#define R_DMA_CONF      0x30
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
#define USER_DOUTDIN    BIT(0)
#define USER_MOSI       BIT(27)
#define USER_MISO       BIT(28)
#define USER_DUMMY      BIT(29)
#define USER_ADDR       BIT(30)
#define USER_COMMAND    BIT(31)
#define MISC_CS0_DIS    BIT(0)
#define MISC_CS_KEEP    BIT(30)
#define DMA_RX_ENA      BIT(27)
#define DMA_TX_ENA      BIT(28)
#define MAX_DATA        (1 << 15)   /* MS_DLEN is 18 bits */

struct Esp32s3GpspiState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    qemu_irq cs0;
    SSIBus *bus;
    ESPGdmaState *gdma;
    uint8_t data[MAX_DATA];
    uint32_t regs[REGS_SIZE / 4];
    uint32_t int_raw;
    uint32_t int_ena;
};

static void gpspi_update_irq(Esp32s3GpspiState *s)
{
    qemu_set_irq(s->irq, (s->int_raw & s->int_ena) != 0);
}

static void gpspi_send_bytes(Esp32s3GpspiState *s, uint32_t v, int bits, int first_shift)
{
    /* first_shift picks where the first byte sits: 0 = low byte first, 24 = high byte first. */
    for (int i = 0; i < (bits + 7) / 8; i++) {
        ssi_transfer(s->bus, (v >> (first_shift ? first_shift - 8 * i : 8 * i)) & 0xff);
    }
}

static void gpspi_transfer(Esp32s3GpspiState *s)
{
    uint32_t user = s->regs[R_USER / 4], dma = s->regs[R_DMA_CONF / 4];
    uint32_t misc = s->regs[R_MISC / 4];
    uint32_t bytes = ((s->regs[R_MS_DLEN / 4] & 0x3ffff) + 1 + 7) / 8;
    bool tx = user & (USER_MOSI | USER_DOUTDIN), rx = user & (USER_MISO | USER_DOUTDIN);
    bool tx_dma = tx && (dma & DMA_TX_ENA), rx_dma = rx && (dma & DMA_RX_ENA);
    uint8_t *w = (uint8_t *)&s->regs[R_W0 / 4];
    uint8_t *buf = (tx_dma || rx_dma) ? s->data : w;
    uint32_t max = (tx_dma || rx_dma) ? MAX_DATA : 64;
    uint32_t chan;

    if (bytes > max) {
        bytes = max;
    }
    if (!(misc & MISC_CS0_DIS)) {
        qemu_set_irq(s->cs0, 0);
    }
    if (user & USER_COMMAND) {
        gpspi_send_bytes(s, s->regs[R_USER2 / 4] & 0xffff, ((s->regs[R_USER2 / 4] >> 28) & 0xf) + 1, 0);
    }
    if (user & USER_ADDR) {
        gpspi_send_bytes(s, s->regs[R_ADDR / 4], ((s->regs[R_USER1 / 4] >> 27) & 0x1f) + 1, 24);
    }
    if (user & USER_DUMMY) {
        for (int i = 0; i < ((s->regs[R_USER1 / 4] & 0xff) + 1 + 7) / 8; i++) {
            ssi_transfer(s->bus, 0xff);
        }
    }
    if (tx || rx) {
        if (tx_dma && !(s->gdma && esp_gdma_get_channel_periph(s->gdma, GDMA_SPI2, ESP_GDMA_OUT_IDX, &chan)
                         && esp_gdma_read_channel(s->gdma, chan, buf, bytes))) {
            qemu_log_mask(LOG_GUEST_ERROR, "gpspi: no GDMA out data for %u bytes\n", bytes);
            memset(buf, 0xff, bytes);
        } else if (!tx) {
            memset(buf, 0xff, bytes);
        } else if (!tx_dma && buf != w) {
            memcpy(buf, w, MIN(bytes, 64));
        }
        for (uint32_t i = 0; i < bytes; i++) {
            /* W registers hold bytes little-endian; the host is assumed LE too. */
            uint8_t in = ssi_transfer(s->bus, buf[i]);
            if (rx) {
                buf[i] = in;
            }
        }
        if (rx_dma && !(s->gdma && esp_gdma_get_channel_periph(s->gdma, GDMA_SPI2, ESP_GDMA_IN_IDX, &chan)
                        && esp_gdma_write_channel(s->gdma, chan, buf, bytes))) {
            qemu_log_mask(LOG_GUEST_ERROR, "gpspi: no GDMA in buffer for %u bytes\n", bytes);
        }
    }
    if (!(misc & MISC_CS_KEEP)) {
        qemu_set_irq(s->cs0, 1);
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
    qdev_init_gpio_out_named(DEVICE(obj), &s->cs0, "cs0", 1);
}

static Property gpspi_properties[] = {
    DEFINE_PROP_LINK("gdma", Esp32s3GpspiState, gdma, TYPE_ESP_GDMA, ESPGdmaState *),
    DEFINE_PROP_END_OF_LIST(),
};

static void gpspi_class_init(ObjectClass *klass, void *data)
{
    device_class_set_props(DEVICE_CLASS(klass), gpspi_properties);
}

static const TypeInfo gpspi_info = {
    .name = TYPE_ESP32S3_GPSPI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Esp32s3GpspiState),
    .instance_init = gpspi_init,
    .class_init = gpspi_class_init,
};

static void gpspi_register_types(void)
{
    type_register_static(&gpspi_info);
}

type_init(gpspi_register_types)
