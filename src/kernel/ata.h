#ifndef SAYLTER_ATA_H
#define SAYLTER_ATA_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int ata_disk_present(void);
int ata_read_sector(uint32_t lba, uint8_t* buf);
int ata_write_sector(uint32_t lba, const uint8_t* buf);
uint32_t ata_total_sectors(void);
void ata_stats(uint32_t *reads, uint32_t *writes);

/* 机械盘健壮性：显式刷盘（FLUSH CACHE）与写缓存开关。
 * 文件系统在日志提交点 / close / unmount 调用 ata_flush()。 */
int  ata_flush(void);
void ata_enable_write_cache(void);

#ifdef __cplusplus
}
#endif

#endif /* SAYLTER_ATA_H */
