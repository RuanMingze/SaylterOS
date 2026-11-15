/* ============================================================
 * kernel.c - SaylterOS C Kernel Main
 * ============================================================ */

#include <stdint.h>
#include <stddef.h>
#include "tty.h"
#include "shell.h"
#include "io.h"
#include "acpi.h"
#ifdef SAYLTER_VBE
#include "vbe.h"
#endif

/* linker.ld 定义的 BSS 边界 */
extern char _bss_start[];
extern char _bss_end[];

#define KERNEL_LOAD_ROWS 25
#define KERNEL_LOAD_COLS 80

static void kernel_delay(uint32_t count) {
    while (count--) __asm__ volatile ("nop");
}

static void kernel_print_centered(const char *text, uint16_t row, uint8_t color) {
    uint16_t len = 0;
    while (text[len] != '\0') {
        len++;
    }
    uint16_t x = (KERNEL_LOAD_COLS - len) / 2;
    tty_set_color(color);
    tty_set_cursor(x, row);
    while (*text) {
        tty_putchar(*text++);
    }
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

    for (int i = 0; logo[i] != NULL; i++) {
        kernel_print_centered(logo[i], 3 + i, 0x0A);
    }
}

static void kernel_clear_line(uint16_t row) {
    tty_set_cursor(0, row);
    tty_set_color(0x00);
    for (int i = 0; i < 80; i++) {
        tty_putchar(' ');
    }
    tty_set_cursor(0, row);
}

static void kernel_loading_anim(void) {
    static const char frames[] = "/-\\|";
    uint16_t base_row = 21;
    uint16_t label_x = (KERNEL_LOAD_COLS - 7) / 2 - 1;
    uint16_t spin_x = label_x + 8;

    kernel_clear_line(base_row);
    tty_set_cursor(label_x, base_row);
    tty_set_color(0x0E);
    printk("BOOTING");

    for (int i = 0; i < 16; i++) {
        tty_set_cursor(spin_x, base_row);
        tty_putchar(frames[i % 4]);
        kernel_delay(1000000);

        if (i + 1 < 16) {
            tty_set_cursor(spin_x, base_row);
            tty_putchar(' ');
        }
    }

    kernel_clear_line(base_row);
    tty_set_cursor(0, base_row + 1);
}

/* 在 asm _start 之前调用，清 BSS — 4 字节对齐快速清零 */
void _bss_init(void) {
    uint32_t *p = (uint32_t *)((uintptr_t)_bss_start & ~(uintptr_t)3);
    uint32_t *end = (uint32_t *)((uintptr_t)_bss_end & ~(uintptr_t)3);
    for (; p < end; p++)
        *p = 0;
}

void kernel_main(void) {
    /* --- 初始化 COM1 串口（QEMU serial log） --- */
    outb(0x3F8 + 3, 0x80);   /* DLAB on */
    outb(0x3F8 + 0, 115200 / 9600);    /* 9600 baud low */
    outb(0x3F8 + 1, 0);                /* 9600 baud high */
    outb(0x3F8 + 3, 0x03);   /* DLAB off, 8N1 */
    outb(0x3F8 + 1, 0x00);   /* interrupts off */

    /* --- 清屏 --- */
    tty_clear();

    /* --- 启动动画：C 内核加载中 --- */
    kernel_draw_logo();
    kernel_loading_anim();

    /* --- 启动信息 --- */
    tty_set_cursor(0, 18);
    tty_set_color(0x0F);
    printk("\n[KERNEL] SaylterOS C Kernel v0.3 booted!\n");
    printk("[KERNEL] Running in 32-bit protected mode\n");
    printk("[KERNEL] Interrupts enabled (PIC remapped to 0x20)\n");

    /* --- ACPI：只做固定表解析，拿到 PM1 后即可支持 S5 与电源键 --- */
    int acpi_ready = acpi_init();
    printk("[KERNEL] ACPI %s\n",
           acpi_ready ? "PM1 ready (S5 + power button)" : "not available");

    /* --- VBE 初始化 (bootloader 已在 RM 设好, 这里读结果) --- */
#ifdef SAYLTER_VBE
    vbe_init();
    if (vbe_is_enabled()) {
        printk("[KERNEL] VBE %dx%dx%d @ phys 0x%x\n",
               vbe_current.width, vbe_current.height, vbe_current.bpp,
               vbe_current.framebuffer);
        gui_main();      /* VBE 版：直接进 GUI */
    } else {
        printk("[KERNEL] VBE not available\n");
        shell_main();
    }
#else
    printk("[KERNEL] TTY mode (no VBE)\n");
    shell_main();
#endif

    /* shell_main 不应返回 */
    while (1) __asm__ volatile ("cli; hlt");
}
