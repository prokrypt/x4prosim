/*
 * ESP32 RTC_CNTL (RTC block controller) device
 *
 * x4prosim adds light sleep (SLEEP_EN until the RTC timer or a GPIO wake) and
 * deep sleep (SLEEP_EN with DIG_PWC DG_WRAP_PD_EN): the VM is suspended until
 * ext0, ext1 or the RTC timer fires, then reset with reason DEEPSLEEP_RESET and
 * the cause in SLP_WAKEUP_CAUSE (ext1 pads in EXT_WAKEUP1_STATUS).
 *
 * Copyright (c) 2019-2024 Espressif Systems (Shanghai) Co. Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "hw/hw.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/misc/esp32s3_reg.h"
#include "hw/misc/esp32s3_rtc_cntl.h"
#include "sysemu/runstate.h"

static void esp32s3_rtc_update_cpu_stall(Esp32s3RtcCntlState* s);
static void esp32s3_rtc_update_clk(Esp32s3RtcCntlState* s);


#define R_SLP_TIMER0        0x04
#define R_SLP_TIMER1        0x08
#define R_STATE0            0x18
#define R_WAKEUP_STATE      0x3C
#define R_INT_ENA           0x40
#define R_INT_RAW           0x44
#define R_INT_ST            0x48
#define R_INT_CLR           0x4C
#define R_SLP_WAKEUP_CAUSE  0x130
#define R_EXT_WAKEUP_CONF   0x64
#define R_DIG_PWC           0x90
#define R_EXT_WAKEUP1       0xE0
#define R_EXT_WAKEUP1_STATUS 0xE4
#define STATE0_SLEEP_EN     BIT(31)
#define SLP_TIMER1_ALARM_EN BIT(16)
#define INT_SLP_WAKEUP      BIT(0)
#define WAKE_EXT0           BIT(0)
#define WAKE_EXT1           BIT(1)
#define WAKE_GPIO           BIT(2)
#define WAKE_TIMER          BIT(3)
#define LIGHT_SLEEP_EARLY_NS (5 * SCALE_MS)
#define DG_WRAP_PD_EN       BIT(31)
#define EXT_WAKEUP0_LV      BIT(30)
#define EXT_WAKEUP1_LV      BIT(31)
#define EXT_WAKEUP1_SEL     0x3fffff
#define EXT_WAKEUP1_STATUS_CLR BIT(22)
#define PADS_ALL            ((1u << ESP32S3_RTC_PAD_COUNT) - 1)

static uint32_t esp32s3_rtc_wake_ena(Esp32s3RtcCntlState *s)
{
    return (s->wakeup_state_reg >> 15) & 0x1ffff;
}

/*
 * Deep-sleep pad sources at their wake level: ext0 is one pad (RTC_IO
 * EXT_WAKEUP0 SEL) at EXT_WAKEUP0_LV; ext1 is any selected pad high (LV 1)
 * or low (LV 0). *ext1_pads gets the ext1 pads that match.
 */
static uint32_t esp32s3_rtc_pads_awake(Esp32s3RtcCntlState *s, uint32_t *ext1_pads)
{
    uint32_t ena = esp32s3_rtc_wake_ena(s), cause = 0;
    uint32_t sel0 = s->ext0_reg >> 27;
    uint32_t lv1_pads = (s->ext_wakeup_conf & EXT_WAKEUP1_LV) ? s->pad_level : ~s->pad_level;

    *ext1_pads = lv1_pads & s->ext_wakeup1 & EXT_WAKEUP1_SEL;
    if ((ena & WAKE_EXT0) && sel0 < ESP32S3_RTC_PAD_COUNT &&
        ((s->pad_level >> sel0) & 1) == !!(s->ext_wakeup_conf & EXT_WAKEUP0_LV)) {
        cause |= WAKE_EXT0;
    }
    if ((ena & WAKE_EXT1) && *ext1_pads) {
        cause |= WAKE_EXT1;
    }
    return cause;
}

/* Deep sleep ends in a reset: the ROM boots again and reports DEEPSLEEP_RESET plus the cause. */
static void esp32s3_rtc_deep_wake(Esp32s3RtcCntlState *s, uint32_t cause, uint32_t ext1_pads)
{
    if (!s->sleeping) {
        return;
    }
    s->sleeping = false;
    timer_del(&s->sleep_timer);
    timer_del(&s->deep_timer);
    /* The RTC counter kept running while the VM (and the virtual clock) was suspended. */
    s->time_base_ns -= qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - s->deep_start_rt_ns;
    /*
     * The SoC reset doesn't reach this device (the RTC domain keeps its
     * registers and counter), so end the sleep state here.
     */
    s->state0_reg &= ~STATE0_SLEEP_EN;
    s->int_raw = 0;
    s->wakeup_cause = cause;
    s->ext_wakeup1_status = (cause & WAKE_EXT1) ? ext1_pads : 0;
    s->reset_cause[0] = ESP32_DEEPSLEEP_RESET;
    s->reset_cause[1] = ESP32_DEEPSLEEP_RESET;
    qemu_system_wakeup_request(QEMU_WAKEUP_REASON_OTHER, NULL);
    qemu_irq_pulse(s->dig_reset_req);
}

static void esp32s3_rtc_deep_timer_cb(void *opaque)
{
    esp32s3_rtc_deep_wake(opaque, WAKE_TIMER, 0);
}

static void esp32s3_rtc_start_deep_sleep(Esp32s3RtcCntlState *s)
{
    uint32_t ext1_pads, cause = esp32s3_rtc_pads_awake(s, &ext1_pads);

    s->sleeping = true;
    s->deep_start_rt_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    if (cause) {
        esp32s3_rtc_deep_wake(s, cause, ext1_pads);
        return;
    }
    if ((esp32s3_rtc_wake_ena(s) & WAKE_TIMER) && (s->slp_timer[1] & SLP_TIMER1_ALARM_EN)) {
        uint64_t target = s->slp_timer[0] | ((uint64_t)(s->slp_timer[1] & 0xffff) << 32);
        int64_t left = s->time_base_ns + muldiv64(target, NANOSECONDS_PER_SECOND, s->rtc_slowclk_freq)
                       - qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        /* Too short to bother suspending (and racing the suspend request). */
        if (left < SCALE_MS) {
            esp32s3_rtc_deep_wake(s, WAKE_TIMER, 0);
            return;
        }
        /* shortcut: the sleep lasts its length in host time, not scaled to the icount rate */
        timer_mod_ns(&s->deep_timer, s->deep_start_rt_ns + left);
    }
    /*
     * The digital core is off: suspend the VM so the virtual clock stops and
     * no watchdog bites the CPU spinning on INT_RAW. A pad or the deep timer
     * resumes it into the reset. No wake source = asleep until a reset (EN).
     */
    qemu_system_suspend_request();
}

static void esp32s3_rtc_set_pad(void *opaque, int n, int level)
{
    Esp32s3RtcCntlState *s = opaque;
    uint32_t ext1_pads, cause;

    s->pad_level = (s->pad_level & ~BIT(n)) | (level ? BIT(n) : 0);
    if (s->sleeping && (s->dig_pwc & DG_WRAP_PD_EN)) {
        cause = esp32s3_rtc_pads_awake(s, &ext1_pads);
        if (cause) {
            esp32s3_rtc_deep_wake(s, cause, ext1_pads);
        }
    }
}

static uint64_t esp32s3_rtcio_ext0_read(void *opaque, hwaddr addr, unsigned int size)
{
    return ((Esp32s3RtcCntlState *)opaque)->ext0_reg;
}

static void esp32s3_rtcio_ext0_write(void *opaque, hwaddr addr, uint64_t value, unsigned int size)
{
    ((Esp32s3RtcCntlState *)opaque)->ext0_reg = value;
}

static const MemoryRegionOps esp32s3_rtcio_ext0_ops = {
    .read = esp32s3_rtcio_ext0_read,
    .write = esp32s3_rtcio_ext0_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void esp32s3_rtc_wake(Esp32s3RtcCntlState *s, uint32_t cause)
{
    if (!s->sleeping) {
        return;
    }
    s->sleeping = false;
    timer_del(&s->sleep_timer);
    /*
     * The systimer (esp_timer) keeps counting in QEMU while the guest "sleeps",
     * but IDF also advances it by the RTC-measured sleep time. Hide the sleep
     * from the RTC counter so the time is only counted once.
     */
    s->time_base_ns += qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->sleep_start_ns;
    s->state0_reg &= ~STATE0_SLEEP_EN;
    s->wakeup_cause = cause;
    s->int_raw |= INT_SLP_WAKEUP;
    qemu_set_irq(s->irq, (s->int_raw & s->int_ena) != 0);
}

static void esp32s3_rtc_sleep_timer_cb(void *opaque)
{
    esp32s3_rtc_wake(opaque, WAKE_TIMER);
}

/* Light sleep: the CPU spins on INT_RAW until a wake source fires. */
static void esp32s3_rtc_start_sleep(Esp32s3RtcCntlState *s)
{
    if (s->dig_pwc & DG_WRAP_PD_EN) {
        esp32s3_rtc_start_deep_sleep(s);
        return;
    }
    uint32_t ena = (s->wakeup_state_reg >> 15) & 0x1ffff;
    bool timer = (ena & WAKE_TIMER) && (s->slp_timer[1] & SLP_TIMER1_ALARM_EN);

    s->sleeping = true;
    s->sleep_start_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if ((ena & WAKE_GPIO) && s->gpio_wake) {
        esp32s3_rtc_wake(s, WAKE_GPIO);
        return;
    }
    if (timer) {
        uint64_t target = s->slp_timer[0] | ((uint64_t)(s->slp_timer[1] & 0xffff) << 32);
        /*
         * Wake a little early: TCG runs the sleep entry/exit code slower than
         * silicon, and FreeRTOS asserts if the measured sleep overshoots the
         * idle time it planned (vTaskStepTick).
         */
        int64_t ns = s->time_base_ns + muldiv64(target, NANOSECONDS_PER_SECOND, s->rtc_slowclk_freq)
                     - LIGHT_SLEEP_EARLY_NS;
        if (ns > qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)) {
            timer_mod_ns(&s->sleep_timer, ns);
            return;
        }
        esp32s3_rtc_wake(s, WAKE_TIMER);
        return;
    }
    if (!(ena & WAKE_GPIO)) {
        /* No wake source we model: don't hang the guest. */
        esp32s3_rtc_wake(s, 0);
    }
}

static void esp32s3_rtc_set_gpio_wake(void *opaque, int n, int level)
{
    Esp32s3RtcCntlState *s = opaque;
    s->gpio_wake = level != 0;
    if (s->gpio_wake && s->sleeping && !(s->dig_pwc & DG_WRAP_PD_EN) && (((s->wakeup_state_reg >> 15) & 0x1ffff) & WAKE_GPIO)) {
        esp32s3_rtc_wake(s, WAKE_GPIO);
    }
}

static uint64_t esp32s3_rtc_cntl_read(void *opaque, hwaddr addr, unsigned int size)
{
    Esp32s3RtcCntlState *s = ESP32S3_RTC_CNTL(opaque);
    uint64_t r = 0;
    switch (addr) {
    case R_SLP_TIMER0:       r = s->slp_timer[0]; break;
    case R_SLP_TIMER1:       r = s->slp_timer[1]; break;
    case R_STATE0:           r = s->state0_reg; break;
    case R_WAKEUP_STATE:     r = s->wakeup_state_reg; break;
    case R_INT_ENA:          r = s->int_ena; break;
    case R_INT_RAW:          r = s->int_raw; break;
    case R_INT_ST:           r = s->int_raw & s->int_ena; break;
    case R_SLP_WAKEUP_CAUSE: r = s->wakeup_cause; break;
    case R_EXT_WAKEUP_CONF:  r = s->ext_wakeup_conf; break;
    case R_DIG_PWC:          r = s->dig_pwc; break;
    case R_EXT_WAKEUP1:      r = s->ext_wakeup1; break;
    case R_EXT_WAKEUP1_STATUS: r = s->ext_wakeup1_status; break;
    case A_RTC_CNTL_OPTIONS0:
        r = s->options0_reg;
        break;
    case A_RTC_CNTL_TIME_UPDATE:
        r = R_RTC_CNTL_TIME_UPDATE_VALID_MASK;
        break;
    case A_RTC_CNTL_TIME0:
        r = s->time_reg & UINT32_MAX;
        break;
    case A_RTC_CNTL_TIME1:
        r = s->time_reg >> 32;
        break;

    case A_RTC_CNTL_RESET_STATE:
        r = FIELD_DP32(r, RTC_CNTL_RESET_STATE, RESET_CAUSE_PROCPU, s->reset_cause[0]);
        r = FIELD_DP32(r, RTC_CNTL_RESET_STATE, RESET_CAUSE_APPCPU, s->reset_cause[1]);
        r = FIELD_DP32(r, RTC_CNTL_RESET_STATE, PROCPU_STAT_VECTOR_SEL, s->stat_vector_sel[0]);
        r = FIELD_DP32(r, RTC_CNTL_RESET_STATE, APPCPU_STAT_VECTOR_SEL, s->stat_vector_sel[1]);
        break;

    case A_RTC_CNTL_STORE0:
    case A_RTC_CNTL_STORE1:
    case A_RTC_CNTL_STORE2:
    case A_RTC_CNTL_STORE3:
        r = s->scratch_reg[(addr - A_RTC_CNTL_STORE0) / 4];
        break;

    case A_RTC_CNTL_CLK_CONF:
        r = FIELD_DP32(r, RTC_CNTL_CLK_CONF, SOC_CLK_SEL, s->soc_clk);
        r = FIELD_DP32(r, RTC_CNTL_CLK_CONF, FAST_CLK_RTC_SEL, s->rtc_fastclk);
        r = FIELD_DP32(r, RTC_CNTL_CLK_CONF, ANA_CLK_RTC_SEL, s->rtc_slowclk);
        break;

    case A_RTC_CNTL_SW_CPU_STALL:
        r = s->sw_cpu_stall_reg;
        break;

    case A_RTC_CNTL_STORE4:
    case A_RTC_CNTL_STORE5:
    case A_RTC_CNTL_STORE6:
    case A_RTC_CNTL_STORE7:
        r = s->scratch_reg[(addr - A_RTC_CNTL_STORE4) / 4 + 4];
        break;
    }
    return r;
}

static void esp32s3_rtc_cntl_write(void *opaque, hwaddr addr, uint64_t value,
                                 unsigned int size)
{
    Esp32s3RtcCntlState *s = ESP32S3_RTC_CNTL(opaque);
    switch (addr) {
    case R_SLP_TIMER0:   s->slp_timer[0] = value; break;
    case R_SLP_TIMER1:   s->slp_timer[1] = value; break;
    case R_WAKEUP_STATE: s->wakeup_state_reg = value; break;
    case R_EXT_WAKEUP_CONF: s->ext_wakeup_conf = value; break;
    case R_DIG_PWC:      s->dig_pwc = value; break;
    case R_EXT_WAKEUP1:
        s->ext_wakeup1 = value & EXT_WAKEUP1_SEL;
        if (value & EXT_WAKEUP1_STATUS_CLR) {
            s->ext_wakeup1_status = 0;
        }
        break;
    case R_INT_ENA:
        s->int_ena = value;
        qemu_set_irq(s->irq, (s->int_raw & s->int_ena) != 0);
        break;
    case R_INT_CLR:
        s->int_raw &= ~value;
        qemu_set_irq(s->irq, (s->int_raw & s->int_ena) != 0);
        break;
    case R_STATE0:
        s->state0_reg = value;
        if ((value & STATE0_SLEEP_EN) && !s->sleeping) {
            esp32s3_rtc_start_sleep(s);
        }
        break;
    case A_RTC_CNTL_OPTIONS0:
        if (value & R_RTC_CNTL_OPTIONS0_SW_SYS_RESET_MASK) {
            s->reset_cause[0] = ESP32_SW_SYS_RESET;
            s->reset_cause[1] = ESP32_SW_SYS_RESET;
            qemu_irq_pulse(s->dig_reset_req);
            value &= ~(R_RTC_CNTL_OPTIONS0_SW_SYS_RESET_MASK);
        }
        if (value & R_RTC_CNTL_OPTIONS0_SW_APPCPU_RESET_MASK) {
            s->reset_cause[1] = ESP32_SW_CPU_RESET;
            qemu_irq_pulse(s->cpu_reset_req[1]);
            value &= ~(R_RTC_CNTL_OPTIONS0_SW_APPCPU_RESET_MASK);
        }
        if (value & R_RTC_CNTL_OPTIONS0_SW_PROCPU_RESET_MASK) {
            s->reset_cause[0] = ESP32_SW_CPU_RESET;
            qemu_irq_pulse(s->cpu_reset_req[0]);
            value &= ~(R_RTC_CNTL_OPTIONS0_SW_PROCPU_RESET_MASK);
        }
        s->options0_reg = value;
        esp32s3_rtc_update_cpu_stall(s);
        break;

    case A_RTC_CNTL_TIME_UPDATE:
        if (value & R_RTC_CNTL_TIME_UPDATE_UPDATE_MASK) {
            s->time_reg = muldiv64(
                qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->time_base_ns,
                s->rtc_slowclk_freq, NANOSECONDS_PER_SECOND);
        }
        break;

    case A_RTC_CNTL_RESET_STATE:
        s->stat_vector_sel[0] = FIELD_EX32(value, RTC_CNTL_RESET_STATE,
                                           PROCPU_STAT_VECTOR_SEL);
        s->stat_vector_sel[1] = FIELD_EX32(value, RTC_CNTL_RESET_STATE,
                                           APPCPU_STAT_VECTOR_SEL);
        break;

    case A_RTC_CNTL_STORE0:
    case A_RTC_CNTL_STORE1:
    case A_RTC_CNTL_STORE2:
    case A_RTC_CNTL_STORE3:
        s->scratch_reg[(addr - A_RTC_CNTL_STORE0) / 4] = value;
        break;

    case A_RTC_CNTL_CLK_CONF:
        s->soc_clk = FIELD_EX32(value, RTC_CNTL_CLK_CONF, SOC_CLK_SEL);
        s->rtc_fastclk = FIELD_EX32(value, RTC_CNTL_CLK_CONF, FAST_CLK_RTC_SEL);
        s->rtc_slowclk = FIELD_EX32(value, RTC_CNTL_CLK_CONF, ANA_CLK_RTC_SEL);
        esp32s3_rtc_update_clk(s);
        break;

    case A_RTC_CNTL_SW_CPU_STALL:
        s->sw_cpu_stall_reg = value;
        esp32s3_rtc_update_cpu_stall(s);
        break;

    case A_RTC_CNTL_STORE4:
    case A_RTC_CNTL_STORE5:
    case A_RTC_CNTL_STORE6:
    case A_RTC_CNTL_STORE7:
        s->scratch_reg[(addr - A_RTC_CNTL_STORE4) / 4 + 4] = value;
        break;
    }
}

static void esp32s3_rtc_update_cpu_stall(Esp32s3RtcCntlState* s)
{
    uint32_t procpu_stall = (FIELD_EX32(s->sw_cpu_stall_reg, RTC_CNTL_SW_CPU_STALL, PROCPU_C1) << 2) |
                            (FIELD_EX32(s->options0_reg, RTC_CNTL_OPTIONS0, SW_STALL_PROCPU_C0));

    uint32_t appcpu_stall = (FIELD_EX32(s->sw_cpu_stall_reg, RTC_CNTL_SW_CPU_STALL, APPCPU_C1) << 2) |
                            (FIELD_EX32(s->options0_reg, RTC_CNTL_OPTIONS0, SW_STALL_APPCPU_C0));

    const uint32_t stall_magic_val = 0x86;

    s->cpu_stall_state[0] = procpu_stall == stall_magic_val;
    s->cpu_stall_state[1] = appcpu_stall == stall_magic_val;

    qemu_set_irq(s->cpu_stall_req[0], s->cpu_stall_state[0]);
    qemu_set_irq(s->cpu_stall_req[1], s->cpu_stall_state[1]);
}

static void esp32s3_rtc_update_clk(Esp32s3RtcCntlState* s)
{
    const uint32_t slowclk_freq[] = {150000, 32768, 8000000/256};
    const uint32_t fastclk_freq[] = {s->xtal_apb_freq / 4, 8000000};
    s->rtc_slowclk_freq = slowclk_freq[s->rtc_slowclk];
    s->rtc_fastclk_freq = fastclk_freq[s->rtc_fastclk];
    qemu_irq_pulse(s->clk_update);
}

static const MemoryRegionOps esp32s3_rtc_cntl_ops = {
    .read =  esp32s3_rtc_cntl_read,
    .write = esp32s3_rtc_cntl_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void esp32s3_rtc_cntl_reset_hold(Object *obj, ResetType type)
{
    Esp32s3RtcCntlState *s = ESP32S3_RTC_CNTL(obj);

    s->time_base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->sleeping = false;
    s->int_raw = 0;
    s->state0_reg = 0;
    timer_del(&s->sleep_timer);
    timer_del(&s->deep_timer);
}

static void esp32s3_rtc_cntl_realize(DeviceState *dev, Error **errp)
{
}

static void esp32s3_rtc_cntl_init(Object *obj)
{
    Esp32s3RtcCntlState *s = ESP32S3_RTC_CNTL(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &esp32s3_rtc_cntl_ops, s,
                          TYPE_ESP32S3_RTC_CNTL, ESP32S3_RTC_CNTL_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    qdev_init_gpio_out_named(DEVICE(sbd), &s->dig_reset_req, ESP32S3_RTC_DIG_RESET_GPIO, 1);
    qdev_init_gpio_out_named(DEVICE(sbd), &s->cpu_reset_req[0], ESP32S3_RTC_CPU_RESET_GPIO, ESP32S3_CPU_COUNT);
    qdev_init_gpio_out_named(DEVICE(sbd), &s->cpu_stall_req[0], ESP32S3_RTC_CPU_STALL_GPIO, ESP32S3_CPU_COUNT);
    qdev_init_gpio_out_named(DEVICE(sbd), &s->clk_update, ESP32S3_RTC_CLK_UPDATE_GPIO, 1);
    qdev_init_gpio_in_named(DEVICE(sbd), esp32s3_rtc_set_gpio_wake, ESP32S3_RTC_GPIO_WAKE, 1);
    timer_init_ns(&s->sleep_timer, QEMU_CLOCK_VIRTUAL, esp32s3_rtc_sleep_timer_cb, s);
    qdev_init_gpio_in_named(DEVICE(sbd), esp32s3_rtc_set_pad, ESP32S3_RTC_PAD, ESP32S3_RTC_PAD_COUNT);
    memory_region_init_io(&s->rtcio_ext0, obj, &esp32s3_rtcio_ext0_ops, s, "esp32s3.rtcio.ext_wakeup0", 4);
    sysbus_init_mmio(sbd, &s->rtcio_ext0);
    timer_init_ns(&s->deep_timer, QEMU_CLOCK_REALTIME, esp32s3_rtc_deep_timer_cb, s);
    qemu_system_wakeup_enable(QEMU_WAKEUP_REASON_OTHER, true);
    s->pad_level = PADS_ALL;

    for (int i = 0; i < ESP32S3_CPU_COUNT; ++i) {
        s->reset_cause[i] = ESP32_POWERON_RESET;
        s->stat_vector_sel[i] = true;
    }

    s->rtc_slowclk = ESP32_SLOW_CLK_RC;
    s->rtc_fastclk = ESP32_FAST_CLK_8M;
    s->soc_clk = ESP32_SOC_CLK_XTAL;
    s->xtal_apb_freq = 40000000;
    s->pll_apb_freq = 80000000;
    esp32s3_rtc_update_clk(s);
}

static Property esp32s3_rtc_cntl_properties[] = {
    DEFINE_PROP_END_OF_LIST(),
};

static void esp32s3_rtc_cntl_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = esp32s3_rtc_cntl_reset_hold;
    dc->realize = esp32s3_rtc_cntl_realize;
    device_class_set_props(dc, esp32s3_rtc_cntl_properties);
}

static const TypeInfo esp32s3_rtc_cntl_info = {
    .name = TYPE_ESP32S3_RTC_CNTL,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Esp32s3RtcCntlState),
    .instance_init = esp32s3_rtc_cntl_init,
    .class_init = esp32s3_rtc_cntl_class_init
};

static void esp32s3_rtc_cntl_register_types(void)
{
    type_register_static(&esp32s3_rtc_cntl_info);
}

type_init(esp32s3_rtc_cntl_register_types)
