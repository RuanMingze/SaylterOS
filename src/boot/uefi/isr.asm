; ============================================================
; isr.asm - UEFI 引导器的中断入口（64 位）
; ------------------------------------------------------------
; 内核（keyboard.c / vm.c）约定：
;   0x60000        256 项 64 位 IDT（由 bootx64.c 填充）
;   0x70000..      页表（PML4 / PDPT / PD）
;   0xA000/0xA002  键盘环形缓冲 head/tail
;   0xA010         64 字节扫描码缓冲
;   0x7DE0         #PF 回调（内核 vm_init 写入）
;   0x7DE8         SCI 回调（内核 ACPICA 写入）
;
; 与 pm.asm 的语义保持一致，保证 UEFI 与 BIOS 两条引导路径
; 对内核呈现相同的中断环境。
; ============================================================
[BITS 64]

section .text

global isr_default
global isr_timer
global isr_keyboard
global isr_sci
global isr_spurious
global isr_ipi
global isr_exc14
global isr_exc_halt

KB_HEAD_ADDR equ 0xA000
KB_TAIL_ADDR equ 0xA002
KB_BUF_ADDR  equ 0xA010
KB_BUF_MASK  equ 63

PF_HANDLER_ADDR equ 0x7DE0
PF_LAST_CR2     equ 0x7DD0
PF_LAST_ERROR   equ 0x7DD8
PF_LAST_RIP     equ 0x7DC8
SCI_CALLBACK_ADDR equ 0x7DE8

; 未注册向量：EOI 双 PIC 后返回
isr_default:
    push rax
    push rdx
    mov al, 0x20
    out 0x20, al
    out 0xA0, al
    pop rdx
    pop rax
    iretq

; LAPIC spurious：不需要任何 EOI
isr_spurious:
    iretq

; fixed IPI：不需要 EOI，绝不能误清 8259
isr_ipi:
    iretq

; PIT 定时器：特定 EOI（主片 IRQ0）
isr_timer:
    push rax
    push rdx
    mov al, 0x60
    out 0x20, al
    pop rdx
    pop rax
    iretq

; PS/2 键盘：扫描码进入环形缓冲，C 层轮询消费
isr_keyboard:
    push rax
    push rcx
    push rdx
    push rsi

    in al, 0x64
    mov cl, al
    and cl, 0x21
    cmp cl, 0x01
    jne .not_kbd

    in al, 0x60
    movzx esi, word [KB_HEAD_ADDR]
    lea edx, [esi + 1]
    and edx, KB_BUF_MASK
    movzx ecx, word [KB_TAIL_ADDR]
    cmp edx, ecx
    je .drop
    mov [rsi + KB_BUF_ADDR], al
    mov [KB_HEAD_ADDR], dx
    jmp .drop

.not_kbd:
    test al, 0x01
    jz .drop
    in al, 0x60              ; 鼠标字节：读走丢弃

.drop:
    mov al, 0x61             ; 特定 EOI：主片 IRQ1
    out 0x20, al
    pop rsi
    pop rdx
    pop rcx
    pop rax
    iretq

; IRQ9 = SCI：转交内核 ACPI 分发器
isr_sci:
    push rax
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11

    mov rax, [SCI_CALLBACK_ADDR]
    test rax, rax
    jz .no_handler
    sub rsp, 32
    call rax
    add rsp, 32

.no_handler:
    mov al, 0x61             ; 从片 IRQ1
    out 0xA0, al
    mov al, 0x62             ; 主片 IRQ2（级联）
    out 0x20, al

    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rax
    iretq

; #PF：调用内核 page_fault_handler(cr2, error, rip)
; 返回值 1 = 已建立映射，重新执行触发指令；0 = 致命。
isr_exc14:
    push rbp
    push rax
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13

    mov rbp, rsp
    mov r12, [rbp + 96]      ; error code
    mov r13, [rbp + 104]     ; rip

    mov rax, cr2
    mov [PF_LAST_CR2], rax
    mov [PF_LAST_ERROR], r12
    mov [PF_LAST_RIP], r13

    mov rax, [PF_HANDLER_ADDR]
    test rax, rax
    jz .fatal

    and rsp, -16
    sub rsp, 32
    mov rcx, cr2
    mov rdx, r12
    mov r8, r13
    call rax
    mov rsp, rbp

    test eax, eax
    jz .fatal

    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rax
    pop rbp
    add rsp, 8               ; 丢弃 error code
    iretq

.fatal:
    mov rsp, rbp
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rax
    pop rbp
    add rsp, 8

.halt_pf:
    cli
    hlt
    jmp .halt_pf

; 其余异常：直接停机，避免异常风暴
isr_exc_halt:
    cli
.halt:
    hlt
    jmp .halt
