/* SaylterOS musl 应用链接脚本
 *
 * 固定加载地址 0x09000000（避开驱动 0x02000000、SlyzFS 0x04000000、
 * VizCormin 0x08000000）。
 *
 * __tls_image / __tls_tdata_end / __tls_end 供 port/crt.c 覆盖 musl 的
 * 静态 TLS 初始化：flat binary 没有 ELF PT_TLS，只能靠链接期符号。
 * .tbss 与 .bss 都是 NOLOAD，由 .slys 加载器按 bss_size 清零。 */

ENTRY(slyzposix_entry)

SECTIONS
{
    . = 0x09000000;

    .text ALIGN(16) : {
        *(.text*)
    }

    .rodata ALIGN(16) : {
        *(.rodata*)
        *(.rdata*)
        *(.rdata$*)
        *(.data.rel.ro*)
    }

    /* musl 的 __libc_start_init 会遍历 __init_array_start..__init_array_end。
     * 这里显式收集，C++ 全局构造与 __attribute__((constructor)) 才能生效。 */
    .init_array ALIGN(8) : {
        __init_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array))
        __init_array_end = .;
    }

    .fini_array ALIGN(8) : {
        __fini_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*)))
        KEEP(*(.fini_array))
        __fini_array_end = .;
    }

    .data ALIGN(16) : {
        *(.data*)
    }

    .tdata ALIGN(64) : {
        __tls_image = .;
        *(.tdata*)
        __tls_tdata_end = .;
    }

    .tbss ALIGN(64) (NOLOAD) : {
        *(.tbss*)
    }

    __tls_end = .;

    __bss_start = .;
    .bss (NOLOAD) : {
        *(.bss*)
        *(COMMON)
    }
    __bss_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
        *(.gcc_except_table*)
    }
}
