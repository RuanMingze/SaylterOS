/* ============================================================
 * reboot.h - SaylterOS Real Hardware Reboot
 * ------------------------------------------------------------
 * x86 裸金属真实硬件复位：
 *   1. CLI 关中断
 *   2. 等待 8042 键盘控制器输入缓冲空
 *   3. 发 0xFE 命令到 0x64 → 8042 拉 CPU RESET# 引脚
 *   4. 如果硬件复位没触发（极少数老旧机器），兜底 CLI;HLT
 *
 * 为什么用 8042？
 *   - 所有 x86 PC/模拟器（QEMU/Bochs/VMware）都支持
 *   - 直接连 CPU 复位引脚，绕过任何 OS/BIOS 状态
 *   - 比 LAPIC reset 更通用（LAPIC reset 需先确认 APIC 启用）
 * ============================================================ */

#ifndef REBOOT_H
#define REBOOT_H

void reboot(void);

#endif /* REBOOT_H */
