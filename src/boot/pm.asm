; ============================================================
; SaylterOS pm.asm - Phase 2 完整版
; GDT ✅ (已验证) → PIC → IDT → ISR → STI → IRQ0+IRQ1
; ============================================================
ORG 0x8600
[BITS 16]

pm_start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x8000

    ; ========================================
    ; VBE BIOS 初始化 (Real Mode, 切 PM 之前)
    ; SAYLTER_VBE 宏开启 (-DSAYLTER_VBE=1)
    ; ========================================
%ifdef SAYLTER_VBE
    call vbe_init
%endif

    lgdt [gdt_descriptor]

    mov eax, cr0
    or eax, 1
    mov cr0, eax

    ; 显式编码 far jump (BITS 16 下必须是 5 字节: EA + 2+2)
    ; dw 是 16-bit 偏移，CPU 在实模式下读取后切到保护模式
    db 0xEA
    dw pm_entry
    dw 0x0008

[BITS 32]
; NASM -f bin + ORG 会自动把 label 值设为 ORG + 文件内偏移（即物理地址）
; 所以所有 label 已经是正确的物理地址，直接使用即可，不能再 ！

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

    lidt [idtr ]

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

    ; --- 跳到 C kernel entry (0xA200) ---
    ; 这里必须用 far jump，而不是 nasm 的 near jump imm32。
    ; 否则 jmp 0xA200 会被编码成相对跳转，不会跳到 0xA200。
    ; LBA 19 → 0x7E00 + 18*512 = 0xA200
    db 0xEA
    dd 0xA200
    dw 0x0008

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
    lea edi, [idt_entries ]
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
    lea edi, [idt_entries  + 0x20 * 8]
    lea esi, [isr_timer ]
    call set_idt_entry

    ; 覆盖 IRQ1 (IDT 0x21): Keyboard
    lea edi, [idt_entries  + 0x21 * 8]
    lea esi, [isr_keyboard ]
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
; VBE BIOS 初始化 (Real Mode)
; 找 1024x768x32, 设 LFB, 结果存 0x7DF0
; 缓冲区用 pm.asm 末尾的 padding (安全!)
; 破坏: ax, bx, cx, dx, di, si, es
; ============================================================
; ============================================================
; VBE 缓冲区 — 直接用物理绝对地址, 避开代码/GDT/IDT
; pm.bin = 6867B, padding 从 ~0x90D3 开始到 0xA1BF
; VBE_CTL_BUF (512B) @ 0x9500
; VBE_MODE_BUF (256B) @ 0x9700
; ============================================================
VBE_CTL_BUF  equ 0x9500   ; 0x9500 - 0x96FF
VBE_MODE_BUF equ 0x9700   ; 0x9700 - 0x97FF

[BITS 16]   ; vbe_init 在实模式下被调用，必须按 16-bit 汇编！
vbe_init:
    ; v0.9: 完整 VBE 初始化 (0x4F00 + 遍历 + 0x4F01 + 0x4F02 + 存 0x7DF0)
    mov ax, 0x4F00
    mov di, VBE_CTL_BUF
    int 0x10
    cmp ax, 0x004F
    jne .vbe_fail

    les si, [VBE_CTL_BUF + 0x0E]   ; VideoModePtr 在 0x0E (0x16 是 VendorNamePtr!)

    xor bx, bx
.mode_loop:
    mov dx, [es:si + bx]
    cmp dx, 0xFFFF
    je .vbe_fail

    mov ax, 0x4F01
    mov cx, dx
    mov di, VBE_MODE_BUF
    push bx                        ; BIOS 可能踩 BX，保住循环偏移
    int 0x10
    pop bx
    les si, [VBE_CTL_BUF + 0x0E]   ; BIOS 可能踩 ES，每次重载
    cmp ax, 0x004F
    jne .next_mode

    cmp word [VBE_MODE_BUF + 0x12], 1024   ; XResolution @ 0x12
    jne .next_mode
    cmp word [VBE_MODE_BUF + 0x14], 768    ; YResolution @ 0x14
    jne .next_mode
    cmp byte [VBE_MODE_BUF + 0x19], 32     ; BitsPerPixel @ 0x19
    jne .next_mode

    ; ============================================================
    ; 找到了! 保存模式号 (DX), 设 LFB bit, VBE 0x4F02 设模式
    ; ============================================================
    mov bx, dx                    ; BX = 模式号
    or bx, 0x4000                 ; 置 LFB bit
    mov ax, 0x4F02
    int 0x10
    cmp ax, 0x004F
    jne .vbe_fail

    ; 重新查模式信息 (设完后 framebuffer 地址才有效)
    mov ax, 0x4F01
    mov cx, dx                    ; CX = 原模式号 (不带 0x4000)
    mov di, VBE_MODE_BUF
    int 0x10

    ; ============================================================
    ; 存结果到 0x7DF0 (C 层直接读)
    ; ============================================================
    mov word [0x7DF0], 0xA1B2          ; magic = OK
    mov eax, [VBE_MODE_BUF + 0x28]     ; FrameBufferBase (phys) @ 0x28
    mov [0x7DF2], eax
    mov ax, [VBE_MODE_BUF + 0x12]      ; XResolution @ 0x12
    mov [0x7DF6], ax
    mov ax, [VBE_MODE_BUF + 0x14]      ; YResolution @ 0x14
    mov [0x7DF8], ax
    mov al, [VBE_MODE_BUF + 0x19]      ; BitsPerPixel @ 0x19
    mov [0x7DFA], al
    mov ax, [VBE_MODE_BUF + 0x10]      ; BytesPerScanLine @ 0x10
    mov [0x7DFB], ax                   ; 注意: 这里覆盖了 0x7DFA 但只多写了 1B, 结构对齐允许
    ret

.next_mode:
    add bx, 2
    jmp .mode_loop

.vbe_fail:
    mov word [0x7DF0], 0x0000          ; magic = fail
    ret

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
