; ============================================================
; entry64.asm - C 内核 64 位入口（长模式）
; ------------------------------------------------------------
; 链接地址由 linker.ld 决定（0x10000，PE image base 64KB 对齐）。
; pm.asm 完成长模式切换、页表、64 位 IDT、PIC remap 后跳到这里。
;
; x86_64-w64-mingw32 使用 Win64 调用约定：
;   - 调用点 RSP 必须 16 字节对齐
;   - 调用者必须预留 32 字节 shadow space
;   - 符号没有前导下划线
; ============================================================
[BITS 64]

global _start
extern kernel_main
extern _bss_init

section .text

_start:
    ; 内核栈顶：0x3FF000（16 字节对齐），向下增长到内核镜像之上。
    ; 0x100000 起是内核镜像，0x400000 起是内核堆，中间留出约 2MB
    ; 给 ACPICA 解释器的深递归调用。
    mov rsp, 0x3FF000
    xor ebp, ebp
    cld

    ; shadow space（Win64 要求），同时保持 RSP 16 字节对齐
    sub rsp, 32

    ; 清 BSS：4 字节时代遗留的 32 位实现已改为指针宽度循环
    call _bss_init

    ; 进入 C 世界
    call kernel_main

    ; kernel_main 不应返回
    add rsp, 32
.halt:
    cli
    hlt
    jmp .halt
