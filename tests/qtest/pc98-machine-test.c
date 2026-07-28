/*
 * PC-98 machine configuration tests
 * Copyright (c) 2026 Awe Morris
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"

#define PC98_IDE_DATA     0x640
#define PC98_IDE_NSECTOR  0x644
#define PC98_IDE_SECTOR   0x646
#define PC98_IDE_LCYL     0x648
#define PC98_IDE_HCYL     0x64a
#define PC98_IDE_SELECT   0x64c
#define PC98_IDE_STATUS   0x64e
#define PC98_IDE_COMMAND  0x64e

#define ATA_SR_BSY 0x80
#define ATA_SR_DRQ 0x08
#define ATA_CMD_READ 0x20
#define ATA_CMD_WRITE 0x30
#define ATA_CMD_IDENTIFY 0xec

#define LGY98_IOBASE       0x00d0
#define LGY98_ISR          (LGY98_IOBASE + 7)
#define LGY98_RESET        0x03d0
#define LGY98_OLD_RESET    0x00e8
#define LGY98_BOARD_ID_A   0x03da
#define LGY98_BOARD_ID_B   0x03db
#define LGY98_BOARD_ID_C   0x03dc
#define LGY98_BOARD_ID_D   0x03dd

#define E8390_STOP         0x01
#define E8390_START        0x02
#define E8390_NODMA        0x20

static char *pc98_qtree(const char *machine)
{
    QTestState *qts;
    char *qtree;

    qts = qtest_initf("-machine %s -nodefaults -display none", machine);
    qtree = qtest_hmp(qts, "info qtree");
    qtest_quit(qts);

    return qtree;
}

static void test_pc9801_has_no_pci(void)
{
    g_autofree char *qtree = pc98_qtree("pc9801");

    g_assert_nonnull(qtree);
    g_assert_null(strstr(qtree, "dev: pc98-pcihost"));
    g_assert_null(strstr(qtree, "dev: pc98-coregraph"));
}

static void test_pc9821_has_pci_coregraph(void)
{
    g_autofree char *qtree = pc98_qtree("pc9821");

    g_assert_nonnull(strstr(qtree, "dev: pc98-pcihost"));
    g_assert_nonnull(strstr(qtree, "dev: pc98-coregraph"));
}

static void test_pc98_lgy98_port_map(void)
{
    QTestState *qts;

    qts = qtest_init(
        "-machine pc9821 -nodefaults -display none "
        "-netdev user,id=n0 -device pc98-lgy98,netdev=n0");

    g_assert_cmphex(qtest_inb(qts, LGY98_BOARD_ID_A), ==, 0x00);
    g_assert_cmphex(qtest_inb(qts, LGY98_BOARD_ID_B), ==, 0x40);
    g_assert_cmphex(qtest_inb(qts, LGY98_BOARD_ID_C), ==, 0x26);
    g_assert_cmphex(qtest_inb(qts, LGY98_BOARD_ID_D), ==, 0x0b);

    qtest_outb(qts, LGY98_IOBASE, E8390_NODMA | E8390_START);
    qtest_outb(qts, LGY98_ISR, 0xff);
    g_assert_cmphex(qtest_inb(qts, LGY98_IOBASE), ==,
                    E8390_NODMA | E8390_START);
    g_assert_cmphex(qtest_inb(qts, LGY98_ISR), ==, 0x00);

    /* The old base+0x18 mapping must not reset the controller. */
    g_assert_cmphex(qtest_inb(qts, LGY98_OLD_RESET), ==, 0xff);
    g_assert_cmphex(qtest_inb(qts, LGY98_ISR), ==, 0x00);

    g_assert_cmphex(qtest_inb(qts, LGY98_RESET), ==, 0x00);
    g_assert_cmphex(qtest_inb(qts, LGY98_ISR), ==, 0x80);

    qtest_quit(qts);
}

static void pc98_ide_wait_drq(QTestState *qts)
{
    int i;

    for (i = 0; i < 10000; i++) {
        uint8_t status = qtest_inb(qts, PC98_IDE_STATUS);

        if (!(status & ATA_SR_BSY) && (status & ATA_SR_DRQ)) {
            return;
        }
        qtest_clock_step(qts, 1000);
    }
    g_assert_not_reached();
}

static void pc98_ide_read_data(QTestState *qts, uint8_t *buf)
{
    int i;

    pc98_ide_wait_drq(qts);
    for (i = 0; i < 256; i++) {
        uint16_t value = qtest_inw(qts, PC98_IDE_DATA);

        stw_le_p(buf + i * 2, value);
    }
}

static void pc98_ide_read_sector(QTestState *qts, uint32_t lba, uint8_t *buf)
{
    qtest_outb(qts, PC98_IDE_NSECTOR, 1);
    qtest_outb(qts, PC98_IDE_SECTOR, lba);
    qtest_outb(qts, PC98_IDE_LCYL, lba >> 8);
    qtest_outb(qts, PC98_IDE_HCYL, lba >> 16);
    qtest_outb(qts, PC98_IDE_SELECT, 0xe0 | ((lba >> 24) & 0x0f));
    qtest_outb(qts, PC98_IDE_COMMAND, ATA_CMD_READ);
    pc98_ide_read_data(qts, buf);
}

static void pc98_ide_write_sector(QTestState *qts, uint32_t lba,
                                  const uint8_t *buf)
{
    int i;

    qtest_outb(qts, PC98_IDE_NSECTOR, 1);
    qtest_outb(qts, PC98_IDE_SECTOR, lba);
    qtest_outb(qts, PC98_IDE_LCYL, lba >> 8);
    qtest_outb(qts, PC98_IDE_HCYL, lba >> 16);
    qtest_outb(qts, PC98_IDE_SELECT, 0xe0 | ((lba >> 24) & 0x0f));
    qtest_outb(qts, PC98_IDE_COMMAND, ATA_CMD_WRITE);
    pc98_ide_wait_drq(qts);
    for (i = 0; i < 256; i++) {
        qtest_outw(qts, PC98_IDE_DATA, lduw_le_p(buf + i * 2));
    }
}

static void pc98_ide_identify(QTestState *qts, uint8_t *buf)
{
    qtest_outb(qts, PC98_IDE_SELECT, 0xa0);
    qtest_outb(qts, PC98_IDE_COMMAND, ATA_CMD_IDENTIFY);
    pc98_ide_read_data(qts, buf);
}

static void test_pc98_vvfat_boot_layout(void)
{
    g_autoptr(GError) err = NULL;
    g_autofree char *dirname = g_dir_make_tmp("qemu-vvfat98-XXXXXX", &err);
    g_autofree char *ordinary = NULL;
    g_autofree char *msdos = NULL;
    g_autofree char *io = NULL;
    g_autofree char *io_contents = g_malloc0(65536);
    QTestState *qts;
    uint8_t identify[512], ipl[512], pbr[512], root[512], data[512] = { 0 };
    uint32_t hidden, fat_start, root_lba, data_start;
    uint16_t bytes_per_sector, sectors_per_fat, root_entries;
    uint8_t sectors_per_cluster, number_of_fats;
    uint16_t io_cluster, msdos_cluster, ordinary_cluster = 0;
    g_autofree char *ordinary_contents = NULL;
    gsize ordinary_length;
    int offset;

    g_assert_no_error(err);
    g_assert_nonnull(dirname);

    /*
     * Create the ordinary file first.  vvfat98 must still allocate the two
     * DOS system files first rather than depending on host readdir() order.
     */
    ordinary = g_build_filename(dirname, "ordinary.txt", NULL);
    msdos = g_build_filename(dirname, "MSDOS.SYS", NULL);
    io = g_build_filename(dirname, "IO.SYS", NULL);
    g_assert_true(g_file_set_contents(ordinary, "x", 1, &err));
    g_assert_no_error(err);
    g_assert_true(g_file_set_contents(msdos, "M", 1, &err));
    g_assert_no_error(err);
    g_assert_true(g_file_set_contents(io, io_contents, 65536, &err));
    g_assert_no_error(err);

    qts = qtest_initf(
        "-machine pc9821 -nodefaults -display none "
        "-drive file=fat98:rw:%s,format=raw,if=none,id=d0 "
        "-device ide-hd,drive=d0,bus=ide.0,unit=0",
        dirname);

    pc98_ide_identify(qts, identify);
    g_assert_cmpuint(lduw_le_p(identify + 2), >, 0);
    g_assert_cmpuint(lduw_le_p(identify + 6), ==, 8);
    g_assert_cmpuint(lduw_le_p(identify + 12), ==, 17);

    pc98_ide_read_sector(qts, 0, ipl);
    g_assert_cmpmem(ipl + 4, 4, "IPL1", 4);
    g_assert_cmpuint(lduw_le_p(ipl + 0x1f6), ==, 0xaa55);
    g_assert_cmpuint(lduw_le_p(ipl + 0x1fe), ==, 0xaa55);

    pc98_ide_read_sector(qts, 136, pbr);
    g_assert_cmpuint(pbr[0], ==, 0xeb);
    g_assert_cmpuint(pbr[1], ==, 0x46);
    bytes_per_sector = lduw_le_p(pbr + 0x0b);
    sectors_per_cluster = pbr[0x0d];
    number_of_fats = pbr[0x10];
    root_entries = lduw_le_p(pbr + 0x11);
    sectors_per_fat = lduw_le_p(pbr + 0x16);
    hidden = ldl_le_p(pbr + 0x1c);

    g_assert_cmpuint(bytes_per_sector, ==, 1024);
    g_assert_cmpuint(hidden, ==, 136);
    g_assert_cmpuint(ldl_le_p(pbr + 0x3e), ==, hidden);
    g_assert_cmpuint(lduw_le_p(pbr + 0x44), ==, 512);

    fat_start = hidden + lduw_le_p(pbr + 0x0e) *
                bytes_per_sector / 512;
    root_lba = fat_start + number_of_fats * sectors_per_fat *
               bytes_per_sector / 512;
    data_start = root_lba + root_entries * 32 / 512;
    g_assert_cmpuint(lduw_le_p(pbr + 0x42), ==, data_start - hidden);

    pc98_ide_read_sector(qts, root_lba, root);
    g_assert_cmpuint(root[0x0b], ==, 0x28);
    g_assert_cmpmem(root + 32, 11, "IO      SYS", 11);
    g_assert_cmpmem(root + 64, 11, "MSDOS   SYS", 11);
    io_cluster = lduw_le_p(root + 32 + 0x1a);
    msdos_cluster = lduw_le_p(root + 64 + 0x1a);
    g_assert_cmpuint(io_cluster, ==, 2);
    g_assert_cmpuint(msdos_cluster, ==,
                     io_cluster + 65536 /
                     (bytes_per_sector * sectors_per_cluster));

    for (offset = 0; offset < sizeof(root); offset += 32) {
        if (!memcmp(root + offset, "ORDINARYTXT", 11)) {
            ordinary_cluster = lduw_le_p(root + offset + 0x1a);
            break;
        }
    }
    g_assert_cmpuint(ordinary_cluster, >=, 2);
    data[0] = 'Z';
    pc98_ide_write_sector(
        qts,
        data_start + (ordinary_cluster - 2) *
        sectors_per_cluster * bytes_per_sector / 512,
        data);
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(ordinary, &ordinary_contents,
                                      &ordinary_length, &err));
    g_assert_no_error(err);
    g_assert_cmpuint(ordinary_length, ==, 1);
    g_assert_cmpuint(ordinary_contents[0], ==, 'Z');
    g_clear_pointer(&io, g_free);
    g_clear_pointer(&msdos, g_free);
    io = g_build_filename(dirname, "io.sys", NULL);
    msdos = g_build_filename(dirname, "msdos.sys", NULL);

    /*
     * IO.SYS is optional for an ordinary data disk.  Removing both system
     * files must not prevent the directory-backed drive from being opened.
     */
    g_assert_cmpint(g_remove(io), ==, 0);
    g_assert_cmpint(g_remove(msdos), ==, 0);
    qts = qtest_initf(
        "-machine pc9821 -nodefaults -display none "
        "-drive file=fat98:rw:%s,format=raw,if=none,id=d0 "
        "-device ide-hd,drive=d0,bus=ide.0,unit=0",
        dirname);
    pc98_ide_read_sector(qts, 0, ipl);
    g_assert_cmpmem(ipl + 4, 4, "IPL1", 4);
    pc98_ide_read_sector(qts, 136, pbr);
    g_assert_cmpuint(lduw_le_p(pbr + 0x0b), ==, 1024);
    qtest_quit(qts);

    g_assert_cmpint(g_remove(ordinary), ==, 0);
    g_assert_cmpint(g_rmdir(dirname), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/pc98/pc9801/no-pci-coregraph",
                   test_pc9801_has_no_pci);
    qtest_add_func("/pc98/pc9821/pci-coregraph",
                   test_pc9821_has_pci_coregraph);
    qtest_add_func("/pc98/lgy98/port-map",
                   test_pc98_lgy98_port_map);
    qtest_add_func("/pc98/vvfat98/boot-layout",
                   test_pc98_vvfat_boot_layout);

    return g_test_run();
}
