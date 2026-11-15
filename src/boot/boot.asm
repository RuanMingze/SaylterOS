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
IMG_LBA_BASE equ 1      ; 镜像第一个被加载的扇区（Stage2 起点）
IMG_TAIL_SECTORS equ 128 ; 第三段总扇区数: LBA66..193，装载到 0x20000
                        ; 当前 .text+.rodata+.data 结束于 0x178A0，预留约 34KB 增长空间；
                        ; 上限受 64KB 段边界约束（0x20000 是段 0x1000 的结束地址，
                        ; 再往上会回卷到 0x10000），因此拆成两次 64 扇区读取。

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

    ; --- 分段加载镜像（LBA 扩展读 int 13h AH=42h） ---
    ; 为什么必须分段：
    ;   1) BIOS 单次读扇区有上限（CHS 方式 63，且不能跨磁道）
    ;   2) 目标缓冲区不能跨越 64KB 段边界
    ; 镜像布局: LBA1..4=stage2(2KB), LBA5..18=pm(7KB), LBA19+=kernel
    ; 需装入的部分: .text+.rodata+.data = 0x159C0-0xA200 = 47040B = 92 扇区
    ; 因此从 LBA1 起共需 110 扇区 → 0x7E00 .. 0x15A00
    ;   段1: LBA 1..63   → 0x0000:0x7E00 (0x7E00..0xFC00)
    ;   段2: LBA 64..65  → 0x0000:0xFC00 (0xFC00..0x10000) 恰好到 64KB 边界
    ;   段3: LBA 66..129 → 0x1000:0x0000 (0x10000..0x18000) 64 扇区
    ;   段4: LBA 130..193→ 0x1800:0x0000 (0x18000..0x20000) 64 扇区
    ; 拆成两个 64 扇区的原因是部分 BIOS 对单次 AH=42h 传输有 127 扇区的上限，
    ; 且每段 32KB 都不跨越 64KB 边界。
    mov eax, IMG_LBA_BASE
    mov cx, 63
    mov dx, 0x0000
    mov bx, STAGE2_ADDR
    call lba_read
    jc .fail

    mov eax, 64
    mov cx, 2
    mov dx, 0x0000
    mov bx, 0xFC00
    call lba_read
    jc .fail

    mov eax, 66
    mov cx, IMG_TAIL_SECTORS / 2
    mov dx, 0x1000
    mov bx, 0x0000
    call lba_read
    jc .fail

    mov eax, 66 + IMG_TAIL_SECTORS / 2
    mov cx, IMG_TAIL_SECTORS / 2
    mov dx, 0x1800
    mov bx, 0x0000
    call lba_read
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
; lba_read : BIOS int 0x13 / AH=0x42 扩展 LBA 读取（带 5 次重试）
; 入参: EAX = LBA(低 32 位), CX = 扇区数, DX = 目标段, BX = 目标偏移
; 出参: CF = 0 成功 / CF = 1 失败
; 说明: 使用 DAP 使得不受 CHS 几何限制，且调用方控制段:偏移
;       以保证单次传输不跨越 64KB 段边界
; ============================================================
lba_read:
    pusha
    mov [dap + 2], cx          ; 扇区数
    mov [dap + 4], bx          ; 偏移
    mov [dap + 6], dx          ; 段
    mov [dap + 8], eax         ; LBA 低 32 位
    mov dword [dap + 12], 0    ; LBA 高 32 位 = 0
    mov bp, 5                  ; 重试次数
.retry:
    mov ah, 0x42
    mov dl, byte [drive_num]
    mov si, dap
    int 0x13
    jnc .done
    dec bp
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

; ---- Disk Address Packet (AH=42h 用) ----
dap:
    db 0x10                 ; DAP 结构大小 = 16
    db 0x00                 ; 保留
    dw 0                    ; 扇区数（运行时填）
    dw 0                    ; 目标偏移（运行时填）
    dw 0                    ; 目标段（运行时填）
    dq 0                    ; 64 位 LBA（运行时填低 32 位）

str_boot:       db '[MBR] SaylterOS Stage1 booting...', 13, 10, 0
str_load:       db '[MBR] Reading Stage2 (4 sectors @ cyl 0 head 0 sec 1)...', 13, 10, 0
str_ok:         db '[MBR] Stage2 loaded OK. Jumping to 0x7E00...', 13, 10, 0
str_fail:       db '[MBR] FATAL: Disk read failed. Halting.', 13, 10, 0

; ============================================================
; 填充 + MBR Magic
; ============================================================
times 510 - ($ - $$) db 0
dw 0xAA55
