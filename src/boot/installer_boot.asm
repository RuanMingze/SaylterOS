; ============================================================
; SaylterOS Installer - 引导扇区（软盘 MBR）
; ------------------------------------------------------------
; 由 BIOS 加载到 0x7C00。职责只有一件事：
;   从软盘 LBA1 起把安装器主体读进 0x7E00，然后跳过去。
;
; 为什么限制在一段连续读取：
;   软盘 int 13h 的 CHS 读取不能跨磁道（每磁道 18 扇区），
;   LBA1..17 落在同一个磁道（C0/H0/S2..S18），可以一次读完。
;   安装器主体必须不超过 17 扇区（8704 字节），build.ps1 会自检。
; ============================================================

ORG 0x7C00
[BITS 16]

MAIN_ADDR       equ 0x7E00
FIRST_MAIN_LBA  equ 1
MAX_MAIN_SECTORS equ 17         ; LBA1..17 同磁道
TRACK_SECTORS   equ 18

%ifndef INSTALLER_MAIN_SECTORS
%define INSTALLER_MAIN_SECTORS 8
%endif

%if INSTALLER_MAIN_SECTORS > MAX_MAIN_SECTORS
%error "installer main exceeds one floppy track (17 sectors)"
%endif

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    cld
    sti

    mov [boot_drive], dl

    ; 文本模式 80x25，清屏
    mov ax, 0x0003
    int 0x10

    mov si, msg_loading
    call puts

    ; 逐扇区读取安装器主体到 0000:7E00。
    ; 单次多扇区 DMA 在部分 FDC/BIOS 组合上会静默失败或只搬一部分，
    ; 逐扇区（AL=1）最稳，LBA1..17 全部落在第 0 磁道，速度也不是问题。
    ; ES 必须为 0：把 0x7E00 装进 ES 会变成物理地址 0x7E000，
    ; 与远跳目标 0x0000:7E00 相差 0x76000，主体根本不会执行。
    mov word [cur_lba], FIRST_MAIN_LBA
    mov byte [cur_cnt], INSTALLER_MAIN_SECTORS
    mov word [cur_off], MAIN_ADDR

.sector_loop:
    mov ax, [cur_lba]
    xor dx, dx
    mov bx, TRACK_SECTORS * 2
    div bx                      ; AX = 柱面，DX = 磁道内偏移
    mov ch, al
    mov ax, dx
    xor dx, dx
    mov bl, TRACK_SECTORS
    div bl                      ; AL = 磁头，AH = 扇区序号（0 基）
    mov dh, al
    mov cl, ah
    inc cl
    mov dl, [boot_drive]

    xor ax, ax
    mov es, ax
    mov bx, [cur_off]
    mov ah, 0x02
    mov al, 1
    int 0x13
    jc .fail

    xor ax, ax
    mov ds, ax
    add word [cur_off], 512
    inc word [cur_lba]
    dec byte [cur_cnt]
    jnz .sector_loop

    ; 跳主体（远跳保证 CS:IP 正确）
    ; x86 远跳的编码顺序是 EA offset16 segment16，写反会跳到
    ; 7E00:0000（物理 0x7E000），而主体实际加载在 0000:7E00。
    xor ax, ax
    mov ds, ax
    mov dl, [boot_drive]
    db 0xEA
    dw MAIN_ADDR
    dw 0x0000

.fail:
    xor ax, ax
    mov ds, ax
    mov si, msg_read_fail
    call puts
.halt:
    cli
    hlt
    jmp .halt

; ------------------------------------------------------------
; puts: 电传打印，SI = 0 结尾字符串
; ------------------------------------------------------------
puts:
    push ax
    push bx
    push si
    mov ah, 0x0E
    mov bx, 0x0007
.loop:
    lodsb
    test al, al
    jz .done
    int 0x10
    jmp .loop
.done:
    pop si
    pop bx
    pop ax
    ret

boot_drive:     db 0
cur_lba:        dw 0
cur_cnt:        db 0
cur_off:        dw 0
msg_loading:    db 'SaylterOS Installer: loading', 13, 10, 0
msg_read_fail:  db 'Installer load failed', 13, 10, 0

times 510 - ($ - $$) db 0
dw 0xAA55
