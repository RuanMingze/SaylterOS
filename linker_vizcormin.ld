/* SaylterOS VizCormin 独立应用链接脚本
 * 固定加载地址 0x08000000，入口 vizcormin_entry。
 * .reloc 排在 BSS 之前，BSS 用 NOLOAD，避免 objcopy 把零页写进模块。 */
ENTRY(vizcormin_entry)

SECTIONS
{
    . = 0x08000000;

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
