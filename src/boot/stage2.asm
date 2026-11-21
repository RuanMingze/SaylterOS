; ============================================================
; SaylterOS Stage 2 Bootloader
; ------------------------------------------------------------
; 由 Stage 1 加载到 0x7E00，处于实模式。
; 职责：
;   1. 打印 SaylterOS Banner
;   2. 检查扩展内存 (BIOS 0xE800)
;   3. 枚举 VGA 模式 / 打印驱动信息
;   4. 打印 CPU 信息 (CPUID)
;   5. 最后打印 "Ready to enter kernel..." 并停机
; 后续：Phase 1 会在这里加入 GDT + 保护模式切换
; ============================================================

ORG 0x7E00
[BITS 16]

; --- 必须先跳转到 start，下面都是数据 ---
jmp start

; ============================================================
; 入口
; ============================================================
start:
    ; --- 重设段 ---
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax

    ; 启动盘号已由 MBR 在 0x500 处保存（int 0x13 会改写 DL，
    ; 到这里再保存拿到的可能是被污染的值），这里不再重复保存。
    ; 栈顶必须避开镜像加载区：pm.bin 从 0x8600 开始，
    ; 若沿用 0x9000 作为栈顶，压栈会直接改写 pm 代码/数据，
    ; pm 体积增大后必然踩中关键指令（表现为跳转后黑屏）。
    ; 0x7000 以下到 0x5000 是空闲常规内存，不会被任何镜像段占用。
    mov sp, 0x7000      ; 大点的栈

    ; --- 重新初始化 COM1 (MBR 设置的可能被 BIOS 中断干扰) ---
    mov dx, 0x3F8 + 3   ; LCR
    mov al, 0x03
    out dx, al
    mov dx, 0x3F8 + 3   ; LCR DLAB=1
    mov al, 0x83
    out dx, al
    mov dx, 0x3F8 + 0   ; DDL
    mov al, 12
    out dx, al
    mov dx, 0x3F8 + 1   ; DDH
    mov al, 0
    out dx, al
    ; ★ MCR: DTR+RTS+OUT1+OUT2
    mov dx, 0x3F8 + 4   ; MCR
    mov al, 0x0F
    out dx, al

    ; --- Banner (blue ASCII art) ---
    mov si, banner
    call print_banner

    ; --- Memory Info ---
    call print_mem_info

    ; --- Drive Info ---
    call print_drive_info

    ; --- VGA Info ---
    call print_vga_info

    ; --- CPU Info ---
    call print_cpu_info

    ; --- 准备好 ---
    mov si, ready_msg
    call print_string

    ; --- 跳转到 Stage 3: 保护模式切换 ---
    ; pm.asm 已由 boot.asm 读到 0x8600
    jmp 0x8600

    ; --- fallback: 如果跳不过去，停在这里 ---
    jmp $

; ============================================================
; 打印字符串 (BIOS int 0x10 / AH=0x0E)
; ============================================================
COM1 equ 0x3F8

print_banner:
    push ax
    push bx
    mov ah, 0x0E        ; BIOS 电传打印 (VGA only)
    mov bh, 0x00
    mov bl, 0x09        ; 蓝色 ASCII 艺术字
    cld
.loop:
    lodsb
    test al, al
    jz .done
    int 0x10            ; VGA
    jmp .loop
.done:
    pop bx
    pop ax
    ret

print_string:
    push ax
    push bx
    mov ah, 0x0E        ; BIOS 电传打印 (VGA only)
    mov bh, 0x00
    mov bl, 0x07        ; 默认白色
    cld
.loop:
    lodsb
    test al, al
    jz .done
    int 0x10            ; VGA
    jmp .loop
.done:
    pop bx
    pop ax
    ret

; ============================================================
; 打印 AX 中的 16 位十进制数
; 入参：AX = 数值，DI = 缓冲区（至少 6 字节）
; ============================================================
print_dec_ax:
    mov cx, 0
    mov bx, 10
.div_loop:
    xor dx, dx
    div bx
    add dl, '0'
    push dx
    inc cx
    test ax, ax
    jnz .div_loop
.pop_loop:
    pop ax              ; dl -> al
    mov ah, 0x0E
    mov bh, 0
    mov bl, 0x07
    int 0x10
    dec cx
    jnz .pop_loop
    ret

; ============================================================
; 打印内存信息
; ============================================================
print_mem_info:
    mov si, mem_header
    call print_string

    ; --- Conventional memory (int 0x12) ---
    mov si, mem_msg1
    call print_string
    mov ah, 0x12
    int 0x15            ; AX = 640K 以下的常规内存（KB）
    call print_dec_ax
    mov si, mem_msg2
    call print_string

    ; --- Extended memory via E820 (int 0x15 / AX=0xE820) ---
    ; 简化版本：先用 AX=0xE801 取扩展内存
    mov si, mem_msg3
    call print_string
    mov ax, 0xE801
    xor bx, bx
    xor cx, cx
    xor dx, dx
    int 0x15
    ; 如果 CF=0，AX 可能是 1MB-16MB 之间的扩展内存（KB），
    ; DX 是 16MB 以上部分的 64KB 块数
    jc .ext_fail
    ; 简单处理：如果 DX != 0，总扩展 = AX + DX * 64
    test dx, dx
    jnz .ext_large
    ; 小内存情况
    mov ax, ax
    call print_dec_ax
    jmp .ext_done
.ext_large:
    ; 大内存需要换算
    mov ax, dx
    mov bx, 64
    mul bx              ; DX:AX = DX * 64 (KB)
    ; 简化：直接显示低 16 位
    call print_dec_ax
    jmp .ext_done
.ext_fail:
    mov ax, 0
    call print_dec_ax
.ext_done:
    mov si, mem_msg4
    call print_string
    ret

; ============================================================
; 打印驱动器信息
; ============================================================
print_drive_info:
    mov si, drv_header
    call print_string

    ; 驱动器数量 (int 0x13 / AH=0x08)
    mov si, drv_count
    call print_string
    mov ah, 0x08
    mov dl, 0x80        ; 硬盘 0
    int 0x13
    jc .drv_err
    mov al, dl          ; DL = 驱动器数量（从 0x80 开始数的总数）
    sub al, 0x80
    inc ax              ; 转为数量
    call print_dec_ax
    mov si, newline
    call print_string

    ; 几何信息
    mov si, drv_geom
    call print_string
    mov ah, 0x08
    mov dl, 0x80
    int 0x13
    jc .geom_err
    ; DH = 最大磁头号 (从 0 开始)，CL 低 6 位 = 最大扇区号
    ; CH = 最大柱面低 8 位，CL 高 2 位 = 最大柱面高 2 位
    mov dh, dh
    movzx ax, dh
    inc ax              ; 实际磁头数
    call print_dec_ax
    mov al, 'H'
    mov ah, 0x0E
    mov bh, 0
    mov bl, 0x07
    int 0x10

    mov si, drv_dots
    call print_string
    ; 简化：不做完整 CHS 计算，打印柱面数（粗略）
    mov cl, cl
    and cl, 0x3F        ; 扇区号
    movzx ax, cl
    call print_dec_ax

    mov al, 'S'
    mov ah, 0x0E
    mov bh, 0
    mov bl, 0x07
    int 0x10

    mov si, newline
    call print_string
    ret
.drv_err:
.geom_err:
    mov ax, 0
    call print_dec_ax
    mov si, newline
    call print_string
    ret

; ============================================================
; 打印 VGA 信息
; ============================================================
print_vga_info:
    mov si, vga_header
    call print_string

    ; 当前视频模式 (int 0x10 / AH=0x0F)
    mov si, vga_mode
    call print_string
    mov ah, 0x0F
    int 0x10
    ; AH = 列数，AL = 模式号，BH = 显示页
    mov al, al
    mov ah, 0x0E
    mov bh, 0
    mov bl, 0x07
    ; 打印模式号（16进制简化版）
    call .print_hex_al
    mov si, newline
    call print_string

    mov si, vga_text
    call print_string
    mov ah, 0x0F
    int 0x10
    movzx ax, ah
    call print_dec_ax
    mov si, newline
    call print_string

    ; 行高 (int 0x10 / AH=0x11)
    mov si, vga_t_rows
    call print_string
    mov ah, 0x11
    mov al, 0x30
    int 0x10
    ; AX 行高 / DL = 实际行数
    movzx ax, dl
    call print_dec_ax
    mov si, newline
    call print_string
    ret

.print_hex_al:
    push ax
    shr al, 4
    call .print_hex_nibble
    pop ax
    and al, 0x0F
    call .print_hex_nibble
    ret
.print_hex_nibble:
    cmp al, 10
    jb .digit
    add al, 'A' - 10
    jmp .put
.digit:
    add al, '0'
.put:
    mov ah, 0x0E
    mov bh, 0
    mov bl, 0x07
    int 0x10
    ret

; ============================================================
; 打印 CPU 信息
; ============================================================
print_cpu_info:
    mov si, cpu_header
    call print_string

    ; Vendor ID (CPUID / EAX=0)
    mov eax, 0
    cpuid
    ; EBX, EDX, ECX 是 12 字符的 vendor ID
    mov di, vendor_id
    mov [di + 0], ebx
    mov [di + 4], edx
    mov [di + 8], ecx

    mov si, cpu_vendor
    call print_string
    mov si, vendor_id
    mov cx, 12
    call print_n_chars

    ; Family, Model, Type (CPUID / EAX=1)
    mov eax, 1
    cpuid
    ; EAX 位域：Type(12-11), Family(8-4), Model(3-0) 等等
    mov si, cpu_family
    call print_string
    mov ebx, eax
    shr ebx, 8
    and bx, 0xF
    mov ax, bx
    call print_dec_ax
    mov si, newline
    call print_string

    mov si, cpu_model
    call print_string
    mov ebx, eax
    and bx, 0xF
    mov ax, bx
    call print_dec_ax
    mov si, newline
    call print_string

    mov si, cpu_type
    call print_string
    mov ebx, eax
    shr ebx, 12
    and bx, 0x3
    mov ax, bx
    call print_dec_ax
    mov si, newline
    call print_string

    ret

print_n_chars:
    mov ah, 0x0E
    mov bh, 0
    mov bl, 0x07
.loop:
    lodsb
    int 0x10
    dec cx
    jnz .loop
    mov si, newline
    call print_string
    ret

; ============================================================
; 数据 (放在最后，避免被 CPU 当成代码执行)
; ============================================================

banner:
    db 0x0D, 0x0A
    db '  _____              _ _          _____ _____ ______ ', 0x0D, 0x0A
    db ' / ____|            | | |        / ____| ____|  ____|', 0x0D, 0x0A
    db '| (___   __ _  ___  | | |_ ___  | (___ | |__ | |__   ', 0x0D, 0x0A
    db ' \___ \ / _` |/ _ \ | | __/ _ \  \___ \|  __||  __|  ', 0x0D, 0x0A
    db ' ____) | (_| |  __/ | | || (_) | ____) | |___| |     ', 0x0D, 0x0A
    db '|_____/ \__,_|\___| |_|\__\___/ |_____/|_____|_|     ', 0x0D, 0x0A
    db 0x0D, 0x0A
    db '  [ SAYLTER OPERATING SYSTEM ] v0.1 - Stage2', 0x0D, 0x0A
    db 0

mem_header: db 0x0D, 0x0A, '[Memory Info]', 0x0D, 0x0A, 0
mem_msg1:   db '  Conventional : ', 0
mem_msg2:   db ' KB  (below 640K)', 0x0D, 0x0A, 0
mem_msg3:   db '  Extended     : ', 0
mem_msg4:   db ' KB  (above 1MB)', 0x0D, 0x0A, 0

cpu_header: db 0x0D, 0x0A, '[CPU Info]', 0x0D, 0x0A, 0
cpu_vendor: db '  Vendor ID    : ', 0
cpu_family: db '  Family       : ', 0
cpu_model:  db '  Model        : ', 0
cpu_type:   db '  Type         : ', 0
cpu_brand:  db '  Brand        : ', 0

drv_header: db 0x0D, 0x0A, '[Drive Info]', 0x0D, 0x0A, 0
drv_count:  db '  Number of drives : ', 0
drv_geom:   db '  Drive 0 geometry : CHS=', 0
drv_dots:   db ',  Total sectors=', 0

vga_header: db 0x0D, 0x0A, '[VGA Info]', 0x0D, 0x0A, 0
vga_mode:   db '  Current mode : ', 0
vga_text:   db '  Text cols    : ', 0
vga_t_rows: db '  Text rows    : ', 0
vga_font:   db '  Font chars   : ', 0

info_sep:   db 0x0D, 0x0A, 0

ready_msg:  db 0x0D, 0x0A, 0x0D, 0x0A, '========================================', 0x0D, 0x0A
            db ' SaylterOS ready.', 0x0D, 0x0A
            db ' Kernel entry at 0x10000 (not yet loaded)', 0x0D, 0x0A
            db ' Next step: GDT + Protected Mode', 0x0D, 0x0A
            db '========================================', 0x0D, 0x0A, 0

newline:    db 0x0D, 0x0A, 0

; 临时缓冲区
vendor_id:  db 'XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX'  ; 12 chars + padding
cpuid_brand: db 'XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX' ; 48 chars + padding

; ============================================================
; 填充到 2KB
; ============================================================
times 2048 - ($ - $$) db 0
