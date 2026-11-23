/* ============================================================
 * msabi.s - Win64(MS) 与 SysV 之间的调用桥
 * ------------------------------------------------------------
 * SaylterOS 内核用 MinGW gcc 编译，遵循 Microsoft x64 ABI：
 *   整数参数 rcx, rdx, r8, r9，调用方必须预留 32 字节 shadow space。
 * 本模块（musl + port 层）用 x86_64-elf gcc 编译，遵循 SysV ABI：
 *   整数参数 rdi, rsi, rdx, rcx, r8, r9。
 *
 * 两套 ABI 的差别不止参数寄存器：
 *   - MS 的被调用方可以随意破坏 xmm6..xmm15（SysV 里它们是易失的，
 *     所以 SysV 调用方不会把它们当作需要保留的值，无需额外保存）；
 *   - MS 把 rdi/rsi 视为非易失，被调用方会保留它们，正好方便本桥
 *     把函数指针留在 rdi 上。
 *
 * 因此跨 ABI 只能走这里的小 thunk：入参按 SysV 排列，thunk 重新
 * 摆放到 MS 寄存器并补上 shadow space，再间接调用函数指针。
 * ============================================================ */

    .text

/* ------------------------------------------------------------
 * 模块导出入口：内核按 MS ABI 把 SlysKernelApi* 放在 rcx。
 * 转成 SysV 的 rdi 后跳到 C 入口。
 * ------------------------------------------------------------ */
    .globl slyzposix_entry
    .type  slyzposix_entry,@function
slyzposix_entry:
    movq %rcx, %rdi
    jmp  slyz_entry_sysv
    .size slyzposix_entry, .-slyzposix_entry

/* 32 字节 shadow space + 8 字节对齐补偿：
 * SysV 函数入口 rsp % 16 == 8，减 0x28 后 rsp % 16 == 0，
 * 满足 MS 对 call 指令处 16 字节对齐的要求。
 * 注意：本文件是 .s（不经过 C 预处理器），只能用汇编器伪指令。 */
    .set SLYZ_SHADOW, 0x28

/* long __slyz_api_call0(const void *fn) */
    .globl __slyz_api_call0
    .type  __slyz_api_call0,@function
__slyz_api_call0:
    subq $SLYZ_SHADOW, %rsp
    call *%rdi
    addq $SLYZ_SHADOW, %rsp
    ret
    .size __slyz_api_call0, .-__slyz_api_call0

/* long __slyz_api_call1(const void *fn, long a1) */
    .globl __slyz_api_call1
    .type  __slyz_api_call1,@function
__slyz_api_call1:
    subq $SLYZ_SHADOW, %rsp
    movq %rsi, %rcx
    call *%rdi
    addq $SLYZ_SHADOW, %rsp
    ret
    .size __slyz_api_call1, .-__slyz_api_call1

/* long __slyz_api_call2(const void *fn, long a1, long a2) */
    .globl __slyz_api_call2
    .type  __slyz_api_call2,@function
__slyz_api_call2:
    subq $SLYZ_SHADOW, %rsp
    movq %rsi, %rcx
    /* a2 已在 rdx，MS 的第二个参数同样是 rdx */
    call *%rdi
    addq $SLYZ_SHADOW, %rsp
    ret
    .size __slyz_api_call2, .-__slyz_api_call2

/* long __slyz_api_call3(const void *fn, long a1, long a2, long a3) */
    .globl __slyz_api_call3
    .type  __slyz_api_call3,@function
__slyz_api_call3:
    subq $SLYZ_SHADOW, %rsp
    movq %rcx, %r8          /* a3 -> MS 第三个参数 */
    movq %rsi, %rcx         /* a1 -> MS 第一个参数 */
    call *%rdi
    addq $SLYZ_SHADOW, %rsp
    ret
    .size __slyz_api_call3, .-__slyz_api_call3

/* long __slyz_api_call4(const void *fn, long a1, long a2, long a3, long a4) */
    .globl __slyz_api_call4
    .type  __slyz_api_call4,@function
__slyz_api_call4:
    subq $SLYZ_SHADOW, %rsp
    movq %r8, %r9           /* a4 -> MS 第四个参数 */
    movq %rcx, %r8          /* a3 */
    movq %rsi, %rcx         /* a1 */
    call *%rdi
    addq $SLYZ_SHADOW, %rsp
    ret
    .size __slyz_api_call4, .-__slyz_api_call4

/* long __slyz_api_call5(const void *fn, long a1..a5)
 * MS 的第五个参数走栈：[rsp+0x20]（shadow space 之上）。 */
    .globl __slyz_api_call5
    .type  __slyz_api_call5,@function
__slyz_api_call5:
    subq $SLYZ_SHADOW, %rsp
    movq %r9, 0x20(%rsp)    /* a5 -> 栈参数 */
    movq %r8, %r9           /* a4 */
    movq %rcx, %r8          /* a3 */
    movq %rsi, %rcx         /* a1 */
    call *%rdi
    addq $SLYZ_SHADOW, %rsp
    ret
    .size __slyz_api_call5, .-__slyz_api_call5

/* long __slyz_api_call6(const void *fn, long a1..a6)
 * a6 在 SysV 入口位于 [rsp+8]，减掉 0x28 后是 [rsp+0x30]；
 * MS 的第六个参数位于 [rsp+0x28]。 */
    .globl __slyz_api_call6
    .type  __slyz_api_call6,@function
__slyz_api_call6:
    subq $SLYZ_SHADOW, %rsp
    movq 0x30(%rsp), %rax
    movq %rax, 0x28(%rsp)   /* a6 -> 栈参数 */
    movq %r9, 0x20(%rsp)    /* a5 -> 栈参数 */
    movq %r8, %r9           /* a4 */
    movq %rcx, %r8          /* a3 */
    movq %rsi, %rcx         /* a1 */
    call *%rdi
    addq $SLYZ_SHADOW, %rsp
    ret
    .size __slyz_api_call6, .-__slyz_api_call6

    .section .note.GNU-stack,"",@progbits
