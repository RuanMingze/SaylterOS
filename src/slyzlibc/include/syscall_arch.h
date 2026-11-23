/* ============================================================
 * syscall_arch.h - SaylterOS 版 musl syscall 覆盖层
 * ------------------------------------------------------------
 * musl 的 src/internal/syscall.h 以 #include "syscall_arch.h" 引入
 * 架构层实现。构建时把本目录放在 -I 列表最前面，即可在完全不修
 * 改 third_party/musl 的前提下替换掉 x86_64 的内联 syscall 指令。
 *
 * SaylterOS 的 .slys 模块运行在 ring0、与内核共享同一地址空间，
 * 没有 int 0x80 / syscall 指令入口，因此这里把所有 __syscallN
 * 直接转发给运行期注册的 C 分发表 __slyz_syscall。
 *
 * 分发表返回 Linux 语义的负 errno（-1..-4095），由 musl 的
 * __syscall_ret 转成 -1 + errno。
 * ============================================================ */

#ifndef SLYZ_SYSCALL_ARCH_H
#define SLYZ_SYSCALL_ARCH_H

#define __SYSCALL_LL_E(x) (x)
#define __SYSCALL_LL_O(x) (x)

/* 实现在 port/syscall.c，由模块入口在首次调用前绑定内核 API。 */
long __slyz_syscall(long n, long a1, long a2, long a3, long a4, long a5, long a6);

static __inline long __syscall0(long n)
{
    return __slyz_syscall(n, 0, 0, 0, 0, 0, 0);
}

static __inline long __syscall1(long n, long a1)
{
    return __slyz_syscall(n, a1, 0, 0, 0, 0, 0);
}

static __inline long __syscall2(long n, long a1, long a2)
{
    return __slyz_syscall(n, a1, a2, 0, 0, 0, 0);
}

static __inline long __syscall3(long n, long a1, long a2, long a3)
{
    return __slyz_syscall(n, a1, a2, a3, 0, 0, 0);
}

static __inline long __syscall4(long n, long a1, long a2, long a3, long a4)
{
    return __slyz_syscall(n, a1, a2, a3, a4, 0, 0);
}

static __inline long __syscall5(long n, long a1, long a2, long a3, long a4, long a5)
{
    return __slyz_syscall(n, a1, a2, a3, a4, a5, 0);
}

static __inline long __syscall6(long n, long a1, long a2, long a3, long a4, long a5, long a6)
{
    return __slyz_syscall(n, a1, a2, a3, a4, a5, a6);
}

/* x86-64 的 Linux 系统调用最多 6 个参数。musl 的 __SYSCALL_NARGS 会
 * 把 7 参数调用派发到 __syscall7（历史架构的 socketcall 需要），这里
 * 提供同签名实现，丢弃第 7 个参数，避免归档里缺少该符号导致链接失败。 */
static __inline long __syscall7(long n, long a1, long a2, long a3,
                                long a4, long a5, long a6, long a7)
{
    (void)a7;
    return __slyz_syscall(n, a1, a2, a3, a4, a5, a6);
}

/* 不声明 VDSO_*：SaylterOS 没有 vdso，musl 会直接走 syscall 路径。 */

#define IPC_64 0

#endif /* SLYZ_SYSCALL_ARCH_H */
