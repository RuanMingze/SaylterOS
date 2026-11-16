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

    ; 键盘环形缓冲头尾清零（ISR 在进入长模式后才会写）
    mov word [0xA000], 0
    mov word [0xA002], 0

    ; A20 必须打开：内核镜像搬到 1MB 以上，ATA PIO 直接写高地址
    call enable_a20_16

    ; --- VBE BIOS 模式设置：终端统一走图形模式 ---
    ; 失败时 magic = 0，C 层自动回退到 80x25 文本终端。
    ; BIOS 的 VBE 调用可能改写 UART 的 LCR/DLAB，导致后续串口日志丢失，
    ; 因此调用前后各重初始化一次 COM1。
    call serial_init16
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

    ; ---------- ATA PIO：把内核从 LBA19 搬到 0x100000 ----------
    mov esi, msg_load
    call serial_puts32

    mov eax, KERNEL_LBA
    mov ebp, KERNEL_SECTORS
    mov edi, KERNEL_ENTRY
    call ata_load_kernel32
    jc .load_failed

    mov esi, msg_loaded
    call serial_puts32
    jmp .load_done

.load_failed:
    mov esi, msg_load_fail
    call serial_puts32
.halt_load:
    cli
    hlt
    jmp .halt_load

.load_done:
    call build_page_tables

    mov esi, msg_paging
    call serial_puts32

    ; ---------- 打开 PAE + 装入 PML4 ----------
    mov eax, cr4
    or eax, 0x20
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

; ============================================================
; ATA PIO 读取内核镜像到指定物理地址
; 入参：EAX = 起始 LBA，EBP = 扇区数，EDI = 目标物理地址
; 出参：CF = 0 成功 / CF = 1 失败
; 说明：只用主通道（0x1F0），28 位 LBA，PIO 单扇区读，
;       与 SlyzFS 的磁盘访问方式一致（同一块 IDE 盘）。
; 破坏：eax/ebx/ecx/edx/esi/edi/ebp
; ============================================================
ata_load_kernel32:
    push eax
    push ebx
    push ecx
    push edx
    push esi
    push edi

    mov esi, eax                 ; ESI = 当前 LBA

    ; 探测：主通道状态寄存器全 1 表示总线无设备
    mov dx, 0x1F7
    in al, dx
    cmp al, 0xFF
    je .fail

.next_sector:
    test ebp, ebp
    jz .ok

    ; 等 BSY 清除（轮询上限约 2M 次）
    mov ecx, 0x200000
.wait_bsy:
    mov dx, 0x1F7
    in al, dx
    test al, 0x80
    jz .bsy_clear
    dec ecx
    jnz .wait_bsy
    jmp .fail
.bsy_clear:

    ; 驱动器/磁头：主盘 + LBA 高 4 位（0xE0 = LBA 模式）
    mov ebx, esi
    shr ebx, 24
    and ebx, 0x0F
    or ebx, 0xE0
    mov dx, 0x1F6
    mov al, bl
    out dx, al

    ; 扇区计数 = 1
    mov dx, 0x1F2
    mov al, 1
    out dx, al

    ; LBA[7:0]
    mov ebx, esi
    mov dx, 0x1F3
    mov al, bl
    out dx, al

    ; LBA[15:8]
    mov ebx, esi
    shr ebx, 8
    mov dx, 0x1F4
    mov al, bl
    out dx, al

    ; LBA[23:16]
    mov ebx, esi
    shr ebx, 16
    mov dx, 0x1F5
    mov al, bl
    out dx, al

    ; READ SECTORS = 0x20
    mov dx, 0x1F7
    mov al, 0x20
    out dx, al

    ; 等 DRQ 置位
    mov ecx, 0x200000
.wait_drq:
    mov dx, 0x1F7
    in al, dx
    test al, 0x80
    jnz .drq_retry
    test al, 0x08
    jnz .drq_ready
    test al, 0x01
    jnz .fail
.drq_retry:
    dec ecx
    jnz .wait_drq
    jmp .fail

.drq_ready:
    ; rep insw 会自动推进 EDI（256 个 word = 512 字节）
    mov ecx, 256
    mov dx, 0x1F0
    rep insw

    inc esi
    dec ebp
    jmp .next_sector

.ok:
    pop edi
    pop esi
    pop edx
    pop ecx
    pop ebx
    pop eax
    clc
    ret

.fail:
    pop edi
    pop esi
    pop edx
    pop ecx
    pop ebx
    pop eax
    stc
    ret

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

; 异常统一出口：串口打印向量号后停机（不做恢复，异常即内核 bug）
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
    mov ax, 0x4F00
    mov di, VBE_CTL_BUF
    int 0x10
    cmp ax, 0x004F
    jne .vbe_fail

    mov si, vbe_prefer
.prefer_loop:
    mov ax, [si]                  ; 期望宽度（0 = 优先级表结束）
    test ax, ax
    jz .vbe_fail
    mov [vbe_want_w], ax
    mov ax, [si + 2]              ; 期望高度
    mov [vbe_want_h], ax

    xor bx, bx                    ; 模式列表字节偏移
.mode_loop:
    les di, [VBE_CTL_BUF + 0x0E]  ; VideoModePtr 在 0x0E
    mov dx, [es:di + bx]
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
    ret

.next_mode:
    add bx, 2
    jmp .mode_loop

.next_pref:
    add si, 4
    jmp .prefer_loop

.vbe_fail:
    mov word [VBE_RESULT], 0x0000          ; magic = fail
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
msg_load:     db '[PM] loading kernel via ATA PIO', 13, 10, 0
msg_loaded:   db '[PM] kernel loaded at 0x100000', 13, 10, 0
msg_load_fail: db '[PM] KERNEL LOAD FAILED (ATA PIO)', 13, 10, 0
msg_paging:   db '[PM] page tables ready', 13, 10, 0
msg_long:     db '[PM] 64-bit long mode', 13, 10, 0
msg_idt:      db '[PM] 64-bit IDT loaded', 13, 10, 0
msg_irq:      db '[PM] IRQ0/IRQ1/IRQ9 enabled', 13, 10, 0
msg_exc:      db '[PM] EXCEPTION vector=0x', 0
msg_pf:       db '[PM] PAGE FAULT cr2=', 0
msg_pf_cr2:   db '0x', 0
msg_pf_err:   db ' error=', 0
msg_pf_rip:   db ' rip=', 0
