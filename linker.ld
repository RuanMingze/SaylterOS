/* SaylterOS Kernel Linker Script (x86_64)
 * 支持 ACPICA 之后的布局（内核镜像上限 3MB，堆 6MB）
 *
 * 物理内存布局：
 *   0x00010000 - 0x0009FFFF  引导（MBR / stage2 / pm.bin）
 *   0x00060000 - 0x00060FFF  64 位 IDT（pm.asm 运行时填充）
 *   0x00070000 - 0x00075FFF  页表（pm.asm 运行时填充）
 *   0x00100000 - 0x003F0000  内核镜像 .text/.rodata/.data/.reloc（上限 3MB）
 *   0x003F0000 - 0x00400000  内核初始栈顶（rsp = 0x3FF000，向下增长）
 *   0x00400000 - 0x00A00000  内核堆（mem.c，6MB）
 *   0x00A00000 - 0x01200000  VBE backbuffer（最多 8MB）
 *   0x01200000 - ...         内核 .bss
 *
 * 内核镜像必须留在 1MB 以上：640KB 传统内存已被 boot/stage2/pm 与
 * VGA/BIOS 区占满，装不下带 AML 解释器的内核。
 * .bss 必须放在 backbuffer 之上：1600x1200 的 backbuffer 占 7.68MB，
 * 若 BSS 与 backbuffer 重叠，会被逐帧像素写入整片清零，
 * 表现为文件系统对象/账户表随机丢失。
 */

ENTRY(_start)

SECTIONS {
    . = 0x100000;

    .text ALIGN(16) : {
        *(.multiboot)
        *(.text*)
    }

    .rodata ALIGN(16) : {
        *(.rodata*)
        *(.rdata*)          /* MinGW const 大数组在这里 */
        *(.rdata$*)         /* MinGW 子段 */
    }

    .data ALIGN(16) : {
        *(.data*)
    }

    /* PE 重定位段：基址固定，运行时不需要。显式放在低地址，
     * 避免它被排到高位 BSS 之后把 kernel.bin 撑到几十 MB。 */
    .reloc ALIGN(16) : {
        *(.reloc)
    }

    /* BSS 固定在 18MB，位于 8MB backbuffer 之后 */
    _bss_start = 0x01200000;
    .bss _bss_start (NOLOAD) : {
        *(.bss*)
        *(COMMON)
    }
    _bss_end = .;

    end = .;
}
