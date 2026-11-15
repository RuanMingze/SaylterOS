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

#ifdef __cplusplus
}
#endif

#endif /* SAYLTER_ATA_H */
