; ============================================================
; entry32.asm - C Kernel 启动入口 (保护模式)
; 链接地址: 0xC200 (由 linker.ld 决定)
; 被 pm.asm 从 0x8600 跳过来
; MinGW 给 C 函数加 _ 前缀!
; ============================================================
; ============================================================
; entry32.asm - C Kernel 启动入口 (保护模式)
; 链接地址: 0xC200 (由 linker.ld 决定)
; 被 pm.asm 从 0x8600 跳过来
; MinGW 生成的实际符号名：_kernel_main, __bss_init
; ============================================================
[BITS 32]

extern kernel_main
extern _bss_init
global _start

_start:
    ; --- 设内核栈 ---
    mov esp, 0x9FFFF
    mov ebp, esp

    ; --- 清 BSS ---
    call _bss_init

    ; --- 调用 C kernel_main() ---
    call kernel_main

.halt:
    cli
    hlt
    jmp .halt
