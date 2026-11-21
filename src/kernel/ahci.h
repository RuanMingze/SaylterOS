/* ============================================================
 * ahci.h - AHCI(SATA) 块设备
 * ------------------------------------------------------------
 * 与 usb_msc.h / ehci_msc.h / ata.h 同构，供 blk.c 统一调度。
 * ============================================================ */

#ifndef AHCI_H
#define AHCI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int ahci_init(void);
int ahci_present(void);

int ahci_read(uint32_t lba, uint8_t *buf);
int ahci_write(uint32_t lba, const uint8_t *buf);

/* FLUSH CACHE：把控制器/磁盘写缓存刷到盘片。 */
int ahci_flush(void);

uint32_t ahci_total_sectors(void);

void ahci_stats(uint32_t *reads, uint32_t *writes);

const char *ahci_name(void);

#ifdef __cplusplus
}
#endif

#endif /* AHCI_H */
