/* ============================================================
 * nvme.h - NVMe 块设备驱动
 * ------------------------------------------------------------
 * 目标：在 UEFI/Legacy 机器上直接驱动 PCIe NVMe SSD，让 SlyzFS
 * 可以跑在现代主硬盘上，不再依赖 AHCI/ATA 兼容层。
 *
 * 边界：
 *   - 只支持 512 字节逻辑块（512e/512n）；4K-native 盘拒绝接管；
 *   - 单命名空间（NSID 1）、单 I/O 队列、轮询完成，不注册中断；
 *   - BAR0 必须位于 4GB 以下（内核只有 4GB 恒等映射）。
 * ============================================================ */

#ifndef NVME_H
#define NVME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 探测并初始化。成功返回 1；无设备/不满足边界返回 0。 */
int nvme_init(void);
int nvme_present(void);

/* 512 字节扇区读写/刷盘，返回 1 成功 / 0 失败。 */
int nvme_read(uint32_t lba, uint8_t *buf);
int nvme_write(uint32_t lba, const uint8_t *buf);
int nvme_flush(void);

uint32_t nvme_total_sectors(void);
void nvme_stats(uint32_t *reads, uint32_t *writes);
const char *nvme_name(void);

#ifdef __cplusplus
}
#endif

#endif /* NVME_H */
