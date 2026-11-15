#include <stdint.h>
#include "../kernel/ata.h"

extern "C" bool slyz_disk_read(uint32_t lba, uint8_t* buf) {
    return ata_read_sector(lba, buf) != 0;
}

extern "C" bool slyz_disk_write(uint32_t lba, const uint8_t* buf) {
    return ata_write_sector(lba, buf) != 0;
}

extern "C" uint32_t slyz_disk_total_sectors(void) {
    return ata_total_sectors();
}
