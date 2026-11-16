/* SaylterOS 可加载音频驱动链接脚本
 * 固定加载地址 0x02000000，入口 slys_driver_entry。
 * .reloc 必须排在 BSS 之前，BSS 用 NOLOAD，否则 objcopy 会把
 * 8 MB 的静态缓冲当成文件内容写进 .slys。 */
ENTRY(slys_driver_entry)

SECTIONS
{
    . = 0x02000000;

    .text ALIGN(16) : {
        *(.text*)
    }

    .rodata ALIGN(16) : {
        *(.rodata*)
        *(.rdata*)
        *(.rdata$*)
    }

    .data ALIGN(16) : {
        *(.data*)
    }

    .reloc ALIGN(16) : {
        *(.reloc)
    }

    __bss_start = .;
    .bss (NOLOAD) : {
        *(.bss*)
        *(COMMON)
    }
    __bss_end = .;
}
