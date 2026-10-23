; ============================================================
; vbe_switch.asm - 正确的 RM <-> PM 切换
; ------------------------------------------------------------
; 流程 (不带 IRET, 只用 far jump):
;   32-bit PM → LGDT(临时GDT) → 32-bit far jump 切 CS 到 16-bit PM
;   → 手动切 SS → 清 PE → 16-bit far jump 到真 RM
;   → BIOS VBE → CLI → 设 PE → 16-bit far jump 回 32-bit PM
;   → LGDT(原GDT) → 恢复栈 → 返回
; ============================================================
; 注意: NASM elf32 下 [BITS 16] 不生效! 必须用 .code16gcc

global vbe_bios_switch_mode

; --- 保存区 ---
saved_esp:  dd 0
saved_ebp:  dd 0
saved_gdtr:
    dw 0
    dd 0
saved_idtr:
    dw 0
    dd 0
saved_cr0:  dd 0
saved_result:   dd 0

; --- 临时 GDT (null + 32bit_code + 32bit_data + 16bit_code + 16bit_data) ---
; 所有段 base=0, G=1, limit=4GB
; 16-bit 段用 G=1 D=0 → CPU 按 16-bit 解释但能访 4GB
gdt_tmp:
    dq 0x0000000000000000        ; 0x00 Null  (8B)
    ; 0x08 32-bit Code  (G=1 D=1, 4GB) - offset 0x08
    dw 0xFFFF                    ; limit low 16
    dw 0x0000                    ; base low 16
    db 0x00                      ; base mid 8
    db 0x9A                      ; access: present, ring0, exec, readable
    db 0xCF                      ; flags: G=1 D=1 + limit hi 4
    db 0x00                      ; base hi 8
    ; 0x10 32-bit Data  (G=1 D=1, 4GB) - offset 0x10
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 0x92                      ; access: present, ring0, rw
    db 0xCF
    db 0x00
    ; 0x18 16-bit Code  (G=1 D=0, 4GB) - offset 0x18
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 0x9A
    db 0xC7                      ; G=1 D=0 → 真 16-bit!
    db 0x00
    ; 0x20 16-bit Data  (G=1 D=0, 4GB) - offset 0x20
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 0x92
    db 0xC7
    db 0x00
gdt_tmp_end:
; 5 entries × 8B = 40B, limit = 39 = 0x27
gdt_tmp_desc:
    dw gdt_tmp_end - gdt_tmp - 1
    dd gdt_tmp

; --- 实模式数据 ---
real_ivt:
    dw 0x03FF
    dd 0x0000
vbe_ctl_buf:
    times 512 db 0                ; VBE 3.0 controller info 最多 512B
vbe_mode_buf:
    times 256 db 0                ; Mode info block 是 256B
; --- 从 32-bit PM 到 16-bit RM 的 far jump 模板: dw off_low, dw seg ---
rm_back_jmp:
    dw 0           ; offset_low (运行时写)
    dw 0           ; segment (运行时写)
; --- 从 RM 回 32-bit PM 的 far jump 模板: dw offset_low, dw selector ---
pm_back_jmp:
    dw 0           ; offset_low (运行时写)
    dw 0x08        ; selector = 临时 GDT 的 32-bit code

; ============================================================
; C callable
; ============================================================
vbe_bios_switch_mode:
    cli
    push ebp
    mov ebp, esp

    ; --- 保存状态 ---
    mov [saved_esp], esp
    mov [saved_ebp], ebp
    sgdt [saved_gdtr]
    sidt [saved_idtr]
    mov eax, cr0
    mov [saved_cr0], eax
    mov dword [saved_result], 0

    ; --- 预计算 rm_back_jmp: real_entry 的 seg:off ---
    ; GDT base=0, 虚拟=物理, real_entry 在 0xAxxx 范围
    lea eax, [real_entry]
    mov edx, eax
    shr edx, 4                    ; seg = phys >> 4
    and eax, 0xF                  ; off = phys & 0xF
    mov [rm_back_jmp], ax         ; offset
    mov [rm_back_jmp + 2], dx     ; segment

    ; --- 预计算 pm_back_jmp: pm_resume 的 offset_low ---
    lea eax, [pm_resume]
    mov [pm_back_jmp], ax

    ; --- 加载临时 GDT ---
    lgdt [gdt_tmp_desc]

    ; ============================================================
    ; Step 1: 32-bit far jump → 切 CS 到 16-bit PM
    ; ============================================================
    ; 32-bit PM 里执行, offset 是 32-bit 虚拟地址
    ; 但目标 CS (0x18) 是 D=0, CPU 会把 EIP 截断为 16-bit IP
    ; 因为链接在 0xA100, 地址高 16 位都是 0, 所以截断安全
    jmp 0x18:bits16_pm_entry

; ============================================================
; Step 2: 16-bit PM (CR0.PE=1)
;   CS=0x18 (16-bit code), 手动切 SS=0x20 (16-bit data)
;   然后清 PE 位 → 16-bit far jump 清 CS 影子 → 真 RM
; ============================================================
.code16gcc
bits16_pm_entry:
    ; CS 已由 far jump 设为 0x18 (16-bit code, G=1, base=0)
    ; 手动切 SS/DS/ES
    mov ax, 0x20
    mov ss, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; 关 PE → 真 RM
    mov eax, cr0
    and eax, 0xFFFFFFFE
    mov cr0, eax

    ; 16-bit far jump 清 CS 影子属性
    ; rm_back_jmp 已预填: [offset_low, segment]
    jmp far [rm_back_jmp]

; ============================================================
; Step 3: Real Mode - BIOS VBE
; ============================================================
real_entry:
    ; 真·实模式! 段寄存器"值" = seg, base = seg << 4
    ; 内核链接到 0xA100, 所以 seg = 0xA100 >> 4 = 0xA10, base = 0xA100
    ; 这样 [label] = seg:offset = (0xA10 << 4) + offset = 0xA100 + offset ✓
    mov ax, 0xA10                  ; 内核代码/数据段 (base=0xA100)
    mov ds, ax                     ; ds=0xA10 → 能访问 0xA100+ 区域
    mov es, ax
    mov fs, ax
    mov gs, ax
    ; 栈放低内存安全区 (VGA/IVT 都不用, 0x7000 安全)
    xor ax, ax
    mov ss, ax                     ; ss=0 → base=0, sp=0x7000
    mov sp, 0x7000

    lidt [real_ivt]

    ; --- 还原 PIC 8259 到 BIOS 默认向量 ---
    mov al, 0x11
    out 0x20, al
    mov al, 0x08                  ; IRQ0-7 → 0x08-0x0F
    out 0x21, al
    mov al, 0x04
    out 0x21, al
    mov al, 0x01
    out 0x21, al
    mov al, 0xFF                  ; 先全屏蔽
    out 0x21, al
    mov al, 0x11
    out 0xA0, al
    mov al, 0x70                  ; IRQ8-15 → 0x70-0x77
    out 0xA1, al
    mov al, 0x02
    out 0xA1, al
    mov al, 0x01
    out 0xA1, al
    mov al, 0xFF
    out 0xA1, al

    ; --- VBE 0x4F00: 获取控制器信息 ---
    ; 注意: BIOS int 可能修改 ds/es, 调用前保存
    sti
    push ds                         ; 保存 0xA10
    mov ax, 0x4F00
    mov di, vbe_ctl_buf
    int 0x10
    pop ds                          ; 恢复 0xA10
    cmp ax, 0x004F
    jne .fail

    ; --- 遍历 mode list, 找 1024x768x32 + linear FB ---
    ; VBE Controller Info: VideoModePtr 在 offset 0xDE (4 bytes: offset+seg)
    les si, [vbe_ctl_buf + 0xDE]   ; es:si = mode list
.mode_loop:
    mov cx, [es:si]                ; cx = 当前 mode number
    cmp cx, 0xFFFF
    je .fail

    push ds                         ; 保存 (BIOS 可能改 ds)
    mov ax, 0x4F01
    mov di, vbe_mode_buf
    int 0x10
    pop ds                          ; 恢复
    cmp ax, 0x004F
    jne .next_mode

    ; VBE Mode Info offsets (OSDev wiki):
    ;   0x00 ModeAttributes (bit7=LFB)
    ;   0x10 XResolution
    ;   0x12 YResolution
    ;   0x17 BitsPerPixel
    cmp word [vbe_mode_buf + 0x10], 1024   ; XResolution
    jne .next_mode
    cmp word [vbe_mode_buf + 0x12], 768    ; YResolution
    jne .next_mode
    cmp byte [vbe_mode_buf + 0x17], 32     ; BitsPerPixel
    jne .next_mode
    mov al, [vbe_mode_buf + 0x00]          ; ModeAttributes
    test al, 0x80                          ; bit7 = LinearFB
    jz .next_mode

    ; --- 找到! VBE 0x4F02 设置 ---
    push ds                         ; 保存
    mov ax, 0x4F02
    mov bx, cx
    or bx, 0x4000                 ; bit14 = request linear FB
    int 0x10
    pop ds                          ; 恢复
    cmp ax, 0x004F
    jne .fail

    mov dword [saved_result], 1
    jmp back_to_pm

.next_mode:
    add si, 2
    jmp .mode_loop

.fail:
    mov dword [saved_result], 0

; ============================================================
; Step 4: RM → 32-bit PM
; ============================================================
back_to_pm:
    cli                           ; 关中断, 防止 PM 切换期间 IRQ
    sti                           ; 快速恢复一下 (如果 BIOS 调 int 会开中断, CLI 会覆盖)
    nop
    cli                           ; 最终关中断

    ; 在 RM 里重新 LGDT 临时 GDT (确保 GDTR 有正确的段描述符)
    lgdt [gdt_tmp_desc]

    mov eax, cr0
    or eax, 1
    mov cr0, eax                  ; PE = 1

    ; 16-bit far jump 到 32-bit PM
    ; pm_back_jmp 已预填: [offset_low=pm_resume_phys & 0xFFFF, selector=0x08]
    jmp far [pm_back_jmp]

; ============================================================
; Step 5: 32-bit PM 恢复
; ============================================================
; 默认 elf32 就是 32-bit, .code16gcc 到此自动恢复
pm_resume:
    ; 清除上面 jmp far 留下的栈残留 (不用手动 pop, 我们会恢复 saved_esp)
    mov ax, 0x10                  ; 32-bit data selector
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    ; 恢复原来的 GDT/IDT
    lgdt [saved_gdtr]
    lidt [saved_idtr]

    ; 恢复栈
    mov esp, [saved_esp]
    mov ebp, [saved_ebp]

    ; 返回结果
    mov eax, [saved_result]
    pop ebp
    ret
