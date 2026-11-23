/* ============================================================
 * slyzfs_part.h - SlyzFS v2 分区/盘符管理（内核侧 API）
 * ------------------------------------------------------------
 * 分区表位于启动盘 LBA8192（备份 8193），格式定义见 slysfs.h。
 * 这里只暴露"读表、建分区、删分区、格式化、挂载/卸载"，
 * 所有写操作都只作用于启动盘。
 *
 * 盘符索引：0 = C:（SYSTEM），1 = R:（RECOVERY），2 = D:（DATA）。
 * ============================================================ */

#ifndef SLYZFS_PART_H
#define SLYZFS_PART_H

#include <stdint.h>

struct SlyzPartInfo {
    int      drive;          /* 0..2 */
    int      type;           /* 1=SYSTEM 2=RECOVERY 3=DATA */
    uint32_t start_lba;
    uint32_t sectors;
    int      mounted;
    char     label[24];
};

/* 读取分区表并返回有效条目数；mounted 来自模块的卷状态。
 * 返回 -1 表示分区表损坏或模块未加载。 */
int slyzfs_part_list(struct SlyzPartInfo *out, int max);

/* 在空闲空间创建分区，完成格式化并挂载。
 * sectors == 0 时按最大空闲区自动取一半（至少 4096 扇区）。
 * label 为空时使用 SYSTEM/RECOVERY/DATA。
 * 返回 0 成功，-1 失败（无空闲空间/盘符占用/格式化失败）。 */
int slyzfs_part_add(int drive, uint32_t sectors, const char *label);

/* 删除分区表条目并卸载卷。拒绝删除正在引导的 C:。
 * 返回 0 成功，-1 失败。 */
int slyzfs_part_delete(int drive);

/* 重新格式化指定分区（销毁数据）并挂载。返回 0 成功。 */
int slyzfs_part_format(int drive, const char *label);

/* 按分区表条目挂载 / 卸载卷。返回 0 成功。 */
int slyzfs_part_mount(int drive);
int slyzfs_part_unmount(int drive);

#endif /* SLYZFS_PART_H */
