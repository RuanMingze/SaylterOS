/* ============================================================
 * kernel.c - SaylterOS C Kernel Main
 * ============================================================ */

#include <stdint.h>
#include <stddef.h>
#include "tty.h"
#include "shell.h"
#include "io.h"
#include "acpi.h"
#include "acpica_glue.h"
#include "vbe.h"

/* linker.ld 定义的 BSS 边界 */
extern char _bss_start[];
extern char _bss_end[];

static void kernel_delay(uint32_t count) {
    while (count--) __asm__ volatile ("nop");
}

static void kernel_print_centered(const char *text, uint16_t row, uint8_t color) {
    uint16_t len = 0;
    while (text[len] != '\0') {
        len++;
    }
    uint16_t cols = tty_get_cols();
    if (row >= tty_get_rows()) return;
    uint16_t x = (len < cols) ? (uint16_t)((cols - len) / 2) : 0;
    tty_set_color(color);
    tty_set_cursor(x, row);
    /* 整行一次性批量绘制，避免每个字符都做一次帧缓冲回写 */
    tty_begin_batch();
    while (*text) {
        tty_putchar(*text++);
    }
    tty_end_batch();
}

static void kernel_draw_logo(void) {
    static const char *logo[] = {
        "  _____              _ _          _____ _____ ______ ",
        " / ____|            | | |        / ____| ____|  ____|",
        "| (___   __ _  ___  | | |_ ___  | (___ | |__ | |__   ",
        " \\___ \\ / _` |/ _ \\ | | __/ _ \\  \\___ \\|  __||  __|  ",
        " ____) | (_| |  __/ | | || (_) | ____) | |___| |     ",
        "|_____/ \\__,_|\\___| |_|\\__\\___/ |_____/|_____|_|     ",
        "  [ SAYLTER OPERATING SYSTEM ] v0.3 - TTY Shell   ",
        NULL
    };

    /* 整块启动画面按屏幕高度垂直居中 */
    uint16_t rows = tty_get_rows();
    uint16_t top = (rows > 18) ? (uint16_t)((rows - 18) / 2) : 1;
    if (top < 1) top = 1;

    for (int i = 0; logo[i] != NULL; i++) {
        kernel_print_centered(logo[i], (uint16_t)(top + i), 0x0A);
    }
}

static uint16_t kernel_boot_top(void) {
    uint16_t rows = tty_get_rows();
    uint16_t top = (rows > 18) ? (uint16_t)((rows - 18) / 2) : 1;
    if (top < 1) top = 1;
    return top;
}

/* ACPI 最小支持（S5 / 电源键）与 AML 解释器初始化。
 * 特意放在启动画面播放期间完成：日志只走 COM1，屏幕上不出现
 * ACPI 初始化信息，启动界面保持干净且不会被日志淹没。 */
static void kernel_boot_acpi(void) {
    (void)acpi_init();
    (void)acpica_init();
}

static void kernel_loading_anim(void) {
    static const char frames[] = "/-\\|";
    uint16_t rows = tty_get_rows();
    uint16_t cols = tty_get_cols();
    uint16_t base_row = (uint16_t)(kernel_boot_top() + 9);
    if (base_row >= rows) base_row = (uint16_t)(rows - 1);

    uint16_t label_x = (cols > 7) ? (uint16_t)((cols - 7) / 2 - 1) : 0;
    uint16_t spin_x = (uint16_t)(label_x + 8);

    tty_clear_row(base_row, 0x00);
    tty_set_cursor(label_x, base_row);
    tty_set_color(0x0E);
    printk("BOOTING");

    for (int i = 0; i < 16; i++) {
        tty_set_cursor(spin_x, base_row);
        tty_putchar(frames[i % 4]);
        kernel_delay(1000000);

        /* 第 4 帧后完成 ACPI / AML 初始化：
         * 初始化耗时被启动动画吸收，用户看到的是持续转圈的启动界面。 */
        if (i == 3) {
            kernel_boot_acpi();
        }

        if (i + 1 < 16) {
            tty_set_cursor(spin_x, base_row);
            tty_putchar(' ');
        }
    }

    tty_clear_row(base_row, 0x00);
    tty_set_cursor(0, (uint16_t)(base_row + 1));
}

/* 在 asm _start 之前调用，清 BSS — 按指针宽度快速清零
 * 0xA00000 由 linker.ld 的 ALIGN 保证 8 字节对齐；尾部不足一个字的
 * 部分再逐字节补齐，避免越界写入 _bss_end 之后的物理内存。 */
void _bss_init(void) {
    uintptr_t start = (uintptr_t)_bss_start;
    uintptr_t end = (uintptr_t)_bss_end;
    uint8_t *byte = (uint8_t *)start;

    while ((uintptr_t)byte < end && (((uintptr_t)byte) & (uintptr_t)7u)) {
        *byte++ = 0;
    }

    uint64_t *word = (uint64_t *)byte;
    while ((uintptr_t)word + (uintptr_t)8u <= end) {
        *word++ = 0;
    }

    byte = (uint8_t *)word;
    while ((uintptr_t)byte < end) {
        *byte++ = 0;
    }
}

void kernel_main(void) {
    /* --- 初始化 COM1 串口（QEMU serial log） --- */
    outb(0x3F8 + 3, 0x80);   /* DLAB on */
    outb(0x3F8 + 0, 115200 / 9600);    /* 9600 baud low */
    outb(0x3F8 + 1, 0);                /* 9600 baud high */
    outb(0x3F8 + 3, 0x03);   /* DLAB off, 8N1 */
    outb(0x3F8 + 1, 0x00);   /* interrupts off */

    /* --- 显示子系统 ---
     * 统一走 VBE 图形终端：bootloader 已在实模式设好模式并把结果留在
     * 0x7DF0。tty_init 依据 framebuffer 尺寸与 16x32 字体算出字符行列。
     * VBE 不可用时 tty_init 自动回退到 80x25 文本模式。 */
    vbe_init();
    tty_init();

    /* --- 启动动画：C 内核加载中 --- */
    kernel_draw_logo();
    kernel_loading_anim();

    /* --- 启动信息 --- */
    uint16_t info_row = (uint16_t)(kernel_boot_top() + 12);
    if (info_row >= tty_get_rows()) info_row = (uint16_t)(tty_get_rows() - 1);
    tty_set_cursor(0, info_row);
    tty_set_color(0x0F);
    printk("[KERNEL] SaylterOS C Kernel v0.3 booted!\n");
    printk("[KERNEL] Running in 64-bit long mode\n");
    printk("[KERNEL] Interrupts enabled (PIC remapped to 0x20)\n");

    if (vbe_is_enabled()) {
        printk("[KERNEL] VBE %ux%u %ubpp @ phys 0x%x\n",
               vbe_current.width, vbe_current.height, vbe_current.bpp,
               vbe_current.framebuffer);
        printk("[KERNEL] Terminal %ux%u chars (16x32 ROM font, %u KB framebuffer)\n",
               tty_get_cols(), tty_get_rows(),
               (unsigned int)(((uint32_t)vbe_current.pitch * vbe_current.height) / 1024u));
    } else {
        printk("[KERNEL] VBE unavailable - 80x25 text mode fallback\n");
    }

    /* --- ACPI 与 AML 解释器已在启动画面播放期间初始化完成 --- */

    shell_main();

    /* shell_main 不应返回 */
    while (1) __asm__ volatile ("cli; hlt");
}
