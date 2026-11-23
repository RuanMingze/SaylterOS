/* ============================================================
 * crt.c - musl 在 SaylterOS 模块环境中的启动代码
 * ------------------------------------------------------------
 * .slys 模块没有 Linux 进程栈、没有 ELF 程序头、没有 _start。
 * 这里做四件事：
 *   1. 构造 __libc_start_main 期望的栈布局（argc/argv/envp/auxv）；
 *   2. 用链接脚本定义的 __tls_* 符号覆盖 musl 的 __init_tls，
 *      因为静态 TLS 初始化原本依赖 ELF PT_TLS 程序头，而模块是
 *      flat binary，加载后程序头已不存在；
 *   3. 用 setjmp/longjmp 把 exit/_Exit 变成"从入口返回"，让内核
 *      在模块调用结束后继续运行，而不是停机；
 *   4. 把内核 API 的 Win64 调用约定桥接到本模块的 SysV 约定。
 *
 * 导出入口是 port/msabi.s 里的 slyzposix_entry：内核按 Microsoft
 * x64 ABI 把 SlysKernelApi* 放在 rcx，汇编跳板把它搬到 rdi 后进入
 * 这里的 slyz_entry_sysv。
 * ============================================================ */

#include <stdint.h>
#include <stddef.h>
#include <setjmp.h>
#include <elf.h>

#include "slyzposix.h"

/* musl 内部头：构建时通过 -I third_party/musl/{src/internal,arch/x86_64} 提供 */
#include "libc.h"
#include "pthread_impl.h"

extern int main(int, char **, char **);

extern int __libc_start_main(int (*)(int, char **, char **), int, char **,
                             void (*)(), void (*)(), void (*)());

extern void *__copy_tls(unsigned char *);
extern int __init_tp(void *);

/* 链接脚本定义 */
extern char __tls_image[];
extern char __tls_tdata_end[];
extern char __tls_end[];

#define SLYZ_TLS_AREA_BYTES (64u * 1024u)
#define SLYZ_TLS_ALIGN      64u
#define SLYZ_MIN_TLS_ALIGN  16u

static struct tls_module main_tls;
static unsigned char tls_area[SLYZ_TLS_AREA_BYTES];

/* -DSLYZ_TRACE=1 时把启动阶段打到串口。内核 API 是 Win64 ABI，
 * 所以这里也必须走 msabi.s 的 thunk。 */
#ifdef SLYZ_TRACE
#define SLYZ_LOG(s) do { const struct SlysKernelApi *a_ = __slyz_api(); \
                         if (a_ && a_->log) { \
                             __slyz_api_call1((const void *)a_->log, \
                                              (long)(intptr_t)("[musl] " s "\n")); \
                         } } while (0)
#else
#define SLYZ_LOG(s) ((void)0)
#endif

/* 覆盖 musl 的 weak __init_tls：不用 AT_PHDR，直接用链接期符号。 */
void __init_tls(size_t *aux)
{
    (void)aux;

    main_tls.image = (void *)__tls_image;
    main_tls.len   = (size_t)(__tls_tdata_end - __tls_image);
    main_tls.size  = (size_t)(__tls_end - __tls_image);
    main_tls.align = SLYZ_TLS_ALIGN;

    libc.tls_cnt  = 1;
    libc.tls_head = &main_tls;

    main_tls.size += (-main_tls.size - (uintptr_t)main_tls.image) & (main_tls.align - 1u);
    main_tls.offset = main_tls.size;

    if (main_tls.align < SLYZ_MIN_TLS_ALIGN) main_tls.align = SLYZ_MIN_TLS_ALIGN;
    libc.tls_align = main_tls.align;
    libc.tls_size = (2u * sizeof(void *) + sizeof(struct pthread)
                     + main_tls.size + main_tls.align + SLYZ_MIN_TLS_ALIGN - 1u)
                    & ~(uintptr_t)(SLYZ_MIN_TLS_ALIGN - 1u);

    if (libc.tls_size > sizeof(tls_area)) __builtin_trap();
    if (__init_tp(__copy_tls(tls_area)) < 0) __builtin_trap();
    SLYZ_LOG("tls ready");
}

static jmp_buf exit_jmp;
static int exit_code;

/* 由 syscall.c 的 SYS_exit / SYS_exit_group 调用。 */
void __slyz_exit_to_host(int status)
{
    SLYZ_LOG("exit to host");
    exit_code = status;
    longjmp(exit_jmp, 1);
}

/* __libc_start_main 期望的连续内存：
 * [argc][argv0][NULL][NULL(envp)][auxv...] */
static uintptr_t bootstrap[32];
static char arg0[] = "app";

/* 真正的 SysV 入口；slyzposix_entry（msabi.s）负责 ABI 转换后跳到这里。 */
void *slyz_entry_sysv(const struct SlysKernelApi *api)
{
    if (!api || api->version < SLYZ_POSIX_ABI) return 0;

    __slyz_bind_api(api);
    SLYZ_LOG("entry bound");

    uintptr_t *sp = bootstrap;
    sp[0] = 1;
    sp[1] = (uintptr_t)arg0;
    sp[2] = 0;
    sp[3] = 0;

    uintptr_t *aux = sp + 4;
    *aux++ = AT_PAGESZ; *aux++ = 4096;
    *aux++ = AT_UID;    *aux++ = 0;
    *aux++ = AT_EUID;   *aux++ = 0;
    *aux++ = AT_GID;    *aux++ = 0;
    *aux++ = AT_EGID;   *aux++ = 0;
    *aux++ = AT_SECURE; *aux++ = 0;
    *aux++ = AT_NULL;   *aux++ = 0;

    if (setjmp(exit_jmp) == 0) {
        __libc_start_main(main, 1, (char **)&sp[1], 0, 0, 0);
    }
    return (void *)(intptr_t)exit_code;
}
