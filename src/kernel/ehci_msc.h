/* ============================================================
 * ehci_msc.h - EHCI(USB 2.0) + Bulk-Only Transport 块设备
 * ------------------------------------------------------------
 * 与 usb_msc.h 同构。blk.c 按 xHCI -> EHCI -> ATA 的顺序探测，
 * 因此老机器（只有 EHCI）和现代机器（xHCI）都能从 U 盘读 SlyzFS。
 * ============================================================ */

#ifndef EHCI_MSC_H
#define EHCI_MSC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int ehci_msc_init(void);
int ehci_msc_present(void);

int ehci_msc_read(uint32_t lba, uint8_t *buf);
int ehci_msc_write(uint32_t lba, const uint8_t *buf);

/* SCSI SYNCHRONIZE CACHE(10)：把设备写缓存刷到介质。 */
int ehci_msc_flush(void);

uint32_t ehci_msc_total_sectors(void);

void ehci_msc_stats(uint32_t *reads, uint32_t *writes);

const char *ehci_msc_name(void);

#ifdef __cplusplus
}
#endif

#endif /* EHCI_MSC_H */
