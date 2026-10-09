/*
 * SSI to SD card adapter.
 *
 * Optional uint32 QOM latencies, in microseconds (all default to zero):
 * read-repeat-us / read-seq-access-us / read-access-us delay the first CMD17/18
 * token from R1 for repeated, sequential and random starts, respectively.
 * read-next-us delays subsequent CMD18 tokens from the preceding block CRC.
 * CMD24 busy after its data response uses write-repeat-busy-us, write-busy-us
 * or write-random-busy-us for those patterns; CMD25 uses write-block-busy-us.
 * STOP_TRAN busy is max(0, write-stop-busy-us + random_extra - decrement * n),
 * where n is the completed CMD25 block count, decrement is
 * write-stop-decrement-us and random_extra is write-stop-random-extra-us for
 * random starts (zero otherwise). Clamp the result to UINT32_MAX microseconds.
 * Repeated means the previous command's start sector; it takes precedence over
 * sequential, the sector after the last completed block in the same direction.
 * Deadlines use QEMU_CLOCK_VIRTUAL: polling returns 0xff before a read token,
 * or 0x00 during write busy, without sleeping on the host. Boards can select
 * defaults; override them with -global ssi-sd.<property>=<microseconds>.
 *
 * Copyright (c) 2007-2009 CodeSourcery.
 * Written by Paul Brook
 *
 * Copyright (c) 2021 Wind River Systems, Inc.
 * Improved by Bin Meng <bin.meng@windriver.com>
 *
 * Validated with U-Boot v2021.01 and Linux v5.10 mmc_spi driver
 *
 * This code is licensed under the GNU GPL v2.
 *
 * Contributions after 2012-01-13 are licensed under the terms of the
 * GNU GPL, version 2 or (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "sysemu/blockdev.h"
#include "hw/ssi/ssi.h"
#include "migration/vmstate.h"
#include "hw/qdev-properties.h"
#include "hw/sd/sd.h"
#include "qapi/error.h"
#include "qemu/crc-ccitt.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qom/object.h"
#include "trace.h"

//#define DEBUG_SSI_SD 1

#ifdef DEBUG_SSI_SD
#define DPRINTF(fmt, ...) \
do { printf("ssi_sd: " fmt , ## __VA_ARGS__); } while (0)
#define BADF(fmt, ...) \
do { fprintf(stderr, "ssi_sd: error: " fmt , ## __VA_ARGS__); exit(1);} while (0)
#else
#define DPRINTF(fmt, ...) do {} while(0)
#define BADF(fmt, ...) \
do { fprintf(stderr, "ssi_sd: error: " fmt , ## __VA_ARGS__);} while (0)
#endif

typedef enum {
    SSI_SD_CMD = 0,
    SSI_SD_CMDARG,
    SSI_SD_PREP_RESP,
    SSI_SD_RESPONSE,
    SSI_SD_PREP_DATA,
    SSI_SD_DATA_START,
    SSI_SD_DATA_READ,
    SSI_SD_DATA_CRC16,
    SSI_SD_DATA_WRITE,
    SSI_SD_SKIP_CRC16,
} ssi_sd_mode;

struct ssi_sd_state {
    SSIPeripheral ssidev;
    uint32_t mode;
    int cmd;
    uint8_t cmdarg[4];
    uint8_t response[5];
    uint16_t crc16;
    int32_t read_bytes;
    int32_t write_bytes;
    int32_t arglen;
    int32_t response_pos;
    int32_t stopping;
    bool idle;          /* R1 idle bit of the last status, for R3/R7 */
    uint32_t read_access_us;
    uint32_t read_seq_access_us;
    uint32_t read_repeat_us;
    uint32_t read_next_us;
    uint32_t write_busy_us;
    uint32_t write_random_busy_us;
    uint32_t write_repeat_busy_us;
    uint32_t write_block_busy_us;
    uint32_t write_stop_busy_us;
    int32_t write_stop_decrement_us;
    uint32_t write_stop_random_extra_us;
    int64_t read_deadline_ns;
    int64_t busy_deadline_ns;
    uint64_t sector;
    uint64_t next_read_sector;
    uint64_t next_write_sector;
    uint64_t last_read_start;
    uint64_t last_write_start;
    uint64_t write_start;
    uint32_t write_blocks;
    bool read_history;
    bool write_history;
    bool high_capacity;
    bool random_write;
    bool repeat_write;
    bool write_response;
    SDBus sdbus;
};

#define TYPE_SSI_SD "ssi-sd"
OBJECT_DECLARE_SIMPLE_TYPE(ssi_sd_state, SSI_SD)

/* State word bits.  */
#define SSI_SDR_LOCKED          0x0001
#define SSI_SDR_WP_ERASE        0x0002
#define SSI_SDR_ERROR           0x0004
#define SSI_SDR_CC_ERROR        0x0008
#define SSI_SDR_ECC_FAILED      0x0010
#define SSI_SDR_WP_VIOLATION    0x0020
#define SSI_SDR_ERASE_PARAM     0x0040
#define SSI_SDR_OUT_OF_RANGE    0x0080
#define SSI_SDR_IDLE            0x0100
#define SSI_SDR_ERASE_RESET     0x0200
#define SSI_SDR_ILLEGAL_COMMAND 0x0400
#define SSI_SDR_COM_CRC_ERROR   0x0800
#define SSI_SDR_ERASE_SEQ_ERROR 0x1000
#define SSI_SDR_ADDRESS_ERROR   0x2000
#define SSI_SDR_PARAMETER_ERROR 0x4000

/* multiple block write */
#define SSI_TOKEN_MULTI_WRITE   0xfc
/* terminate multiple block write */
#define SSI_TOKEN_STOP_TRAN     0xfd
/* single block read/write, multiple block read */
#define SSI_TOKEN_SINGLE        0xfe

/* dummy value - don't care */
#define SSI_DUMMY               0xff

/* data accepted */
#define DATA_RESPONSE_ACCEPTED  0x05

static int64_t ssi_sd_deadline(ssi_sd_state *s, const char *kind, uint32_t us)
{
    int64_t deadline = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                       (int64_t)us * 1000;

    trace_ssi_sd_latency(s->sector, kind, us, deadline);
    return deadline;
}

static uint32_t ssi_sd_transfer(SSIPeripheral *dev, uint32_t val)
{
    ssi_sd_state *s = SSI_SD(dev);
    SDRequest request;
    uint8_t longresp[16];

    if (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) < s->busy_deadline_ns) {
        return 0x00;
    }

    /*
     * Special case: allow CMD12 (STOP TRANSMISSION) while reading data.
     *
     * See "Physical Layer Specification Version 8.00" chapter 7.5.2.2,
     * to avoid conflict between CMD12 response and next data block,
     * timing of CMD12 should be controlled as follows:
     *
     * - CMD12 issued at the timing that end bit of CMD12 and end bit of
     *   data block is overlapped
     * - CMD12 issued after one clock cycle after host receives a token
     *   (either Start Block token or Data Error token)
     *
     * We need to catch CMD12 in all of the data read states.
     */
    if (s->mode >= SSI_SD_PREP_DATA && s->mode <= SSI_SD_DATA_CRC16) {
        if (val == 0x4c) {
            s->mode = SSI_SD_CMD;
            /* There must be at least one byte delay before the card responds */
            s->stopping = 1;
        }
    }

dispatch:
    switch (s->mode) {
    case SSI_SD_CMD:
        switch (val) {
        case SSI_DUMMY:
            DPRINTF("NULL command\n");
            return SSI_DUMMY;
            break;
        case SSI_TOKEN_SINGLE:
        case SSI_TOKEN_MULTI_WRITE:
            DPRINTF("Start write block\n");
            s->mode = SSI_SD_DATA_WRITE;
            return SSI_DUMMY;
        case SSI_TOKEN_STOP_TRAN:
            DPRINTF("Stop multiple write\n");

            /* manually issue cmd12 to stop the transfer */
            request.cmd = 12;
            request.arg = 0;
            s->arglen = sdbus_do_command(&s->sdbus, &request, longresp);
            if (s->arglen <= 0) {
                s->arglen = 1;
                /* a zero value indicates the card is busy */
                s->response[0] = 0;
                DPRINTF("SD card busy\n");
            } else {
                s->arglen = 1;
                /* a non-zero value indicates the card is ready */
                s->response[0] = SSI_DUMMY;
            }

            if (s->cmd == 25) {
                uint64_t busy_us = s->write_stop_busy_us;
                int64_t credit_us = (int64_t)s->write_stop_decrement_us *
                                    s->write_blocks;

                if (s->random_write) {
                    busy_us += s->write_stop_random_extra_us;
                }
                if (credit_us >= 0) {
                    busy_us -= MIN(busy_us, credit_us);
                } else {
                    /* Negative decrement models growth with burst length. */
                    busy_us += MIN((uint64_t)-credit_us, UINT32_MAX);
                }
                s->busy_deadline_ns = ssi_sd_deadline(s, "write-stop",
                                                     MIN(busy_us, UINT32_MAX));
            }
            return SSI_DUMMY;
        }

        s->cmd = val & 0x3f;
        s->mode = SSI_SD_CMDARG;
        s->arglen = 0;
        return SSI_DUMMY;
    case SSI_SD_CMDARG:
        if (s->arglen == 4) {
            /* FIXME: Check CRC.  */
            request.cmd = s->cmd;
            request.arg = ldl_be_p(s->cmdarg);
            s->read_deadline_ns = 0;
            if (s->cmd == 0) {
                s->read_history = false;
                s->write_history = false;
                s->high_capacity = false;
                s->busy_deadline_ns = 0;
                s->write_response = false;
            }
            DPRINTF("CMD%d arg 0x%08x\n", s->cmd, request.arg);
            s->arglen = sdbus_do_command(&s->sdbus, &request, longresp);
            if (s->arglen <= 0) {
                s->arglen = 1;
                s->response[0] = 4;
                DPRINTF("SD command failed\n");
            } else if (s->cmd == 8 || s->cmd == 58) {
                /* CMD8/CMD58 returns R3/R7 response */
                DPRINTF("Returned R3/R7\n");
                s->arglen = 5;
                /*
                 * x4prosim: R1's idle bit follows the card: CMD58 after
                 * ACMD41 has completed answers 0x00, which SdFat requires.
                 */
                s->response[0] = s->idle ? 1 : 0;
                memcpy(&s->response[1], longresp, 4);
                if (s->cmd == 58) {
                    s->high_capacity = (ldl_be_p(longresp) & BIT(30)) != 0;
                }
            } else if (s->cmd == 13 && s->arglen == 16) {
                /*
                 * x4prosim: the SD core answers SEND_STATUS with the CSD; the
                 * SPI R2 is the idle bit and a clean error byte (SdFat checks
                 * it after every single-block write).
                 */
                s->arglen = 2;
                s->response[0] = s->idle ? 1 : 0;
                s->response[1] = 0;
            } else if (s->arglen != 4) {
                BADF("Unexpected response to cmd %d\n", s->cmd);
                /* Illegal command is about as near as we can get.  */
                s->arglen = 1;
                s->response[0] = 4;
            } else {
                /* All other commands return status.  */
                uint32_t cardstatus;
                uint16_t status;
                /* CMD13 returns a 2-byte statuse work. Other commands
                   only return the first byte.  */
                s->arglen = (s->cmd == 13) ? 2 : 1;

                /* handle R1b */
                if (s->cmd == 28 || s->cmd == 29 || s->cmd == 38) {
                    s->stopping = 1;
                }

                cardstatus = ldl_be_p(longresp);
                status = 0;
                if (((cardstatus >> 9) & 0xf) < 4)
                    status |= SSI_SDR_IDLE;
                if (cardstatus & ERASE_RESET)
                    status |= SSI_SDR_ERASE_RESET;
                if (cardstatus & ILLEGAL_COMMAND)
                    status |= SSI_SDR_ILLEGAL_COMMAND;
                if (cardstatus & COM_CRC_ERROR)
                    status |= SSI_SDR_COM_CRC_ERROR;
                if (cardstatus & ERASE_SEQ_ERROR)
                    status |= SSI_SDR_ERASE_SEQ_ERROR;
                if (cardstatus & ADDRESS_ERROR)
                    status |= SSI_SDR_ADDRESS_ERROR;
                if (cardstatus & CARD_IS_LOCKED)
                    status |= SSI_SDR_LOCKED;
                if (cardstatus & (LOCK_UNLOCK_FAILED | WP_ERASE_SKIP))
                    status |= SSI_SDR_WP_ERASE;
                if (cardstatus & SD_ERROR)
                    status |= SSI_SDR_ERROR;
                if (cardstatus & CC_ERROR)
                    status |= SSI_SDR_CC_ERROR;
                if (cardstatus & CARD_ECC_FAILED)
                    status |= SSI_SDR_ECC_FAILED;
                if (cardstatus & WP_VIOLATION)
                    status |= SSI_SDR_WP_VIOLATION;
                if (cardstatus & ERASE_PARAM)
                    status |= SSI_SDR_ERASE_PARAM;
                if (cardstatus & (OUT_OF_RANGE | CID_CSD_OVERWRITE))
                    status |= SSI_SDR_OUT_OF_RANGE;
                /* ??? Don't know what Parameter Error really means, so
                   assume it's set if the second byte is nonzero.  */
                if (status & 0xff)
                    status |= SSI_SDR_PARAMETER_ERROR;
                s->response[0] = status >> 8;
                s->response[1] = status;
                s->idle = (status & SSI_SDR_IDLE) != 0;
                DPRINTF("Card status 0x%02x\n", status);
            }
            if (s->response[0] == 0 &&
                (s->cmd == 17 || s->cmd == 18 ||
                 s->cmd == 24 || s->cmd == 25)) {
                s->sector = s->high_capacity ? request.arg : request.arg / 512;
                if (s->cmd == 24 || s->cmd == 25) {
                    s->repeat_write = s->write_history &&
                                      s->sector == s->last_write_start;
                    s->random_write = !s->repeat_write &&
                        (!s->write_history ||
                         s->sector != s->next_write_sector);
                    s->write_start = s->sector;
                    s->write_blocks = 0;
                }
            }
            s->mode = SSI_SD_PREP_RESP;
            s->response_pos = 0;
        } else {
            s->cmdarg[s->arglen++] = val;
        }
        return SSI_DUMMY;
    case SSI_SD_PREP_RESP:
        DPRINTF("Prepare card response (Ncr)\n");
        s->mode = SSI_SD_RESPONSE;
        return SSI_DUMMY;
    case SSI_SD_RESPONSE:
        if (s->response_pos < s->arglen) {
            DPRINTF("Response 0x%02x\n", s->response[s->response_pos]);
            if (s->write_response) {
                s->write_response = false;
                s->busy_deadline_ns = ssi_sd_deadline(s,
                    s->cmd == 25 ? "write-block" :
                    s->repeat_write ? "write-repeat" :
                    s->random_write ? "write-random" : "write",
                    s->cmd == 25 ? s->write_block_busy_us :
                    s->repeat_write ? s->write_repeat_busy_us :
                    s->random_write ? s->write_random_busy_us :
                                      s->write_busy_us);
                s->next_write_sector = ++s->sector;
                s->last_write_start = s->write_start;
                if (s->write_blocks < UINT32_MAX) {
                    s->write_blocks++;
                }
                s->write_history = true;
            } else if (s->response_pos == 0 && s->response[0] == 0 &&
                       (s->cmd == 17 || s->cmd == 18)) {
                bool sequential = s->read_history &&
                                  s->sector == s->next_read_sector;
                bool repeated = s->read_history &&
                                s->sector == s->last_read_start;

                s->read_deadline_ns = ssi_sd_deadline(s,
                    repeated ? "read-repeat" :
                    sequential ? "read-seq-access" : "read-access",
                    repeated ? s->read_repeat_us :
                    sequential ? s->read_seq_access_us : s->read_access_us);
                s->last_read_start = s->sector;
            }
            return s->response[s->response_pos++];
        }
        if (s->stopping) {
            s->stopping = 0;
            s->mode = SSI_SD_CMD;
            return SSI_DUMMY;
        }
        if (sdbus_data_ready(&s->sdbus)) {
            DPRINTF("Data read\n");
            s->mode = SSI_SD_DATA_START;
            return SSI_DUMMY;
        }
        DPRINTF("End of command\n");
        s->mode = SSI_SD_CMD;
        /*
         * x4prosim: this byte already belongs to the host's next transfer. A
         * write's data token may follow the R1 response without a fill byte
         * (SdFat does that), so take it as a command/token, not as a dummy.
         */
        goto dispatch;
    case SSI_SD_PREP_DATA:
        DPRINTF("Prepare data block (Nac)\n");
        s->mode = SSI_SD_DATA_START;
        return SSI_DUMMY;
    case SSI_SD_DATA_START:
        if (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) < s->read_deadline_ns) {
            return SSI_DUMMY;
        }
        DPRINTF("Start read block\n");
        s->mode = SSI_SD_DATA_READ;
        s->response_pos = 0;
        return SSI_TOKEN_SINGLE;
    case SSI_SD_DATA_READ:
        val = sdbus_read_byte(&s->sdbus);
        s->read_bytes++;
        s->crc16 = crc_ccitt_false(s->crc16, (uint8_t *)&val, 1);
        if (!sdbus_data_ready(&s->sdbus) || s->read_bytes == 512) {
            DPRINTF("Data read end\n");
            s->mode = SSI_SD_DATA_CRC16;
        }
        return val;
    case SSI_SD_DATA_CRC16:
        val = (s->crc16 & 0xff00) >> 8;
        s->crc16 <<= 8;
        s->response_pos++;
        if (s->response_pos == 2) {
            DPRINTF("CRC16 read end\n");
            if (s->read_bytes == 512 && (s->cmd == 17 || s->cmd == 18)) {
                s->next_read_sector = ++s->sector;
                s->read_history = true;
                if (s->cmd == 18) {
                    s->read_deadline_ns = ssi_sd_deadline(s, "read-next",
                                                         s->read_next_us);
                }
            }
            if (s->read_bytes == 512 && s->cmd != 17) {
                s->mode = SSI_SD_PREP_DATA;
            } else {
                s->mode = SSI_SD_CMD;
            }
            s->read_bytes = 0;
            s->response_pos = 0;
        }
        return val;
    case SSI_SD_DATA_WRITE:
        sdbus_write_byte(&s->sdbus, val);
        s->write_bytes++;
        if (!sdbus_receive_ready(&s->sdbus) || s->write_bytes == 512) {
            DPRINTF("Data write end\n");
            s->mode = SSI_SD_SKIP_CRC16;
            s->response_pos = 0;
        }
        return val;
    case SSI_SD_SKIP_CRC16:
        /* we don't verify the crc16 */
        s->response_pos++;
        if (s->response_pos == 2) {
            DPRINTF("CRC16 receive end\n");
            s->mode = SSI_SD_RESPONSE;
            s->write_bytes = 0;
            s->arglen = 1;
            s->response[0] = DATA_RESPONSE_ACCEPTED;
            s->write_response = s->cmd == 24 || s->cmd == 25;
            s->response_pos = 0;
        }
        return SSI_DUMMY;
    }
    /* Should never happen.  */
    return SSI_DUMMY;
}

static int ssi_sd_post_load(void *opaque, int version_id)
{
    ssi_sd_state *s = (ssi_sd_state *)opaque;

    if (s->mode > SSI_SD_SKIP_CRC16) {
        return -EINVAL;
    }
    if (s->mode == SSI_SD_CMDARG &&
        (s->arglen < 0 || s->arglen >= ARRAY_SIZE(s->cmdarg))) {
        return -EINVAL;
    }
    if (s->mode == SSI_SD_RESPONSE &&
        (s->response_pos < 0 || s->response_pos >= ARRAY_SIZE(s->response) ||
        (!s->stopping && s->arglen > ARRAY_SIZE(s->response)))) {
        return -EINVAL;
    }

    return 0;
}

static bool ssi_sd_timing_needed(void *opaque)
{
    ssi_sd_state *s = opaque;

    return s->read_access_us || s->read_seq_access_us || s->read_repeat_us ||
           s->read_next_us || s->write_busy_us || s->write_random_busy_us ||
           s->write_repeat_busy_us || s->write_block_busy_us ||
           s->write_stop_busy_us || s->write_stop_decrement_us ||
           s->write_stop_random_extra_us;
}

static const VMStateDescription vmstate_ssi_sd_timing = {
    .name = "ssi_sd/timing",
    .version_id = 2,
    .minimum_version_id = 1,
    .needed = ssi_sd_timing_needed,
    .fields = (const VMStateField []) {
        VMSTATE_INT64(read_deadline_ns, ssi_sd_state),
        VMSTATE_INT64(busy_deadline_ns, ssi_sd_state),
        VMSTATE_UINT64(sector, ssi_sd_state),
        VMSTATE_UINT64(next_read_sector, ssi_sd_state),
        VMSTATE_UINT64(next_write_sector, ssi_sd_state),
        VMSTATE_BOOL(read_history, ssi_sd_state),
        VMSTATE_BOOL(write_history, ssi_sd_state),
        VMSTATE_BOOL(high_capacity, ssi_sd_state),
        VMSTATE_BOOL(random_write, ssi_sd_state),
        VMSTATE_BOOL(write_response, ssi_sd_state),
        VMSTATE_BOOL(idle, ssi_sd_state),
        VMSTATE_UINT64_V(last_read_start, ssi_sd_state, 2),
        VMSTATE_UINT64_V(last_write_start, ssi_sd_state, 2),
        VMSTATE_UINT64_V(write_start, ssi_sd_state, 2),
        VMSTATE_UINT32_V(write_blocks, ssi_sd_state, 2),
        VMSTATE_BOOL_V(repeat_write, ssi_sd_state, 2),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_ssi_sd = {
    .name = "ssi_sd",
    .version_id = 7,
    .minimum_version_id = 7,
    .post_load = ssi_sd_post_load,
    .fields = (const VMStateField []) {
        VMSTATE_UINT32(mode, ssi_sd_state),
        VMSTATE_INT32(cmd, ssi_sd_state),
        VMSTATE_UINT8_ARRAY(cmdarg, ssi_sd_state, 4),
        VMSTATE_UINT8_ARRAY(response, ssi_sd_state, 5),
        VMSTATE_UINT16(crc16, ssi_sd_state),
        VMSTATE_INT32(read_bytes, ssi_sd_state),
        VMSTATE_INT32(write_bytes, ssi_sd_state),
        VMSTATE_INT32(arglen, ssi_sd_state),
        VMSTATE_INT32(response_pos, ssi_sd_state),
        VMSTATE_INT32(stopping, ssi_sd_state),
        VMSTATE_SSI_PERIPHERAL(ssidev, ssi_sd_state),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &vmstate_ssi_sd_timing,
        NULL
    },
};

static void ssi_sd_realize(SSIPeripheral *d, Error **errp)
{
    ssi_sd_state *s = SSI_SD(d);

    qbus_init(&s->sdbus, sizeof(s->sdbus), TYPE_SD_BUS, DEVICE(d), "sd-bus");
}

static void ssi_sd_reset(DeviceState *dev)
{
    ssi_sd_state *s = SSI_SD(dev);

    s->mode = SSI_SD_CMD;
    s->cmd = 0;
    memset(s->cmdarg, 0, sizeof(s->cmdarg));
    memset(s->response, 0, sizeof(s->response));
    s->crc16 = 0;
    s->read_bytes = 0;
    s->write_bytes = 0;
    s->arglen = 0;
    s->response_pos = 0;
    s->stopping = 0;
    s->idle = true;
    s->read_deadline_ns = 0;
    s->busy_deadline_ns = 0;
    s->sector = 0;
    s->next_read_sector = 0;
    s->next_write_sector = 0;
    s->last_read_start = 0;
    s->last_write_start = 0;
    s->write_start = 0;
    s->write_blocks = 0;
    s->read_history = false;
    s->write_history = false;
    s->high_capacity = false;
    s->random_write = false;
    s->repeat_write = false;
    s->write_response = false;
}

static Property ssi_sd_properties[] = {
    DEFINE_PROP_UINT32("read-access-us", ssi_sd_state, read_access_us, 0),
    DEFINE_PROP_UINT32("read-seq-access-us", ssi_sd_state,
                       read_seq_access_us, 0),
    DEFINE_PROP_UINT32("read-next-us", ssi_sd_state, read_next_us, 0),
    DEFINE_PROP_UINT32("read-repeat-us", ssi_sd_state, read_repeat_us, 0),
    DEFINE_PROP_UINT32("write-busy-us", ssi_sd_state, write_busy_us, 0),
    DEFINE_PROP_UINT32("write-random-busy-us", ssi_sd_state,
                       write_random_busy_us, 0),
    DEFINE_PROP_UINT32("write-stop-busy-us", ssi_sd_state,
                       write_stop_busy_us, 0),
    DEFINE_PROP_UINT32("write-repeat-busy-us", ssi_sd_state,
                       write_repeat_busy_us, 0),
    DEFINE_PROP_UINT32("write-block-busy-us", ssi_sd_state,
                       write_block_busy_us, 0),
    DEFINE_PROP_INT32("write-stop-decrement-us", ssi_sd_state,
                       write_stop_decrement_us, 0),
    DEFINE_PROP_UINT32("write-stop-random-extra-us", ssi_sd_state,
                       write_stop_random_extra_us, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static void ssi_sd_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SSIPeripheralClass *k = SSI_PERIPHERAL_CLASS(klass);

    k->realize = ssi_sd_realize;
    k->transfer = ssi_sd_transfer;
    k->cs_polarity = SSI_CS_LOW;
    dc->vmsd = &vmstate_ssi_sd;
    device_class_set_props(dc, ssi_sd_properties);
    device_class_set_legacy_reset(dc, ssi_sd_reset);
    /* Reason: GPIO chip-select line should be wired up */
    dc->user_creatable = false;
}

static const TypeInfo ssi_sd_types[] = {
    {
        .name           = TYPE_SSI_SD,
        .parent         = TYPE_SSI_PERIPHERAL,
        .instance_size  = sizeof(ssi_sd_state),
        .class_init     = ssi_sd_class_init,
    },
};

DEFINE_TYPES(ssi_sd_types)
