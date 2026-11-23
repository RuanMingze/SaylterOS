/* ============================================================
 * blk.c - 块设备后端选择
 * ------------------------------------------------------------
 * 顺序：xHCI(USB3) -> EHCI(USB2) -> AHCI(SATA) -> ATA PIO。
 * 选择在初始化时确定一次：U 盘在系统运行期间不会被替换，
 * 固定后端可以避免每次读写都做探测。
 * ============================================================ */

#include "blk.h"
#include "usb_msc.h"
#include "ehci_msc.h"
#include "nvme.h"
#include "ahci.h"
#include "ata.h"

#define BLK_ATA   0
#define BLK_XHCI  1
#define BLK_EHCI  2
#define BLK_AHCI  3
#define BLK_NVME  4

static int g_initialized;
static int g_backend;

void blk_init(void) {
    if (g_initialized) {
        return;
    }
    g_initialized = 1;

    if (usb_msc_init() && usb_msc_present()) {
        g_backend = BLK_XHCI;
        return;
    }
    if (ehci_msc_init() && ehci_msc_present()) {
        g_backend = BLK_EHCI;
        return;
    }
    if (nvme_init() && nvme_present()) {
        g_backend = BLK_NVME;
        return;
    }
    if (ahci_init() && ahci_present()) {
        g_backend = BLK_AHCI;
        return;
    }
    g_backend = BLK_ATA;
    /* ACPI 已完成控制器/设备枚举与 D0 上电，这里再打开 ATA 写缓存：
     * 顺序写走缓存、提交点显式 flush，兼顾机械盘性能与掉电一致性。 */
    ata_enable_write_cache();
}

int blk_try_usb(void) {
    if (g_initialized) {
        return 1;
    }
    if (usb_msc_init() && usb_msc_present()) {
        g_backend = BLK_XHCI;
        g_initialized = 1;
        return 1;
    }
    if (ehci_msc_init() && ehci_msc_present()) {
        g_backend = BLK_EHCI;
        g_initialized = 1;
        return 1;
    }
    return 0;
}

int blk_present(void) {
    if (!g_initialized) {
        blk_init();
    }
    if (g_backend == BLK_XHCI || g_backend == BLK_EHCI ||
        g_backend == BLK_AHCI || g_backend == BLK_NVME) {
        return 1;
    }
    return ata_disk_present();
}

int blk_read(uint32_t lba, uint8_t *buf) {
    if (!g_initialized) {
        blk_init();
    }
    if (g_backend == BLK_XHCI) {
        return usb_msc_read(lba, buf);
    }
    if (g_backend == BLK_EHCI) {
        return ehci_msc_read(lba, buf);
    }
    if (g_backend == BLK_AHCI) {
        return ahci_read(lba, buf);
    }
    if (g_backend == BLK_NVME) {
        return nvme_read(lba, buf);
    }
    return ata_read_sector(lba, buf);
}

int blk_read_multi(uint32_t lba, uint32_t count, uint8_t *buf) {
    if (!g_initialized) {
        blk_init();
    }
    if (count == 0) return 1;

    /* ATA 后端支持总线主控 DMA；其它后端逐扇区回退，
     * 语义与 blk_read 完全一致（AHCI/NVMe 本身已是 DMA）。 */
    if (g_backend == BLK_ATA && count > 1) {
        if (ata_read_multi(lba, count, buf)) return 1;
    }

    for (uint32_t i = 0; i < count; i++) {
        if (!blk_read(lba + i, buf + i * 512u)) return 0;
    }
    return 1;
}

int blk_write(uint32_t lba, const uint8_t *buf) {
    if (!g_initialized) {
        blk_init();
    }
    if (g_backend == BLK_XHCI) {
        return usb_msc_write(lba, buf);
    }
    if (g_backend == BLK_EHCI) {
        return ehci_msc_write(lba, buf);
    }
    if (g_backend == BLK_AHCI) {
        return ahci_write(lba, buf);
    }
    if (g_backend == BLK_NVME) {
        return nvme_write(lba, buf);
    }
    return ata_write_sector(lba, buf);
}

int blk_flush(void) {
    if (!g_initialized) {
        blk_init();
    }
    if (g_backend == BLK_XHCI) {
        return usb_msc_flush();
    }
    if (g_backend == BLK_EHCI) {
        return ehci_msc_flush();
    }
    if (g_backend == BLK_AHCI) {
        return ahci_flush();
    }
    if (g_backend == BLK_NVME) {
        return nvme_flush();
    }
    return ata_flush();
}

uint32_t blk_total_sectors(void) {    if (!g_initialized) {
        blk_init();
    }
    if (g_backend == BLK_XHCI) {
        return usb_msc_total_sectors();
    }
    if (g_backend == BLK_EHCI) {
        return ehci_msc_total_sectors();
    }
    if (g_backend == BLK_AHCI) {
        return ahci_total_sectors();
    }
    if (g_backend == BLK_NVME) {
        return nvme_total_sectors();
    }
    return ata_total_sectors();
}

void blk_stats(uint32_t *reads, uint32_t *writes) {
    if (!g_initialized) {
        blk_init();
    }
    if (g_backend == BLK_XHCI) {
        usb_msc_stats(reads, writes);
    } else if (g_backend == BLK_EHCI) {
        ehci_msc_stats(reads, writes);
    } else if (g_backend == BLK_AHCI) {
        ahci_stats(reads, writes);
    } else if (g_backend == BLK_NVME) {
        nvme_stats(reads, writes);
    } else {
        ata_stats(reads, writes);
    }
}

const char *blk_backend(void) {
    if (!g_initialized) {
        blk_init();
    }
    if (g_backend == BLK_XHCI) {
        return "USB3(xHCI/MSC)";
    }
    if (g_backend == BLK_EHCI) {
        return "USB2(EHCI/MSC)";
    }
    if (g_backend == BLK_AHCI) {
        return "SATA(AHCI)";
    }
    if (g_backend == BLK_NVME) {
        return "NVMe(PCIe)";
    }
    return "ATA PIO";
}
