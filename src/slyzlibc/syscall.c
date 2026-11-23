/* ============================================================
 * syscall.c - musl 的 Linux syscall 编号 → SaylterOS 内核 API
 * ------------------------------------------------------------
 * musl 的全部系统调用最终都收敛到 __slyz_syscall。这里只翻译
 * 模块 ABI 能覆盖的调用；其余返回 -ENOSYS，让 musl 走降级路径
 * 或让调用者拿到明确的失败，而不是静默产生错误行为。
 *
 * 返回约定：>=0 成功；负数是 -errno。
 * ============================================================ */

#include <stdint.h>
#include <errno.h>
#include <limits.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include "slyzposix.h"

static const struct SlysKernelApi *g_api;

long __slyz_syscall(long n, long a1, long a2, long a3, long a4, long a5, long a6);

/* 内核是 Win64 ABI，本模块是 SysV ABI：所有 API 调用都必须经过
 * msabi.s 的 thunk，否则第一个参数会落在 rdi，而内核去 rcx 里取。
 * 这一层内联包装把跨 ABI 的细节收在一处。 */
static inline long api_write(int fd, const void *buf, int len)
{
    return __slyz_api_call3((const void *)g_api->posix_write, fd,
                            (long)(intptr_t)buf, len);
}

static inline long api_read(int fd, void *buf, int len)
{
    return __slyz_api_call3((const void *)g_api->posix_read, fd,
                            (long)(intptr_t)buf, len);
}

static inline long api_open(const char *path, int flags)
{
    return __slyz_api_call2((const void *)g_api->posix_open,
                            (long)(intptr_t)path, flags);
}

static inline long api_close(int fd)
{
    return __slyz_api_call1((const void *)g_api->posix_close, fd);
}

static inline long api_lseek(int fd, int64_t offset, int whence)
{
    return __slyz_api_call3((const void *)g_api->posix_lseek, fd,
                            (long)offset, whence);
}

static inline long api_fstat(int fd, void *st)
{
    return __slyz_api_call2((const void *)g_api->posix_fstat, fd,
                            (long)(intptr_t)st);
}

static inline long api_map(uint64_t len)
{
    return __slyz_api_call1((const void *)g_api->posix_map, (long)len);
}

static inline long api_unmap(void *ptr, uint64_t len)
{
    return __slyz_api_call2((const void *)g_api->posix_unmap,
                            (long)(intptr_t)ptr, (long)len);
}

static inline uint64_t api_time_ms(void)
{
    return (uint64_t)__slyz_api_call0((const void *)g_api->posix_time_ms);
}

static inline void api_log(const char *s)
{
    __slyz_api_call1((const void *)g_api->log, (long)(intptr_t)s);
}

/* 调试期追踪：把系统调用号打到串口，定位模块崩溃前最后一条请求。
 * 默认关闭，-DSLYZ_TRACE=1 时打开。 */
#ifdef SLYZ_TRACE
static void slyz_trace(long n)
{
    if (!g_api) return;
    api_log("[musl] sys ");
    __slyz_api_call1((const void *)g_api->log_u32, (long)(uint32_t)n);
    api_log("\n");
}
#else
#define slyz_trace(n) ((void)0)
#endif

void __slyz_bind_api(const struct SlysKernelApi *api)
{
    g_api = api;
}

const struct SlysKernelApi *__slyz_api(void)
{
    return g_api;
}

/* 由 crt 提供：把退出码交回宿主入口，而不是冻结 CPU。 */
extern void __slyz_exit_to_host(int status) __attribute__((noreturn));

static inline void wrmsr(uint32_t msr, uint64_t value)
{
    __asm__ __volatile__("wrmsr" : : "c"(msr), "a"((uint32_t)value),
                         "d"((uint32_t)(value >> 32)) : "memory");
}

static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr) : "memory");
    return ((uint64_t)hi << 32) | lo;
}

#define SLYZ_MSR_FS_BASE 0xC0000100u

/* musl 的 x86_64 版 __set_thread_area 用裸 syscall 指令执行 arch_prctl。
 * SaylterOS 内核没有配置 STAR/LSTAR/SFMASK，ring0 里执行 syscall 会直接
 * 触发 #GP。模块本来就运行在 ring0，这里用 wrmsr 写 IA32_FS_BASE 完成
 * 同样的语义，并且完全不依赖内核的系统调用入口。 */
int __set_thread_area(void *p)
{
    wrmsr(SLYZ_MSR_FS_BASE, (uint64_t)(uintptr_t)p);
    return 0;
}

/* musl 的取消点版本 __syscall_cp 最终会走 __syscall_cp_asm（同样是裸
 * syscall 指令，还要读写 pthread 的 cancel 标志）。T0 阶段只有单线程，
 * 取消语义等价于"直接执行系统调用"，这里做强定义把它短路掉。 */
long __syscall_cp(long n, long a1, long a2, long a3,
                  long a4, long a5, long a6)
{
    return __slyz_syscall(n, a1, a2, a3, a4, a5, a6);
}

/* musl 的 O_* 与内核 mode 字符串的映射。 */
static int open_flags_to_mode(int flags)
{
    int acc = flags & O_ACCMODE;
    int plus = (flags & O_CREAT) ? 1 : 0;
    if (acc == O_RDONLY) return 0;
    if (acc == O_WRONLY) return 1;
    if (plus) return 2;
    return 2;
}

long __slyz_syscall(long n, long a1, long a2, long a3,
                    long a4, long a5, long a6)
{
    (void)a4; (void)a5; (void)a6;

    if (!g_api) return -ENOSYS;
    slyz_trace(n);

    switch (n) {
    case SYS_read:
        return api_read((int)a1, (void *)a2, (int)a3);

    case SYS_write:
        return api_write((int)a1, (const void *)a2, (int)a3);

    /* musl 的 stdio 不用 write，而是把缓冲区拼成 iovec 交给 writev。
     * 不支持 writev 时 printf 会整段失败并置 F_ERR，所以必须在这里
     * 逐段展开成内核的 posix_write。 */
    case SYS_writev: {
        const struct iovec *iov = (const struct iovec *)a2;
        long cnt = a3;
        long total = 0;
        if (!iov || cnt < 0 || cnt > 1024) return -EINVAL;
        for (long i = 0; i < cnt; i++) {
            size_t remain = iov[i].iov_len;
            const uint8_t *base = (const uint8_t *)iov[i].iov_base;
            while (remain > 0) {
                int chunk = remain > (size_t)INT_MAX ? INT_MAX : (int)remain;
                int n = api_write((int)a1, base, chunk);
                if (n < 0) return total > 0 ? total : n;
                total += n;
                if (n == 0) return total;
                base += n;
                remain -= (size_t)n;
            }
        }
        return total;
    }

    case SYS_readv: {
        const struct iovec *iov = (const struct iovec *)a2;
        long cnt = a3;
        long total = 0;
        if (!iov || cnt < 0 || cnt > 1024) return -EINVAL;
        for (long i = 0; i < cnt; i++) {
            size_t remain = iov[i].iov_len;
            uint8_t *base = (uint8_t *)iov[i].iov_base;
            while (remain > 0) {
                int chunk = remain > (size_t)INT_MAX ? INT_MAX : (int)remain;
                int n = api_read((int)a1, base, chunk);
                if (n < 0) return total > 0 ? total : n;
                total += n;
                if (n == 0) return total;
                base += n;
                remain -= (size_t)n;
            }
        }
        return total;
    }

    case SYS_openat:
        return api_open((const char *)a2, open_flags_to_mode((int)a3));

    case SYS_open:
        return api_open((const char *)a1, open_flags_to_mode((int)a2));

    case SYS_close:
        /* 0/1/2 由内核的 tty 直接持有，fclose(stdout) 之类的调用不应该失败。 */
        if ((int)a1 >= 0 && (int)a1 <= 2) return 0;
        return api_close((int)a1);

    case SYS_lseek:
        return api_lseek((int)a1, (int64_t)a2, (int)a3);

    case SYS_fstat:
        return api_fstat((int)a1, (void *)a2);

    case SYS_newfstatat:
        return -ENOENT;

    case SYS_mmap: {
        /* 0 是合法地址，不能拿来当失败值：musl 会把 0 当成成功映射，
         * 随后对空指针做 memset，直接把内核低地址写坏。 */
        void *p = (void *)(intptr_t)api_map((uint64_t)a2);
        return p ? (long)(intptr_t)p : -ENOMEM;
    }

    case SYS_munmap:
        api_unmap((void *)a1, (uint64_t)a2);
        return 0;

    case SYS_mprotect:
        return 0;

    case SYS_brk:
        /* musl 的 malloc 只走 mmap；brk 仅作为兼容桩存在。 */
        return 0;

    case SYS_arch_prctl: {
        int code = (int)a1;
        if (code == 0x1002) {          /* ARCH_SET_FS */
            wrmsr(SLYZ_MSR_FS_BASE, (uint64_t)a2);
            return 0;
        }
        if (code == 0x1003) {          /* ARCH_GET_FS */
            *(uint64_t *)a2 = rdmsr(SLYZ_MSR_FS_BASE);
            return 0;
        }
        return -EINVAL;
    }

    case SYS_exit:
    case SYS_exit_group:
        __slyz_exit_to_host((int)a1);

    case SYS_clock_gettime: {
        struct timespec *ts = (struct timespec *)a2;
        uint64_t ms = api_time_ms();
        if (ts) {
            ts->tv_sec = (long)(ms / 1000u);
            ts->tv_nsec = (long)((ms % 1000u) * 1000000u);
        }
        return 0;
    }

    case SYS_gettimeofday: {
        struct timeval *tv = (struct timeval *)a1;
        uint64_t ms = api_time_ms();
        if (tv) {
            tv->tv_sec = (long)(ms / 1000u);
            tv->tv_usec = (long)((ms % 1000u) * 1000u);
        }
        return 0;
    }

    case SYS_time: {
        uint64_t ms = api_time_ms();
        long sec = (long)(ms / 1000u);
        if ((long *)a1) *(long *)a1 = sec;
        return sec;
    }

    case SYS_getrandom:
        return -ENOSYS;

    case SYS_futex:
        /* 单线程：没有竞争，等待立即成功。 */
        return 0;

    case SYS_set_tid_address:
        return 1;

    case SYS_set_robust_list:
        return 0;

    case SYS_rseq:
        return -ENOSYS;

    case SYS_rt_sigaction:
    case SYS_rt_sigprocmask:
        return 0;

    case SYS_getpid:
    case SYS_getppid:
        return 1;

    case SYS_getuid:
    case SYS_geteuid:
    case SYS_getgid:
    case SYS_getegid:
        return 0;

    case SYS_ioctl:
        return -ENOTTY;

    case SYS_getcwd:
        if (a1 && a2) {
            const char *cwd = "/";
            unsigned long len = 0;
            while (cwd[len]) len++;
            if (len + 1 > (unsigned long)a2) return -ERANGE;
            for (unsigned long i = 0; i <= len; i++) ((char *)a1)[i] = cwd[i];
            return (long)len + 1;
        }
        return -EINVAL;

    default:
        return -ENOSYS;
    }
}
