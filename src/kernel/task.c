/* ============================================================
 * task.c - 任务、内存配额与协作式调度
 * ------------------------------------------------------------
 * 上下文切换只保存 callee-saved 寄存器（ebp/ebx/esi/edi）与 esp，
 * 因为调用约定保证其余寄存器由调用者保存。切换点固定在 task_yield /
 * task_exit 里，不会在中断处理中切栈，避免与 PIC/IDT 状态纠缠。
 * ============================================================ */

#include "task.h"
#include "mem.h"
#include "tty.h"

static struct Task g_tasks[TASK_MAX];
static uint8_t g_stacks[TASK_MAX][TASK_STACK_SIZE];
static int g_initialized = 0;
static int g_current = -1;
static uint32_t g_switch_count = 0;

/* ============================================================
 * 上下文切换（AT&T 语法，-fno-leading-underscore 下符号为 task_switch）
 * ============================================================ */
__asm__(
    ".globl task_switch\n"
    ".text\n"
    "task_switch:\n"
    "    pushl %ebp\n"
    "    pushl %ebx\n"
    "    pushl %esi\n"
    "    pushl %edi\n"
    "    movl 20(%esp), %eax\n"     /* old_esp */
    "    movl %esp, (%eax)\n"
    "    movl 24(%esp), %esp\n"     /* new_esp */
    "    popl %edi\n"
    "    popl %esi\n"
    "    popl %ebx\n"
    "    popl %ebp\n"
    "    ret\n"
);

extern void task_switch(uint32_t *old_esp, uint32_t new_esp);

/* ============================================================
 * 内部工具
 * ============================================================ */

static int task_pick_next(void) {
    if (g_current < 0) return -1;

    for (int step = 1; step <= TASK_MAX; step++) {
        int id = (g_current + step) % TASK_MAX;
        if (g_tasks[id].state == TASK_READY) return id;
    }
    return -1;
}

static void task_release_blocks(struct Task *t) {
    for (int i = 0; i < t->block_count; i++) {
        if (t->blocks[i]) {
            kfree(t->blocks[i]);
            t->blocks[i] = 0;
        }
        t->block_sizes[i] = 0;
    }
    t->block_count = 0;
    t->used = 0;
}

/* 任务第一次被调度时从这里进入 */
static void task_trampoline(void) {
    struct Task *t = task_current();
    if (t && t->entry) {
        t->entry();
    }
    task_exit(0);
}

void task_init(void) {
    if (g_initialized) return;

    for (int i = 0; i < TASK_MAX; i++) {
        g_tasks[i].id = i;
        g_tasks[i].state = TASK_FREE;
        g_tasks[i].esp = 0;
        g_tasks[i].entry = 0;
        g_tasks[i].quota = TASK_QUOTA_DEFAULT;
        g_tasks[i].used = 0;
        g_tasks[i].peak = 0;
        g_tasks[i].allocs = 0;
        g_tasks[i].faults = 0;
        g_tasks[i].steps = 0;
        g_tasks[i].exit_code = 0;
        g_tasks[i].block_count = 0;
        g_tasks[i].name[0] = '\0';
    }

    /* 任务 0 是内核/shell 任务，配额不设上限 */
    g_tasks[0].state = TASK_RUNNING;
    g_tasks[0].quota = TASK_QUOTA_UNLIMITED;
    const char *kernel_name = "kernel";
    int i = 0;
    while (kernel_name[i] && i < TASK_NAME_MAX - 1) {
        g_tasks[0].name[i] = kernel_name[i];
        i++;
    }
    g_tasks[0].name[i] = '\0';

    g_current = 0;
    g_initialized = 1;
}

struct Task *task_current(void) {
    if (!g_initialized || g_current < 0) return 0;
    return &g_tasks[g_current];
}

struct Task *task_by_id(int id) {
    if (id < 0 || id >= TASK_MAX) return 0;
    return &g_tasks[id];
}

int task_spawn(const char *name, void (*entry)(void), uint32_t quota) {
    if (!g_initialized) task_init();
    if (!name || !entry) return -1;

    int id = -1;
    for (int i = 1; i < TASK_MAX; i++) {
        if (g_tasks[i].state == TASK_FREE || g_tasks[i].state == TASK_DEAD) {
            id = i;
            break;
        }
    }
    if (id < 0) return -1;

    struct Task *t = &g_tasks[id];
    t->state = TASK_READY;
    t->quota = quota ? quota : TASK_QUOTA_DEFAULT;
    t->used = 0;
    t->peak = 0;
    t->allocs = 0;
    t->faults = 0;
    t->steps = 0;
    t->exit_code = 0;
    t->block_count = 0;
    t->entry = entry;

    int i = 0;
    while (name[i] && i < TASK_NAME_MAX - 1) {
        t->name[i] = name[i];
        i++;
    }
    t->name[i] = '\0';

    /* 构造初始栈帧：pop edi/esi/ebx/ebp 之后 ret 到 trampoline */
    uint32_t *top = (uint32_t *)(g_stacks[id] + TASK_STACK_SIZE);
    *(--top) = (uint32_t)(uintptr_t)task_trampoline;   /* ret 目标 */
    *(--top) = 0;                                       /* ebp */
    *(--top) = 0;                                       /* ebx */
    *(--top) = 0;                                       /* esi */
    *(--top) = 0;                                       /* edi */
    t->esp = (uint32_t)(uintptr_t)top;

    return id;
}

void task_yield(void) {
    if (!g_initialized) return;

    int next = task_pick_next();
    if (next < 0 || next == g_current) return;

    int prev = g_current;

    __asm__ volatile ("cli");
    g_current = next;
    g_tasks[prev].state = TASK_READY;
    g_tasks[next].state = TASK_RUNNING;
    g_switch_count++;

    task_switch(&g_tasks[prev].esp, g_tasks[next].esp);

    /* 切回本任务后恢复中断 */
    __asm__ volatile ("sti");
}

void task_exit(int code) {
    struct Task *t = task_current();
    if (!t) return;

    task_release_blocks(t);
    t->state = TASK_DEAD;
    t->exit_code = code;

    int next = task_pick_next();
    if (next < 0) {
        /* 没有其他就绪任务：回到内核任务 */
        next = 0;
        if (next == t->id) {
            for (;;) {
                __asm__ volatile ("cli; hlt");
            }
        }
        if (g_tasks[0].state == TASK_DEAD) {
            g_tasks[0].state = TASK_READY;
        }
    }

    __asm__ volatile ("cli");
    g_current = next;
    g_tasks[next].state = TASK_RUNNING;
    g_switch_count++;
    task_switch(&t->esp, g_tasks[next].esp);

    /* DEAD 任务不会再被调度，永远不会回到这里 */
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

void task_account_step(void) {
    struct Task *t = task_current();
    if (!t) return;

    t->steps++;
    if ((t->steps & 0x3FFu) == 0) {
        task_yield();
    }
}

void *task_alloc(uint32_t bytes) {
    struct Task *t = task_current();
    if (!t) {
        return kmalloc(bytes);
    }

    if (t->quota != TASK_QUOTA_UNLIMITED) {
        if (t->used + bytes > t->quota) {
            t->faults++;
            return 0;
        }
    }

    void *ptr = kmalloc(bytes);
    if (!ptr) return 0;

    if (t->block_count < TASK_BLOCKS) {
        t->blocks[t->block_count] = ptr;
        t->block_sizes[t->block_count] = bytes;
        t->block_count++;
    }

    t->used += bytes;
    t->allocs++;
    if (t->used > t->peak) t->peak = t->used;
    return ptr;
}

void task_free(void *ptr) {
    struct Task *t = task_current();
    if (!t || !ptr) return;

    for (int i = 0; i < t->block_count; i++) {
        if (t->blocks[i] == ptr) {
            uint32_t size = t->block_sizes[i];
            kfree(ptr);
            if (t->used >= size) t->used -= size;

            for (int j = i; j < t->block_count - 1; j++) {
                t->blocks[j] = t->blocks[j + 1];
                t->block_sizes[j] = t->block_sizes[j + 1];
            }
            t->block_count--;
            t->blocks[t->block_count] = 0;
            t->block_sizes[t->block_count] = 0;
            return;
        }
    }
}

void task_release_all(void) {
    struct Task *t = task_current();
    if (t) task_release_blocks(t);
}

void task_oom_kill(uint32_t requested) {
    struct Task *t = task_current();
    if (!t) return;

    printk("[OOM] task '%s': requested %u bytes, quota %u, used %u\n",
           t->name, (unsigned int)requested,
           (unsigned int)t->quota, (unsigned int)t->used);
    printk("[OOM] task '%s' killed, releasing %u bytes\n",
           t->name, (unsigned int)t->used);

    t->state = TASK_DEAD;
    t->exit_code = -2;
    task_release_blocks(t);

    int next = task_pick_next();
    if (next < 0 || next == t->id) {
        next = 0;
    }

    __asm__ volatile ("cli");
    g_current = next;
    g_tasks[next].state = TASK_RUNNING;
    task_switch(&t->esp, g_tasks[next].esp);

    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

void task_wait(int id) {
    struct Task *t = task_by_id(id);
    if (!t) return;

    int guard = 0;
    while (t->state != TASK_DEAD && t->state != TASK_FREE) {
        int alive = task_count_alive();
        if (alive <= 1) break;

        task_yield();
        if (++guard > 1000000) break;
    }
}

int task_count_alive(void) {
    int n = 0;
    for (int i = 0; i < TASK_MAX; i++) {
        if (g_tasks[i].state == TASK_READY || g_tasks[i].state == TASK_RUNNING) {
            n++;
        }
    }
    return n;
}

uint32_t task_total_used(void) {
    uint32_t total = 0;
    for (int i = 0; i < TASK_MAX; i++) {
        total += g_tasks[i].used;
    }
    return total;
}

void task_dump(void) {
    printk("ID  STATE    QUOTA   USED   PEAK   STEPS  NAME\n");
    static const char *state_name[] = { "free", "ready", "run", "dead" };

    for (int i = 0; i < TASK_MAX; i++) {
        struct Task *t = &g_tasks[i];
        if (t->state == TASK_FREE) continue;
        printk("%u   %s   %u  %u  %u  %u  %s\n",
               (unsigned int)t->id,
               state_name[t->state & 3],
               (unsigned int)t->quota,
               (unsigned int)t->used,
               (unsigned int)t->peak,
               (unsigned int)t->steps,
               t->name);
    }
}
