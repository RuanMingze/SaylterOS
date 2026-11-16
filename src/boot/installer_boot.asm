; ============================================================
; SaylterOS Installer - 引导扇区（安装介质 MBR）
; ------------------------------------------------------------
; 由 BIOS 加载到 0x7C00。职责只有一件事：
;   从介质 LBA1 起把安装器主体读进 0x7E00，然后跳过去。
;
; 介质已经从 1.44MB 软盘换成 16MB 硬盘镜像：软盘的 CHS 几何
; 假设（18 扇区/磁道、2 磁头）在硬盘上不成立，因此这里统一
; 使用 INT 13h AH=42h 的 LBA 扩展读，不再做任何 CHS 换算。
; BIOS 传入的 DL 可能是 0x80（硬盘），全程原样保存使用。
; ============================================================

ORG 0x7C00
[BITS 16]

MAIN_ADDR       equ 0x7E00
FIRST_MAIN_LBA  equ 1

%ifndef INSTALLER_MAIN_SECTORS
%define INSTALLER_MAIN_SECTORS 8
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

    mov word [cur_lba], FIRST_MAIN_LBA
    mov byte [cur_cnt], INSTALLER_MAIN_SECTORS
    mov word [cur_off], MAIN_ADDR

.sector_loop:
    ; 逐扇区读：DAP 只描述一个扇区，避免部分 BIOS 对跨段多扇区
    ; DMA 的处理差异。数量很少，性能无所谓。
    mov word [dap_count], 1
    mov ax, [cur_off]
    mov [dap_off], ax
    mov word [dap_seg], 0

    mov ax, [cur_lba]
    movzx eax, ax
    mov [dap_lba], eax
    mov dword [dap_lba + 4], 0

    mov si, dap
    mov dl, [boot_drive]
    mov ah, 0x42
    int 0x13
    jc .fail

    xor ax, ax
    mov ds, ax
    add word [cur_off], 512
    inc word [cur_lba]
    dec byte [cur_cnt]
    jnz .sector_loop

    ; 跳主体（远跳保证 CS:IP 正确）
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

; Disk Address Packet（16 字节）
dap:
    db 0x10, 0
dap_count:  dw 1
dap_off:    dw 0
dap_seg:    dw 0
dap_lba:    dd 0
            dd 0

boot_drive: db 0
cur_lba:    dw 0
cur_cnt:    db 0
cur_off:    dw 0
msg_loading:    db 'SaylterOS Installer: loading', 13, 10, 0
msg_read_fail:  db 'Installer load failed', 13, 10, 0

times 510 - ($ - $$) db 0
dw 0xAA55
