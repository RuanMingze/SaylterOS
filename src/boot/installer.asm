; ============================================================
; SaylterOS Installer - 主体（实模式，ORG 0x7E00）
; ------------------------------------------------------------
; 安装器不进入保护模式、不依赖内核：它用 BIOS int 13h 读软盘，
; 用 ATA PIO（主通道 0x1F0）逐扇区写 IDE 硬盘，把一个完整的
; 系统镜像从安装介质搬到目标盘：
;
;   软盘 LBA PAYLOAD_LBA .. PAYLOAD_LBA+PAYLOAD_SECTORS-1
;     -> 硬盘 LBA 0 .. PAYLOAD_SECTORS-1
;
; payload 的布局与硬盘完全一致：
;   +0     系统引导扇区（boot.bin）
;   +1..4  stage2.bin
;   +5..18 pm.bin
;   +19..  内核
;
; 安装完成后还会清零文件系统超级块区域（LBA8192..8207），
; 让首次启动时 SlyzFS 走"自动格式化并创建 root"的路径。
;
; 流程：界面 -> 检测硬盘 -> 确认 -> 写入 -> 逐扇区校验 -> 完成
;
; 约定：所有 I/O 函数把结果写进 io_result（0=成功，1=失败），
;       调用者用 cmp byte [io_result], 0 判断，避免 CF 被 pop 破坏。
; ============================================================

ORG 0x7E00
[BITS 16]

%ifndef PAYLOAD_LBA
%define PAYLOAD_LBA 17
%endif

%ifndef PAYLOAD_SECTORS
%define PAYLOAD_SECTORS 853
%endif

TRACK_SECTORS   equ 18
; 缓冲区全部放在 1MB 以下：不依赖 A20 状态。
;   0x5000..0x7400  软盘整磁道缓冲（9216 字节）
;   0x7500..0x7700  单扇区校验缓冲（512 字节）
;   0x4C00          栈顶
;   0x7E00..0x9E00  安装器主体自身
BUF_SEG         equ 0x0500
VERIFY_SEG      equ 0x0750
STACK_TOP       equ 0x4C00
FS_HEAD_LBA     equ 8192
FS_HEAD_SECTORS equ 16

ATTR_TITLE      equ 0x1F
ATTR_LABEL      equ 0x0F
ATTR_WARN       equ 0x0E
ATTR_ERR        equ 0x0C
ATTR_OK         equ 0x0A
ATTR_BAR        equ 0x2A

; ============================================================
; 入口
; ============================================================
main:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, STACK_TOP
    cld                         ; rep movs/stos/insw/outsw 依赖 DF=0
    sti

    mov [boot_drive], dl
    mov byte [io_result], 0

    call enable_a20
    call screen_init
    call draw_header
    call probe_floppy

    call detect_ata
    cmp byte [io_result], 0
    jne .no_disk

    call draw_summary
    call wait_confirm
    cmp byte [confirm], 0
    je .aborted

    call install_phase
    cmp byte [io_result], 0
    jne .write_fail

    call verify_phase
    cmp byte [io_result], 0
    jne .verify_fail

    call zero_fs_header
    cmp byte [io_result], 0
    jne .write_fail

    call flush_disk
    cmp byte [io_result], 0
    jne .write_fail

    call draw_done
    call wait_key
    jmp boot_from_disk

.no_disk:
    mov si, msg_no_disk
    mov dh, 18
    mov dl, 2
    mov bl, ATTR_ERR
    mov cx, msg_no_disk_end - msg_no_disk
    call print_at

    mov si, msg_ata_state
    mov dh, 19
    mov dl, 2
    mov bl, ATTR_LABEL
    mov cx, msg_ata_state_end - msg_ata_state
    call print_at

    mov al, [ata_status]
    mov dh, 19
    mov dl, 18
    mov bl, ATTR_WARN
    call print_hex8
    jmp .halt
.aborted:
    mov si, msg_aborted
    mov dh, 18
    mov dl, 2
    mov bl, ATTR_WARN
    mov cx, msg_aborted_end - msg_aborted
    call print_at
    jmp .halt
.write_fail:
    mov si, msg_write_fail
    mov dh, 18
    mov dl, 2
    mov bl, ATTR_ERR
    mov cx, msg_write_fail_end - msg_write_fail
    call print_at
    jmp .halt
.verify_fail:
    mov si, msg_verify_fail
    mov dh, 18
    mov dl, 2
    mov bl, ATTR_ERR
    mov cx, msg_verify_fail_end - msg_verify_fail
    call print_at
.halt:
    cli
    hlt
    jmp .halt

; ============================================================
; 屏幕
; ============================================================
screen_init:
    mov ax, 0x0003
    int 0x10
    ret

; enable_a20: Fast A20 Gate。缓冲区在 0x10000，A20 关闭时会被
; 回绕到 0x0000（覆盖中断向量表），必须显式打开。
enable_a20:
    in al, 0x92
    test al, 0x02
    jnz .done
    or al, 0x02
    and al, 0xFE
    out 0x92, al
.done:
    ret

; print_at: DH=行 DL=列 BL=属性 CX=长度 SI=字符串
; 直接写 VGA 文本显存（0xB8000），不经过 int 10h：
; BIOS 的 AH=0E/09 在部分实现里会破坏 SI/CX/BX，之前的版本因此
; 出现"蓝条能画、文字不显示"。显存写入没有这些副作用，速度也更快。
; 字符串位于 CS:SI（CS=0，ORG 0x7E00 后的符号值即物理地址）。
print_at:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push es
    push ds

    xor ax, ax
    mov ds, ax
    mov ax, 0xB800
    mov es, ax

    ; DI = ((row * 80) + col) * 2
    mov al, dh
    mov ah, 80
    mul ah                      ; AX = row * 80（8 位乘法，不影响 DX）
    xor dh, dh
    add ax, dx
    shl ax, 1
    mov di, ax

    mov ah, bl                  ; AH = 字符属性
.loop:
    mov al, [cs:si]
    test al, al
    jz .done
    cmp cx, 0
    je .done
    mov [es:di], ax
    add di, 2
    inc si
    dec cx
    jmp .loop
.done:
    pop ds
    pop es
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; print_hex8: AL=数值 DH=行 DL=列 BL=属性（输出两位大写十六进制）
print_hex8:
    push ax
    push bx
    push cx
    push dx
    push si
    push di

    mov [hex_row], dh
    mov [hex_col], dl
    mov [hex_attr], bl

    mov di, hex_buf
    mov ah, al
    shr al, 4
    call .nibble
    mov al, ah
    call .nibble
    mov byte [di], 0

    mov si, hex_buf
    mov dh, [hex_row]
    mov dl, [hex_col]
    mov bl, [hex_attr]
    mov cx, 2
    call print_at

    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret
.nibble:
    and al, 0x0F
    cmp al, 10
    jb .digit
    add al, 'A' - 10
    jmp .store
.digit:
    add al, '0'
.store:
    mov [di], al
    inc di
    ret

; print_num: AX=数值 DH=行 DL=列 BL=属性
print_num:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push bp

    mov [num_row], dh
    mov [num_col], dl
    mov [num_attr], bl

    mov di, dec_buf
    mov bx, 10
    xor cx, cx
.conv:
    xor dx, dx
    div bx
    add dl, '0'
    push dx
    inc cx
    test ax, ax
    jnz .conv

.store:
    pop dx
    mov [di], dl
    inc di
    dec cx
    jnz .store
    mov byte [di], 0

    mov cx, di
    sub cx, dec_buf

    mov si, dec_buf
    mov dh, [num_row]
    mov dl, [num_col]
    mov bl, [num_attr]
    call print_at

    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; ============================================================
; 界面
; ============================================================
draw_header:
    mov ax, 0x0600
    mov bh, ATTR_TITLE
    mov cx, 0x0000
    mov dx, 0x004F              ; 只填充第 0 行，避免整屏蓝底
    int 0x10

    mov si, msg_title
    mov dh, 0
    mov dl, 2
    mov bl, ATTR_TITLE
    mov cx, msg_title_end - msg_title
    call print_at

    mov si, msg_rule
    mov dh, 1
    mov dl, 0
    mov bl, ATTR_TITLE
    mov cx, 80
    call print_at
    ret

draw_summary:
    mov si, msg_source
    mov dh, 3
    mov dl, 2
    mov bl, ATTR_LABEL
    mov cx, msg_source_end - msg_source
    call print_at

    mov si, msg_target
    mov dh, 4
    mov dl, 2
    mov bl, ATTR_LABEL
    mov cx, msg_target_end - msg_target
    call print_at

    mov si, msg_payload
    mov dh, 5
    mov dl, 2
    mov bl, ATTR_LABEL
    mov cx, msg_payload_end - msg_payload
    call print_at

    mov ax, PAYLOAD_SECTORS
    mov dh, 5
    mov dl, 12
    mov bl, ATTR_OK
    call print_num

    mov si, msg_sectors
    mov dh, 5
    mov dl, 17
    mov bl, ATTR_LABEL
    mov cx, msg_sectors_end - msg_sectors
    call print_at

    mov ax, PAYLOAD_SECTORS / 2
    mov dh, 5
    mov dl, 29
    mov bl, ATTR_OK
    call print_num

    mov si, msg_kb
    mov dh, 5
    mov dl, 35
    mov bl, ATTR_LABEL
    mov cx, msg_kb_end - msg_kb
    call print_at

    mov si, msg_ide
    mov dh, 6
    mov dl, 2
    mov bl, ATTR_LABEL
    mov cx, msg_ide_end - msg_ide
    call print_at

    xor ax, ax
    mov al, [ata_status]
    mov dh, 6
    mov dl, 14
    mov bl, ATTR_OK
    call print_num

    mov si, msg_fs
    mov dh, 8
    mov dl, 2
    mov bl, ATTR_LABEL
    mov cx, msg_fs_end - msg_fs
    call print_at

    mov si, msg_warning
    mov dh, 10
    mov dl, 2
    mov bl, ATTR_WARN
    mov cx, msg_warning_end - msg_warning
    call print_at

    mov si, msg_warning2
    mov dh, 11
    mov dl, 2
    mov bl, ATTR_WARN
    mov cx, msg_warning2_end - msg_warning2
    call print_at

    mov si, msg_confirm
    mov dh, 16
    mov dl, 2
    mov bl, ATTR_WARN
    mov cx, msg_confirm_end - msg_confirm
    call print_at
    ret

; ============================================================
; 诊断：读软盘第 0 磁道，检查 payload 首字节（系统 boot.bin 为 EB）
; ============================================================
probe_floppy:
    ; 计算 payload 首扇区在本磁道内的字节偏移：payload 可能不落在磁道起始处。
    xor eax, eax
    xor edx, edx
    mov ax, PAYLOAD_LBA
    mov bx, TRACK_SECTORS
    div bx                      ; AX = 磁道基 LBA，DX = 磁道内扇区序号
    mov [probe_off], dx
    shl word [probe_off], 9

    mov bx, BUF_SEG
    mov es, bx
    xor bx, bx
    call floppy_read_track
    cmp byte [io_result], 0
    jne .bad

    mov si, [probe_off]
    mov al, [es:si]
    cmp al, 0xEB
    jne .bad

    mov si, msg_probe_ok
    mov dh, 7
    mov dl, 2
    mov bl, ATTR_OK
    mov cx, msg_probe_ok_end - msg_probe_ok
    call print_at
    ret
.bad:
    mov si, msg_probe_bad
    mov dh, 7
    mov dl, 2
    mov bl, ATTR_ERR
    mov cx, msg_probe_bad_end - msg_probe_bad
    call print_at
    ret

wait_confirm:
    mov byte [confirm], 0
    mov ah, 0x00
    int 0x16
    cmp al, 'Y'
    je .yes
    cmp al, 'y'
    je .yes
    ret
.yes:
    mov byte [confirm], 1
    ret

wait_key:
    mov ah, 0x00
    int 0x16
    ret

; ============================================================
; ATA
; ============================================================
detect_ata:
    mov byte [io_result], 0

    ; 显式选中 primary master（0x1F6 = 1010_0000：LBA 模式 + DEV0）。
    ; BIOS 探测光驱/从盘后可能把设备选择寄存器留在别的设备上，此时
    ; 直接读 0x1F7 会得到 0x00（无 DRDY），检测会误判成"没有硬盘"。
    mov dx, 0x1F6
    mov al, 0xA0
    out dx, al

    ; 400ns 延迟：连读 4 次状态端口（ATA 规范规定的设备选择延迟）
    mov dx, 0x1F7
    in al, dx
    in al, dx
    in al, dx
    in al, dx

    ; 轮询 DRDY，最多约 65536 次（设备上电自检完成需要一点时间）
    mov cx, 0
.wait_ready:
    mov dx, 0x1F7
    in al, dx
    mov [ata_status], al
    test al, 0x80               ; BSY
    jnz .next
    test al, 0x40               ; DRDY
    jnz .ok
    test al, 0x01               ; ERR
    jnz .bad
.next:
    loop .wait_ready
    jmp .bad
.ok:
    mov byte [io_result], 0
    ret
.bad:
    mov byte [io_result], 1
    ret

; ata_wait_bsy: 结果写 io_result（同时检查 ERR，避免写失败被当成成功）
ata_wait_bsy:
    push ax
    push cx
    push dx
    mov cx, 0
.loop:
    mov dx, 0x1F7
    in al, dx
    test al, 0x80
    jz .ready
    loop .loop
    mov byte [io_result], 1
    jmp .out
.ready:
    test al, 0x01               ; ERR
    jnz .err
    mov byte [io_result], 0
    jmp .out
.err:
    mov byte [io_result], 1
.out:
    pop dx
    pop cx
    pop ax
    ret

; ata_wait_drq: 结果写 io_result
ata_wait_drq:
    push ax
    push cx
    push dx
    mov cx, 0
.loop:
    mov dx, 0x1F7
    in al, dx
    test al, 0x80
    jnz .next
    test al, 0x08
    jnz .ok
    test al, 0x01
    jnz .err
.next:
    loop .loop
.err:
    mov byte [io_result], 1
    jmp .out
.ok:
    mov byte [io_result], 0
.out:
    pop dx
    pop cx
    pop ax
    ret

; ata_setup_lba: EAX=LBA，写 0x1F6/0x1F2/0x1F3..0x1F5
; 用 EBP 保存 LBA：mov al, bl 会破坏 EAX 的低字节，
; 若后续仍从 EAX 取 LBA 会把低位写错。
ata_setup_lba:
    push eax
    push ebx
    push dx
    push bp

    mov bp, ax                  ; LBA 低 16 位足够（安装目标 < 65536 扇区）
    mov ebx, eax
    shr ebx, 24
    and bl, 0x0F
    or bl, 0xE0
    mov dx, 0x1F6
    mov al, bl
    out dx, al

    mov dx, 0x1F2
    mov al, 1
    out dx, al

    mov dx, 0x1F3
    mov ax, bp
    out dx, al                  ; LBA[7:0]

    mov dx, 0x1F4
    mov ax, bp
    shr ax, 8
    out dx, al                  ; LBA[15:8]

    mov dx, 0x1F5
    xor al, al
    out dx, al                  ; LBA[23:16] = 0（目标盘 < 65536 扇区）

    pop bp
    pop dx
    pop ebx
    pop eax
    ret

; ata_write_sector: EAX=LBA, DS:SI=512 字节数据；结果写 io_result
ata_write_sector:
    push eax
    push bx
    push cx
    push dx
    push si

    call ata_wait_bsy
    cmp byte [io_result], 0
    jne .out

    call ata_setup_lba

    mov dx, 0x1F7
    mov al, 0x30
    out dx, al

    call ata_wait_drq
    cmp byte [io_result], 0
    jne .out

    mov cx, 256
    mov dx, 0x1F0
    cld
    rep outsw

    call ata_wait_bsy
    cmp byte [io_result], 0
    jne .out

    mov byte [io_result], 0
.out:
    pop si
    pop dx
    pop cx
    pop bx
    pop eax
    ret

; ata_read_sector: EAX=LBA, ES:DI=目标；结果写 io_result
ata_read_sector:
    push eax
    push bx
    push cx
    push dx
    push di

    call ata_wait_bsy
    cmp byte [io_result], 0
    jne .out

    call ata_setup_lba

    mov dx, 0x1F7
    mov al, 0x20
    out dx, al

    call ata_wait_drq
    cmp byte [io_result], 0
    jne .out

    mov cx, 256
    mov dx, 0x1F0
    cld
    rep insw

    call ata_wait_bsy
    cmp byte [io_result], 0
    jne .out

    mov byte [io_result], 0
.out:
    pop di
    pop dx
    pop cx
    pop bx
    pop eax
    ret

flush_disk:
    call ata_wait_bsy
    cmp byte [io_result], 0
    jne .out
    mov dx, 0x1F7
    mov al, 0xE7
    out dx, al
    call ata_wait_bsy
.out:
    ret

; ============================================================
; 软盘
; ============================================================
; floppy_read_track: EAX=起始 LBA（18 的倍数）, ES:BX=缓冲；结果写 io_result
; 逐扇区读取（AH=02, AL=1）：整磁道 18 扇区的单次 DMA 在部分 BIOS 上
; ------------------------------------------------------------
; floppy_read_track: 读取从 AX 开始的 TRACK_SECTORS 个连续扇区
; ------------------------------------------------------------
; 函数名保留（调用点不改），实现已从软盘 CHS 换成 LBA 扩展读：
; 安装介质现在是 16MB 硬盘镜像，18 扇区/磁道、2 磁头的软盘几何
; 不再成立，继续按 CHS 换算会读到完全错误的位置。
; 这里的"磁道"只是"连续 18 个扇区"的批处理单位，不再对应物理磁道。
; ------------------------------------------------------------
floppy_read_track:
    push eax
    push ebx
    push cx
    push dx

    movzx eax, ax               ; 只保留 16 位 LBA：高位残留会让寻址错位
    mov [cur_sector_lba], eax
    mov word [cur_off], 0
    mov word [cur_cnt], TRACK_SECTORS

.sector_loop:
    xor ax, ax
    mov ds, ax                  ; BIOS 可能改写 DS，每轮重置

    mov ax, BUF_SEG
    mov [dap_seg], ax
    mov ax, [cur_off]
    mov [dap_off], ax
    mov word [dap_count], 1

    mov eax, [cur_sector_lba]
    mov dword [dap_lba], eax
    mov dword [dap_lba + 4], 0

    mov si, dap
    mov dl, [boot_drive]
    mov ah, 0x42
    int 0x13
    jc .bad

    xor ax, ax
    mov ds, ax
    add word [cur_off], 512
    mov eax, [cur_sector_lba]
    inc eax
    mov [cur_sector_lba], eax
    dec word [cur_cnt]
    jnz .sector_loop

    mov byte [io_result], 0
    jmp .out
.bad:
    mov byte [io_result], 1
.out:
    pop dx
    pop cx
    pop ebx
    pop eax
    ret

; ============================================================
; 写入阶段
; ============================================================
install_phase:
    mov si, msg_writing
    mov dh, 15
    mov dl, 2
    mov bl, ATTR_LABEL
    mov cx, msg_writing_end - msg_writing
    call print_at

    mov word [cur_track_lba], 0
    mov word [written], 0
    call compute_track_count

.track_loop:
    mov bx, 0
    mov ax, BUF_SEG
    mov es, ax
    mov ax, [cur_track_lba]
    call floppy_read_track
    cmp byte [io_result], 0
    jne .read_fail

    call compute_overlap
    cmp cx, dx
    jae .next_track

.sector_loop:
    mov ax, cx
    sub ax, [cur_track_lba]
    shl ax, 9
    mov si, ax

    xor eax, eax
    mov ax, cx
    sub ax, PAYLOAD_LBA

    push cx
    push dx
    push ds
    mov bx, BUF_SEG
    mov ds, bx
    call ata_write_sector
    pop ds
    pop dx
    pop cx

    cmp byte [io_result], 0
    jne .write_fail

    inc word [written]
    inc cx
    cmp cx, dx
    jb .sector_loop

.next_track:
    call draw_progress
    call next_track
    cmp byte [done_all], 0
    je .track_loop

    mov byte [io_result], 0
    ret

.read_fail:
    mov si, msg_floppy_fail
    mov dh, 18
    mov dl, 2
    mov bl, ATTR_ERR
    mov cx, msg_floppy_fail_end - msg_floppy_fail
    call print_at
    mov byte [io_result], 1
    ret
.write_fail:
    mov byte [io_result], 1
    ret

; ============================================================
; 校验阶段
; ============================================================
verify_phase:
    mov si, msg_verifying
    mov dh, 15
    mov dl, 2
    mov bl, ATTR_LABEL
    mov cx, msg_verifying_end - msg_verifying
    call print_at

    mov word [cur_track_lba], 0
    mov word [written], 0
    call compute_track_count

.track_loop:
    mov bx, 0
    mov ax, BUF_SEG
    mov es, ax
    mov ax, [cur_track_lba]
    call floppy_read_track
    cmp byte [io_result], 0
    jne .read_fail

    call compute_overlap
    cmp cx, dx
    jae .next_track

.sector_loop:
    mov ax, cx
    sub ax, [cur_track_lba]
    shl ax, 9
    mov si, ax

    push cx
    push dx
    push si

    ; 目标缓冲必须先设好：ata_read_sector 的参数 EAX 是 LBA，
    ; 若把 mov ax, VERIFY_SEG 放在算完 LBA 之后，会把 LBA 直接覆盖成段值。
    mov ax, VERIFY_SEG
    mov es, ax
    xor di, di

    xor eax, eax
    mov ax, cx
    sub ax, PAYLOAD_LBA
    call ata_read_sector

    pop si
    pop dx
    pop cx

    cmp byte [io_result], 0
    jne .read_disk_fail

    push cx
    push dx
    push si
    push ds
    push es

    mov ax, BUF_SEG
    mov ds, ax
    mov ax, VERIFY_SEG
    mov es, ax
    xor di, di
    mov cx, 256
    cld
    repe cmpsw
    jne .mismatch

    pop es
    pop ds
    pop si
    pop dx
    pop cx

    inc word [written]
    inc cx
    cmp cx, dx
    jb .sector_loop
    jmp .next_track

.mismatch:
    pop es
    pop ds
    pop si
    pop dx
    pop cx
    mov si, msg_mismatch
    mov dh, 18
    mov dl, 2
    mov bl, ATTR_ERR
    mov cx, msg_mismatch_end - msg_mismatch
    call print_at
    mov byte [io_result], 1
    ret

.next_track:
    call draw_progress
    call next_track
    cmp byte [done_all], 0
    je .track_loop

    mov byte [io_result], 0
    ret

.read_fail:
    mov si, msg_floppy_fail
    mov dh, 18
    mov dl, 2
    mov bl, ATTR_ERR
    mov cx, msg_floppy_fail_end - msg_floppy_fail
    call print_at
    mov byte [io_result], 1
    ret
.read_disk_fail:
    mov si, msg_disk_read_fail
    mov dh, 18
    mov dl, 2
    mov bl, ATTR_ERR
    mov cx, msg_disk_read_fail_end - msg_disk_read_fail
    call print_at
    mov byte [io_result], 1
    ret

; ============================================================
; 遍历辅助
; ============================================================
; compute_track_count: track_count = ceil((PAYLOAD_LBA + PAYLOAD_SECTORS) / 18)
compute_track_count:
    mov ax, PAYLOAD_LBA + PAYLOAD_SECTORS
    xor dx, dx
    mov bx, TRACK_SECTORS
    div bx
    test dx, dx
    jz .rounded
    inc ax
.rounded:
    mov [track_count], ax
    ret

; compute_overlap: 基于 cur_track_lba 算出交集 [CX, DX)
compute_overlap:
    mov ax, [cur_track_lba]
    mov bx, ax
    add bx, TRACK_SECTORS

    mov cx, ax
    cmp cx, PAYLOAD_LBA
    jae .start_ok
    mov cx, PAYLOAD_LBA
.start_ok:

    mov dx, bx
    cmp dx, PAYLOAD_LBA + PAYLOAD_SECTORS
    jbe .end_ok
    mov dx, PAYLOAD_LBA + PAYLOAD_SECTORS
.end_ok:
    ret

; next_track: 推进磁道；全部完成后置 done_all=1
next_track:
    mov ax, [cur_track_lba]
    add ax, TRACK_SECTORS
    mov [cur_track_lba], ax

    mov ax, [track_count]
    mov bx, TRACK_SECTORS
    mul bx
    cmp [cur_track_lba], ax
    jb .more
    mov byte [done_all], 1
    ret
.more:
    mov byte [done_all], 0
    ret

; ============================================================
; 清零 FS 超级块
; ============================================================
zero_fs_header:
    push ds
    push es
    push si
    push di
    push cx

    mov ax, VERIFY_SEG
    mov es, ax
    xor di, di
    mov cx, 256
    xor ax, ax
    cld
    rep stosw

    mov word [written2], 0
.loop:
    mov ax, FS_HEAD_LBA
    add ax, [written2]

    push ds
    mov bx, VERIFY_SEG
    mov ds, bx
    xor si, si
    xor eax, eax
    mov ax, FS_HEAD_LBA
    add ax, [written2]
    call ata_write_sector
    pop ds

    cmp byte [io_result], 0
    jne .fail

    inc word [written2]
    cmp word [written2], FS_HEAD_SECTORS
    jb .loop

    mov byte [io_result], 0
    jmp .out
.fail:
    mov byte [io_result], 1
.out:
    pop cx
    pop di
    pop si
    pop es
    pop ds
    ret

; ============================================================
; 进度
; ============================================================
draw_progress:
    push ax
    push bx
    push cx
    push dx
    push si
    push di

    mov ax, [written]
    mov bx, 100
    mul bx
    mov bx, PAYLOAD_SECTORS
    div bx
    mov [percent], ax

    mov ax, [percent]
    mov dh, 15
    mov dl, 13
    mov bl, ATTR_OK
    call print_num

    mov ax, [percent]
    mov bx, 20
    mul bx
    xor dx, dx
    mov bx, 100
    div bx
    mov [bar_fill], ax

    mov di, bar_buf
    mov cx, 20
    xor bx, bx
.copy:
    cmp bx, [bar_fill]
    jb .filled
    mov al, ' '
    jmp .store
.filled:
    mov al, '#'
.store:
    mov [di], al
    inc di
    inc bx
    loop .copy
    mov byte [di], 0

    mov si, bar_buf
    mov dh, 17
    mov dl, 2
    mov bl, ATTR_BAR
    mov cx, 20
    call print_at

    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; ============================================================
; 完成 / 重启
; ============================================================
draw_done:
    mov si, msg_done
    mov dh, 19
    mov dl, 2
    mov bl, ATTR_OK
    mov cx, msg_done_end - msg_done
    call print_at

    mov si, msg_done2
    mov dh, 20
    mov dl, 2
    mov bl, ATTR_OK
    mov cx, msg_done2_end - msg_done2
    call print_at

    mov si, msg_reboot
    mov dh, 22
    mov dl, 2
    mov bl, ATTR_LABEL
    mov cx, msg_reboot_end - msg_reboot
    call print_at

    mov ax, [written]
    mov dh, 23
    mov dl, 2
    mov bl, ATTR_LABEL
    call print_num
    ret

; ============================================================
; 从目标盘引导：把硬盘 LBA0 读回 0x7C00 并跳转
; ------------------------------------------------------------
; 这样安装完成后不需要重启、也不需要手动移除软盘，直接进入
; 刚装好的系统（系统引导扇区会继续从硬盘 LBA1 起加载）。
; ============================================================
boot_from_disk:
    push es
    xor ax, ax
    mov es, ax
    mov di, 0x7C00
    xor eax, eax
    call ata_read_sector
    pop es

    cmp byte [io_result], 0
    jne .fail

    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    mov dl, 0x80                ; 启动设备 = 第一块硬盘
    db 0xEA
    dw 0x7C00
    dw 0x0000

.fail:
    mov si, msg_boot_fail
    mov dh, 17
    mov dl, 2
    mov bl, ATTR_ERR
    mov cx, msg_boot_fail_end - msg_boot_fail
    call print_at
.halt:
    cli
    hlt
    jmp .halt

; ============================================================
; 数据
; ============================================================
boot_drive:     db 0
ata_status:     db 0
confirm:        db 0
done_all:       db 0
io_result:      db 0
cur_track_lba:  dw 0
track_count:    dw 0
written:        dw 0
written2:       dw 0
percent:        dw 0
bar_fill:       dw 0
num_row:        db 0
num_col:        db 0
num_attr:       db 0
hex_row:        db 0
hex_col:        db 0
hex_attr:       db 0
hex_buf:        times 4 db 0
cur_attr:       db 0
cur_cyl:        db 0
cur_head:       db 0
cur_sec:        db 0
cur_off:        dw 0
cur_cnt:        dw 0
cur_sector_lba: dd 0

; Disk Address Packet：LBA 扩展读用，与 CHS 几何无关
dap:
                db 0x10, 0
dap_count:      dw 1
dap_off:        dw 0
dap_seg:        dw 0
dap_lba:        dd 0
                dd 0
probe_off:      dw 0
dec_buf:        times 8 db 0
bar_buf:        times 24 db 0

msg_title:      db ' SaylterOS Installer v1.0 '
msg_title_end:
msg_rule:       times 80 db 0x2D
msg_rule_end:
msg_source:     db 'Source : floppy (1.44MB)'
msg_source_end:
msg_target:     db 'Target : IDE primary master (0x1F0)'
msg_target_end:
msg_payload:    db 'Payload:'
msg_payload_end:
msg_sectors:    db 'sectors'
msg_sectors_end:
msg_kb:         db 'KB'
msg_kb_end:
msg_fs:         db 'Filesystem: SlyzFS will be formatted on first boot.'
msg_fs_end:
msg_ide:        db 'IDE status:'
msg_ide_end:
msg_probe_ok:   db 'Floppy payload: readable'
msg_probe_ok_end:
msg_probe_bad:  db 'Floppy payload: UNREADABLE'
msg_probe_bad_end:
msg_warning:    db 'WARNING: target disk LBA0..payload will be overwritten.'
msg_warning_end:
msg_warning2:   db '         filesystem header LBA8192..8207 will be cleared.'
msg_warning2_end:
msg_confirm:    db 'Press Y to install, any other key aborts.'
msg_confirm_end:
msg_writing:    db 'Writing   '
msg_writing_end:
msg_verifying:  db 'Verify    '
msg_verifying_end:
msg_done:       db 'Installation complete.'
msg_done_end:
msg_done2:      db 'SlyzFS will be formatted on first boot.'
msg_done2_end:
msg_reboot:     db 'Remove the floppy, then press any key to boot from disk.'
msg_reboot_end:
msg_boot_fail:  db 'ERROR: cannot boot from installed disk.'
msg_boot_fail_end:
msg_no_disk:    db 'ERROR: no IDE disk on primary channel.'
msg_no_disk_end:
msg_ata_state:  db 'ATA status = 0x'
msg_ata_state_end:
msg_aborted:    db 'Installation aborted by user.'
msg_aborted_end:
msg_write_fail: db 'ERROR: disk write failed.'
msg_write_fail_end:
msg_verify_fail: db 'ERROR: verification failed.'
msg_verify_fail_end:
msg_floppy_fail: db 'ERROR: floppy read failed.'
msg_floppy_fail_end:
msg_disk_read_fail: db 'ERROR: disk read failed during verify.'
msg_disk_read_fail_end:
msg_mismatch:   db 'ERROR: sector mismatch during verify.'
msg_mismatch_end:

times 8192 - ($ - $$) db 0
