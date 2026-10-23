; ============================================================
; SaylterOS Stage 1 Bootloader (MBR)
; ------------------------------------------------------------
; 512 bytes. BIOS 加载到物理地址 0x7C00。
; 双输出：VGA (BIOS int 0x10) + COM1 串口 (0x3F8)
;   -> QEMU 里 -serial file:xxx.log 可以直接看到启动日志
; ============================================================

ORG 0x7C00

; ============================================================
; 常量
; ============================================================
COM1        equ 0x3F8
STAGE2_ADDR equ 0x7E00
STAGE2_CYL  equ 0
STAGE2_HEAD equ 0
STAGE2_SEC  equ 2       ; 扇区 1 是 MBR，从扇区 2 开始读
STAGE2_CNT  equ 63      ; int 0x13 AH=2 单次上限 63 扇区!
                        ; stage2(2KB)+pm(2KB)+kernel(15KB) ≈ 40 扇区, 留余量

; ============================================================
; 入口
; ============================================================
jmp short start
nop

start:
    ; --- 先保存 BIOS 传入的启动驱动器号 (DL) ---
    ;    我们后面设 ax=0 会覆盖它
    mov byte [drive_num], dl

    ; --- 关中断，设置栈 ---
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti

    ; --- 清屏 + 光标归位 ---
    call clear_screen

    ; --- 初始化 COM1 串口 (9600 8N1) ---
    ; Line Control: 0x03 = 8 bits, no parity, 1 stop
    mov dx, COM1 + 3    ; LCR
    mov al, 0x03
    out dx, al
    ; Divisor Latch Access Bit = 1
    mov dx, COM1 + 3
    mov al, 0x83
    out dx, al
    ; Baud rate divisor = 12 (115200 / 9600)
    mov dx, COM1 + 0
    mov al, 12
    out dx, al
    mov dx, COM1 + 1
    mov al, 0
    out dx, al
    ; ★ MCR: DTR=1 + RTS=1 + OUT1=1 + OUT2=1 (让串口"就绪")
    mov dx, COM1 + 4    ; MCR
    mov al, 0x0F
    out dx, al

    ; --- 串口探针：确认最早的 boot 代码确实在输出 ---
    mov dx, COM1
    mov al, 'A'
    out dx, al

    ; --- 启动日志 ---
    mov si, str_boot
    call puts

    ; --- 读 Stage 2 ---
    mov si, str_load
    call puts

    ; --- 读 Stage 2 ---
    ; BIOS int 0x13 / AH=2 参数：
    ;   CH = 柱面低 8 位, CL = 扇区号(低6位) + 柱面高2位
    ;   DH = 磁头, DL = 驱动器号, AL = 扇区数
    ;   ES:BX = 目标缓冲区
    mov ah, 2           ; BIOS 读扇区功能号
    mov al, STAGE2_CNT  ; 扇区数量
    mov ch, STAGE2_CYL  ; ★ 关键：显式设 CH = 柱面号！
    mov cl, STAGE2_SEC  ; 扇区号
    mov dh, STAGE2_HEAD ; 磁头
    mov dl, byte [drive_num]  ; ★ 用 BIOS 传入的启动驱动器号
    mov bx, STAGE2_ADDR ; ES:BX = 0x0000:0x7E00
    call read_sectors
    jc .fail

    mov si, str_ok
    call puts

    jmp STAGE2_ADDR     ; 进入 Stage 2

.fail:
    mov si, str_fail
    call puts
    jmp $

; ============================================================
; clear_screen : BIOS int 0x10 / AH=0x06 滚屏清屏 + 光标归位
; ============================================================
clear_screen:
    push ax
    push bx
    push cx
    push dx
    mov ax, 0x0600      ; AH=6 滚屏, AL=0 整个窗口
    mov bh, 0x07        ; 属性：灰底白字
    mov cx, 0           ; 左上角 (0, 0)
    mov dh, 24          ; 右下角 (24, 79) = 25x80
    mov dl, 79
    int 0x10
    ; 光标归位
    mov ah, 0x02
    mov bh, 0
    mov dh, 0
    mov dl, 0
    int 0x10
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; ============================================================
; puts : VGA only (BIOS int 0x10)
; SI = 0 结尾字符串
; TODO: COM1 发送（需要正确的 MCR + 等待时间）
; ============================================================
puts:
    push ax
    push bx
    push dx
    mov ah, 0x0E        ; BIOS 电传打印
    mov bh, 0
    mov bl, 0x07        ; 灰底白字
    cld
.loop:
    lodsb               ; AL = [SI++]
    test al, al
    jz .end

    ; 串口日志：更前置、更详细，方便直接看启动流程
    push ax
    mov dx, COM1
    out dx, al
    pop ax

    int 0x10            ; VGA
    jmp .loop
.end:
    pop dx
    pop bx
    pop ax
    ret

; ============================================================
; read_sectors : BIOS int 0x13 / AH=2
; 重试 5 次
; ============================================================
read_sectors:
    pusha
    mov si, 5
.retry:
    int 0x13
    jnc .done
    dec si
    jnz .retry
    popa
    stc
    ret
.done:
    popa
    clc
    ret

; ============================================================
; 数据 (放在最后，紧凑排列)
; ============================================================
com_buf:        db 0        ; COM 发送临时缓冲
drive_num:      db 0        ; BIOS 传入的启动驱动器号

str_boot:       db '[MBR] SaylterOS Stage1 booting...', 13, 10, 0
str_load:       db '[MBR] Reading Stage2 (4 sectors @ cyl 0 head 0 sec 2)...', 13, 10, 0
str_ok:         db '[MBR] Stage2 loaded OK. Jumping to 0x7E00...', 13, 10, 0
str_fail:       db '[MBR] FATAL: Disk read failed. Halting.', 13, 10, 0

; ============================================================
; 填充 + MBR Magic
; ============================================================
times 510 - ($ - $$) db 0
dw 0xAA55
