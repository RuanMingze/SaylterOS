/* ============================================================
 * blk.h - 块设备抽象层
 * ------------------------------------------------------------
 * 上层（SlyzFS / GUI / shell）只依赖这里，不再直接绑定 ATA。
 * 启动时 blk_init() 先尝试 USB(xHCI MSC)，成功则所有扇区读写走
 * U 盘；失败则回退到原来的 ATA PIO。
 * ============================================================ */

#ifndef BLK_H
#define BLK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 探测后端。可重复调用；已初始化时直接返回。 */
void blk_init(void);

/* 是否有可用磁盘 */
int blk_present(void);

/* 512 字节扇区读写，返回 1 成功 / 0 失败 */
int blk_read(uint32_t lba, uint8_t *buf);
int blk_write(uint32_t lba, const uint8_t *buf);

/* 把设备写缓存刷到介质。文件系统在日志提交 / close / unmount
 * 之后调用；机械盘与 USB 盘的掉电一致性依赖它。 */
int blk_flush(void);

/* 总扇区数 */
uint32_t blk_total_sectors(void);

/* 累计成功读/写扇区数 */
void blk_stats(uint32_t *reads, uint32_t *writes);

/* "USB(xHCI/MSC)" 或 "ATA PIO" */
const char *blk_backend(void);

#ifdef __cplusplus
}
#endif

#endif /* BLK_H */
