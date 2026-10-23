; ============================================================
; SaylterOS pm.asm - Phase 2 完整版
; GDT ✅ (已验证) → PIC → IDT → ISR → STI → IRQ0+IRQ1
; ============================================================
ORG 0x8600
[BITS 16]

pm_start:
    cli
    xor ax, ax
    mov ss, ax
    mov sp, 0x8000

    lgdt [gdt_descriptor]

    mov eax, cr0
    or eax, 1
    mov cr0, eax

    jmp 0x08:pm_entry

[BITS 32]
pm_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov esp, 0x9FFFF

    ; --- 清屏 ---
    mov edi, 0xB8000
    mov ecx, 2000
    mov eax, 0x0720
    rep stosw

    ; --- VGA 打印辅助 (esi=字符串, edi=VGA地址, ah=颜色) ---
    ; 复用 ax, esi, edi, 会被覆盖
    ; 调用前设置好 esi, edi, ah

    ; ========================================
    ; Step 1: PIC 8259 重映射
    ; ========================================
    mov al, 0x11          ; ICW1: edge-triggered, cascade, need ICW4
    out 0x20, al
    mov al, 0x20          ; ICW2: IRQ0-7 → IDT 0x20-0x27
    out 0x21, al
    mov al, 0x04          ; ICW3: master 上 IRQ2 连 slave
    out 0x21, al
    mov al, 0x01          ; ICW4: 8086 mode
    out 0x21, al
    mov al, 0xFC          ; IMR: 开 IRQ0(timer) + IRQ1(keyboard)
    out 0x21, al

    mov al, 0x11          ; Slave PIC
    out 0xA0, al
    mov al, 0x28          ; ICW2: IRQ8-15 → IDT 0x28-0x2F
    out 0xA1, al
    mov al, 0x02          ; ICW3: slave 接 master IRQ2
    out 0xA1, al
    mov al, 0x01          ; ICW4
    out 0xA1, al
    mov al, 0xFF          ; Slave: 全屏蔽
    out 0xA1, al

    mov esi, msg_pic_ok
    mov edi, 0xB8000 + 2 * 160
    mov ah, 0x2F
    call print_at

    ; ========================================
    ; Step 2: 构建 IDT
    ; ========================================
    call build_idt

    mov esi, msg_idt_ok
    mov edi, 0xB8000 + 4 * 160
    mov ah, 0x2F
    call print_at

    lidt [idtr]

    mov esi, msg_idtr_ok
    mov edi, 0xB8000 + 6 * 160
    mov ah, 0x2F
    call print_at

    ; ========================================
    ; Step 3: 开中断 + 跳到 C kernel
    ; ========================================
    sti

    mov esi, msg_sti_ok
    mov edi, 0xB8000 + 9 * 160
    mov ah, 0x0F
    call print_at

    ; --- 跳到 C kernel entry (0xA100) ---
    ; kernel 链接在 0xA100, 给 pm.bin (6.5KB) 留足空间
    jmp 0xA100

; ============================================================
; 辅助：VGA 打印
; 入参: esi=字符串, edi=VGA地址, ah=颜色
; 破坏: al, esi, edi
; ============================================================
print_at:
    mov al, [esi]
    test al, al
    jz .done
    mov [edi], ax
    inc esi
    add edi, 2
    jmp print_at
.done:
    ret

; ============================================================
; 构建 IDT 表
; ============================================================
build_idt:
    pusha
    lea edi, [idt_entries]
    ; 🔥 关键修复：所有 256 项先填 default handler
    mov ecx, 256
.fill_default:
    mov eax, isr_default
    mov [edi], ax               ; offset low
    mov word [edi + 2], 0x08    ; CS
    mov byte [edi + 4], 0
    mov byte [edi + 5], 0x8E    ; P=1, DPL=0, interrupt gate
    shr eax, 16
    mov [edi + 6], ax           ; offset high
    mov byte [edi + 7], 0
    add edi, 8
    dec ecx
    jnz .fill_default

    ; 覆盖 IRQ0 (IDT 0x20): Timer
    lea edi, [idt_entries + 0x20 * 8]
    lea esi, [isr_timer]
    call set_idt_entry

    ; 覆盖 IRQ1 (IDT 0x21): Keyboard
    lea edi, [idt_entries + 0x21 * 8]
    lea esi, [isr_keyboard]
    call set_idt_entry
    popa
    ret

; EDI = entry addr, ESI = handler addr
set_idt_entry:
    mov eax, esi
    mov [edi], ax          ; offset low 16
    mov word [edi + 2], 0x08    ; CS = Code Segment
    mov byte [edi + 4], 0        ; reserved
    mov byte [edi + 5], 0x8E     ; P=1, DPL=0, S=1 (interrupt gate 32-bit)
    shr eax, 16
    mov [edi + 6], ax          ; offset high 16
    ret

; ============================================================
; ISR 存根
; ============================================================
isr_default:
    pusha
    ; 对于未注册的 CPU 异常 (0-31)，有些会 push error code 在 iret 之上
    ; 简单起见：跳过 error code (esp+32 的位置)
    ; 以后补完整的 exception handler + error code 处理
    popa
    add esp, 4          ; 跳过可能的 error code
    iret

isr_timer:
    pusha
    ; Timer IRQ 只需要发 EOI；不要写屏幕，否则会污染 UI。
    mov al, 0x20
    out 0x20, al
    popa
    iret

isr_keyboard:
    pusha
    in al, 0x60
    ; 传递原始扫描码到 mailbox，保留按下/抬起状态。
    ; C 层会在 kb_read_char() 中解释 0x80 release code。
    mov byte [0xA000], al
    mov byte [0xA001], 1
    mov al, 0x20
    out 0x20, al
    popa
    iret

; ============================================================
; GDT (每字段单独 dw/db，确保字节对齐正确)
; ============================================================
gdt_start:
    dq 0x0000000000000000      ; Null

    ; Code: Limit=4GB, Base=0, Ring0, 32-bit
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 0x9A                    ; P=1, DPL=0, S=1, Code/Readable
    db 0xCF                    ; G=1, D=1 (32-bit), LimitH=0xF
    db 0x00

    ; Data: Limit=4GB, Base=0, Ring0, 32-bit
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 0x92                    ; P=1, DPL=0, S=1, Data/Writable
    db 0xCF                    ; G=1, D=1 (32-bit)
    db 0x00
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

; ============================================================
; IDT (256 × 8B = 2KB)
; ============================================================
ALIGN 8
idt_entries:
    times 256 * 8 db 0
idtr:
    dw (idt_end - idt_entries - 1)
    dd idt_entries
idt_end:

; ============================================================
; 消息
; ============================================================
msg_pic_ok:      db '[PIC] 8259 remapped OK', 0
msg_idt_ok:      db '[IDT] 256 entries built', 0
msg_idtr_ok:     db '[IDTR] loaded', 0
msg_sti_ok:      db '[STI] Interrupts enabled!', 0
msg_keypress:    db '[KB] Press any key...', 0

times 4096 db 0
