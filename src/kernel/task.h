/* ============================================================
 * task.h - 任务、内存配额与协作式调度
 * ------------------------------------------------------------
 * 任务不是完整的进程（没有独立地址空间），但具备：
 *   - 独立内核栈（静态池）
 *   - 独立内存配额与分配记账
 *   - 状态机 FREE / READY / RUNNING / DEAD
 *   - 可被 OOM 回收：配额超限时任务被标记 DEAD 并释放全部内存
 * 调度为协作式：任务在 VM 循环、shell 空闲处主动 task_yield()。
 * 真正的硬件分页与地址空间隔离尚未实现，见 README 的已知限制。
 * ============================================================ */

#ifndef TASK_H
#define TASK_H

#include <stdint.h>

#define TASK_MAX           4
#define TASK_STACK_SIZE    8192
#define TASK_QUOTA_DEFAULT (64u * 1024u)
#define TASK_BLOCKS        8
#define TASK_NAME_MAX      32
#define TASK_QUOTA_UNLIMITED 0xFFFFFFFFu

enum {
    TASK_FREE = 0,
    TASK_READY,
    TASK_RUNNING,
    TASK_DEAD
};

struct Task {
    int      id;
    int      state;
    char     name[TASK_NAME_MAX];
    uint32_t esp;
    void   (*entry)(void);

    /* 内存配额与记账 */
    uint32_t quota;
    uint32_t used;
    uint32_t peak;
    uint32_t allocs;
    uint32_t faults;

    /* 执行统计 */
    uint32_t steps;
    int      exit_code;

    /* 该任务名下的分配块，退出时统一回收 */
    void    *blocks[TASK_BLOCKS];
    uint32_t block_sizes[TASK_BLOCKS];
    int      block_count;
};

void task_init(void);

struct Task *task_current(void);
struct Task *task_by_id(int id);

/* 创建任务；成功返回任务号，失败返回 -1。 */
int task_spawn(const char *name, void (*entry)(void), uint32_t quota);

/* 主动让出 CPU；没有其他就绪任务时立即返回。 */
void task_yield(void);

/* 结束当前任务，不再返回。 */
void task_exit(int code);

/* 当前任务执行一步（VM 每 N 条指令调用一次），内部会周期性 yield。 */
void task_account_step(void);

/* 内存：配额检查 + 实际分配。失败（超配额或物理堆耗尽）返回 0。 */
void *task_alloc(uint32_t bytes);
void task_free(void *ptr);

/* 回收当前任务名下全部内存 */
void task_release_all(void);

/* OOM：打印并杀死当前任务，不返回。 */
void task_oom_kill(uint32_t requested);

/* 杀死指定任务（id 1..TASK_MAX-1）。内核任务 0 不可杀，返回 0。 */
int task_kill(int id);

/* 等待指定任务结束（协作式） */
void task_wait(int id);

int  task_count_alive(void);
uint32_t task_total_used(void);

/* ps 输出 */
void task_dump(void);

#endif /* TASK_H */
