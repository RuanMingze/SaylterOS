; ============================================================
; SaylterOS pm.asm - 实模式 -> 32 位 -> 64 位长模式
; ------------------------------------------------------------
; 职责：
;   1. 实模式：VBE BIOS 模式设置（int 0x10），结果写到 0x7DF0
;   2. 16 -> 32 位保护模式（GDT 0x08 代码 / 0x10 数据）
;   3. 建 4GB 恒等映射页表（PML4 + PDPT + 4 x PD，2MB 大页）
;   4. 32 -> 64 位长模式（CR4.PAE / CR3 / EFER.LME / CR0.PG）
;   5. 建 64 位 IDT + PIC remap + 开中断
;   6. 跳到内核入口 0x10000
;
; 物理内存布局（与 boot.asm / linker.ld 保持一致）：
;   0x06000 - 0x06FFF : 64 位 IDT（256 x 16B）
;   0x07000 - 0x075FF : 页表（PML4 0x70000, PDPT 0x71000, 4 x PD 0x72000..）
;   0x07DF0           : VBE 结果结构（C 层读取）
;   0x08600 - ...     : 本文件（pm.bin，由 stage2 从 0x7E00 跳入）
;   0x09C00 / 0x09E00 : VBE 查询缓冲区（仅实模式使用）
;   0x100000 - ...    : 内核映像（本文件在 32 位模式下用 ATA PIO 搬运）
;   0x3FF000          : 内核初始栈顶（向下增长到内核镜像之上）
;
; 注意：本文件必须以 -f bin 汇编，且代码+数据长度必须小于
;       0x9C00 - 0x8600 = 0x1600 字节（build.ps1 会自检），
;       否则会覆盖 VBE 实模式缓冲区。
;
; 为什么内核改由 ATA PIO 搬运：内核带 AML 解释器后会超过 640KB，
; BIOS int 13h 的实模式段:偏移寻址无法可靠写入 1MB 以上，
; 而 32 位保护模式下 ATA PIO 可以直接写任意物理地址。
; ============================================================

ORG 0x8600

KERNEL_ENTRY   equ 0x100000
KERNEL_LBA     equ 19          ; 内核起始扇区（与 build.ps1 的 SECTOR_KERNEL 一致）
STACK_TOP      equ 0x3FF000    ; 内核初始栈顶（< 内核堆基址 0x400000）

; 内核占用扇区数，由 build.ps1 按 kernel.bin 实际大小计算后 -D 传入
%ifndef KERNEL_SECTORS
%define KERNEL_SECTORS 900
%endif

; 裸日志：VBE 初始化阶段直接写启动盘（不依赖文件系统）。
; 每个槽固定占 1 个扇区，写入即覆盖旧内容，用作"阶段进度表"。
; 槽 0..127 位于内核载荷之后、模块(6144)与 SlyzFS(8192)之前。
; 只要定义了 SAYLTER_DEBUG（无论 C 侧还是构建脚本传入），
; 就同时启用 pm 阶段的落盘与蜂鸣，避免两个宏不一致导致静默失效。
%ifdef SAYLTER_DEBUG
%ifndef SAYLTER_DEBUG_BOOT
%define SAYLTER_DEBUG_BOOT
%endif
%endif
RAW_LOG_LBA    equ 7168
RAW_LOG_SLOTS  equ 128

; 蜂鸣器音型：不同阶段用不同次数/音高，黑屏时也能听出卡在哪一步。
BEEP_DIV_LOW   equ 0x1000      ; ~291 Hz
BEEP_DIV_MID   equ 1193        ; ~1 kHz
BEEP_DIV_HIGH  equ 596         ; ~2 kHz
BEEP_LEN       equ 0x280
BEEP_LEN_LONG  equ 0x700

IDT_BASE       equ 0x60000
idt_entries    equ IDT_BASE     ; 256 x 16B，运行时由 build_idt64 填充
PML4_BASE      equ 0x70000
PDPT_BASE      equ 0x71000
PD_BASE        equ 0x72000
VBE_RESULT     equ 0x7DF0
VBE_CTL_BUF    equ 0x9C00   ; 0x9C00 - 0x9DFF (512B)
VBE_MODE_BUF   equ 0x9E00   ; 0x9E00 - 0x9EFF (256B)

; SCI 回调地址（内核在 ACPICA 初始化完成后写入，0 = 未安装）。
; 放在 VBE_RESULT 之前的 8 字节空隙，避免与引导参数结构冲突。
; ISR 直接间接调用它：SCI 是电平触发，必须在中断上下文里清掉 GPE，
; 否则 EOI 之后电平仍有效会立刻重入，形成中断风暴。
SCI_CALLBACK_ADDR equ 0x7DE8

; #PF 处理函数地址（内核在 vm_init 中写入，0 = 未安装）。
; 与 SCI 一样，pm.asm 是 -f bin 独立镜像，链接期无法解析 C 符号，
; 因此通过低地址函数指针槽间接调用。
PF_HANDLER_ADDR equ 0x7DE0

; 最近一次缺页现场（诊断用）：CR2 / error code / RIP。
; 即使嵌套缺页覆盖了寄存器保存区，这两个槽也能给出最近一次的原始值。
PF_LAST_CR2     equ 0x7DD0
PF_LAST_ERROR   equ 0x7DD8
PF_LAST_RIP     equ 0x7DC8



; ============================================================
; 实模式入口
; ============================================================
[BITS 16]

pm_start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7000

    ; SCI 回调尚未注册
    mov dword [SCI_CALLBACK_ADDR], 0
    mov dword [SCI_CALLBACK_ADDR + 4], 0

    ; #PF 处理函数尚未注册
    mov dword [PF_HANDLER_ADDR], 0
    mov dword [PF_HANDLER_ADDR + 4], 0

    ; 缺页现场清零
    mov dword [PF_LAST_CR2], 0
    mov dword [PF_LAST_CR2 + 4], 0
    mov dword [PF_LAST_ERROR], 0
    mov dword [PF_LAST_ERROR + 4], 0
    mov dword [PF_LAST_RIP], 0
    mov dword [PF_LAST_RIP + 4], 0

    ; 引导器提供的 ACPI RSDP 地址（UEFI 路径专用，BIOS 下保持 0）
    mov dword [0x7DC0], 0
    mov dword [0x7DC4], 0
    ; 引导器类型标志：0 = BIOS/legacy，1 = UEFI（UEFI 下不启用 AP）
    mov dword [0x7DBC], 0

    ; 键盘环形缓冲头尾清零（ISR 在进入长模式后才会写）
    mov word [0xA000], 0
    mov word [0xA002], 0

    ; A20 必须打开：内核镜像搬到 1MB 以上，ATA PIO 直接写高地址
    call enable_a20_16

    ; --- 阶段 0：已进入 pm.bin（实模式） ---
    ; 先落盘再鸣叫：即使随后 VBE 切模式黑屏，盘上也能看到进度。
    ; 只有调试镜像（-DSAYLTER_DEBUG_BOOT=1）才写盘/鸣叫。
%ifdef SAYLTER_DEBUG_BOOT
    mov dx, 0
    call raw_slot_start
    mov si, msg_raw_enter
    call raw_append
    mov dx, 0
    call raw_commit
    mov si, 1
    mov ax, BEEP_DIV_MID
    mov cx, BEEP_LEN
    call beep_times
%endif

    ; --- VBE BIOS 模式设置：终端统一走图形模式 ---
    ; 失败时 magic = 0，C 层自动回退到 80x25 文本终端。
    ; BIOS 的 VBE 调用可能改写 UART 的 LCR/DLAB，导致后续串口日志丢失，
    ; 因此调用前后各重初始化一次 COM1。
    call serial_init16

    ; 阶段 1 预标记：4F00 若卡死/黑屏，盘上至少能看到已进入 VBE 初始化
%ifdef SAYLTER_DEBUG_BOOT
    mov dx, 1
    call raw_slot_start
    mov si, msg_raw_vbe_start
    call raw_append
    mov dx, 1
    call raw_commit
    mov si, 1
    mov ax, BEEP_DIV_MID
    mov cx, BEEP_LEN
    call beep_times
%endif

    call vbe_init
    call serial_init16

    cmp word [VBE_RESULT], 0xA1B2
    jne .vbe_failed
    mov si, msg_vbe_ok
    call serial_puts16
    jmp .vbe_done
.vbe_failed:
    mov si, msg_vbe_fail
    call serial_puts16
.vbe_done:

    ; --- 用 BIOS INT 13h 把内核从 LBA19 搬到 0x100000 ---
    ; 这样 U 盘 / SATA / NVMe（只要 BIOS/CSM 把它暴露成 INT 13h 盘）
    ; 都能启动；不再依赖 0x1F0 的 ATA PIO。
    call load_kernel_bios
    jc .kernel_fail
    mov si, msg_kernel_ok
    call serial_puts16
    jmp .kernel_done
.kernel_fail:
    mov si, msg_kernel_fail
    call serial_puts16
.halt_kernel:
    cli
    hlt
    jmp .halt_kernel
.kernel_done:

    ; 阶段 5：即将切保护模式（之后 BIOS 不可用，只能靠蜂鸣器）
%ifdef SAYLTER_DEBUG_BOOT
    mov dx, 5
    call raw_slot_start
    mov si, msg_raw_pmode
    call raw_append
    mov dx, 5
    call raw_commit
    mov si, 2
    mov ax, BEEP_DIV_HIGH
    mov cx, BEEP_LEN
    call beep_times
%endif

    ; --- 进入 32 位保护模式 ---
    lgdt [gdt_descriptor]

    mov eax, cr0
    or eax, 1
    mov cr0, eax

    ; 远跳：EA + 16 位偏移 + 16 位选择子（实模式下编码）
    db 0xEA
    dw pm_entry
    dw 0x0008

; ============================================================
; 打开 A20（Fast A20 Gate，端口 0x92 bit1）
; 保留 bit0（原为快速复位位，写 1 会复位机器）
; ============================================================
enable_a20_16:
    in al, 0x92
    test al, 0x02
    jnz .done
    or al, 0x02
    and al, 0xFE
    out 0x92, al
.done:
    ret

; ============================================================
; 16 位串口初始化：COM1 9600 8N1，DTR/RTS/OUT1/OUT2 置位
; 破坏：ax, dx
; ============================================================
serial_init16:
    push ax
    push dx
    mov dx, 0x3F8 + 3    ; LCR: DLAB=1
    mov al, 0x83
    out dx, al
    mov dx, 0x3F8 + 0    ; divisor low = 12 (115200 / 9600)
    mov al, 12
    out dx, al
    mov dx, 0x3F8 + 1    ; divisor high = 0
    xor al, al
    out dx, al
    mov dx, 0x3F8 + 3    ; LCR: DLAB=0, 8N1
    mov al, 0x03
    out dx, al
    mov dx, 0x3F8 + 4    ; MCR: DTR|RTS|OUT1|OUT2
    mov al, 0x0F
    out dx, al
    pop dx
    pop ax
    ret

; ============================================================
; 16 位串口输出（带等待超时，避免 LSR 异常时死循环）
; 入参：AL = 字符
; ============================================================
serial_putc16:
    push ax
    push dx
    push cx
    mov ah, al
    mov cx, 0
.wait:
    mov dx, 0x3FD
    in al, dx
    test al, 0x20
    jnz .ready
    loop .wait
.ready:
    mov dx, 0x3F8
    mov al, ah
    out dx, al
    pop cx
    pop dx
    pop ax
    ret

; 入参：SI = 0 结尾字符串
serial_puts16:
    push ax
    push si
.loop:
    lodsb
    test al, al
    jz .done
    call serial_putc16
    jmp .loop
.done:
    pop si
    pop ax
    ret

; ============================================================
; BIOS INT 13h 把内核从 LBA19 搬到 0x100000
; ------------------------------------------------------------
; 每次读 64 扇区（32KB）到 0x1000:0000，然后进入 unreal mode
; （设 PE -> 装载 4GB 平坦数据段 -> 清 PE，保留段描述符缓存），
; 用 32 位 rep movsd 搬到目标物理地址。
; 盘号取自 stage2 在 0x7DB0 保存的 BIOS 启动盘号，因此 U 盘、
; SATA、以及与 CSM 映射为 INT 13h 盘的介质都能启动。
; 出参：CF=0 成功 / CF=1 读盘失败
; ============================================================
load_kernel_bios:
    ; VBE 调用可能留下非零 ES/DS，先强制归零
    xor ax, ax
    mov ds, ax
    mov es, ax

    ; 阶段 4：开始用 BIOS 读内核
%ifdef SAYLTER_DEBUG_BOOT
    mov dx, 4
    call raw_slot_start
    mov si, msg_raw_load
    call raw_append
    mov ax, KERNEL_SECTORS
    call raw_hex16
    mov dx, 4
    call raw_commit
%endif

    pusha

    mov si, msg_drv
    call serial_puts16
    mov al, [0x0500]
    call serial_hex8_16
    mov si, msg_crlf
    call serial_puts16

    mov eax, KERNEL_LBA
    mov [bios_lba], eax
    mov dword [bios_dst], KERNEL_ENTRY
    mov word [bios_left], KERNEL_SECTORS

    mov si, msg_load
    call serial_puts16

.next_chunk:
    mov ax, [bios_left]
    test ax, ax
    jz .done

    cmp ax, 64
    jbe .count_ready
    mov ax, 64
.count_ready:
    mov [bios_secs], ax
    sub word [bios_left], ax

    mov word [bios_dap + 2], ax
    mov word [bios_dap + 4], 0
    mov word [bios_dap + 6], 0x1000
    mov eax, [bios_lba]
    mov dword [bios_dap + 8], eax
    mov dword [bios_dap + 12], 0

    mov bp, 5
.retry:
    mov ah, 0x42
    mov dl, [0x0500]
    mov si, bios_dap
    int 0x13
    jnc .read_ok

    mov [bios_err], ah
    xor ah, ah
    mov dl, [0x0500]
    int 0x13
    dec bp
    jnz .retry
    jmp .fail

.read_ok:
    call unreal_copy_high

    movzx eax, word [bios_secs]
    add dword [bios_lba], eax

    movzx eax, word [bios_secs]
    shl eax, 9
    add dword [bios_dst], eax

    jmp .next_chunk

.done:
    ; 阶段 4：内核读取成功
%ifdef SAYLTER_DEBUG_BOOT
    mov dx, 4
    call raw_slot_start
    mov si, msg_raw_load_ok
    call raw_append
    mov dx, 4
    call raw_commit
    mov si, 2
    mov ax, BEEP_DIV_MID
    mov cx, BEEP_LEN
    call beep_times
%endif
    popa
    clc
    ret
.fail:
    mov si, msg_bios_err
    call serial_puts16
    mov al, [bios_err]
    call serial_hex8_16
    mov si, msg_crlf
    call serial_puts16

    ; 阶段 4：内核读取失败（err = BIOS AH）
%ifdef SAYLTER_DEBUG_BOOT
    mov dx, 4
    call raw_slot_start
    mov si, msg_raw_load_fail
    call raw_append
    mov al, [bios_err]
    movzx ax, al
    call raw_hex16
    mov dx, 4
    call raw_commit
    mov si, 1
    mov ax, BEEP_DIV_LOW
    mov cx, BEEP_LEN_LONG
    call beep_times
%endif

    popa
    stc
    ret

; 16 位十六进制输出（AL）
serial_hex8_16:
    push ax
    push bx
    mov bl, al
    shr al, 4
    call .nibble
    mov al, bl
    and al, 0x0F
    call .nibble
    pop bx
    pop ax
    ret
.nibble:
    cmp al, 10
    jb .digit
    add al, 'A' - 10
    jmp .out
.digit:
    add al, '0'
.out:
    call serial_putc16
    ret

; 建立 unreal mode，从 0x10000 拷贝 [bios_secs]*512 字节到 [bios_dst]
unreal_copy_high:
    movzx ecx, word [bios_secs]
    shl ecx, 9
    shr ecx, 2

    lgdt [gdt_descriptor]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp short $+2
    mov ax, 0x10
    mov ds, ax
    mov es, ax

    mov eax, cr0
    and eax, 0xFFFFFFFE
    mov cr0, eax
    jmp 0:unreal_here
unreal_here:
    mov esi, 0x10000
    mov edi, [bios_dst]
    a32 rep movsd

    xor ax, ax
    mov ds, ax
    mov es, ax
    ret

; ============================================================
; 32 位保护模式入口
; ============================================================
[BITS 32]

pm_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov esp, 0x09FFF0
    cld

    mov esi, msg_pm32
    call serial_puts32

    ; 内核已在 16 位阶段由 BIOS INT 13h 加载到 0x100000
    mov esi, msg_loaded
    call serial_puts32

    call build_page_tables

    mov esi, msg_paging
    call serial_puts32

    ; ---------- 打开 PAE + SSE/SSE2 支持 + 装入 PML4 ----------
    ; CR4.OSFXSR / OSXMMEXCPT 未置位时，任何 SSE 指令（movaps、pxor、
    ; movdqa…）都会触发 #UD。x86-64 System V ABI 要求用 XMM 传递和
    ; 保存浮点参数，libc（musl -O2）一定会生成 SSE2 指令，所以必须在
    ; 进入长模式前打开。内核自身仍以 -mno-sse 编译，不受影响。
    ; 同时清 CR0.EM/TS 并置 MP，保证 x87 与 SSE 状态切换合法。
    mov eax, cr0
    and eax, 0xFFFFFFF3        ; 清 EM(bit2)、TS(bit3)
    or  eax, 0x2               ; MP(bit1) = 1
    mov cr0, eax

    mov eax, cr4
    or eax, 0x600              ; OSFXSR | OSXMMEXCPT
    or eax, 0x20               ; PAE
    mov cr4, eax

    mov eax, PML4_BASE
    mov cr3, eax

    ; ---------- EFER.LME = 1 ----------
    mov ecx, 0xC0000080
    rdmsr
    or eax, 0x100
    wrmsr

    ; ---------- CR0.PG = 1，正式进入长模式 ----------
    mov eax, cr0
    or eax, 0x80000000
    mov cr0, eax

    ; 远跳进 64 位代码段（0x18，L=1）：EA + 32 位偏移 + 16 位选择子
    db 0xEA
    dd long_mode_entry
    dw 0x0018

; 内核加载已移到 16 位阶段的 load_kernel_bios（BIOS INT 13h），
; 不再使用 0x1F0 的 ATA PIO，因此 U 盘也能启动。

; ============================================================
; 建 4GB 恒等映射页表，2MB 大页
;   PML4[0]  -> PDPT
;   PDPT[0..3] -> 4 个 PD（每个覆盖 1GB）
;   每个 PD 512 项，每项 2MB
; 破坏：eax/ecx/edx/edi（已用 pushad 保护）
; ============================================================
build_page_tables:
    pushad
    cld

    ; 清零 5 个页（PML4 + PDPT + 4 个 PD = 0x70000..0x75FFF）
    mov edi, PML4_BASE
    xor eax, eax
    mov ecx, (5 * 4096) / 4
    rep stosd

    ; PML4[0] = PDPT | P | RW
    mov dword [PML4_BASE], (PDPT_BASE + 0x03)

    ; PDPT[0..3] = 4 个 PD | P | RW
    mov edi, PDPT_BASE
    mov eax, PD_BASE
    mov ecx, 4
.pdpt_loop:
    mov edx, eax
    or edx, 0x03
    mov [edi], edx
    add edi, 8
    add eax, 0x1000
    dec ecx
    jnz .pdpt_loop

    ; 4 x 512 项：phys N*2MB | P | RW | PS
    mov edi, PD_BASE
    mov eax, 0x00000083
    mov ecx, 2048
.pd_loop:
    mov edx, eax
    mov [edi], edx
    mov dword [edi + 4], 0
    add edi, 8
    add eax, 0x200000
    dec ecx
    jnz .pd_loop

    popad
    ret

; ============================================================
; 32 位串口输出
; ============================================================
serial_putc32:
    push eax
    push edx
    push ecx
    mov ah, al
    mov ecx, 0x10000
.wait:
    mov dx, 0x3FD
    in al, dx
    test al, 0x20
    jnz .ready
    dec ecx
    jnz .wait
.ready:
    mov dx, 0x3F8
    mov al, ah
    out dx, al
    pop ecx
    pop edx
    pop eax
    ret

; 入参：ESI = 0 结尾字符串
serial_puts32:
    push eax
    push esi
.loop:
    lodsb
    test al, al
    jz .done
    call serial_putc32
    jmp .loop
.done:
    pop esi
    pop eax
    ret

; ============================================================
; 64 位长模式入口
; ============================================================
[BITS 64]
default abs

long_mode_entry:
    mov ax, 0x20
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov rsp, STACK_TOP
    cld

    ; 两短高音：已进入 64 位长模式（BIOS 已不可用，只能靠声音）
%ifdef SAYLTER_DEBUG_BOOT
    mov ax, 0x255
    mov ecx, 0x200
    call beep64
    mov rdx, 0x80000
.bgap:
    dec rdx
    jnz .bgap
    mov ax, 0x255
    mov ecx, 0x200
    call beep64
%endif

    mov rsi, msg_long
    call serial_puts64

    call build_idt64
    lidt [idtr64]

    mov rsi, msg_idt
    call serial_puts64

    call pic_remap64

    mov rsi, msg_irq
    call serial_puts64

    sti

    mov rax, KERNEL_ENTRY
    jmp rax

; ============================================================
; 64 位串口输出
; ============================================================
serial_putc64:
    push rax
    push rdx
    push rcx
    mov ah, al
    mov ecx, 0x10000
.wait:
    mov dx, 0x3FD
    in al, dx
    test al, 0x20
    jnz .ready
    dec ecx
    jnz .wait
.ready:
    mov dx, 0x3F8
    mov al, ah
    out dx, al
    pop rcx
    pop rdx
    pop rax
    ret

; 入参：RSI = 0 结尾字符串
serial_puts64:
    push rax
    push rsi
.loop:
    lodsb
    test al, al
    jz .done
    call serial_putc64
    jmp .loop
.done:
    pop rsi
    pop rax
    ret

; ============================================================
; 64 位蜂鸣器：AX = 分频值，RCX = 持续时间
; 长模式下 PIT 与 0x61 端口仍然可用，用于指示已进入内核。
; ============================================================
%ifdef SAYLTER_DEBUG_BOOT
beep64:
    ; 与 16 位蜂鸣一致：默认关闭
%ifndef SAYLTER_BEEP_ENABLE
    ret
%endif
    push rax
    push rbx
    push rcx
    push rdx
    mov bx, ax
    mov al, 0xB6
    out 0x43, al
    mov ax, bx
    out 0x42, al
    mov al, ah
    out 0x42, al
    in al, 0x61
    or al, 0x03
    out 0x61, al
.wait:
    mov rdx, 0x10000
.inner:
    dec rdx
    jnz .inner
    loop .wait
    in al, 0x61
    and al, 0xFC
    out 0x61, al
    pop rdx
    pop rcx
    pop rbx
    pop rax
    ret
%endif

; ============================================================
; 构建 256 项 64 位 IDT（16B/项，位于 0x60000）
;   - 0..31   : 异常诊断入口（打印向量号后停机）
;   - 0x20    : PIT 定时器
;   - 0x21    : PS/2 键盘
;   - 其余    : 忽略型默认门（EOI 双 PIC + iretq）
; ============================================================
build_idt64:
    push rdi
    push rsi
    push rcx
    push rdx
    push rax

    lea rdi, [idt_entries]
    mov ecx, 256
.fill:
    mov rax, isr_default
    call set_gate64
    add rdi, 16
    dec ecx
    jnz .fill

    ; 32 个异常入口（地址表在 exc_table）
    lea rsi, [exc_table]
    xor ecx, ecx
.exc:
    mov rax, [rsi + rcx * 8]
    mov rdi, rcx
    shl rdi, 4
    add rdi, idt_entries
    call set_gate64
    inc ecx
    cmp ecx, 32
    jb .exc

    ; IRQ0 / IRQ1
    mov rax, isr_timer
    mov rdi, idt_entries + 0x20 * 16
    call set_gate64

    mov rax, isr_keyboard
    mov rdi, idt_entries + 0x21 * 16
    call set_gate64

    ; IRQ9 = SCI（从片 IRQ1 → 主片 IRQ2 级联 → 向量 0x29）
    mov rax, isr_sci
    mov rdi, idt_entries + 0x29 * 16
    call set_gate64

    ; 向量 0xFF：LAPIC spurious（SVR 中配置的向量号）
    mov rax, isr_spurious
    mov rdi, idt_entries + 0xFF * 16
    call set_gate64

    ; 向量 0xF0：AP 工作唤醒 IPI（fixed delivery）
    mov rax, isr_ipi
    mov rdi, idt_entries + 0xF0 * 16
    call set_gate64

    pop rax
    pop rdx
    pop rcx
    pop rsi
    pop rdi
    ret

; ============================================================
; 写一个中断门
; 入参：RAX = handler 地址，RDI = 门结构地址
; 破坏：RAX
; ============================================================
set_gate64:
    mov [rdi], ax
    mov word [rdi + 2], 0x0018      ; CS = 64 位代码段
    mov byte [rdi + 4], 0           ; IST = 0
    mov byte [rdi + 5], 0x8E        ; P=1, DPL=0, 64 位中断门
    shr rax, 16
    mov [rdi + 6], ax
    shr rax, 16
    mov [rdi + 8], eax
    mov dword [rdi + 12], 0
    ret

; ============================================================
; PIC 8259 重映射：IRQ0-7 -> 0x20-0x27，IRQ8-15 -> 0x28-0x2F
;   主片开放 IRQ0(timer) / IRQ1(keyboard) / IRQ2(cascade)
;   从片只开放 IRQ9(SCI)
;   ELCR 把 IRQ9 设为电平触发：ACPI SCI 是低电平有效、共享的，
;   边沿触发会漏事件（QEMU 的 PIIX4 与真实芯片组都遵循这条）。
; ============================================================
pic_remap64:
    mov al, 0x11
    out 0x20, al
    mov al, 0x20
    out 0x21, al
    mov al, 0x04
    out 0x21, al
    mov al, 0x01
    out 0x21, al
    mov al, 0xF8                ; IRQ0/1/2 开放
    out 0x21, al

    mov al, 0x11
    out 0xA0, al
    mov al, 0x28
    out 0xA1, al
    mov al, 0x02
    out 0xA1, al
    mov al, 0x01
    out 0xA1, al
    mov al, 0xFD                ; 只开放 IRQ9
    out 0xA1, al

    ; ELCR（0x4D0 主片 / 0x4D1 从片）：IRQ9 = 从片 bit1
    ; 注意端口号 > 0xFF，必须放 DX 用 in/out dx 访问，
    ; `out 0x4D1, al` 会被 NASM 截断成 0xD1 并写坏 DMA 控制器。
    mov dx, 0x4D1
    in al, dx
    or al, 0x02
    out dx, al
    ret

; ============================================================
; 中断 / 异常入口
; ------------------------------------------------------------
; #PF（14）有独立的入口：CPU 会额外压入 error code，而且必须能够在
; C 层修复后 iretq 续跑，不能走"打印后停机"的统一出口。
; 其余异常仍然只用于诊断，因此共用 exc_common。
; ============================================================
%macro EXC_STUB 1
isr_exc%1:
    push qword %1
    jmp exc_common
%endmacro

%assign exc_index 0
%rep 32
%if exc_index != 14
    EXC_STUB exc_index
%endif
%assign exc_index exc_index + 1
%endrep

; 异常统一出口：串口打印向量号与现场 RIP 后停机（不做恢复，异常即内核 bug）
; 进入时栈顶布局（由 EXC_STUB 压入向量号后）：
;   [rsp+0] 向量号  [rsp+8] rip  [rsp+16] cs  [rsp+24] rflags
; 只打印 RIP：定位"哪条指令非法"最短路径就是它，CS/RFLAGS 在 ring0 恒为
; 0x18/固定值，没有诊断增量。
exc_common:
    pop rdi
    mov rsi, msg_exc
    call serial_puts64
    mov al, dil
    shr al, 4
    call hex_nibble64
    mov al, dil
    and al, 0x0F
    call hex_nibble64

    mov rsi, msg_exc_rip
    call serial_puts64
    ; 硬件帧（同一特权级）从栈顶依次是：错误码（仅部分异常）、RIP、
    ; CS、RFLAGS。这里把 4 个 qword 原样打出来，避免因为"有没有错误码"
    ; 判断失误而把错误码当成 RIP 解释，误导定位。
    mov rbx, rsp
    mov rbp, 4
.frame_q:
    mov rdx, [rbx]
    mov ecx, 16
.q_hex:
    rol rdx, 4
    mov al, dl
    and al, 0x0F
    call hex_nibble64
    loop .q_hex
    mov al, ' '
    call serial_putc64
    add rbx, 8
    dec rbp
    jnz .frame_q

    mov al, 10
    call serial_putc64
.halt:
    cli
    hlt
    jmp .halt

; ============================================================
; #PF（向量 14）专用入口
; ------------------------------------------------------------
; CPU 压栈顺序（从栈顶向高地址）：
;   [rsp+0]  error_code
;   [rsp+8]  rip
;   [rsp+16] cs
;   [rsp+24] rflags
;   [rsp+32] rsp（CPL 变化时压入；本内核异常始终来自 ring0）
;   [rsp+40] ss
;
; 调用 C 处理函数前必须满足 Win64 ABI：
;   - 32 字节 shadow space
;   - call 指令执行时 rsp 16 字节对齐
; 中断可能发生在任意指令边界，进入时 rsp 不保证对齐，
; 因此保存现场后用 and rsp, -16 动态对齐，调用结束再回到 rbp。
;
; C 签名：
;   int page_fault_handler(uint64_t cr2, uint64_t error, uint64_t rip)
; 返回 1 = 映射已建立，恢复现场后 iretq 重新执行触发缺页的指令。
;
; 保存 r12/r13 是必须的：它们属于 Win64 的 callee-saved 寄存器，
; 被中断的代码可能正在使用；不保存会在成功续跑后悄悄改坏调用者状态。
; ============================================================
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
    mov r12, [rbp + 96]          ; error code（12 个 push = 96 字节）
    mov r13, [rbp + 104]         ; rip

    ; 在调用 C 之前固化现场，避免嵌套缺页覆盖寄存器保存区后无法排障
    mov rax, cr2
    mov [PF_LAST_CR2], rax
    mov [PF_LAST_ERROR], r12
    mov [PF_LAST_RIP], r13

    mov rax, [PF_HANDLER_ADDR]
    test rax, rax
    jz .fatal

    and rsp, -16
    sub rsp, 32                  ; Win64 shadow space
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
    add rsp, 8                   ; 丢弃 error code
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
    add rsp, 8                   ; 丢弃 error code

    mov rsi, msg_pf
    call serial_puts64
    mov rsi, msg_pf_cr2
    call serial_puts64
    mov rax, [PF_LAST_CR2]
    call serial_hex64
    mov rsi, msg_pf_err
    call serial_puts64
    mov rax, [PF_LAST_ERROR]
    call serial_hex64
    mov rsi, msg_pf_rip
    call serial_puts64
    mov rax, [PF_LAST_RIP]
    call serial_hex64
    mov al, 13
    call serial_putc64
    mov al, 10
    call serial_putc64
.halt_pf:
    cli
    hlt
    jmp .halt_pf

; 输出 RAX 的 16 位十六进制（大写，无前缀）
serial_hex64:
    push rax
    push rcx
    push rdx
    push rbx
    mov rbx, rax
    mov ecx, 16
.loop:
    mov rax, rbx
    shr rax, 60
    cmp al, 10
    jb .digit
    add al, 'A' - 10
    jmp .emit
.digit:
    add al, '0'
.emit:
    call serial_putc64
    shl rbx, 4
    dec ecx
    jnz .loop
    pop rbx
    pop rdx
    pop rcx
    pop rax
    ret

hex_nibble64:
    cmp al, 10
    jb .digit
    add al, 'A' - 10
    jmp serial_putc64
.digit:
    add al, '0'
    jmp serial_putc64

; 未注册向量：EOI 双 PIC 后直接返回
isr_default:
    push rax
    push rdx
    mov al, 0x20
    out 0x20, al
    out 0xA0, al
    pop rdx
    pop rax
    iretq

; LAPIC spurious 中断（向量 0xFF）专用：直接返回。
; spurious 不需要任何 EOI；更不能写 8259 的 EOI，否则会把 BSP
; 正在服务的 PIC 中断提前清掉，导致后续 IRQ 丢失。
isr_spurious:
    iretq

; LAPIC fixed IPI（向量 0xF0）：唤醒 AP 去取并行工作。
; fixed IPI 不需要 EOI。这里绝不能像 isr_default 那样写 8259 的
; EOI —— AP 与 BSP 共享 8259，误清 EOI 会让 BSP 丢失正在处理的
; 键盘/定时器中断。
isr_ipi:
    iretq

; PIT 定时器：只发 EOI，不写显存
; 用特定 EOI（0x60 | 0）：非特定 EOI 在 IRQ2 级联服务期间会清错位。
isr_timer:
    push rax
    push rdx
    mov al, 0x60
    out 0x20, al
    pop rdx
    pop rax
    iretq

; PS/2 键盘：扫描码写入环形缓冲（C 层轮询消费）
;   0xA000 : head（本 ISR 写）
;   0xA002 : tail（C 层写）
;   0xA010 : 64 字节环形缓冲
; 单字节 mailbox 在连续扫描码（如 Ctrl+S = ctrl down / s down / s up /
; ctrl up 四个事件）时会互相覆盖，导致修饰键状态错乱，因此改为环形缓冲。
KB_HEAD_ADDR equ 0xA000
KB_TAIL_ADDR equ 0xA002
KB_BUF_ADDR  equ 0xA010
KB_BUF_MASK  equ 63

isr_keyboard:
    push rax
    push rcx
    push rdx
    push rsi
    ; 8042 输出缓冲只有一个字节，键盘和鼠标共用。
    ; 只有"OBF=1 且 AUX=0"才是键盘数据。
    ; AUX=1 的鼠标字节必须读走丢弃：TTY 模式没有鼠标轮询者，
    ; 不读的话输出缓冲会一直被鼠标包占住，键盘中断再也取不到
    ; 数据，表现为"从 GUI 退出到 TTY 后键盘完全没反应"。
    in al, 0x64
    mov cl, al
    and cl, 0x21                ; bit0=OBF, bit5=AUX
    cmp cl, 0x01
    jne .not_kbd
    in al, 0x60
    movzx esi, word [KB_HEAD_ADDR]
    lea edx, [esi + 1]
    and edx, KB_BUF_MASK
    movzx ecx, word [KB_TAIL_ADDR]
    cmp edx, ecx
    je .drop                       ; 缓冲满：丢弃最新扫描码
    mov [KB_BUF_ADDR + rsi], al
    mov [KB_HEAD_ADDR], dx
    jmp .drop
.not_kbd:
    test al, 0x01                  ; OBF=0 就没什么可读
    jz .drop
    in al, 0x60                    ; 鼠标字节：读走丢弃
.drop:
    mov al, 0x61                ; 特定 EOI：主片 IRQ1
    out 0x20, al
    pop rsi
    pop rdx
    pop rcx
    pop rax
    iretq

; ============================================================
; IRQ9 = SCI：转交内核注册的 ACPI 分发器
; ------------------------------------------------------------
; 回调地址由内核在 ACPICA 初始化完成后写入 SCI_CALLBACK_ADDR。
; 必须在中断上下文里清掉 GPE（ACPICA 的 SCI handler 会做），
; 否则 EOI 之后 SCI 电平仍然有效，会立刻再次进入本 ISR。
;
; Win64 ABI：被调函数需要 32 字节 shadow space，而且是 16 字节对齐；
; 9 个 push（72 字节）刚好把 RSP 从 8 mod 16 对齐到 0 mod 16。
; ============================================================
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
    ; IRQ9 在从片：必须用特定 EOI（OCW2 = 0x60 | IRQ 位）。
    ; 非特定 EOI（0x20）清的是 PIC 中"当前最高优先级 ISR 位"：
    ; 定时器 IRQ0 若在 SCI 服务期间触发，会把 IRQ2 的 ISR 位留下，
    ; 主片从此屏蔽级联，后续从片中断（包括下一次电源键）全部丢失。
    mov al, 0x61                ; 从片 IRQ1（= IRQ9）
    out 0xA0, al
    mov al, 0x62                ; 主片 IRQ2（级联）
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

; ============================================================
; VBE BIOS 初始化（实模式）
; 按优先级挑选 32bpp + 线性帧缓冲模式，结果写 0x7DF0
; 破坏：ax, bx, cx, dx, di, si, es
; ============================================================
[BITS 16]

vbe_init:
    ; VBE 2.0+：控制块前 4 字节必须写 "VBE2"，否则部分 BIOS
    ; （VMware / Phoenix）只返回 VBE 1.x 模式列表，看不到 LFB 模式。
    mov dword [VBE_CTL_BUF], 0x32454256

    xor ax, ax
    mov es, ax

    mov ax, 0x4F00
    mov di, VBE_CTL_BUF
    int 0x10
    cmp ax, 0x004F
    je .vbe_4f_ok
    push ax
    mov si, msg_vbe_4f_fail
    call serial_puts16
    pop ax
    call serial_hex_ax16
    mov si, msg_crlf
    call serial_puts16

    ; 阶段 1：VBE 4F00 失败（AX 为 BIOS 返回码）
%ifdef SAYLTER_DEBUG_BOOT
    push ax
    mov dx, 1
    call raw_slot_start
    mov si, msg_raw_4f_fail
    call raw_append
    pop ax
    call raw_hex16
    mov dx, 1
    call raw_commit
    mov si, 1
    mov ax, BEEP_DIV_LOW
    mov cx, BEEP_LEN_LONG
    call beep_times
%endif

    jmp .vbe_fail
.vbe_4f_ok:
    mov si, msg_vbe_4f_ok
    call serial_puts16

    ; 阶段 1：VBE 4F00 成功
%ifdef SAYLTER_DEBUG_BOOT
    mov dx, 1
    call raw_slot_start
    mov si, msg_raw_4f_ok
    call raw_append
    mov dx, 1
    call raw_commit
    mov si, 2
    mov ax, BEEP_DIV_MID
    mov cx, BEEP_LEN
    call beep_times
%endif

    mov si, vbe_prefer
.prefer_loop:
    mov ax, [si]                  ; 期望宽度（0 = 优先级表结束）
    test ax, ax
    jz .fallback_start
    mov [vbe_want_w], ax
    mov ax, [si + 2]              ; 期望高度
    mov [vbe_want_h], ax

    xor bx, bx                    ; 模式列表字节偏移
.mode_loop:
    push es
    les di, [VBE_CTL_BUF + 0x0E]  ; VideoModePtr 在 0x0E
    mov dx, [es:di + bx]
    pop es
    cmp dx, 0xFFFF
    je .next_pref

    mov ax, 0x4F01
    mov cx, dx
    mov di, VBE_MODE_BUF
    push bx                       ; BIOS 可能踩 BX，保住循环偏移
    int 0x10
    pop bx
    cmp ax, 0x004F
    jne .next_mode

    ; 模式必须满足：支持线性帧缓冲、直接色、32bpp
    test byte [VBE_MODE_BUF + 0x00], 0x80   ; ModeAttributes.bit7 = LFB
    jz .next_mode
    cmp byte [VBE_MODE_BUF + 0x1B], 6       ; MemoryModel = Direct Color
    jne .next_mode
    cmp byte [VBE_MODE_BUF + 0x19], 32      ; BitsPerPixel
    jne .next_mode

    mov ax, [vbe_want_w]
    cmp word [VBE_MODE_BUF + 0x12], ax      ; XResolution
    jne .next_mode
    mov ax, [vbe_want_h]
    cmp word [VBE_MODE_BUF + 0x14], ax      ; YResolution
    jne .next_mode

    ; ---------- 命中：设置模式（置 LFB bit），再查一次模式信息 ----------
    ; 先把"即将设置的模式号"落盘：4F02 内部若卡死，也能知道选了哪个模式
%ifdef SAYLTER_DEBUG_BOOT
    push dx
    mov dx, 2
    call raw_slot_start
    mov si, msg_raw_setting_mode
    call raw_append
    pop ax
    call raw_hex16
    mov bx, ax                    ; 暂存模式号（raw_commit 会保护 BX）
    mov dx, 2
    call raw_commit
    mov si, 1
    mov ax, BEEP_DIV_MID
    mov cx, BEEP_LEN
    call beep_times
    mov dx, bx                    ; 恢复模式号，后续 4F02 必须用它
%endif

    push dx                       ; 保住原始模式号（不带 0x4000）
    mov bx, dx
    or bx, 0x4000
    mov ax, 0x4F02
    int 0x10
    pop dx
    cmp ax, 0x004F
    jne .next_pref

    mov ax, 0x4F01
    mov cx, dx
    mov di, VBE_MODE_BUF
    int 0x10
    cmp ax, 0x004F
    jne .next_pref

    ; ---------- 存结果到 0x7DF0（C 层直接读） ----------
    ; 阶段 2：模式设置成功（DX = 模式号）
%ifdef SAYLTER_DEBUG_BOOT
    push dx
    mov dx, 2
    call raw_slot_start
    mov si, msg_raw_mode_set
    call raw_append
    pop ax
    call raw_hex16
    mov dx, 2
    call raw_commit
    mov si, 3
    mov ax, BEEP_DIV_HIGH
    mov cx, BEEP_LEN
    call beep_times
%endif

    mov word [VBE_RESULT], 0xA1B2          ; magic = OK
    mov eax, [VBE_MODE_BUF + 0x28]         ; FrameBufferBase (phys) @ 0x28
    mov [VBE_RESULT + 2], eax
    mov ax, [VBE_MODE_BUF + 0x12]          ; XResolution @ 0x12
    mov [VBE_RESULT + 6], ax
    mov ax, [VBE_MODE_BUF + 0x14]          ; YResolution @ 0x14
    mov [VBE_RESULT + 8], ax
    mov al, [VBE_MODE_BUF + 0x19]          ; BitsPerPixel @ 0x19
    mov [VBE_RESULT + 10], al
    mov ax, [VBE_MODE_BUF + 0x10]          ; BytesPerScanLine @ 0x10
    mov [VBE_RESULT + 11], ax

    ; 阶段 3：VBE 结果已写入 0x7DF0，落盘供真机黑屏后排查
%ifdef SAYLTER_DEBUG_BOOT
    pusha
    mov dx, 3
    call raw_slot_start
    mov si, msg_raw_result
    call raw_append
    mov eax, [VBE_RESULT + 2]
    call raw_hex32
    mov si, msg_raw_size
    call raw_append
    movzx ax, word [VBE_RESULT + 6]
    call raw_hex16
    mov si, msg_raw_x
    call raw_append
    movzx ax, word [VBE_RESULT + 8]
    call raw_hex16
    mov si, msg_raw_bpp
    call raw_append
    movzx ax, byte [VBE_RESULT + 10]
    call raw_hex16
    mov dx, 3
    call raw_commit
    mov si, 1
    mov ax, BEEP_DIV_HIGH
    mov cx, BEEP_LEN_LONG
    call beep_times
    popa
%endif
    ret

.next_mode:
    add bx, 2
    jmp .mode_loop

.next_pref:
    add si, 4
    jmp .prefer_loop

; ---------- 兜底：优先表没有匹配时，选面积最大的 32bpp LFB 模式 ----------
.fallback_start:
    xor bx, bx
    mov word [vbe_best_mode], 0xFFFF
    mov dword [vbe_best_bytes], 0

.fb_mode_loop:
    push es
    les di, [VBE_CTL_BUF + 0x0E]
    mov dx, [es:di + bx]
    pop es
    cmp dx, 0xFFFF
    je .fb_done

    mov ax, 0x4F01
    mov cx, dx
    mov di, VBE_MODE_BUF
    push bx
    int 0x10
    pop bx
    cmp ax, 0x004F
    jne .fb_next

    test byte [VBE_MODE_BUF + 0x00], 0x80
    jz .fb_next
    cmp byte [VBE_MODE_BUF + 0x1B], 6
    jne .fb_next
    cmp byte [VBE_MODE_BUF + 0x19], 32
    jne .fb_next

    ; bytes = pitch * height，必须 <= 8MB（backbuffer 上限）
    movzx eax, word [VBE_MODE_BUF + 0x10]
    movzx ecx, word [VBE_MODE_BUF + 0x14]
    imul eax, ecx
    cmp eax, 8*1024*1024
    ja .fb_next

    cmp eax, [vbe_best_bytes]
    jbe .fb_next
    mov [vbe_best_bytes], eax
    mov [vbe_best_mode], dx

.fb_next:
    add bx, 2
    jmp .fb_mode_loop

.fb_done:
    mov dx, [vbe_best_mode]
    cmp dx, 0xFFFF
    jne .fb_set
    mov si, msg_vbe_nomode
    call serial_puts16
    jmp .vbe_fail
.fb_set:

    ; 先把"即将设置的兜底模式号"落盘，4F02 卡死时也能定位
%ifdef SAYLTER_DEBUG_BOOT
    push dx
    mov dx, 2
    call raw_slot_start
    mov si, msg_raw_setting_fb
    call raw_append
    pop ax
    call raw_hex16
    mov bx, ax                    ; 暂存模式号
    mov dx, 2
    call raw_commit
    mov si, 1
    mov ax, BEEP_DIV_MID
    mov cx, BEEP_LEN
    call beep_times
    mov dx, bx                    ; 恢复模式号，后续 4F02 必须用它
%endif

    push dx
    mov bx, dx
    or bx, 0x4000
    mov ax, 0x4F02
    int 0x10
    pop dx
    cmp ax, 0x004F
    jne .vbe_fail

    mov ax, 0x4F01
    mov cx, dx
    mov di, VBE_MODE_BUF
    int 0x10
    cmp ax, 0x004F
    jne .vbe_fail

    ; 阶段 2：兜底模式设置成功（DX = 模式号）
%ifdef SAYLTER_DEBUG_BOOT
    push dx
    mov dx, 2
    call raw_slot_start
    mov si, msg_raw_mode_fb
    call raw_append
    pop ax
    call raw_hex16
    mov dx, 2
    call raw_commit
    mov si, 3
    mov ax, BEEP_DIV_HIGH
    mov cx, BEEP_LEN
    call beep_times
%endif

    mov word [VBE_RESULT], 0xA1B2
    mov eax, [VBE_MODE_BUF + 0x28]
    mov [VBE_RESULT + 2], eax
    mov ax, [VBE_MODE_BUF + 0x12]
    mov [VBE_RESULT + 6], ax
    mov ax, [VBE_MODE_BUF + 0x14]
    mov [VBE_RESULT + 8], ax
    mov al, [VBE_MODE_BUF + 0x19]
    mov [VBE_RESULT + 10], al
    mov ax, [VBE_MODE_BUF + 0x10]
    mov [VBE_RESULT + 11], ax

    ; 阶段 3：结果落盘
%ifdef SAYLTER_DEBUG_BOOT
    pusha
    mov dx, 3
    call raw_slot_start
    mov si, msg_raw_result
    call raw_append
    mov eax, [VBE_RESULT + 2]
    call raw_hex32
    mov si, msg_raw_size
    call raw_append
    movzx ax, word [VBE_RESULT + 6]
    call raw_hex16
    mov si, msg_raw_x
    call raw_append
    movzx ax, word [VBE_RESULT + 8]
    call raw_hex16
    mov si, msg_raw_bpp
    call raw_append
    movzx ax, byte [VBE_RESULT + 10]
    call raw_hex16
    mov dx, 3
    call raw_commit
    mov si, 1
    mov ax, BEEP_DIV_HIGH
    mov cx, BEEP_LEN_LONG
    call beep_times
    popa
%endif
    ret

.vbe_fail:
    mov word [VBE_RESULT], 0x0000          ; magic = fail

    ; 阶段 3：VBE 完全失败（回退文本模式）
%ifdef SAYLTER_DEBUG_BOOT
    mov dx, 3
    call raw_slot_start
    mov si, msg_raw_vbe_fail
    call raw_append
    mov dx, 3
    call raw_commit
    mov si, 1
    mov ax, BEEP_DIV_LOW
    mov cx, BEEP_LEN_LONG
    call beep_times
%endif
    ret

; 期望模式表：宽, 高（按优先级，0,0 结束）
; 16x32 字体下：1600x900 → 100x28 字符，1440x900 → 90x28，
; 1280x720 → 80x22，1024x768 → 64x24。改这张表即可改默认终端尺寸。
vbe_prefer:
    dw 1600, 900
    dw 1440, 900
    dw 1280, 720
    dw 1024, 768
    dw 0, 0
vbe_want_w: dw 0
vbe_want_h: dw 0
vbe_best_mode:  dw 0xFFFF
vbe_best_bytes: dd 0

msg_vbe_4f_ok:   db '[PM] VBE 4F00 ok', 13, 10, 0
msg_vbe_4f_fail: db '[PM] VBE 4F00 ax=', 0
msg_vbe_nomode:  db '[PM] VBE no 32bpp LFB mode', 13, 10, 0

; AX 的 4 位十六进制输出（16 位实模式串口）
serial_hex_ax16:
    push ax
    push bx
    push cx
    mov bx, ax
    mov cx, 4
.hx_loop:
    rol bx, 4
    mov al, bl
    and al, 0x0F
    cmp al, 10
    jb .hx_digit
    add al, 'A' - 10
    jmp .hx_out
.hx_digit:
    add al, '0'
.hx_out:
    call serial_putc16
    loop .hx_loop
    pop cx
    pop bx
    pop ax
    ret

; ============================================================
; GDT
;   0x08 : 32 位代码（用于 16->32 跳转）
;   0x10 : 32 位数据
;   0x18 : 64 位代码（L=1, D=0）
;   0x20 : 数据（长模式下平坦寻址）
; ============================================================
ALIGN 8
gdt_start:
    dq 0x0000000000000000      ; Null

    ; 0x08: 32-bit Code
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 0x9A                    ; P=1, DPL=0, S=1, Code/Readable
    db 0xCF                    ; G=1, D=1, LimitH=0xF
    db 0x00

    ; 0x10: 32-bit Data
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 0x92                    ; P=1, DPL=0, S=1, Data/Writable
    db 0xCF
    db 0x00

    ; 0x18: 64-bit Code
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 0x9A
    db 0xAF                    ; G=1, D=0, L=1, LimitH=0xF
    db 0x00

    ; 0x20: Data（长模式）
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 0x92
    db 0xCF
    db 0x00
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

; ============================================================
; 64 位 IDT 描述符与异常地址表
; IDT 本体在 0x60000，这里只放 10 字节的 IDTR 与 32 个入口地址
; ============================================================
ALIGN 8
idtr64:
    dw 256 * 16 - 1
    dq IDT_BASE

exc_table:
    dq isr_exc0
    dq isr_exc1
    dq isr_exc2
    dq isr_exc3
    dq isr_exc4
    dq isr_exc5
    dq isr_exc6
    dq isr_exc7
    dq isr_exc8
    dq isr_exc9
    dq isr_exc10
    dq isr_exc11
    dq isr_exc12
    dq isr_exc13
    dq isr_exc14
    dq isr_exc15
    dq isr_exc16
    dq isr_exc17
    dq isr_exc18
    dq isr_exc19
    dq isr_exc20
    dq isr_exc21
    dq isr_exc22
    dq isr_exc23
    dq isr_exc24
    dq isr_exc25
    dq isr_exc26
    dq isr_exc27
    dq isr_exc28
    dq isr_exc29
    dq isr_exc30
    dq isr_exc31

; ============================================================
; 串口诊断消息（全部纯 ASCII）
; ============================================================
msg_vbe_ok:   db '[PM] VBE mode set', 13, 10, 0
msg_vbe_fail: db '[PM] VBE unavailable, text fallback', 13, 10, 0
msg_pm32:     db '[PM] 32-bit protected mode', 13, 10, 0
msg_load:       db '[PM] loading kernel via BIOS INT 13h', 13, 10, 0
msg_loaded:     db '[PM] kernel loaded at 0x100000', 13, 10, 0
msg_kernel_ok:  db '[PM] kernel transfer complete', 13, 10, 0
msg_kernel_fail: db '[PM] KERNEL LOAD FAILED (INT 13h)', 13, 10, 0
msg_drv:        db '[PM] boot drive=0x', 0
msg_bios_err:   db '[PM] int13h AH=', 0
msg_crlf:       db 13, 10, 0
msg_paging:   db '[PM] page tables ready', 13, 10, 0
msg_long:     db '[PM] 64-bit long mode', 13, 10, 0
msg_idt:      db '[PM] 64-bit IDT loaded', 13, 10, 0
msg_irq:      db '[PM] IRQ0/IRQ1/IRQ9 enabled', 13, 10, 0
msg_exc:      db '[PM] EXCEPTION vector=0x', 0
msg_exc_rip:  db ' rip=', 0
msg_pf:       db '[PM] PAGE FAULT cr2=', 0

; ============================================================
; BIOS 内核加载状态
; ============================================================
ALIGN 4
bios_dap:
    db 0x10
    db 0x00
    dw 0            ; 扇区数
    dw 0            ; 目标偏移
    dw 0x1000       ; 目标段
    dq 0            ; LBA

bios_lba:   dd 0
bios_dst:   dd 0
bios_left:  dw 0
bios_secs:  dw 0
bios_err:   db 0
msg_pf_cr2:   db '0x', 0
msg_pf_err:   db ' error=', 0
msg_pf_rip:   db ' rip=', 0

; ============================================================
; 裸日志消息
; ============================================================
%ifdef SAYLTER_DEBUG_BOOT
msg_raw_enter:     db '[PM] enter (real mode)', 0
msg_raw_vbe_start: db '[PM] VBE init start (call 4F00)', 0
msg_raw_setting_mode: db '[PM] VBE setting mode 0x', 0
msg_raw_setting_fb:   db '[PM] VBE setting fallback 0x', 0
msg_raw_4f_ok:     db '[PM] VBE 4F00 ok', 0
msg_raw_4f_fail:   db '[PM] VBE 4F00 FAIL ax=', 0
msg_raw_mode_set:  db '[PM] VBE mode set 0x', 0
msg_raw_mode_fb:   db '[PM] VBE fallback mode 0x', 0
msg_raw_result:    db '[PM] VBE RESULT fb=0x', 0
msg_raw_size:      db ' size=', 0
msg_raw_x:         db 'x', 0
msg_raw_bpp:       db ' bpp=0x', 0
msg_raw_vbe_fail:  db '[PM] VBE FAILED, text fallback', 0
msg_raw_load:      db '[PM] kernel load start sectors=0x', 0
msg_raw_load_ok:   db '[PM] kernel load OK', 0
msg_raw_load_fail: db '[PM] kernel load FAIL err=0x', 0
msg_raw_pmode:     db '[PM] entering protected mode', 0
raw_prefix:        db 'SLYPM ', 0
raw_sep:           db ': ', 0

; ============================================================
; 启动盘裸日志（BIOS INT 13h AH=43h，固定槽位）
; ------------------------------------------------------------
; 只写 [0x500] 保存的 BIOS 启动盘号，绝不枚举/写入其它磁盘。
; 每个槽 = 1 个扇区，内容先清空再写，写失败静默跳过。
; ============================================================
raw_clear:
    pusha
    push ds
    push es
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov di, raw_buf
    xor ax, ax
    mov cx, 256
    rep stosw
    mov word [raw_pos], 0
    pop es
    pop ds
    popa
    ret

; SI = 0 结尾字符串
raw_append:
    pusha
    push ds
    xor ax, ax
    mov ds, ax
    mov bx, [raw_pos]
.loop:
    lodsb
    test al, al
    jz .done
    cmp bx, 510
    jae .done
    mov [raw_buf + bx], al
    inc bx
    jmp .loop
.done:
    mov [raw_pos], bx
    pop ds
    popa
    ret

; AL = 0..15
raw_hex_nibble:
    pusha
    push ds
    mov bl, al
    xor ax, ax
    mov ds, ax
    mov al, bl
    cmp al, 10
    jb .digit
    add al, 'A' - 10
    jmp .put
.digit:
    add al, '0'
.put:
    mov bx, [raw_pos]
    cmp bx, 510
    jae .out
    mov [raw_buf + bx], al
    inc bx
    mov [raw_pos], bx
.out:
    pop ds
    popa
    ret

; AL = 字节
raw_hex8:
    push ax
    mov bl, al
    shr al, 4
    call raw_hex_nibble
    mov al, bl
    and al, 0x0F
    call raw_hex_nibble
    pop ax
    ret

; AX = 字
raw_hex16:
    push ax
    mov al, ah
    call raw_hex8
    pop ax
    call raw_hex8
    ret

; EAX = 双字
raw_hex32:
    push eax
    shr eax, 16
    call raw_hex16
    pop eax
    call raw_hex16
    ret

; DX = 槽号：清空缓冲并写入 "SLYPM <槽号>: "
raw_slot_start:
    push dx
    call raw_clear
    mov si, raw_prefix
    call raw_append
    pop ax
    call raw_hex16
    mov si, raw_sep
    call raw_append
    ret

; DX = 槽号：补零到 512B 并写到启动盘 RAW_LOG_LBA + DX
raw_commit:
    pusha
    push ds
    push es
    xor ax, ax
    mov ds, ax
    mov es, ax

    mov bx, [raw_pos]
    cmp bx, 512
    jae .filled
    mov di, raw_buf
    add di, bx
    mov cx, 512
    sub cx, bx
    xor ax, ax
    rep stosb

.filled:
    mov word [raw_dap + 2], 1
    mov word [raw_dap + 4], raw_buf
    mov word [raw_dap + 6], 0
    mov ax, RAW_LOG_LBA
    add ax, dx
    mov [raw_dap + 8], ax
    mov word [raw_dap + 10], 0
    mov word [raw_dap + 12], 0
    mov word [raw_dap + 14], 0

    mov bp, 3
.retry:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ah, 0x43          ; extended write
    mov al, 0x00          ; no verify
    mov dl, [0x0500]      ; BIOS 启动盘号（0x500 由 MBR 保存）
    mov si, raw_dap
    int 0x13
    jnc .ok
    xor ah, ah
    mov dl, [0x0500]
    int 0x13              ; reset disk
    dec bp
    jnz .retry
.ok:
    pop es
    pop ds
    popa
    ret

; ============================================================
; PC 蜂鸣器（PIT 通道 2 + 0x61）
; 入参：AX = 分频值，CX = 持续时间
; ============================================================
beep_once:
    ; 主板无蜂鸣器时直接跳过（默认关闭，避免无谓的启动延迟）
%ifndef SAYLTER_BEEP_ENABLE
    ret
%endif
    pusha
    mov bx, ax
    mov al, 0xB6
    out 0x43, al
    mov ax, bx
    out 0x42, al
    mov al, ah
    out 0x42, al
    in al, 0x61
    or al, 0x03
    out 0x61, al
.wait:
    mov dx, 0x100
.inner:
    dec dx
    jnz .inner
    loop .wait
    in al, 0x61
    and al, 0xFC
    out 0x61, al
    popa
    ret

; 入参：SI = 次数，AX = 分频值，CX = 每次持续
beep_times:
    push si
    push ax
    push cx
.next:
    push cx
    call beep_once
    pop cx
    push cx
    mov dx, 0x600
.gap:
    dec dx
    jnz .gap
    pop cx
    dec si
    jnz .next
    pop cx
    pop ax
    pop si
    ret

ALIGN 16
raw_pos:  dw 0
raw_dap:
    db 0x10
    db 0x00
    dw 1
    dw 0
    dw 0
    dq 0
raw_buf:  times 512 db 0
%endif
