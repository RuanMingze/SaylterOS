/* ============================================================
 * slyzvm.h - SLYZ v2 虚拟机接口
 * ------------------------------------------------------------
 * 同一个解释器同时服务两个宿主：
 *   - SaylterOS 内核：内存由任务配额提供，输出走屏幕/串口
 *   - Windows 端 slyzc run：内存由 malloc 提供，输出走 stdout
 * 差异全部收敛在 SlyzVmOps 回调里，指令语义只有一份实现，
 * 避免编译器自测通过而内核解释器行为不同。
 *
 * 执行模型：
 *   slyzvm_create 校验镜像并建立工作区；
 *   slyzvm_step   最多执行 budget 条指令后返回，返回 1 表示仍在运行；
 *                  GUI 应用每帧只给有限预算，避免独占 CPU；
 *   slyzvm_run    内部循环调用 step 直到结束，等价于 budget = 全速。
 *
 * 值所有权：
 *   值进入值栈、全局槽、数组元素时增加引用计数，离开时减少。
 *   宿主 sys 回调收到的参数在回调返回后由虚拟机释放；宿主若要把
 *   参数原样作为返回值，必须通过 slyz_ret_* 重新构造，不能直接
 *   把传入指针写回 ret。
 * ============================================================ */

#ifndef SLYZVM_H
#define SLYZVM_H

#include <stdint.h>

#include "slyz.h"

#ifdef SLYZ_HOST
#define SLYZ_INLINE static inline
#else
#define SLYZ_INLINE static inline
#endif

/* ---------- 运行时对象 ----------
 * kind: SLYZ_T_STR / SLYZ_T_ARR
 * flags: bit0 = 静态对象（镜像常量池副本），不参与释放
 * 数据区紧随头部，保证 8 字节对齐，数组元素可直接按 SlyzVal 访问。 */

struct SlyzObj {
    uint8_t  kind;
    uint8_t  flags;
    uint16_t refs;
    uint32_t len;        /* 字符串字节数 / 数组元素数 */
    uint32_t cap;        /* 数组容量 */
    uint32_t reserved0;
    void    *data;       /* 字符串：char*；数组：SlyzVal*，可扩容替换 */
};

#define SLYZ_OBJ_STATIC 0x01

struct SlyzVal {
    int32_t type;
    int32_t aux;
    union {
        int64_t i;
        struct SlyzObj *obj;
    } u;
};

/* ---------- 宿主回调 ---------- */

struct SlyzVm;

typedef struct SlyzVmOps {
    /* 执行一条系统调用。id 见 slyz.h 的 SLE_SYS_*；
     * args[0] 是源码里的第一个参数，返回 0 表示成功，非 0 终止程序。 */
    int  (*sys)(struct SlyzVm *vm, uint16_t id,
                struct SlyzVal *args, uint16_t argc,
                struct SlyzVal *ret);

    /* 工作区内存。内核传 task_alloc/task_free，宿主传 malloc/free。
     * 申请失败必须返回 0，虚拟机据此报告 OOM 而不是崩溃。 */
    void *(*alloc)(uint32_t bytes);
    void  (*free)(void *ptr);

    /* 每执行若干条指令调用一次，用于协作式调度与步数统计。可为 0。 */
    void (*tick)(void);

    /* 格式/运行错误文本。内核写串口，宿主写 stderr。可为 0。 */
    void (*log)(const char *text);
} SlyzVmOps;

/* ---------- 生命周期 ---------- */

int  slyzvm_is_binary(const uint8_t *data, int size);

struct SlyzVm *slyzvm_create(const uint8_t *image, int size,
                             const struct SlyzVmOps *ops, const char *name);

/* 返回 1 仍在运行，0 正常结束，<0 运行错误（SLYZ_ERR_*） */
int  slyzvm_step(struct SlyzVm *vm, uint32_t budget);

/* 跑到结束，返回 1 成功，0 失败 */
int  slyzvm_run(const uint8_t *image, int size,
                const struct SlyzVmOps *ops, const char *name);

void slyzvm_destroy(struct SlyzVm *vm);

int  slyzvm_state(const struct SlyzVm *vm);
int  slyzvm_exit_code(const struct SlyzVm *vm);
const char *slyzvm_error(const struct SlyzVm *vm);
uint32_t slyzvm_steps(const struct SlyzVm *vm);
int  slyzvm_uses_gui(const struct SlyzVm *vm);

/* 宿主主动结束程序（SLE_SYS_EXIT 使用） */
void slyzvm_request_exit(struct SlyzVm *vm, int code);

/* 宿主请求在执行完当前指令后让出控制权。
 * 图形宿主用它实现"绘制一帧就暂停"：gfx.flush 调用后 step 立即返回，
 * 下一帧从下一条指令继续，事件处理与重绘因此被切成固定节奏。 */
void slyzvm_request_yield(struct SlyzVm *vm);

/* ---------- 值访问 ---------- */

SLYZ_INLINE int slyz_val_is_nil(const struct SlyzVal *v) {
    return v->type == SLYZ_T_NIL;
}

SLYZ_INLINE int64_t slyz_val_int(const struct SlyzVal *v) {
    if (v->type == SLYZ_T_INT) return v->u.i;
    return 0;
}

/* 字符串内容；非字符串返回 0。*len 可为 0。 */
const char *slyz_val_str(const struct SlyzVal *v, int *len);

int  slyz_val_arr_len(const struct SlyzVal *v);
struct SlyzVal *slyz_val_arr_at(struct SlyzVal *v, int index);

/* ---------- 返回值构造 ---------- */

void slyz_ret_nil(struct SlyzVal *ret);
void slyz_ret_int(struct SlyzVal *ret, int64_t x);
int  slyz_ret_str(struct SlyzVm *vm, struct SlyzVal *ret, const char *s, int len);
int  slyz_ret_arr(struct SlyzVm *vm, struct SlyzVal *ret, int count);
int  slyz_arr_set(struct SlyzVm *vm, struct SlyzVal *arr, int index, struct SlyzVal value);

/* 把任意值渲染成可打印文本，返回写入长度（不含结尾 0） */
int  slyz_val_to_text(struct SlyzVm *vm, const struct SlyzVal *v, char *buf, int cap);

#endif /* SLYZVM_H */
