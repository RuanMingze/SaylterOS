/* ============================================================
 * exit.c - 终结进程的底层实现（_Exit / _exit）
 * ------------------------------------------------------------
 * musl 源码里 src/exit/_Exit.c 与 src/unistd/_exit.c 只差大小写。
 * Windows 文件系统与 MSYS2 的 ar 都不区分大小写，归档时 _Exit.o
 * 会被 _exit.o 覆盖。musl 的 _exit 本身是"转发到 _Exit"，而 _Exit
 * 才是真正执行 SYS_exit_group 的那个；如果只补一个转发回 _exit 的
 * _Exit，两者就会互相调用形成无限递归（实测表现为串口不停打印
 * _Exit 直到栈溢出）。
 *
 * 因此这里直接把 _Exit 实现为内核调用，并同时提供 _exit，两个
 * 符号都不再依赖归档里被覆盖的那一份。
 * ============================================================ */

#include <stdint.h>
#include <unistd.h>
#include <sys/syscall.h>

#include "slyzposix.h"

extern long __slyz_syscall(long n, long a1, long a2, long a3,
                           long a4, long a5, long a6);

#ifdef SLYZ_TRACE
static void exit_trace(const char *s)
{
    const struct SlysKernelApi *a = __slyz_api();
    if (a && a->log) {
        __slyz_api_call1((const void *)a->log, (long)(intptr_t)s);
    }
}
#else
#define exit_trace(s) ((void)0)
#endif

/* SYS_exit_group 在 port/syscall.c 里会通过 longjmp 回到模块入口，
 * 因此这里不会返回；循环只是编译器与 CPU 的安全网。 */
static __attribute__((noreturn)) void slyz_terminate(int status)
{
    __slyz_syscall(SYS_exit_group, status, 0, 0, 0, 0, 0);
    for (;;) {
        __asm__ __volatile__("hlt");
    }
}

__attribute__((noreturn)) void _Exit(int status)
{
    exit_trace("[musl] _Exit\n");
    slyz_terminate(status);
}

__attribute__((noreturn)) void _exit(int status)
{
    slyz_terminate(status);
}
