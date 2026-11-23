/* ============================================================
 * usb_msc.h - xHCI + USB Mass Storage (Bulk-Only Transport) 块设备
 * ------------------------------------------------------------
 * 供 blk.c 调用。初始化失败（没有 xHCI、没有 U 盘、传输超时）
 * 时 present() 返回 0，块层自动回退 ATA。
 * ============================================================ */

#ifndef USB_MSC_H
#define USB_MSC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 探测并初始化第一个 xHCI 控制器上的第一个 BOT/SCSI 大容量设备。
 * 返回 1 表示可用。可重复调用，已初始化时直接返回状态。 */
int usb_msc_init(void);

int usb_msc_present(void);

/* 512 字节扇区读写，返回 1 成功 / 0 失败 */
int usb_msc_read(uint32_t lba, uint8_t *buf);
int usb_msc_write(uint32_t lba, const uint8_t *buf);

/* SCSI SYNCHRONIZE CACHE(10)：把设备写缓存刷到介质。 */
int usb_msc_flush(void);

/* READ CAPACITY 返回的总扇区数 */
uint32_t usb_msc_total_sectors(void);

/* 累计成功读/写扇区数 */
void usb_msc_stats(uint32_t *reads, uint32_t *writes);

/* 设备型号（INQUIRY 的 vendor + product，静态缓冲） */
const char *usb_msc_name(void);

/* ---- USB HID 启动键盘（2.4G 无线键盘接收器 / USB 键盘） ----
 * 复用同一个 xHCI 控制器，使用独立槽位与中断 IN 端点。
 * usb_kbd_poll_report() 非阻塞：有新报告返回 1，并把 8 字节启动
 * 协议报告写入 out8（修饰键 + 保留 + 6 个 HID 键码）。 */
int  usb_kbd_init(void);
int  usb_kbd_poll_report(uint8_t *out8);
int  usb_kbd_present(void);

#ifdef __cplusplus
}
#endif

#endif /* USB_MSC_H */
