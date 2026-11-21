/* ============================================================
 * kernel_disk.cpp - SlyzFS 内核磁盘后端
 * ------------------------------------------------------------
 * 只做转发，具体后端（USB xHCI MSC 或 ATA PIO）由 blk.c 选择。
 * 这样文件系统不需要知道底层是 U 盘还是 IDE。
 * ============================================================ */

#include <stdint.h>
#include "../kernel/blk.h"

extern "C" bool slyz_disk_read(uint32_t lba, uint8_t* buf) {
    return blk_read(lba, buf) != 0;
}

extern "C" bool slyz_disk_write(uint32_t lba, const uint8_t* buf) {
    return blk_write(lba, buf) != 0;
}

extern "C" uint32_t slyz_disk_total_sectors(void) {
    return blk_total_sectors();
}
