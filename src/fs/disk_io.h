#ifndef SLYZ_DISK_IO_H
#define SLYZ_DISK_IO_H

/*
 * disk_io.h —— SlyzFS 磁盘抽象层
 *
 * 内核端需要实现这两个函数（目前 shell.c 里已有 ata_read_sector / ata_write_sector）。
 * 宿主测试端（main.cpp）用内存模拟磁盘来跑完整文件系统逻辑。
 *
 * 所有 sector 读写都是 512 字节，LBA 模式，ATA PIO 兼容。
 */

#include <cstdint>
#include <cstddef>

static constexpr uint32_t SLYZ_SECTOR_SIZE = 512;

extern "C" {
/* 读/写一个扇区。返回 true 表示成功，false 表示 ATA 超时或磁盘不存在。 */
bool slyz_disk_read(uint32_t lba, uint8_t* buf);
bool slyz_disk_write(uint32_t lba, const uint8_t* buf);

/* 可选：返回磁盘总扇区数（用于校验边界）。返回 0 表示未知。 */
uint32_t slyz_disk_total_sectors();
}

#endif /* SLYZ_DISK_IO_H */
