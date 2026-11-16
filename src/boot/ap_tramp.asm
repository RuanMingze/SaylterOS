; ============================================================
; ap_tramp.asm - AP（Application Processor）启动引导
; ------------------------------------------------------------
; INIT-SIPI-SIPI 之后，AP 从 CS:IP = (vector<<12):0000 开始执行，
; 此时它处于 16 位实模式、A20 可能未开、段寄存器内容不确定。
; 本段代码负责把它带到与 BSP 相同的 64 位长模式环境：
;
;   实模式 -> 装载临时 GDT -> 保护模式(32 位)
;          -> PAE + CR3(复用 BSP 页表) + EFER.LME -> 长模式(64 位)
;          -> 切到每核栈 -> call C 入口
;
; 关键约束：
;   1. 代码会被内核复制到固定物理地址 0x7000 执行，但链接地址在
;      1MB 以上，所以所有"地址类"操作数都必须用 RUN() 换成运行时
;      地址，否则跳转与数据访问会落到错误位置。
;   2. 0x7000..0x7FFF 只有一页，0x7DF0 起是 bootloader 留下的 VBE
;      信息，因此整段代码必须远小于 0xDF0 字节。
;   3. 数据区放在最前面，C 侧按符号偏移填写 CR3、入口与栈顶。
; ============================================================

BITS 16

%define TRAMP_BASE 0x7000
%define TRAMP_STACK16 0x6000          ; 16 位阶段临时栈（低于代码页）

; 把链接期地址换算成运行时地址
%define RUN(x) ((x) - ap_tramp_start + TRAMP_BASE)

section .text

global ap_tramp_start
global ap_tramp_end
global ap_tramp_cr3_off
global ap_tramp_entry_off
global ap_tramp_stack_off

ap_tramp_start:
    ; ---- 数据区（C 侧写入）----
    align 8
ap_tramp_cr3_off:
    dd 0                               ; BSP 的 CR3（同一份页表）
ap_tramp_entry_off:
    dq 0                               ; 64 位 C 入口
ap_tramp_stack_off:
    dq 0                               ; 本核栈顶

ap_start16:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, TRAMP_STACK16

    ; 装载临时 GDT（16 位模式下 lgdt 使用 32 位基址）
    lgdt [RUN(gdtr32)]

    ; 打开保护模式
    mov eax, cr0
    or al, 1
    mov cr0, eax

    ; 远跳进 32 位代码段（0x08）
    jmp 0x08:RUN(pm32_entry)

; ------------------------------------------------------------
; 32 位保护模式阶段
; ------------------------------------------------------------
BITS 32
pm32_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    ; CR4.PAE = 1
    mov eax, cr4
    or eax, 0x20
    mov cr4, eax

    ; 复用 BSP 的页表：4GB 恒等映射，2MB 大页
    mov eax, [RUN(ap_tramp_cr3_off)]
    mov cr3, eax

    ; EFER.LME = 1
    mov ecx, 0xC0000080
    rdmsr
    or eax, 0x100
    wrmsr

    ; CR0.PG = 1，进入长模式
    mov eax, cr0
    or eax, 0x80000000
    mov cr0, eax

    ; 远跳进 64 位代码段（0x18）
    jmp 0x18:RUN(pm64_entry)

; ------------------------------------------------------------
; 64 位长模式阶段
; ------------------------------------------------------------
BITS 64
pm64_entry:
    mov ax, 0x20
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    mov rsp, [abs RUN(ap_tramp_stack_off)]

    mov rax, [abs RUN(ap_tramp_entry_off)]
    call rax

.halt:
    cli
    hlt
    jmp .halt

; ------------------------------------------------------------
; 临时 GDT：包含 32 位与 64 位两套段描述符
; ------------------------------------------------------------
align 8
gdt32:
    dq 0x0000000000000000              ; 0x00 Null

    ; 0x08: 32 位代码
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 0x9A
    db 0xCF
    db 0x00

    ; 0x10: 32 位数据
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 0x92
    db 0xCF
    db 0x00

    ; 0x18: 64 位代码
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 0x9A
    db 0xAF
    db 0x00

    ; 0x20: 长模式数据
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 0x92
    db 0xCF
    db 0x00
gdt32_end:

gdtr32:
    dw gdt32_end - gdt32 - 1
    dd RUN(gdt32)

ap_tramp_end:
