/* SaylterOS Kernel Linker Script
 * pm.bin ORG 0x8600, 大小 ~6.5KB → 末尾 0xA00B
 * kernel 链接在 0xA100, 给 pm 留 4KB 余量
 */

ENTRY(_start)

SECTIONS {
    . = 0xA100;

    .text ALIGN(4) : {
        *(.multiboot)
        *(.text*)
    }

    .rodata ALIGN(4) : {
        *(.rodata*)
        *(.rdata*)          /* MinGW const 大数组在这里 */
        *(.rdata$*)         /* MinGW 子段 */
    }

    .data ALIGN(4) : {
        *(.data*)
    }

    _bss_start = .;
    .bss ALIGN(4) : {
        *(.bss*)
        *(COMMON)
    }
    _bss_end = .;

    end = .;
}
