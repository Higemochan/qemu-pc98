/*
 * NEC PC-9801-92 compatible SCSI host adapter
 * Copyright (C) 2026 Awe Morris
 *
 * The board is a PC-98 C-Bus wrapper around an AMD/Western Digital 33C93
 * SCSI bus interface controller.  This model implements the indirect SBIC
 * register interface, select-and-transfer commands, programmed I/O, the
 * PC-98 DMA channel, interrupt selection straps and the 8 KiB option-ROM
 * window used by the original board.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/datadir.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/core/loader.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/irq.h"
#include "hw/i386/pc98.h"
#include "hw/isa/isa.h"
#include "hw/scsi/pc98-scsi.h"
#include "hw/scsi/scsi.h"
#include "scsi/constants.h"
#include "system/block-backend.h"
#include "system/blockdev.h"
#include "system/ioport.h"
#include "system/memory.h"
#include "system/system.h"
#include "qemu/timer.h"
#include "migration/vmstate.h"
#include "trace.h"

#define PC98_SCSI_IOBASE       0x0cc0
#define PC98_SCSI_ROM_BASE     0x0d2000
#define PC98_SCSI_ROM_SIZE     0x2000
#define PC98_SCSI_BOOT_ENTRY   0x0015
#define PC98_SCSI_DEFAULT_ROM  "pc98scsi.bin"
#define PC98_SCSI_LEGACY_ROM   "SCSIBIOS.ROM"
#define PC98_SCSI_IRQ_DELAY_NS  100000
#define PC98_MODE_PAGE_FORMAT_DEVICE 0x03
/* PC-9801-92 geometry; later PCI adapters may use 8 heads by 128 sectors. */
#define PC98_SCSI_BIOS_HEADS     8
#define PC98_SCSI_BIOS_SECTORS   32

/* WD33C93 registers */
enum {
    SBIC_OWN_ID       = 0x00,
    SBIC_CONTROL      = 0x01,
    SBIC_TIMEOUT      = 0x02,
    SBIC_CDB          = 0x03,
    SBIC_TARGET_LUN   = 0x0f,
    SBIC_CMD_PHASE    = 0x10,
    SBIC_SYNC         = 0x11,
    SBIC_COUNT        = 0x12,
    SBIC_DEST_ID      = 0x15,
    SBIC_SOURCE_ID    = 0x16,
    SBIC_STATUS       = 0x17,
    SBIC_COMMAND      = 0x18,
    SBIC_DATA         = 0x19,
    SBIC_QUEUE_TAG    = 0x1a,

    BOARD_MEM_BANK    = 0x30,
    BOARD_MEM_WINDOW  = 0x31,
    BOARD_AUX_CONFIG  = 0x33,
};

/* Auxiliary status register */
enum {
    ASR_INT = 0x80,
    ASR_LCI = 0x40,
    ASR_BSY = 0x20,
    ASR_CIP = 0x10,
    ASR_PE  = 0x02,
    ASR_DBR = 0x01,
};

/* WD33C93 commands */
enum {
    CMD_RESET              = 0x00,
    CMD_ABORT              = 0x01,
    CMD_ASSERT_ATN         = 0x02,
    CMD_NEGATE_ACK         = 0x03,
    CMD_DISCONNECT         = 0x04,
    CMD_SELECT_ATN         = 0x06,
    CMD_SELECT             = 0x07,
    CMD_SELECT_ATN_XFER    = 0x08,
    CMD_SELECT_XFER        = 0x09,
    CMD_TRANSFER_INFO      = 0x20,
    CMD_SINGLE_BYTE        = 0x80,
};

/* WD33C93 completion and bus-phase status values */
enum {
    CSR_RESET_ADVANCED = 0x01,
    CSR_SELECTED       = 0x11,
    CSR_SEL_XFER_DONE  = 0x16,
    CSR_XFER_DONE      = 0x18,
    CSR_DATA_OUT       = 0x88,
    CSR_DATA_IN        = 0x89,
    CSR_COMMAND_OUT    = 0x8a,
    CSR_STATUS_IN      = 0x8b,
    CSR_MSG_OUT        = 0x8e,
    CSR_MSG_IN         = 0x8f,
    CSR_DISCONNECT     = 0x85,
    CSR_TIMEOUT        = 0x42,
};

/* Board command/status port */
enum {
    BOARD_DMA_ENABLE  = 0x01,
    BOARD_DMA_DISABLE = 0x02,
};

typedef enum Pc98ScsiPhase {
    PHASE_IDLE,
    PHASE_MSG_OUT,
    PHASE_COMMAND,
    PHASE_DATA_IN,
    PHASE_DATA_OUT,
    PHASE_STATUS_IN,
    PHASE_MSG_IN,
} Pc98ScsiPhase;

typedef struct Pc98ScsiState {
    ISADevice parent_obj;

    SCSIBus bus;
    qemu_irq irq;
    uint32_t irq_num;
    QEMUTimer *irq_timer;
    IsaDma *dma;
    MemoryRegion rom;
    PortioList portio;

    uint8_t regs[0x80];
    uint8_t reg_index;
    uint8_t asr;
    uint8_t csr;
    uint8_t board_dma;
    uint8_t cdb[16];
    unsigned cdb_pos;
    unsigned cdb_len;
    uint8_t target_lun;
    uint8_t status_byte;
    uint32_t phase;              /* Pc98ScsiPhase */
    bool status_irq_unread;
    bool msg_irq_unread;

    SCSIRequest *req;
    uint8_t *async_buf;
    uint32_t async_len;
    bool req_to_initiator;
    bool sat;

    char *romfile;
    bool bios_boot;
    Pc98MemState *mem;
    bool rom_mapped;
} Pc98ScsiState;

OBJECT_DECLARE_SIMPLE_TYPE(Pc98ScsiState, PC98_SCSI)

static int pc98_scsi_irq_index(uint32_t irq_num)
{
    static const uint8_t irq_map[] = { 3, 5, 6, 9, 12, 13 };
    int index;

    for (index = 0; index < ARRAY_SIZE(irq_map); index++) {
        if (irq_map[index] == irq_num) {
            return index;
        }
    }
    return -1;
}

static uint32_t pc98_scsi_get_count(Pc98ScsiState *s)
{
    return ((uint32_t)s->regs[SBIC_COUNT] << 16) |
           ((uint32_t)s->regs[SBIC_COUNT + 1] << 8) |
           s->regs[SBIC_COUNT + 2];
}

static void pc98_scsi_set_count(Pc98ScsiState *s, uint32_t count)
{
    s->regs[SBIC_COUNT] = count >> 16;
    s->regs[SBIC_COUNT + 1] = count >> 8;
    s->regs[SBIC_COUNT + 2] = count;
}

static void pc98_scsi_irq_timer(void *opaque)
{
    Pc98ScsiState *s = opaque;

    trace_pc98_scsi_irq_fire(s->csr, s->asr,
                             s->regs[BOARD_MEM_BANK]);
    if ((s->regs[BOARD_MEM_BANK] & 0x04) && (s->asr & ASR_INT)) {
        qemu_set_irq(s->irq, 1);
    }
}

static void pc98_scsi_lower_irq(Pc98ScsiState *s)
{
    timer_del(s->irq_timer);
    s->asr &= ~ASR_INT;
    qemu_set_irq(s->irq, 0);
}

static void pc98_scsi_raise_irq(Pc98ScsiState *s, uint8_t csr)
{
    s->csr = csr;
    s->regs[SBIC_STATUS] = csr;
    s->asr &= ~(ASR_CIP | ASR_BSY | ASR_DBR);
    s->asr |= ASR_INT;
    trace_pc98_scsi_irq_schedule(csr, s->asr,
                                 s->regs[BOARD_MEM_BANK],
                                 !!(s->regs[BOARD_MEM_BANK] & 0x04));
    if (s->regs[BOARD_MEM_BANK] & 0x04) {
        timer_mod(s->irq_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  PC98_SCSI_IRQ_DELAY_NS);
    }
}

static void pc98_scsi_release_dma(Pc98ScsiState *s)
{
    if (s->dma) {
        IsaDmaClass *dc = ISADMA_GET_CLASS(s->dma);
        int channel = (s->regs[BOARD_AUX_CONFIG] >> 6) & 3;

        dc->release_DREQ(s->dma, channel);
    }
}

static void pc98_scsi_cancel_request(Pc98ScsiState *s)
{
    SCSIRequest *req = s->req;

    s->req = NULL;
    s->async_buf = NULL;
    s->async_len = 0;
    pc98_scsi_release_dma(s);
    if (req) {
        scsi_req_cancel(req);
    }
}

static void pc98_scsi_finish_chunk(Pc98ScsiState *s)
{
    SCSIRequest *req = s->req;

    s->async_buf = NULL;
    s->async_len = 0;
    s->asr &= ~ASR_DBR;
    pc98_scsi_release_dma(s);
    if (req) {
        scsi_req_continue(req);
    }
}

static int pc98_scsi_dma_transfer(void *opaque, int nchan,
                                  int dma_pos, int dma_len)
{
    Pc98ScsiState *s = opaque;
    IsaDmaClass *dc = ISADMA_GET_CLASS(s->dma);
    int available = dma_len - dma_pos;
    int count;
    uint32_t tc;

    if (!s->req || !s->async_len || available <= 0) {
        pc98_scsi_release_dma(s);
        return dma_pos;
    }

    count = MIN((uint32_t)available, s->async_len);
    tc = pc98_scsi_get_count(s);
    if (tc) {
        count = MIN((uint32_t)count, tc);
    }
    if (count <= 0) {
        pc98_scsi_release_dma(s);
        return dma_pos;
    }

    if (s->req_to_initiator) {
        dc->write_memory(s->dma, nchan, s->async_buf, dma_pos, count);
    } else {
        dc->read_memory(s->dma, nchan, s->async_buf, dma_pos, count);
    }
    s->async_buf += count;
    s->async_len -= count;
    if (tc) {
        pc98_scsi_set_count(s, tc - count);
    }
    dma_pos += count;

    if (!s->async_len) {
        pc98_scsi_finish_chunk(s);
    }
    return dma_pos;
}

static void pc98_scsi_start_data(Pc98ScsiState *s)
{
    if (!s->async_len) {
        return;
    }

    s->phase = s->req_to_initiator ? PHASE_DATA_IN : PHASE_DATA_OUT;
    if (s->board_dma & BOARD_DMA_ENABLE) {
        IsaDmaClass *dc;
        int channel = (s->regs[BOARD_AUX_CONFIG] >> 6) & 3;

        if (!s->dma) {
            return;
        }
        dc = ISADMA_GET_CLASS(s->dma);
        dc->hold_DREQ(s->dma, channel);
        dc->schedule(s->dma);
    } else {
        s->asr |= ASR_DBR;
        if (!s->sat) {
            pc98_scsi_raise_irq(s, s->req_to_initiator ?
                                CSR_DATA_IN : CSR_DATA_OUT);
        }
    }
}

/*
 * The PC-9801-92 common BIOS derives its fixed-disk geometry from both
 * MODE SENSE page 3 (Format Device) and page 4 (Rigid Disk Geometry).
 * QEMU's image-backed scsi-hd supplies page 4 but not page 3, leaving the
 * BIOS with zero heads/sectors and therefore no bootable fixed disk.
 *
 * Keep this legacy response synthesis in the PC-98 HBA.  A real SG_IO
 * device receives its native response unchanged, as does a CD-ROM.
 */
static void pc98_scsi_patch_mode_sense_geometry(SCSIRequest *req,
                                                uint8_t *buf, uint32_t len)
{
    uint8_t block_desc[8];
    uint8_t geometry_page[24];
    uint32_t block_size;
    uint32_t off;
    uint32_t page_len;
    bool found = false;

    if (req->cmd.buf[0] != MODE_SENSE ||
        (req->cmd.buf[2] & 0x3f) != 0x3f ||
        req->dev->type != TYPE_DISK ||
        blk_is_sg(req->dev->conf.blk) ||
        len < 64 || buf[3] != sizeof(block_desc)) {
        return;
    }

    memcpy(block_desc, buf + 4, sizeof(block_desc));
    off = 4 + buf[3];
    while (off + 2 <= len) {
        page_len = buf[off + 1] + 2;
        if (page_len < 2 || off + page_len > len) {
            break;
        }
        if ((buf[off] & 0x3f) == MODE_PAGE_HD_GEOMETRY &&
            page_len == sizeof(geometry_page)) {
            memcpy(geometry_page, buf + off, sizeof(geometry_page));
            found = true;
            break;
        }
        off += page_len;
    }
    if (!found) {
        return;
    }

    block_size = req->dev->conf.logical_block_size ?: 512;

    /*
     * The legacy BIOS expects a compact 64-byte sequence: page 1 with a
     * six-byte body, page 3 with a 22-byte body, then the NEC-compatible
     * 18-byte form of page 4.  The standard QEMU page 4 has a 22-byte body.
     */
    memset(buf, 0, 64);
    buf[0] = 63;
    buf[3] = sizeof(block_desc);
    memcpy(buf + 4, block_desc, sizeof(block_desc));

    buf[12] = MODE_PAGE_R_W_ERROR;
    buf[13] = 0x06;
    buf[14] = 0x80;

    buf[20] = PC98_MODE_PAGE_FORMAT_DEVICE;
    buf[21] = 0x16;
    /* The common BIOS uses tracks-per-zone as its logical head count. */
    buf[23] = PC98_SCSI_BIOS_HEADS;
    buf[30] = PC98_SCSI_BIOS_SECTORS >> 8;
    buf[31] = PC98_SCSI_BIOS_SECTORS;
    buf[32] = block_size >> 8;
    buf[33] = block_size;
    buf[35] = 1;
    /* Mark a fixed disk as hard-sectored in the PC-98 BIOS work area. */
    buf[40] = 0x40;

    buf[44] = MODE_PAGE_HD_GEOMETRY;
    buf[45] = 0x12;
    memcpy(buf + 46, geometry_page + 2, 4);
    memcpy(buf + 50, geometry_page + 6, 6);
    memcpy(buf + 56, geometry_page + 12, 2);
    memcpy(buf + 58, geometry_page + 14, 3);
}

static void pc98_scsi_transfer_data(SCSIRequest *req, uint32_t len)
{
    Pc98ScsiState *s = req->hba_private;
    uint32_t target = req->dev->id;

    if (req != s->req) {
        return;
    }
    s->async_buf = scsi_req_get_buf(req);
    s->async_len = len;
    pc98_scsi_patch_mode_sense_geometry(req, s->async_buf, len);
    trace_pc98_scsi_transfer(req->cmd.buf[0], len,
                             len ? s->async_buf[0] : 0,
                             len > 1 ? s->async_buf[1] : 0,
                             req->cmd.buf[2],
                             req->cmd.buf[4]);
    if (len >= 36 && req->cmd.buf[0] == INQUIRY &&
        !(req->cmd.buf[1] & 0x01)) {
        trace_pc98_scsi_inquiry(target, req->lun, s->async_buf[0],
                                !!(s->async_buf[1] & 0x80),
                                (char *)&s->async_buf[8],
                                (char *)&s->async_buf[16]);
    }
    pc98_scsi_start_data(s);
}

static void pc98_scsi_command_complete(SCSIRequest *req, size_t resid)
{
    Pc98ScsiState *s = req->hba_private;

    if (req != s->req) {
        scsi_req_unref(req);
        return;
    }

    trace_pc98_scsi_complete(req->cmd.buf[0], req->status, resid);
    pc98_scsi_release_dma(s);
    s->regs[SBIC_TARGET_LUN] = req->status;
    s->status_byte = req->status;
    s->async_buf = NULL;
    s->async_len = 0;
    s->phase = PHASE_IDLE;
    s->status_irq_unread = false;
    s->msg_irq_unread = false;
    s->req = NULL;
    if (s->sat) {
        /*
         * A completed Select-and-Transfer has received the target status
         * and the Command Complete message.  The real WD33C93A reports
         * command phase 60h; the PC-9801-92 common BIOS uses this value to
         * distinguish completion from a resumable intermediate phase.
         */
        s->regs[SBIC_CMD_PHASE] = 0x60;
        pc98_scsi_raise_irq(s, CSR_SEL_XFER_DONE);
    } else {
        s->phase = PHASE_STATUS_IN;
        s->status_irq_unread = true;
        pc98_scsi_raise_irq(s, CSR_STATUS_IN);
    }
    scsi_req_unref(req);
}

static void pc98_scsi_request_cancelled(SCSIRequest *req)
{
    Pc98ScsiState *s = req->hba_private;

    if (req == s->req) {
        s->req = NULL;
        s->async_buf = NULL;
        s->async_len = 0;
        s->phase = PHASE_IDLE;
        pc98_scsi_release_dma(s);
    }
    scsi_req_unref(req);
}

static const SCSIBusInfo pc98_scsi_bus_info = {
    .max_target = 7,
    .max_lun = 7,
    .transfer_data = pc98_scsi_transfer_data,
    .complete = pc98_scsi_command_complete,
    .cancel = pc98_scsi_request_cancelled,
};

static void pc98_scsi_submit(Pc98ScsiState *s, const uint8_t *cdb,
                             unsigned cdb_len)
{
    SCSIDevice *dev;
    uint8_t effective_cdb[sizeof(s->cdb)];
    int32_t datalen;
    int target = s->regs[SBIC_DEST_ID] & 7;
    int lun = s->target_lun & 7;

    trace_pc98_scsi_submit(target, lun, cdb[0], cdb_len);

    pc98_scsi_cancel_request(s);
    dev = scsi_device_find(&s->bus, 0, target, lun);
    if (!dev) {
        s->phase = PHASE_IDLE;
        pc98_scsi_raise_irq(s, CSR_TIMEOUT);
        return;
    }

    memcpy(effective_cdb, cdb, cdb_len);
    /*
     * Legacy -drive if=scsi creates one image-backed LUN (LUN 0) per
     * target.  The PC-98 common BIOS mixes SCSI-1 CDB LUN encoding with the
     * WD33C93 Target LUN register while resuming combination commands.  Use
     * the actual image LUN for I/O, but keep the requested LUN for INQUIRY
     * so probes of nonexistent LUNs still return the standard no-LUN data.
     * SG_IO devices always retain the guest-selected LUN and CDB.
     */
    if (!blk_is_sg(dev->conf.blk) && effective_cdb[0] != INQUIRY) {
        lun = dev->lun;
    }
    /*
     * The PC-9801-92 BIOS first asks fixed disks for MODE SENSE saved
     * values.  QEMU's image-backed scsi-hd has no persistent mode-page
     * store and reports SAVING PARAMETERS NOT SUPPORTED, after which this
     * BIOS retries device initialisation indefinitely.  Current values are
     * the only values an image-backed device can preserve, so provide them
     * as the board compatibility fallback.
     *
     * Do not alter commands for a real SCSI generic device.  In particular,
     * a USB MO exposed through /dev/sg* must receive the guest CDB unchanged.
     */
    if (!blk_is_sg(dev->conf.blk) && effective_cdb[0] == 0x1a &&
        (effective_cdb[2] & 0xc0) == 0xc0) {
        effective_cdb[2] &= 0x3f;
    }
    /*
     * REZERO UNIT is meaningful only for legacy seekable media.  Modern
     * QEMU image backends do not implement opcode 01h; their logical head is
     * always ready at the requested LBA, so TEST UNIT READY is the matching
     * no-op completion.  Preserve opcode 01h for an SG_IO device.
     */
    if (!blk_is_sg(dev->conf.blk) && effective_cdb[0] == 0x01) {
        effective_cdb[0] = 0x00;
    }
    /*
     * Image-backed devices have no mechanical head to position.  The Xa7
     * PC-9801-92 BIOS nevertheless requires SEEK (6)/(10) to succeed while
     * probing both fixed and removable direct-access media.  Complete these
     * commands as TEST UNIT READY for images, while leaving real SG_IO
     * devices to execute the original seek command.
     */
    if (!blk_is_sg(dev->conf.blk) &&
        (effective_cdb[0] == 0x0b || effective_cdb[0] == 0x2b)) {
        memset(effective_cdb, 0, cdb_len);
        cdb_len = 6;
    }
    /*
     * This SCSI-1 BIOS does not recover from a power-on Unit Attention on
     * image-backed fixed disks.  It leaves the target in its exclusion
     * bitmap even though the following geometry commands succeed.  Legacy
     * disks presented by this board are ready at power-on, so clear only
     * the emulated Unit Attention before TEST UNIT READY.  Real SG_IO
     * devices retain their native reset and media-change reporting.
     */
    if (!blk_is_sg(dev->conf.blk) && dev->type == TYPE_DISK &&
        effective_cdb[0] == TEST_UNIT_READY) {
        dev->unit_attention = SENSE_CODE(NO_SENSE);
        s->bus.unit_attention = SENSE_CODE(NO_SENSE);
    }

    s->req = scsi_req_new(dev, 0, lun, effective_cdb, cdb_len, s);
    datalen = scsi_req_enqueue(s->req);
    s->req_to_initiator = datalen > 0;
    if (datalen != 0) {
        s->asr |= ASR_BSY;
        scsi_req_continue(s->req);
    }
}

static void pc98_scsi_command(Pc98ScsiState *s, uint8_t command)
{
    unsigned cdb_len;

    trace_pc98_scsi_command(command);
    s->regs[SBIC_COMMAND] = command;
    s->asr &= ~(ASR_LCI | ASR_INT);
    qemu_set_irq(s->irq, 0);

    switch (command & ~CMD_SINGLE_BYTE) {
    case CMD_RESET:
        pc98_scsi_cancel_request(s);
        memset(s->regs, 0, 0x1b);
        /*
         * A chip reset does not reset the PC-9801-92 board registers
         * outside the 33C93 register file.  In particular, keep the IRQ
         * enable bit so that the reset-complete interrupt is observable.
         */
        s->phase = PHASE_IDLE;
        pc98_scsi_raise_irq(s, CSR_RESET_ADVANCED);
        break;

    case CMD_ABORT:
        pc98_scsi_cancel_request(s);
        s->phase = PHASE_IDLE;
        pc98_scsi_raise_irq(s, CSR_DISCONNECT);
        break;

    case CMD_SELECT_ATN_XFER:
    case CMD_SELECT_XFER:
        cdb_len = scsi_cdb_length(&s->regs[SBIC_CDB]);
        if ((int)cdb_len <= 0 || cdb_len > sizeof(s->cdb)) {
            s->asr |= ASR_LCI;
            break;
        }
        memcpy(s->cdb, &s->regs[SBIC_CDB], cdb_len);
        s->sat = true;
        s->regs[SBIC_CMD_PHASE] = 0x00;
        pc98_scsi_submit(s, s->cdb, cdb_len);
        break;

    case CMD_SELECT_ATN:
        if (!scsi_device_find(&s->bus, 0,
                              s->regs[SBIC_DEST_ID] & 7, 0)) {
            s->phase = PHASE_IDLE;
            pc98_scsi_raise_irq(s, CSR_TIMEOUT);
            break;
        }
        s->sat = false;
        s->phase = PHASE_MSG_OUT;
        s->cdb_pos = 0;
        pc98_scsi_raise_irq(s, CSR_SELECTED);
        break;

    case CMD_SELECT:
        if (!scsi_device_find(&s->bus, 0,
                              s->regs[SBIC_DEST_ID] & 7, 0)) {
            s->phase = PHASE_IDLE;
            pc98_scsi_raise_irq(s, CSR_TIMEOUT);
            break;
        }
        s->sat = false;
        s->phase = PHASE_COMMAND;
        s->cdb_pos = 0;
        pc98_scsi_raise_irq(s, CSR_SELECTED);
        break;

    case CMD_ASSERT_ATN:
        break;

    case CMD_NEGATE_ACK:
        if (s->phase == PHASE_IDLE) {
            pc98_scsi_raise_irq(s, CSR_DISCONNECT);
        }
        break;

    case CMD_DISCONNECT:
        pc98_scsi_cancel_request(s);
        s->phase = PHASE_IDLE;
        pc98_scsi_raise_irq(s, CSR_DISCONNECT);
        break;

    case CMD_TRANSFER_INFO:
        if (s->phase == PHASE_DATA_IN || s->phase == PHASE_DATA_OUT) {
            pc98_scsi_start_data(s);
        } else if (s->phase == PHASE_STATUS_IN) {
            /*
             * The phase interrupt only announces STATUS IN.  TRANSFER INFO
             * then exposes the status byte through the data register; the
             * read advances to MESSAGE IN and raises that phase interrupt.
             * Advancing here would leave DBR clear, so the PC-9801-92 ROM
             * would skip the byte and interpret stale stack data as target
             * status.
             */
            if (s->status_irq_unread) {
                pc98_scsi_raise_irq(s, CSR_STATUS_IN);
            } else {
                s->asr |= ASR_DBR | ASR_BSY;
            }
        } else if (s->phase == PHASE_MSG_IN) {
            if (s->msg_irq_unread) {
                pc98_scsi_raise_irq(s, CSR_MSG_IN);
            } else {
                s->asr |= ASR_DBR | ASR_BSY;
            }
        } else {
            s->asr |= ASR_DBR | ASR_BSY;
        }
        break;

    default:
        s->asr |= ASR_LCI;
        break;
    }
}

static uint8_t pc98_scsi_data_read(Pc98ScsiState *s)
{
    uint8_t value = 0xff;
    uint32_t count;

    if (s->phase == PHASE_STATUS_IN) {
        value = s->status_byte;
        count = pc98_scsi_get_count(s);
        if (count) {
            pc98_scsi_set_count(s, count - 1);
        }
        s->asr &= ~ASR_DBR;
        s->phase = PHASE_MSG_IN;
        s->status_irq_unread = false;
        s->msg_irq_unread = true;
        pc98_scsi_raise_irq(s, CSR_MSG_IN);
        return value;
    }

    if (s->phase == PHASE_MSG_IN) {
        /* COMMAND COMPLETE message */
        count = pc98_scsi_get_count(s);
        if (count) {
            pc98_scsi_set_count(s, count - 1);
        }
        s->asr &= ~ASR_DBR;
        s->phase = PHASE_IDLE;
        s->msg_irq_unread = false;
        pc98_scsi_raise_irq(s, CSR_DISCONNECT);
        return 0x00;
    }

    if (s->phase == PHASE_DATA_IN && s->async_len) {
        value = *s->async_buf++;
        s->async_len--;
        count = pc98_scsi_get_count(s);
        if (count) {
            pc98_scsi_set_count(s, count - 1);
        }
        if (!s->async_len) {
            pc98_scsi_finish_chunk(s);
        }
    }
    return value;
}

static void pc98_scsi_data_write(Pc98ScsiState *s, uint8_t value)
{
    uint32_t count;

    if (s->phase == PHASE_MSG_OUT) {
        /*
         * SELECT WITH ATN initiators normally specify the LUN in an
         * IDENTIFY message rather than programming TARGET_LUN.  Henry ASPI
         * uses this path; ignoring the message leaves stale register bits
         * and makes every command address a nonexistent LUN.
         */
        if (value & 0x80) {
            s->target_lun = value & 7;
        }
        trace_pc98_scsi_message_out(value, s->target_lun);
        s->phase = PHASE_COMMAND;
        s->cdb_pos = 0;
        s->asr &= ~ASR_DBR;
        pc98_scsi_raise_irq(s, CSR_COMMAND_OUT);
        return;
    }

    if (s->phase == PHASE_COMMAND) {
        if (!s->cdb_pos) {
            s->cdb_len = scsi_cdb_length(&value);
            if ((int)s->cdb_len <= 0 || s->cdb_len > sizeof(s->cdb)) {
                s->asr |= ASR_LCI;
                return;
            }
        }
        trace_pc98_scsi_cdb_byte(value, s->cdb_pos, s->cdb_len);
        s->cdb[s->cdb_pos++] = value;
        if (s->cdb_pos == s->cdb_len) {
            s->asr &= ~ASR_DBR;
            pc98_scsi_submit(s, s->cdb, s->cdb_len);
        }
        return;
    }

    if (s->phase == PHASE_DATA_OUT && s->async_len) {
        *s->async_buf++ = value;
        s->async_len--;
        count = pc98_scsi_get_count(s);
        if (count) {
            pc98_scsi_set_count(s, count - 1);
        }
        if (!s->async_len) {
            pc98_scsi_finish_chunk(s);
        }
    }
}

static uint32_t pc98_scsi_io_read(void *opaque, uint32_t port)
{
    Pc98ScsiState *s = opaque;
    uint8_t value;

    switch (port) {
    case PC98_SCSI_IOBASE:
        return s->asr;

    case PC98_SCSI_IOBASE + 2:
        if (s->reg_index == SBIC_STATUS) {
            value = s->csr;
            trace_pc98_scsi_csr_read(value, s->asr);
            if (value == CSR_STATUS_IN) {
                s->status_irq_unread = false;
            } else if (value == CSR_MSG_IN) {
                s->msg_irq_unread = false;
            }
            pc98_scsi_lower_irq(s);
            /*
             * A plain SELECT first reports arbitration/selection complete,
             * then the selected target changes to its initial information
             * phase.  There is no guest command between these interrupts.
             */
            if (value == CSR_SELECTED) {
                pc98_scsi_raise_irq(s, s->phase == PHASE_MSG_OUT ?
                                    CSR_MSG_OUT : CSR_COMMAND_OUT);
            }
        } else if (s->reg_index == SBIC_DATA) {
            value = pc98_scsi_data_read(s);
        } else {
            value = s->regs[s->reg_index & 0x7f];
        }
        /*
         * The WD33C93 address register auto-increments after indirect
         * accesses except for the Command and Data registers.  Firmware
         * deliberately relies on those two registers retaining selection.
         */
        if (s->reg_index < 0x1a &&
            s->reg_index != SBIC_COMMAND &&
            s->reg_index != SBIC_DATA) {
            s->reg_index++;
        }
        return value;

    case PC98_SCSI_IOBASE + 4:
        /* Low bits are the fixed PC-98 DMA channel number (DMA0). */
        return 0x00;

    case PC98_SCSI_IOBASE + 6:
        return pc98_scsi_data_read(s);

    default:
        return 0xff;
    }
}

static void pc98_scsi_io_write(void *opaque, uint32_t port, uint32_t data)
{
    Pc98ScsiState *s = opaque;
    uint8_t value = data;

    switch (port) {
    case PC98_SCSI_IOBASE:
        s->reg_index = value & 0x7f;
        break;

    case PC98_SCSI_IOBASE + 2:
        if (s->reg_index == SBIC_COMMAND) {
            pc98_scsi_command(s, value);
        } else if (s->reg_index == SBIC_DATA) {
            pc98_scsi_data_write(s, value);
        } else {
            s->regs[s->reg_index & 0x7f] = value;
            if (s->reg_index == SBIC_TARGET_LUN) {
                /*
                 * During STATUS IN the 33C93 replaces this register's
                 * readable value with target status.  Keep the programmed
                 * LUN separately for the next selection.
                 */
                s->target_lun = value & 7;
                trace_pc98_scsi_target_lun_write(value, s->target_lun);
            }
            if (s->reg_index == BOARD_MEM_BANK &&
                (value & 0x04) && (s->asr & ASR_INT)) {
                timer_mod(s->irq_timer,
                          qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                          PC98_SCSI_IRQ_DELAY_NS);
            }
        }
        if (s->reg_index < 0x1a &&
            s->reg_index != SBIC_COMMAND &&
            s->reg_index != SBIC_DATA) {
            s->reg_index++;
        }
        break;

    case PC98_SCSI_IOBASE + 4:
        if (value == BOARD_DMA_ENABLE) {
            s->board_dma = BOARD_DMA_ENABLE;
            pc98_scsi_start_data(s);
        } else if (value == BOARD_DMA_DISABLE) {
            s->board_dma = 0;
            pc98_scsi_release_dma(s);
        }
        break;

    case PC98_SCSI_IOBASE + 6:
        pc98_scsi_data_write(s, value);
        break;
    }
}

static const MemoryRegionPortio pc98_scsi_portio[] = {
    { PC98_SCSI_IOBASE,     1, 1,
      .read = pc98_scsi_io_read, .write = pc98_scsi_io_write },
    { PC98_SCSI_IOBASE + 2, 1, 1,
      .read = pc98_scsi_io_read, .write = pc98_scsi_io_write },
    { PC98_SCSI_IOBASE + 4, 1, 1,
      .read = pc98_scsi_io_read, .write = pc98_scsi_io_write },
    { PC98_SCSI_IOBASE + 6, 1, 1,
      .read = pc98_scsi_io_read, .write = pc98_scsi_io_write },
    PORTIO_END_OF_LIST(),
};

static void pc98_scsi_load_rom(Pc98ScsiState *s, Error **errp)
{
    char *path = NULL;
    int size;

    if (s->romfile && s->romfile[0]) {
        path = qemu_find_file(QEMU_FILE_TYPE_BIOS, s->romfile);
    }
    if (!path && (!s->romfile ||
                  !strcmp(s->romfile, PC98_SCSI_DEFAULT_ROM))) {
        path = qemu_find_file(QEMU_FILE_TYPE_BIOS, PC98_SCSI_LEGACY_ROM);
    }
    if (!path) {
        warn_report("pc98-scsi: option ROM '%s' not found; "
                    "the controller remains available to OS drivers",
                    s->romfile ?: PC98_SCSI_DEFAULT_ROM);
        return;
    }

    memory_region_init_rom(&s->rom, OBJECT(s), "pc98-scsi.rom",
                           PC98_SCSI_ROM_SIZE, errp);
    if (*errp) {
        g_free(path);
        return;
    }
    size = load_image_size(path, memory_region_get_ram_ptr(&s->rom),
                           PC98_SCSI_ROM_SIZE);
    if (size < 0) {
        error_setg(errp, "pc98-scsi: could not load option ROM '%s'", path);
        g_free(path);
        return;
    }
    if (size < PC98_SCSI_ROM_SIZE) {
        memset((uint8_t *)memory_region_get_ram_ptr(&s->rom) + size, 0xff,
               PC98_SCSI_ROM_SIZE - size);
    }
    if (!s->bios_boot && size >= PC98_SCSI_BOOT_ENTRY + 3) {
        uint8_t *rom = memory_region_get_ram_ptr(&s->rom);

        /*
         * The PC-98 C-Bus ROM ABI uses independent three-byte entries.
         * Entry 15h is the disk boot hook; entry 18h provides the BIOS
         * services used by DOS ASPI managers.  Disable only boot probing so
         * an IDE system disk can start while retaining those SCSI services.
         */
        rom[PC98_SCSI_BOOT_ENTRY] = 0xcb;      /* RETF */
        rom[PC98_SCSI_BOOT_ENTRY + 1] = 0x90;
        rom[PC98_SCSI_BOOT_ENTRY + 2] = 0x90;
    }
    pc98_mem_register_cbus_rom(s->mem, &s->rom, PC98_SCSI_ROM_BASE);
    s->rom_mapped = true;
    g_free(path);
}

static void pc98_scsi_reset(DeviceState *dev)
{
    Pc98ScsiState *s = PC98_SCSI(dev);
    int irq_index = pc98_scsi_irq_index(s->irq_num);

    pc98_scsi_cancel_request(s);
    timer_del(s->irq_timer);
    memset(s->regs, 0, sizeof(s->regs));
    s->regs[SBIC_OWN_ID] = 0x9f;        /* 20 MHz, parity, advanced, ID 7 */
    s->regs[BOARD_MEM_BANK] = 0x04;     /* ROM bank 0, interrupt enabled */
    s->regs[BOARD_MEM_WINDOW] = 0x09;   /* D2000h */
    /* Bits 3..5 are the PC-9801-92 IRQ jumper encoding; ID 7 is initiator. */
    s->regs[BOARD_AUX_CONFIG] = (irq_index << 3) | 7;
    s->reg_index = 0;
    s->asr = 0;
    s->csr = 0;
    s->board_dma = 0;
    s->cdb_pos = 0;
    s->cdb_len = 0;
    s->target_lun = 0;
    s->phase = PHASE_IDLE;
    s->status_irq_unread = false;
    s->msg_irq_unread = false;
    s->sat = false;
    qemu_set_irq(s->irq, 0);
}

/*
 * The generic SCSI layer migrates the attached targets.  This controller can
 * safely migrate between commands and while presenting the final status or
 * message byte.  An in-flight request owns backend buffers that cannot be
 * represented by this register-level model, so fail the snapshot explicitly
 * instead of silently restoring a half-completed command.
 */
static int pc98_scsi_pre_save(void *opaque)
{
    Pc98ScsiState *s = opaque;

    if (s->req || s->async_buf || s->async_len ||
        s->phase == PHASE_DATA_IN || s->phase == PHASE_DATA_OUT) {
        return -EBUSY;
    }
    return 0;
}

static int pc98_scsi_post_load(void *opaque, int version_id)
{
    Pc98ScsiState *s = opaque;
    bool irq_level;

    if (s->cdb_pos > sizeof(s->cdb) || s->cdb_len > sizeof(s->cdb)) {
        return -EINVAL;
    }
    s->req = NULL;
    s->async_buf = NULL;
    s->async_len = 0;
    irq_level = (s->asr & ASR_INT) &&
                (s->regs[BOARD_MEM_BANK] & 0x04) &&
                !timer_pending(s->irq_timer);
    qemu_set_irq(s->irq, irq_level);
    return 0;
}

static const VMStateDescription vmstate_pc98_scsi = {
    .name = "pc98-scsi",
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = pc98_scsi_pre_save,
    .post_load = pc98_scsi_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, Pc98ScsiState, 0x80),
        VMSTATE_UINT8(reg_index, Pc98ScsiState),
        VMSTATE_UINT8(asr, Pc98ScsiState),
        VMSTATE_UINT8(csr, Pc98ScsiState),
        VMSTATE_UINT8(board_dma, Pc98ScsiState),
        VMSTATE_UINT8_ARRAY(cdb, Pc98ScsiState, 16),
        VMSTATE_UINT32(cdb_pos, Pc98ScsiState),
        VMSTATE_UINT32(cdb_len, Pc98ScsiState),
        VMSTATE_UINT8(target_lun, Pc98ScsiState),
        VMSTATE_UINT8(status_byte, Pc98ScsiState),
        VMSTATE_UINT32(phase, Pc98ScsiState),
        VMSTATE_BOOL(status_irq_unread, Pc98ScsiState),
        VMSTATE_BOOL(msg_irq_unread, Pc98ScsiState),
        VMSTATE_BOOL(req_to_initiator, Pc98ScsiState),
        VMSTATE_BOOL(sat, Pc98ScsiState),
        VMSTATE_TIMER_PTR(irq_timer, Pc98ScsiState),
        VMSTATE_END_OF_LIST()
    }
};

/*
 * Keep PC-9801-92 inquiry strings local to this board.  Extending the generic
 * legacy SCSI helper for these two properties would make every SCSI caller
 * carry a PC-98-only interface change.
 */
static SCSIDevice *pc98_scsi_add_legacy_drive(SCSIBus *bus,
                                              BlockBackend *blk,
                                              int unit, bool removable,
                                              BlockConf *conf, Error **errp)
{
    DriveInfo *dinfo = blk_legacy_dinfo(blk);
    const char *driver;
    const char *product;
    DeviceState *dev;
    SCSIDevice *s;
    Error *local_err = NULL;
    g_autofree char *name = NULL;

    if (blk_is_sg(blk)) {
        driver = "scsi-generic";
    } else if (dinfo && dinfo->media_cd) {
        driver = "scsi-cd";
    } else {
        driver = "scsi-hd";
    }
    product = dinfo && dinfo->media_cd ? "CD-ROM DRIVE" :
                                         "PC-9801-92 DISK";

    dev = qdev_new(driver);
    name = g_strdup_printf("legacy[%d]", unit);
    object_property_add_child(OBJECT(bus), name, OBJECT(dev));

    s = SCSI_DEVICE(dev);
    s->conf = *conf;

    check_boot_index(conf->bootindex, &local_err);
    if (local_err) {
        object_unparent(OBJECT(dev));
        error_propagate(errp, local_err);
        return NULL;
    }
    add_boot_device_path(conf->bootindex, dev, NULL);

    qdev_prop_set_uint32(dev, "scsi-id", unit);
    if (object_property_find(OBJECT(dev), "removable")) {
        qdev_prop_set_bit(dev, "removable", removable);
    }
    if (object_property_find(OBJECT(dev), "vendor")) {
        qdev_prop_set_string(dev, "vendor", "NEC");
    }
    if (object_property_find(OBJECT(dev), "product")) {
        qdev_prop_set_string(dev, "product", product);
    }
    if (!qdev_prop_set_drive_err(dev, "drive", blk, errp)) {
        object_unparent(OBJECT(dev));
        return NULL;
    }
    if (!qdev_realize_and_unref(dev, &bus->qbus, errp)) {
        object_unparent(OBJECT(dev));
        return NULL;
    }
    return s;
}

static void pc98_scsi_realize(DeviceState *dev, Error **errp)
{
    Pc98ScsiState *s = PC98_SCSI(dev);
    ISADevice *isa = ISA_DEVICE(dev);
    ISABus *bus = ISA_BUS(qdev_get_parent_bus(dev));
    BlockConf conf = DEFAULT_BLOCK_CONF;
    int unit;

    if (pc98_scsi_irq_index(s->irq_num) < 0) {
        error_setg(errp, "pc98-scsi: irq must be one of 3, 5, 6, 9, 12, 13");
        return;
    }
    s->irq = isa_get_irq(isa, s->irq_num);
    scsi_bus_init(&s->bus, sizeof(s->bus), dev, &pc98_scsi_bus_info);
    s->dma = isa_bus_get_dma(bus, 0);
    if (s->dma) {
        IsaDmaClass *dc = ISADMA_GET_CLASS(s->dma);

        dc->register_channel(s->dma, 0, pc98_scsi_dma_transfer, s);
    }

    isa_register_portio_list(isa, &s->portio, 0, pc98_scsi_portio, s,
                             "pc98-scsi");
    pc98_scsi_load_rom(s, errp);
    if (*errp) {
        return;
    }
    s->irq_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, pc98_scsi_irq_timer, s);

    for (unit = 0; unit < 7; unit++) {
        DriveInfo *dinfo = drive_get(IF_SCSI, 0, unit);

        if (!dinfo) {
            continue;
        }
        pc98_scsi_add_legacy_drive(&s->bus, blk_by_legacy_dinfo(dinfo),
                                   unit, dinfo->media_cd, &conf,
                                   &error_fatal);
    }
}

static void pc98_scsi_unrealize(DeviceState *dev)
{
    Pc98ScsiState *s = PC98_SCSI(dev);

    pc98_scsi_cancel_request(s);
    timer_free(s->irq_timer);
    s->irq_timer = NULL;
    if (s->rom_mapped) {
        /*
         * PC-98 machines are not hot-unpluggable.  The memory controller
         * owns the registration for the machine lifetime.
         */
        memory_region_set_enabled(&s->rom, false);
    }
}

static const Property pc98_scsi_properties[] = {
    DEFINE_PROP_STRING("romfile", Pc98ScsiState, romfile),
    DEFINE_PROP_BOOL("bios-boot", Pc98ScsiState, bios_boot, true),
    DEFINE_PROP_UINT32("irq", Pc98ScsiState, irq_num, 5),
};

static void pc98_scsi_initfn(Object *obj)
{
    Pc98ScsiState *s = PC98_SCSI(obj);

    s->romfile = g_strdup(PC98_SCSI_DEFAULT_ROM);
}

static void pc98_scsi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = pc98_scsi_realize;
    dc->unrealize = pc98_scsi_unrealize;
    dc->vmsd = &vmstate_pc98_scsi;
    device_class_set_legacy_reset(dc, pc98_scsi_reset);
    device_class_set_props(dc, pc98_scsi_properties);
    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
}

static const TypeInfo pc98_scsi_info = {
    .name = TYPE_PC98_SCSI,
    .parent = TYPE_ISA_DEVICE,
    .instance_size = sizeof(Pc98ScsiState),
    .instance_init = pc98_scsi_initfn,
    .class_init = pc98_scsi_class_init,
};

static void pc98_scsi_register_types(void)
{
    type_register_static(&pc98_scsi_info);
}

type_init(pc98_scsi_register_types)

ISADevice *pc98_scsi_init(ISABus *bus, Pc98MemState *mem)
{
    ISADevice *dev = isa_new(TYPE_PC98_SCSI);
    Pc98ScsiState *s = PC98_SCSI(dev);

    s->mem = mem;
    isa_realize_and_unref(dev, bus, &error_fatal);
    return dev;
}
