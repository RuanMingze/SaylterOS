/* ============================================================
 * slyzvm.c - SLYZ v2 字节码解释器
 * ------------------------------------------------------------
 * 执行核心与宿主完全解耦：本文件不包含任何 tty/stdio 头，
 * 只通过 SlyzVmOps 申请内存、输出错误与执行系统调用。因此
 * 同一份实现同时用于内核与 Windows 端 slyzc run，行为一致。
 *
 * 内存模型：
 *   create 时一次性申请工作区，切成值栈、全局槽与堆三段。
 *   栈与全局槽是固定容量的值数组；堆是按 16 字节块头管理的
 *   first-fit 分配器，负责字符串与数组对象及其数据缓冲。
 *
 * 引用计数：
 *   值进入栈、全局槽、数组元素时 retain，离开时 release。
 *   PUSH_S 每次生成独立的字符串对象，宁可多一次复制，也不
 *   让字面量指向镜像常量池——否则镜像释放后悬空指针极难排查。
 * ============================================================ */

#include "slyzvm.h"

/* ---------- 本地内存函数（内核没有 libc） ---------- */

static void *vm_memcpy(void *dst, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

static void *vm_memset(void *dst, int c, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (uint32_t i = 0; i < n; i++) d[i] = (uint8_t)c;
    return dst;
}

static int vm_memcmp(const void *a, const void *b, uint32_t n) {
    const uint8_t *x = (const uint8_t *)a;
    const uint8_t *y = (const uint8_t *)b;
    for (uint32_t i = 0; i < n; i++) {
        if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    }
    return 0;
}

static int vm_strlen(const char *s) {
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int64_t rd64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (i * 8);
    return (int64_t)v;
}

static uint32_t fnv1a(const uint8_t *p, uint32_t n) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++) {
        h ^= (uint32_t)p[i];
        h *= 16777619u;
    }
    return h;
}

/* ---------- 堆 ---------- */

struct HeapBlock {
    uint32_t size;      /* 数据区字节数，按 8 对齐 */
    uint32_t free;      /* 1 = 可分配 */
    uint32_t pad0;
    uint32_t pad1;
};

#define HEAP_HDR   ((uint32_t)sizeof(struct HeapBlock))
#define HEAP_ALIGN 8u

static uint32_t align8(uint32_t n) {
    return (n + (HEAP_ALIGN - 1)) & ~(HEAP_ALIGN - 1);
}

static struct HeapBlock *heap_next(struct HeapBlock *b) {
    return (struct HeapBlock *)((uint8_t *)b + HEAP_HDR + b->size);
}

/* ---------- 虚拟机状态 ---------- */

struct SlyzFrame {
    uint32_t base;
    uint32_t ret_pc;
};

struct SlyzVm {
    const uint8_t *image;
    const uint8_t *pool;
    const uint8_t *code;
    const char    *name;

    uint32_t const_size;
    uint32_t code_size;
    uint32_t entry;
    uint32_t global_count;
    uint32_t pc;

    const struct SlyzVmOps *ops;

    uint8_t *workspace;
    uint32_t workspace_size;

    struct SlyzVal *stack;
    uint32_t stack_cap;
    uint32_t sp;

    struct SlyzVal *globals;

    struct SlyzFrame frames[SLYZ_MAX_FRAMES];
    uint32_t depth;

    uint8_t *heap;
    uint32_t heap_size;
    struct HeapBlock *heap_head;

    uint32_t steps;
    int      state;        /* 0 运行/未开始, 1 结束, <0 错误 */
    int      exit_code;
    const char *error;

    uint32_t rng;
    int      uses_gui;
    int      yield_request;
};

/* ---------- 诊断 ---------- */

static void vm_log(struct SlyzVm *vm, const char *text) {
    if (vm && vm->ops && vm->ops->log && text) vm->ops->log(text);
}

static void fail_msg(struct SlyzVm *vm, int code, const char *msg) {
    vm->state = code;
    vm->error = msg;
    vm_log(vm, msg);
}

/* ---------- 堆实现 ---------- */

static int heap_init(struct SlyzVm *vm, uint8_t *base, uint32_t size) {
    if (size < HEAP_HDR + 1024) return 0;
    vm->heap = base;
    vm->heap_size = size;
    vm->heap_head = (struct HeapBlock *)base;
    vm->heap_head->size = size - HEAP_HDR;
    vm->heap_head->free = 1;
    vm->heap_head->pad0 = 0;
    vm->heap_head->pad1 = 0;
    return 1;
}

static void *heap_alloc(struct SlyzVm *vm, uint32_t bytes) {
    if (bytes == 0) bytes = 1;
    uint32_t need = align8(bytes);

    struct HeapBlock *b = vm->heap_head;
    while (b) {
        if (b->free && b->size >= need) {
            /* 足够大就切分，剩余部分留在空闲链里 */
            if (b->size >= need + HEAP_HDR + 64) {
                struct HeapBlock *rest = (struct HeapBlock *)((uint8_t *)b + HEAP_HDR + need);
                rest->size = b->size - need - HEAP_HDR;
                rest->free = 1;
                rest->pad0 = 0;
                rest->pad1 = 0;
                b->size = need;
            }
            b->free = 0;
            return (uint8_t *)b + HEAP_HDR;
        }
        b = heap_next(b);
    }
    return 0;
}

static void heap_free(struct SlyzVm *vm, void *ptr) {
    if (!ptr) return;
    struct HeapBlock *b = (struct HeapBlock *)((uint8_t *)ptr - HEAP_HDR);
    b->free = 1;

    /* 向后合并 */
    for (;;) {
        struct HeapBlock *nx = heap_next(b);
        if ((uint8_t *)nx >= vm->heap + vm->heap_size) break;
        if (!nx->free) break;
        b->size += HEAP_HDR + nx->size;
    }

    /* 向前合并：找到紧邻的空闲前块 */
    struct HeapBlock *p = vm->heap_head;
    uint8_t *heap_end = vm->heap + vm->heap_size;
    while (p && (uint8_t *)p < heap_end && p != b) {
        struct HeapBlock *nx = heap_next(p);
        if (nx == b) {
            if (p->free) {
                p->size += HEAP_HDR + b->size;
                /* 合并后仍需向后吸收 */
                for (;;) {
                    struct HeapBlock *nn = heap_next(p);
                    if ((uint8_t *)nn >= vm->heap + vm->heap_size) break;
                    if (!nn->free) break;
                    p->size += HEAP_HDR + nn->size;
                }
            }
            break;
        }
        p = nx;
    }
}

/* ---------- 对象 ---------- */

static struct SlyzObj *obj_alloc(struct SlyzVm *vm, int kind, uint32_t len, uint32_t cap) {
    struct SlyzObj *o = (struct SlyzObj *)heap_alloc(vm, (uint32_t)sizeof(struct SlyzObj));
    if (!o) return 0;

    o->kind = (uint8_t)kind;
    o->flags = 0;
    o->refs = 1;          /* 创建者取得第一个强引用 */
    o->len = len;
    o->cap = cap;
    o->reserved0 = 0;
    o->data = 0;

    if (kind == SLYZ_T_STR) {
        char *buf = (char *)heap_alloc(vm, len + 1);
        if (!buf) {
            heap_free(vm, o);
            return 0;
        }
        buf[len] = 0;
        o->data = buf;
    } else {
        uint32_t bytes = cap * (uint32_t)sizeof(struct SlyzVal);
        if (bytes == 0) bytes = (uint32_t)sizeof(struct SlyzVal);
        struct SlyzVal *items = (struct SlyzVal *)heap_alloc(vm, bytes);
        if (!items) {
            heap_free(vm, o);
            return 0;
        }
        vm_memset(items, 0, bytes);
        o->data = items;
    }

    return o;
}

static void obj_destroy(struct SlyzVm *vm, struct SlyzObj *o) {
    if (!o) return;

    if (o->kind == SLYZ_T_ARR && o->data) {
        struct SlyzVal *items = (struct SlyzVal *)o->data;
        for (uint32_t i = 0; i < o->len; i++) {
            struct SlyzVal v = items[i];
            if ((v.type == SLYZ_T_STR || v.type == SLYZ_T_ARR) && v.u.obj &&
                !(v.u.obj->flags & SLYZ_OBJ_STATIC)) {
                if (v.u.obj->refs > 0) v.u.obj->refs--;
                if (v.u.obj->refs == 0) obj_destroy(vm, v.u.obj);
            }
        }
    }

    if (o->data) heap_free(vm, o->data);
    heap_free(vm, o);
}

static void val_retain(struct SlyzVal v) {
    if ((v.type == SLYZ_T_STR || v.type == SLYZ_T_ARR) && v.u.obj) {
        if (!(v.u.obj->flags & SLYZ_OBJ_STATIC)) {
            if (v.u.obj->refs < 0xFFFF) v.u.obj->refs++;
        }
    }
}

static void val_release(struct SlyzVm *vm, struct SlyzVal v) {
    if ((v.type == SLYZ_T_STR || v.type == SLYZ_T_ARR) && v.u.obj) {
        if (!(v.u.obj->flags & SLYZ_OBJ_STATIC)) {
            if (v.u.obj->refs > 0) v.u.obj->refs--;
            if (v.u.obj->refs == 0) obj_destroy(vm, v.u.obj);
        }
    }
}

static struct SlyzVal val_nil(void) {
    struct SlyzVal v;
    v.type = SLYZ_T_NIL;
    v.aux = 0;
    v.u.i = 0;
    return v;
}

static struct SlyzVal val_int(int64_t x) {
    struct SlyzVal v;
    v.type = SLYZ_T_INT;
    v.aux = 0;
    v.u.i = x;
    return v;
}

static struct SlyzVal val_new_str(struct SlyzVm *vm, const char *s, uint32_t len) {
    struct SlyzObj *o = obj_alloc(vm, SLYZ_T_STR, len, len);
    if (!o) return val_nil();
    if (len > 0 && s) vm_memcpy(o->data, s, len);
    ((char *)o->data)[len] = 0;

    struct SlyzVal v;
    v.type = SLYZ_T_STR;
    v.aux = 0;
    v.u.obj = o;
    return v;
}

static struct SlyzVal val_new_arr(struct SlyzVm *vm, uint32_t count) {
    uint32_t cap = count ? count : 1;
    struct SlyzObj *o = obj_alloc(vm, SLYZ_T_ARR, count, cap);
    if (!o) return val_nil();

    struct SlyzVal v;
    v.type = SLYZ_T_ARR;
    v.aux = 0;
    v.u.obj = o;
    return v;
}

/* 字符串访问：非字符串返回 0 */
const char *slyz_val_str(const struct SlyzVal *v, int *len) {
    if (!v || v->type != SLYZ_T_STR || !v->u.obj) {
        if (len) *len = 0;
        return 0;
    }
    if (len) *len = (int)v->u.obj->len;
    return (const char *)v->u.obj->data;
}

int slyz_val_arr_len(const struct SlyzVal *v) {
    if (!v || v->type != SLYZ_T_ARR || !v->u.obj) return 0;
    return (int)v->u.obj->len;
}

struct SlyzVal *slyz_val_arr_at(struct SlyzVal *v, int index) {
    if (!v || v->type != SLYZ_T_ARR || !v->u.obj) return 0;
    if (index < 0 || (uint32_t)index >= v->u.obj->len) return 0;
    return &((struct SlyzVal *)v->u.obj->data)[index];
}

void slyz_ret_nil(struct SlyzVal *ret) {
    if (ret) *ret = val_nil();
}

void slyz_ret_int(struct SlyzVal *ret, int64_t x) {
    if (ret) *ret = val_int(x);
}

int slyz_ret_str(struct SlyzVm *vm, struct SlyzVal *ret, const char *s, int len) {
    if (!ret) return 0;
    if (len < 0) len = vm_strlen(s);
    struct SlyzVal v = val_new_str(vm, s, (uint32_t)len);
    if (v.type != SLYZ_T_STR) return 0;
    *ret = v;
    return 1;
}

int slyz_ret_arr(struct SlyzVm *vm, struct SlyzVal *ret, int count) {
    if (!ret || count < 0) return 0;
    struct SlyzVal v = val_new_arr(vm, (uint32_t)count);
    if (v.type != SLYZ_T_ARR) return 0;
    *ret = v;
    return 1;
}

int slyz_arr_set(struct SlyzVm *vm, struct SlyzVal *arr, int index, struct SlyzVal value) {
    (void)vm;
    if (!arr || arr->type != SLYZ_T_ARR || !arr->u.obj) return 0;
    if (index < 0 || (uint32_t)index >= arr->u.obj->cap) return 0;

    struct SlyzVal *items = (struct SlyzVal *)arr->u.obj->data;
    val_retain(value);
    items[index] = value;
    if ((uint32_t)index >= arr->u.obj->len) arr->u.obj->len = (uint32_t)index + 1;
    return 1;
}

/* ---------- 数组扩容 ---------- */

static int arr_reserve(struct SlyzVm *vm, struct SlyzVal arr, uint32_t need) {
    if (arr.type != SLYZ_T_ARR || !arr.u.obj) return 0;
    struct SlyzObj *o = arr.u.obj;

    if (need <= o->cap) return 1;

    uint32_t newcap = o->cap ? o->cap * 2 : 4;
    while (newcap < need) newcap *= 2;
    if (newcap > 4096) newcap = 4096;
    if (newcap < need) return 0;

    struct SlyzVal *items = (struct SlyzVal *)heap_alloc(
        vm, newcap * (uint32_t)sizeof(struct SlyzVal));
    if (!items) return 0;

    vm_memset(items, 0, newcap * (uint32_t)sizeof(struct SlyzVal));
    if (o->data && o->len > 0) {
        vm_memcpy(items, o->data, o->len * (uint32_t)sizeof(struct SlyzVal));
    }
    if (o->data) heap_free(vm, o->data);
    o->data = items;
    o->cap = newcap;
    return 1;
}

/* ---------- 文本渲染 ---------- */

static int render_value(struct SlyzVm *vm, const struct SlyzVal *v,
                        char *buf, int cap, int depth) {
    if (cap <= 0) return 0;

    if (!v || v->type == SLYZ_T_NIL) {
        if (cap < 4) return 0;
        vm_memcpy(buf, "nil", 3);
        buf[3] = 0;
        return 3;
    }

    if (v->type == SLYZ_T_INT) {
        int64_t x = v->u.i;
        char tmp[24];
        int n = 0;
        int neg = 0;
        uint64_t u;

        if (x < 0) {
            neg = 1;
            u = (uint64_t)(-(x + 1)) + 1u;
        } else {
            u = (uint64_t)x;
        }

        if (u == 0) tmp[n++] = '0';
        while (u > 0) {
            tmp[n++] = (char)('0' + (int)(u % 10u));
            u /= 10u;
        }

        int o = 0;
        if (neg && o < cap - 1) buf[o++] = '-';
        while (n > 0 && o < cap - 1) buf[o++] = tmp[--n];
        buf[o] = 0;
        return o;
    }

    if (v->type == SLYZ_T_STR) {
        int len = 0;
        const char *s = slyz_val_str(v, &len);
        if (!s) s = "";
        int n = len < cap - 1 ? len : cap - 1;
        vm_memcpy(buf, s, (uint32_t)n);
        buf[n] = 0;
        return n;
    }

    if (v->type == SLYZ_T_ARR) {
        if (depth >= 3) {
            if (cap < 9) return 0;
            vm_memcpy(buf, "[...]", 5);
            buf[5] = 0;
            return 5;
        }

        int o = 0;
        if (o < cap - 1) buf[o++] = '[';

        int n = slyz_val_arr_len(v);
        for (int i = 0; i < n; i++) {
            struct SlyzVal copy = *v;
            struct SlyzVal *item = slyz_val_arr_at(&copy, i);
            char tmp[64];
            int k = render_value(vm, item, tmp, (int)sizeof(tmp), depth + 1);
            if (i > 0 && o < cap - 1) buf[o++] = ',';
            if (i > 0 && o < cap - 1) buf[o++] = ' ';
            for (int j = 0; j < k && o < cap - 1; j++) buf[o++] = tmp[j];
            if (o >= cap - 1) break;
        }

        if (o < cap - 1) buf[o++] = ']';
        buf[o] = 0;
        return o;
    }

    buf[0] = 0;
    return 0;
}

int slyz_val_to_text(struct SlyzVm *vm, const struct SlyzVal *v, char *buf, int cap) {
    return render_value(vm, v, buf, cap, 0);
}

/* ---------- 真值与数值转换 ---------- */

static int val_truthy(const struct SlyzVal *v) {
    if (!v) return 0;
    if (v->type == SLYZ_T_NIL) return 0;
    if (v->type == SLYZ_T_INT) return v->u.i != 0;
    if (v->type == SLYZ_T_STR) return v->u.obj && v->u.obj->len > 0;
    if (v->type == SLYZ_T_ARR) return v->u.obj && v->u.obj->len > 0;
    return 0;
}

static int64_t val_number(const struct SlyzVal *v) {
    if (!v) return 0;
    if (v->type == SLYZ_T_INT) return v->u.i;

    if (v->type == SLYZ_T_STR && v->u.obj) {
        const char *s = (const char *)v->u.obj->data;
        int n = (int)v->u.obj->len;
        int i = 0;
        int neg = 0;
        int64_t out = 0;

        while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
        if (i < n && (s[i] == '-' || s[i] == '+')) {
            neg = (s[i] == '-');
            i++;
        }
        int digits = 0;
        while (i < n && s[i] >= '0' && s[i] <= '9') {
            out = out * 10 + (s[i] - '0');
            digits++;
            i++;
        }
        if (!digits) return 0;
        return neg ? -out : out;
    }

    if (v->type == SLYZ_T_ARR && v->u.obj) return (int64_t)v->u.obj->len;
    return 0;
}

static int val_equal(const struct SlyzVal *a, const struct SlyzVal *b) {
    if (a->type != b->type) {
        if ((a->type == SLYZ_T_INT || a->type == SLYZ_T_STR || a->type == SLYZ_T_ARR) &&
            (b->type == SLYZ_T_INT || b->type == SLYZ_T_STR || b->type == SLYZ_T_ARR)) {
            return val_number(a) == val_number(b);
        }
        return a->type == SLYZ_T_NIL && b->type == SLYZ_T_NIL;
    }

    if (a->type == SLYZ_T_NIL) return 1;
    if (a->type == SLYZ_T_INT) return a->u.i == b->u.i;
    if (a->type == SLYZ_T_STR) {
        if (a->u.obj == b->u.obj) return 1;
        int la = (int)a->u.obj->len;
        int lb = (int)b->u.obj->len;
        if (la != lb) return 0;
        return vm_memcmp(a->u.obj->data, b->u.obj->data, (uint32_t)la) == 0;
    }
    if (a->type == SLYZ_T_ARR) return a->u.obj == b->u.obj;

    return 0;
}

static int val_compare(const struct SlyzVal *a, const struct SlyzVal *b) {
    if (a->type == SLYZ_T_STR && b->type == SLYZ_T_STR) {
        int la = (int)a->u.obj->len;
        int lb = (int)b->u.obj->len;
        int n = la < lb ? la : lb;
        int c = n > 0 ? vm_memcmp(a->u.obj->data, b->u.obj->data, (uint32_t)n) : 0;
        if (c != 0) return c;
        return la - lb;
    }

    int64_t x = val_number(a);
    int64_t y = val_number(b);
    if (x < y) return -1;
    if (x > y) return 1;
    return 0;
}

/* ---------- 栈 ---------- */

static int vm_push(struct SlyzVm *vm, struct SlyzVal v) {
    if (vm->sp >= vm->stack_cap) {
        fail_msg(vm, SLYZ_ERR_STACK, "value stack overflow");
        return 0;
    }
    /* 入栈不改变引用计数：调用者要么转移所有权（新建对象、
     * 弹栈结果），要么在入栈前自行 retain（读取既有槽位）。 */
    vm->stack[vm->sp++] = v;
    return 1;
}

static struct SlyzVal vm_pop(struct SlyzVm *vm) {
    vm->sp--;
    return vm->stack[vm->sp];
}

/* ---------- 池字符串 ---------- */

static const char *pool_str(struct SlyzVm *vm, uint32_t off, uint32_t *out_len) {
    if (off >= vm->const_size) return 0;

    const char *s = (const char *)vm->pool + off;
    uint32_t left = vm->const_size - off;
    uint32_t n = 0;
    while (n < left && s[n]) n++;
    if (n >= left) return 0;

    if (out_len) *out_len = n;
    return s;
}

/* ---------- 创建 ---------- */

int slyzvm_is_binary(const uint8_t *data, int size) {
    if (!data || size < SLYZ_HEADER_SIZE) return 0;
    return data[0] == SLYZ_MAGIC0 && data[1] == SLYZ_MAGIC1 &&
           data[2] == SLYZ_MAGIC2 && data[3] == SLYZ_MAGIC3 &&
           data[4] == SLYZ_MAGIC4 && data[5] == SLYZ_MAGIC5 &&
           data[6] == SLYZ_MAGIC6 && data[7] == SLYZ_MAGIC7;
}

struct SlyzVm *slyzvm_create(const uint8_t *image, int size,
                             const struct SlyzVmOps *ops, const char *name) {
    if (!image || !ops || !ops->alloc || size < SLYZ_HEADER_SIZE) return 0;
    if (!slyzvm_is_binary(image, size)) return 0;

    uint16_t version = rd16(image + SLYZ_HDR_VERSION);
    if (version != SLYZ_VERSION) return 0;

    uint32_t const_size = rd32(image + SLYZ_HDR_CONSTSIZE);
    uint32_t code_size = rd32(image + SLYZ_HDR_CODESIZE);
    uint32_t entry = rd32(image + SLYZ_HDR_ENTRY);
    uint32_t checksum = rd32(image + SLYZ_HDR_CHECKSUM);
    uint32_t globals = rd32(image + SLYZ_HDR_GLOBALS);
    uint32_t stack_hint = rd32(image + SLYZ_HDR_STACKHINT);

    if (const_size > SLYZ_MAX_CONST || code_size > SLYZ_MAX_CODE) return 0;
    if ((uint64_t)SLYZ_HEADER_SIZE + const_size + code_size > (uint64_t)size) return 0;
    if (entry >= code_size) return 0;
    if (globals > SLYZ_MAX_GLOBALS) return 0;

    const uint8_t *pool = image + SLYZ_HEADER_SIZE;
    const uint8_t *code = pool + const_size;

    if (fnv1a(pool, const_size + code_size) != checksum) return 0;

    uint32_t stack_cap = stack_hint + 16;
    if (stack_cap < 128) stack_cap = 128;
    if (stack_cap > SLYZ_MAX_STACK) stack_cap = SLYZ_MAX_STACK;

    /* 先尝试 32KB 堆，失败再退到 16KB；仍失败就报告创建失败 */
    static const uint32_t heap_try[2] = { 32u * 1024u, 16u * 1024u };

    struct SlyzVm *vm = 0;

    for (int attempt = 0; attempt < 2 && !vm; attempt++) {
        uint32_t heap_size = heap_try[attempt];
        uint32_t total = (uint32_t)sizeof(struct SlyzVm) + 512 +
                         stack_cap * (uint32_t)sizeof(struct SlyzVal) +
                         globals * (uint32_t)sizeof(struct SlyzVal) +
                         heap_size;

        uint8_t *ws = (uint8_t *)ops->alloc(total);
        if (!ws) continue;

        vm_memset(ws, 0, total);
        struct SlyzVm *cand = (struct SlyzVm *)ws;
        uint32_t off = (uint32_t)sizeof(struct SlyzVm);
        off = align8(off);

        cand->workspace = ws;
        cand->workspace_size = total;

        cand->image = image;
        cand->pool = pool;
        cand->code = code;
        cand->name = name;
        cand->const_size = const_size;
        cand->code_size = code_size;
        cand->entry = entry;
        cand->global_count = globals;
        cand->pc = entry;
        cand->ops = ops;
        cand->rng = checksum ^ (uint32_t)(uintptr_t)image ^ 0x9E3779B9u;

        cand->stack = (struct SlyzVal *)(ws + off);
        cand->stack_cap = stack_cap;
        cand->sp = 0;
        off += stack_cap * (uint32_t)sizeof(struct SlyzVal);
        off = align8(off);

        cand->globals = (struct SlyzVal *)(ws + off);
        off += globals * (uint32_t)sizeof(struct SlyzVal);
        off = align8(off);

        if (!heap_init(cand, ws + off, total - off)) {
            ops->free(ws);
            continue;
        }

        /* 全局槽全部初始化为 nil（工作区已清零，类型 0 就是 nil） */
        vm = cand;
    }

    return vm;
}

void slyzvm_destroy(struct SlyzVm *vm) {
    if (!vm) return;
    if (vm->ops && vm->ops->free && vm->workspace) {
        vm->ops->free(vm->workspace);
    }
}

int slyzvm_state(const struct SlyzVm *vm) {
    return vm ? vm->state : SLYZ_ERR_FORMAT;
}

int slyzvm_exit_code(const struct SlyzVm *vm) {
    return vm ? vm->exit_code : 0;
}

const char *slyzvm_error(const struct SlyzVm *vm) {
    return vm ? vm->error : "no vm";
}

uint32_t slyzvm_steps(const struct SlyzVm *vm) {
    return vm ? vm->steps : 0;
}

int slyzvm_uses_gui(const struct SlyzVm *vm) {
    if (!vm) return 0;
    return (rd16(vm->image + SLYZ_HDR_FLAGS) & SLYZ_FLAG_GUI) ? 1 : 0;
}

void slyzvm_request_exit(struct SlyzVm *vm, int code) {
    if (!vm) return;
    vm->exit_code = code;
    vm->state = 1;
}

void slyzvm_request_yield(struct SlyzVm *vm) {
    if (!vm) return;
    vm->yield_request = 1;
}

/* ---------- 内置函数 ---------- */

static int bif_result_str(struct SlyzVm *vm, struct SlyzVal *ret,
                          const char *s, uint32_t len) {
    struct SlyzVal v = val_new_str(vm, s, len);
    if (v.type != SLYZ_T_STR) {
        fail_msg(vm, SLYZ_ERR_OOM, "out of memory building string");
        return 0;
    }
    *ret = v;
    return 1;
}

static int bif_simple_str(struct SlyzVm *vm, struct SlyzVal *ret,
                          const struct SlyzVal *arg, int mode) {
    /* mode: 0 upper, 1 lower, 2 trim */
    if (arg->type != SLYZ_T_STR) {
        return bif_result_str(vm, ret, "", 0);
    }

    const char *s = (const char *)arg->u.obj->data;
    uint32_t n = arg->u.obj->len;

    if (mode == 2) {
        uint32_t a = 0;
        while (a < n && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a++;
        while (n > a && (s[n - 1] == ' ' || s[n - 1] == '\t' ||
                         s[n - 1] == '\r' || s[n - 1] == '\n')) n--;
        return bif_result_str(vm, ret, s + a, n - a);
    }

    struct SlyzVal v = val_new_str(vm, s, n);
    if (v.type != SLYZ_T_STR) {
        fail_msg(vm, SLYZ_ERR_OOM, "out of memory building string");
        return 0;
    }

    char *d = (char *)v.u.obj->data;
    for (uint32_t i = 0; i < n; i++) {
        if (mode == 0 && d[i] >= 'a' && d[i] <= 'z') d[i] = (char)(d[i] - 'a' + 'A');
        if (mode == 1 && d[i] >= 'A' && d[i] <= 'Z') d[i] = (char)(d[i] - 'A' + 'a');
    }

    *ret = v;
    return 1;
}

static int bif_index_of(struct SlyzVm *vm, struct SlyzVal *ret,
                        const struct SlyzVal *hay, const struct SlyzVal *needle) {
    (void)vm;
    int hn = 0, nn = 0;
    const char *h = slyz_val_str(hay, &hn);
    const char *n = slyz_val_str(needle, &nn);

    if (!h) { *ret = val_int(-1); return 1; }
    if (!n) { *ret = val_int(-1); return 1; }
    if (nn == 0) { *ret = val_int(0); return 1; }

    for (int i = 0; i + nn <= hn; i++) {
        if (vm_memcmp(h + i, n, (uint32_t)nn) == 0) {
            *ret = val_int(i);
            return 1;
        }
    }

    *ret = val_int(-1);
    return 1;
}

static int bif_split(struct SlyzVm *vm, struct SlyzVal *ret,
                     const struct SlyzVal *src, const struct SlyzVal *sep) {
    int sn = 0, sepn = 0;
    const char *s = slyz_val_str(src, &sn);
    const char *p = slyz_val_str(sep, &sepn);

    if (!s) { *ret = val_nil(); return 1; }

    /* 先数段数 */
    uint32_t parts = 1;
    if (sepn > 0) {
        for (int i = 0; i + sepn <= sn; i++) {
            if (vm_memcmp(s + i, p, (uint32_t)sepn) == 0) {
                parts++;
                i += sepn - 1;
            }
        }
    }
    if (parts > 4096) parts = 4096;

    struct SlyzVal arr = val_new_arr(vm, parts);
    if (arr.type != SLYZ_T_ARR) {
        fail_msg(vm, SLYZ_ERR_OOM, "out of memory building array");
        return 0;
    }

    uint32_t idx = 0;
    int start = 0;
    int i = 0;
    while (i <= sn && idx < parts) {
        int hit = 0;
        if (sepn > 0 && i + sepn <= sn && vm_memcmp(s + i, p, (uint32_t)sepn) == 0) hit = 1;

        if (hit || i == sn) {
            struct SlyzVal piece = val_new_str(vm, s + start, (uint32_t)(i - start));
            if (piece.type != SLYZ_T_STR) {
                val_release(vm, arr);
                fail_msg(vm, SLYZ_ERR_OOM, "out of memory splitting string");
                return 0;
            }
            struct SlyzVal *items = (struct SlyzVal *)arr.u.obj->data;
            items[idx++] = piece;
            arr.u.obj->len = idx;

            if (hit) {
                i += sepn;
                start = i;
                continue;
            }
            break;
        }
        i++;
    }

    *ret = arr;
    return 1;
}

static int bif_join(struct SlyzVm *vm, struct SlyzVal *ret,
                    const struct SlyzVal *arr, const struct SlyzVal *sep) {
    if (arr->type != SLYZ_T_ARR) {
        return bif_result_str(vm, ret, "", 0);
    }

    int sepn = 0;
    const char *sepstr = slyz_val_str(sep, &sepn);
    if (!sepstr) sepn = 0;

    uint32_t total = 0;
    uint32_t n = arr->u.obj->len;
    struct SlyzVal *items = (struct SlyzVal *)arr->u.obj->data;
    char tmp[128];

    for (uint32_t i = 0; i < n; i++) {
        int k = render_value(vm, &items[i], tmp, (int)sizeof(tmp), 2);
        total += (uint32_t)k;
        if (i + 1 < n) total += (uint32_t)sepn;
        if (total > SLYZ_MAX_CONST) break;
    }

    struct SlyzVal out = val_new_str(vm, "", total);
    if (out.type != SLYZ_T_STR) {
        fail_msg(vm, SLYZ_ERR_OOM, "out of memory joining array");
        return 0;
    }

    char *d = (char *)out.u.obj->data;
    uint32_t o = 0;
    for (uint32_t i = 0; i < n && o < total; i++) {
        int k = render_value(vm, &items[i], tmp, (int)sizeof(tmp), 2);
        for (int j = 0; j < k && o < total; j++) d[o++] = tmp[j];
        if (i + 1 < n) {
            for (int j = 0; j < sepn && o < total; j++) d[o++] = sepstr[j];
        }
    }
    d[o] = 0;
    out.u.obj->len = o;

    *ret = out;
    return 1;
}

static int bif_replace(struct SlyzVm *vm, struct SlyzVal *ret,
                       const struct SlyzVal *src, const struct SlyzVal *from,
                       const struct SlyzVal *to) {
    int sn = 0, fn = 0, tn = 0;
    const char *s = slyz_val_str(src, &sn);
    const char *f = slyz_val_str(from, &fn);
    const char *t = slyz_val_str(to, &tn);

    if (!s) s = "";
    if (!f || fn == 0) {
        return bif_result_str(vm, ret, s, (uint32_t)sn);
    }
    if (!t) t = "";

    uint32_t cap = (uint32_t)(sn + 32);
    struct SlyzVal out = val_new_str(vm, "", cap);
    if (out.type != SLYZ_T_STR) {
        fail_msg(vm, SLYZ_ERR_OOM, "out of memory replacing text");
        return 0;
    }

    char *d = (char *)out.u.obj->data;
    uint32_t o = 0;
    int i = 0;
    while (i < sn) {
        if (i + fn <= sn && vm_memcmp(s + i, f, (uint32_t)fn) == 0) {
            for (int j = 0; j < tn && o + 1 < cap; j++) d[o++] = t[j];
            i += fn;
        } else {
            if (o + 1 < cap) d[o++] = s[i];
            i++;
        }
    }
    d[o] = 0;
    out.u.obj->len = o;
    if (o > 0) {
        /* 重新按实际长度收缩：创建精确副本，避免字符串对象声称的长度偏大 */
        struct SlyzVal exact = val_new_str(vm, d, o);
        val_release(vm, out);
        if (exact.type != SLYZ_T_STR) {
            fail_msg(vm, SLYZ_ERR_OOM, "out of memory replacing text");
            return 0;
        }
        *ret = exact;
        return 1;
    }

    *ret = out;
    return 1;
}

static int bif_hex(struct SlyzVm *vm, struct SlyzVal *ret, int64_t value, int base) {
    char tmp[72];
    int n = 0;
    uint64_t u;

    if (base == 16) {
        tmp[n++] = '0';
        tmp[n++] = 'x';
    } else if (base == 2) {
        tmp[n++] = '0';
        tmp[n++] = 'b';
    }

    if (value < 0) {
        u = (uint64_t)(-(value + 1)) + 1u;
    } else {
        u = (uint64_t)value;
    }

    char digits[72];
    int d = 0;
    if (u == 0) digits[d++] = '0';
    while (u > 0) {
        int r = (int)(u % (uint64_t)base);
        digits[d++] = (char)(r < 10 ? ('0' + r) : ('a' + r - 10));
        u /= (uint64_t)base;
    }
    while (d > 0) tmp[n++] = digits[--d];

    return bif_result_str(vm, ret, tmp, (uint32_t)n);
}

static int bif_exec(struct SlyzVm *vm, uint16_t id,
                    struct SlyzVal *args, uint16_t argc, struct SlyzVal *ret) {
    switch (id) {
        case SLE_BIF_LEN: {
            if (argc < 1) break;
            struct SlyzVal *v = &args[0];
            if (v->type == SLYZ_T_STR) *ret = val_int((int64_t)v->u.obj->len);
            else if (v->type == SLYZ_T_ARR) *ret = val_int((int64_t)v->u.obj->len);
            else *ret = val_int(0);
            return 1;
        }

        case SLE_BIF_STR: {
            if (argc < 1) break;
            char buf[256];
            int n = render_value(vm, &args[0], buf, (int)sizeof(buf), 0);
            return bif_result_str(vm, ret, buf, (uint32_t)n);
        }

        case SLE_BIF_INT:
            if (argc < 1) break;
            *ret = val_int(val_number(&args[0]));
            return 1;

        case SLE_BIF_SUBSTR: {
            if (argc < 3) break;
            int sn = 0;
            const char *s = slyz_val_str(&args[0], &sn);
            int64_t start = val_number(&args[1]);
            int64_t want = val_number(&args[2]);

            if (!s) s = "";
            if (start < 0) start = 0;
            if (start > sn) start = sn;
            if (want < 0) want = 0;
            if (start + want > sn) want = sn - start;
            return bif_result_str(vm, ret, s + start, (uint32_t)want);
        }

        case SLE_BIF_INDEXOF:
            if (argc < 2) break;
            return bif_index_of(vm, ret, &args[0], &args[1]);

        case SLE_BIF_UPPER:
            if (argc < 1) break;
            return bif_simple_str(vm, ret, &args[0], 0);

        case SLE_BIF_LOWER:
            if (argc < 1) break;
            return bif_simple_str(vm, ret, &args[0], 1);

        case SLE_BIF_TRIM:
            if (argc < 1) break;
            return bif_simple_str(vm, ret, &args[0], 2);

        case SLE_BIF_ABS: {
            if (argc < 1) break;
            int64_t x = val_number(&args[0]);
            if (x < 0) x = -x;
            *ret = val_int(x);
            return 1;
        }

        case SLE_BIF_MIN:
            if (argc < 2) break;
            *ret = val_int(val_number(&args[0]) < val_number(&args[1]) ?
                           val_number(&args[0]) : val_number(&args[1]));
            return 1;

        case SLE_BIF_MAX:
            if (argc < 2) break;
            *ret = val_int(val_number(&args[0]) > val_number(&args[1]) ?
                           val_number(&args[0]) : val_number(&args[1]));
            return 1;

        case SLE_BIF_CLAMP: {
            if (argc < 3) break;
            int64_t x = val_number(&args[0]);
            int64_t lo = val_number(&args[1]);
            int64_t hi = val_number(&args[2]);
            if (lo > hi) { int64_t t = lo; lo = hi; hi = t; }
            if (x < lo) x = lo;
            if (x > hi) x = hi;
            *ret = val_int(x);
            return 1;
        }

        case SLE_BIF_SIGN: {
            if (argc < 1) break;
            int64_t x = val_number(&args[0]);
            *ret = val_int(x > 0 ? 1 : (x < 0 ? -1 : 0));
            return 1;
        }

        case SLE_BIF_ARRAY: {
            if (argc < 1) break;
            int64_t n = val_number(&args[0]);
            if (n < 0) n = 0;
            if (n > 4096) n = 4096;
            struct SlyzVal v = val_new_arr(vm, (uint32_t)n);
            if (v.type != SLYZ_T_ARR) {
                fail_msg(vm, SLYZ_ERR_OOM, "out of memory building array");
                return 0;
            }
            *ret = v;
            return 1;
        }

        case SLE_BIF_PUSH: {
            if (argc < 2) break;
            if (args[0].type != SLYZ_T_ARR) {
                *ret = val_int(0);
                return 1;
            }
            struct SlyzObj *o = args[0].u.obj;
            if (!arr_reserve(vm, args[0], o->len + 1)) {
                fail_msg(vm, SLYZ_ERR_OOM, "array capacity exceeded");
                return 0;
            }
            struct SlyzVal *items = (struct SlyzVal *)o->data;
            val_retain(args[1]);
            items[o->len] = args[1];
            o->len++;
            *ret = val_int((int64_t)o->len);
            return 1;
        }

        case SLE_BIF_POP: {
            if (argc < 1) break;
            if (args[0].type != SLYZ_T_ARR) {
                *ret = val_nil();
                return 1;
            }
            struct SlyzObj *o = args[0].u.obj;
            if (o->len == 0) {
                *ret = val_nil();
                return 1;
            }
            struct SlyzVal *items = (struct SlyzVal *)o->data;
            o->len--;
            *ret = items[o->len];    /* 所有权转移给返回值 */
            items[o->len] = val_nil();
            return 1;
        }

        case SLE_BIF_RANGE: {
            if (argc < 2) break;
            int64_t a = val_number(&args[0]);
            int64_t b = val_number(&args[1]);
            int64_t n = b - a;
            if (n < 0) n = 0;
            if (n > 4096) n = 4096;

            struct SlyzVal arr = val_new_arr(vm, (uint32_t)n);
            if (arr.type != SLYZ_T_ARR) {
                fail_msg(vm, SLYZ_ERR_OOM, "out of memory building range");
                return 0;
            }
            struct SlyzVal *items = (struct SlyzVal *)arr.u.obj->data;
            for (int64_t i = 0; i < n; i++) items[i] = val_int(a + i);
            *ret = arr;
            return 1;
        }

        case SLE_BIF_RANDOM: {
            if (argc < 1) break;
            int64_t n = val_number(&args[0]);
            if (n <= 0) {
                *ret = val_int(0);
                return 1;
            }
            vm->rng = vm->rng * 1664525u + 1013904223u;
            *ret = val_int((int64_t)(vm->rng % (uint32_t)n));
            return 1;
        }

        case SLE_BIF_TYPEOF: {
            if (argc < 1) break;
            *ret = val_int((int64_t)args[0].type);
            return 1;
        }

        case SLE_BIF_REPLACE:
            if (argc < 3) break;
            return bif_replace(vm, ret, &args[0], &args[1], &args[2]);

        case SLE_BIF_SPLIT:
            if (argc < 2) break;
            return bif_split(vm, ret, &args[0], &args[1]);

        case SLE_BIF_JOIN:
            if (argc < 2) break;
            return bif_join(vm, ret, &args[0], &args[1]);

        case SLE_BIF_STR_AT: {
            if (argc < 2) break;
            int sn = 0;
            const char *s = slyz_val_str(&args[0], &sn);
            int64_t i = val_number(&args[1]);
            if (!s || i < 0 || i >= sn) {
                *ret = val_nil();
                return 1;
            }
            return bif_result_str(vm, ret, s + i, 1);
        }

        case SLE_BIF_ORD: {
            if (argc < 1) break;
            int sn = 0;
            const char *s = slyz_val_str(&args[0], &sn);
            if (!s || sn <= 0) {
                *ret = val_int(0);
                return 1;
            }
            *ret = val_int((int64_t)(unsigned char)s[0]);
            return 1;
        }

        case SLE_BIF_CHR: {
            if (argc < 1) break;
            char c = (char)(unsigned char)(val_number(&args[0]) & 0xFF);
            return bif_result_str(vm, ret, &c, 1);
        }

        case SLE_BIF_HEX:
            if (argc < 1) break;
            return bif_hex(vm, ret, val_number(&args[0]), 16);

        case SLE_BIF_BIN:
            if (argc < 1) break;
            return bif_hex(vm, ret, val_number(&args[0]), 2);

        default:
            break;
    }

    fail_msg(vm, SLYZ_ERR_SYS, "unknown builtin call id");
    return 0;
}

/* ---------- 算术 ---------- */

static int do_binary(struct SlyzVm *vm, uint8_t op,
                     struct SlyzVal a, struct SlyzVal b, struct SlyzVal *out) {
    switch (op) {
        case SLE_OP_ADD: {
            if (a.type == SLYZ_T_STR || b.type == SLYZ_T_STR) {
                char ta[256];
                char tb[256];
                int na = render_value(vm, &a, ta, (int)sizeof(ta), 0);
                int nb = render_value(vm, &b, tb, (int)sizeof(tb), 0);

                uint32_t total = (uint32_t)(na + nb);
                struct SlyzVal s = val_new_str(vm, "", total);
                if (s.type != SLYZ_T_STR) {
                    fail_msg(vm, SLYZ_ERR_OOM, "out of memory concatenating strings");
                    return 0;
                }
                char *d = (char *)s.u.obj->data;
                for (int i = 0; i < na; i++) d[i] = ta[i];
                for (int i = 0; i < nb; i++) d[na + i] = tb[i];
                d[total] = 0;
                *out = s;
                return 1;
            }
            *out = val_int(val_number(&a) + val_number(&b));
            return 1;
        }

        case SLE_OP_SUB:
            *out = val_int(val_number(&a) - val_number(&b));
            return 1;

        case SLE_OP_MUL:
            *out = val_int(val_number(&a) * val_number(&b));
            return 1;

        case SLE_OP_DIV: {
            int64_t d = val_number(&b);
            if (d == 0) {
                fail_msg(vm, SLYZ_ERR_TYPE, "division by zero");
                return 0;
            }
            *out = val_int(val_number(&a) / d);
            return 1;
        }

        case SLE_OP_MOD: {
            int64_t d = val_number(&b);
            if (d == 0) {
                fail_msg(vm, SLYZ_ERR_TYPE, "modulo by zero");
                return 0;
            }
            *out = val_int(val_number(&a) % d);
            return 1;
        }

        case SLE_OP_EQ: *out = val_int(val_equal(&a, &b) ? 1 : 0); return 1;
        case SLE_OP_NE: *out = val_int(val_equal(&a, &b) ? 0 : 1); return 1;
        case SLE_OP_LT: *out = val_int(val_compare(&a, &b) < 0 ? 1 : 0); return 1;
        case SLE_OP_LE: *out = val_int(val_compare(&a, &b) <= 0 ? 1 : 0); return 1;
        case SLE_OP_GT: *out = val_int(val_compare(&a, &b) > 0 ? 1 : 0); return 1;
        case SLE_OP_GE: *out = val_int(val_compare(&a, &b) >= 0 ? 1 : 0); return 1;

        case SLE_OP_AND:
            *out = val_int((val_truthy(&a) && val_truthy(&b)) ? 1 : 0);
            return 1;

        case SLE_OP_OR:
            *out = val_int((val_truthy(&a) || val_truthy(&b)) ? 1 : 0);
            return 1;

        default:
            break;
    }

    fail_msg(vm, SLYZ_ERR_TYPE, "unsupported binary operator");
    return 0;
}

/* ---------- 执行 ---------- */

int slyzvm_step(struct SlyzVm *vm, uint32_t budget) {
    if (!vm) return SLYZ_ERR_FORMAT;
    if (vm->state == 1) return 0;
    if (vm->state < 0) return vm->state;
    if (budget == 0) return 1;

    const uint8_t *code = vm->code;
    uint32_t executed = 0;

    while (executed < budget) {
        if (vm->pc >= vm->code_size) {
            fail_msg(vm, SLYZ_ERR_ENTRY, "instruction pointer out of range");
            return vm->state;
        }

        uint8_t op = code[vm->pc++];
        executed++;
        vm->steps++;

        if (vm->steps > SLYZ_MAX_STEPS) {
            fail_msg(vm, SLYZ_ERR_STEPS, "instruction budget exceeded");
            return vm->state;
        }
        if ((vm->steps & 0x3FFu) == 0 && vm->ops->tick) vm->ops->tick();

        switch (op) {
            case SLE_OP_HALT:
                vm->state = 1;
                return 0;

            case SLE_OP_NOP:
                break;

            case SLE_OP_POP: {
                if (vm->sp == 0) { fail_msg(vm, SLYZ_ERR_STACK, "stack underflow"); return vm->state; }
                struct SlyzVal v = vm_pop(vm);
                val_release(vm, v);
                break;
            }

            case SLE_OP_DUP: {
                if (vm->sp == 0) { fail_msg(vm, SLYZ_ERR_STACK, "stack underflow"); return vm->state; }
                struct SlyzVal v = vm->stack[vm->sp - 1];
                val_retain(v);
                if (!vm_push(vm, v)) return vm->state;
                break;
            }

            case SLE_OP_SWAP: {
                if (vm->sp < 2) { fail_msg(vm, SLYZ_ERR_STACK, "stack underflow"); return vm->state; }
                struct SlyzVal t = vm->stack[vm->sp - 1];
                vm->stack[vm->sp - 1] = vm->stack[vm->sp - 2];
                vm->stack[vm->sp - 2] = t;
                break;
            }

            case SLE_OP_DUP2: {
                if (vm->sp < 2) { fail_msg(vm, SLYZ_ERR_STACK, "stack underflow"); return vm->state; }
                struct SlyzVal a = vm->stack[vm->sp - 2];
                struct SlyzVal b = vm->stack[vm->sp - 1];
                val_retain(a);
                val_retain(b);
                if (!vm_push(vm, a)) return vm->state;
                if (!vm_push(vm, b)) return vm->state;
                break;
            }

            case SLE_OP_PUSH_I: {
                if (vm->pc + 8 > vm->code_size) { fail_msg(vm, SLYZ_ERR_TRUNC, "truncated PUSH_I"); return vm->state; }
                int64_t v = rd64(code + vm->pc);
                vm->pc += 8;
                if (!vm_push(vm, val_int(v))) return vm->state;
                break;
            }

            case SLE_OP_PUSH_S: {
                if (vm->pc + 4 > vm->code_size) { fail_msg(vm, SLYZ_ERR_TRUNC, "truncated PUSH_S"); return vm->state; }
                uint32_t off = rd32(code + vm->pc);
                vm->pc += 4;
                uint32_t len = 0;
                const char *s = pool_str(vm, off, &len);
                if (!s) { fail_msg(vm, SLYZ_ERR_FORMAT, "bad string constant offset"); return vm->state; }
                struct SlyzVal v = val_new_str(vm, s, len);
                if (v.type != SLYZ_T_STR) { fail_msg(vm, SLYZ_ERR_OOM, "out of memory pushing string"); return vm->state; }
                if (!vm_push(vm, v)) return vm->state;
                break;
            }

            case SLE_OP_PUSH_NIL:
                if (!vm_push(vm, val_nil())) return vm->state;
                break;

            case SLE_OP_PUSH_TRUE:
                if (!vm_push(vm, val_int(1))) return vm->state;
                break;

            case SLE_OP_PUSH_FALSE:
                if (!vm_push(vm, val_int(0))) return vm->state;
                break;

            case SLE_OP_LOAD_L: {
                if (vm->pc + 4 > vm->code_size) { fail_msg(vm, SLYZ_ERR_TRUNC, "truncated LOAD_L"); return vm->state; }
                uint32_t slot = rd32(code + vm->pc);
                vm->pc += 4;
                uint32_t base = vm->depth ? vm->frames[vm->depth - 1].base : 0;
                uint32_t idx = base + slot;
                if (idx >= vm->sp) { fail_msg(vm, SLYZ_ERR_STACK, "local slot out of range"); return vm->state; }
                struct SlyzVal v = vm->stack[idx];
                val_retain(v);
                if (!vm_push(vm, v)) return vm->state;
                break;
            }

            case SLE_OP_STORE_L: {
                if (vm->pc + 4 > vm->code_size) { fail_msg(vm, SLYZ_ERR_TRUNC, "truncated STORE_L"); return vm->state; }
                uint32_t slot = rd32(code + vm->pc);
                vm->pc += 4;
                if (vm->sp == 0) { fail_msg(vm, SLYZ_ERR_STACK, "stack underflow"); return vm->state; }
                uint32_t base = vm->depth ? vm->frames[vm->depth - 1].base : 0;
                uint32_t idx = base + slot;
                if (idx >= vm->sp) { fail_msg(vm, SLYZ_ERR_STACK, "local slot out of range"); return vm->state; }
                val_release(vm, vm->stack[idx]);
                vm->stack[idx] = vm_pop(vm);
                break;
            }

            case SLE_OP_LOAD_G: {
                if (vm->pc + 4 > vm->code_size) { fail_msg(vm, SLYZ_ERR_TRUNC, "truncated LOAD_G"); return vm->state; }
                uint32_t slot = rd32(code + vm->pc);
                vm->pc += 4;
                if (slot >= vm->global_count) { fail_msg(vm, SLYZ_ERR_FORMAT, "global slot out of range"); return vm->state; }
                struct SlyzVal v = vm->globals[slot];
                val_retain(v);
                if (!vm_push(vm, v)) return vm->state;
                break;
            }

            case SLE_OP_STORE_G: {
                if (vm->pc + 4 > vm->code_size) { fail_msg(vm, SLYZ_ERR_TRUNC, "truncated STORE_G"); return vm->state; }
                uint32_t slot = rd32(code + vm->pc);
                vm->pc += 4;
                if (slot >= vm->global_count) { fail_msg(vm, SLYZ_ERR_FORMAT, "global slot out of range"); return vm->state; }
                if (vm->sp == 0) { fail_msg(vm, SLYZ_ERR_STACK, "stack underflow"); return vm->state; }
                val_release(vm, vm->globals[slot]);
                vm->globals[slot] = vm_pop(vm);
                break;
            }

            case SLE_OP_JMP: {
                if (vm->pc + 4 > vm->code_size) { fail_msg(vm, SLYZ_ERR_TRUNC, "truncated JMP"); return vm->state; }
                uint32_t target = rd32(code + vm->pc);
                if (target >= vm->code_size) { fail_msg(vm, SLYZ_ERR_ENTRY, "jump target out of range"); return vm->state; }
                vm->pc = target;
                break;
            }

            case SLE_OP_JZ:
            case SLE_OP_JNZ: {
                if (vm->pc + 4 > vm->code_size) { fail_msg(vm, SLYZ_ERR_TRUNC, "truncated conditional jump"); return vm->state; }
                uint32_t target = rd32(code + vm->pc);
                vm->pc += 4;
                if (vm->sp == 0) { fail_msg(vm, SLYZ_ERR_STACK, "stack underflow"); return vm->state; }

                struct SlyzVal cond = vm_pop(vm);
                int t = val_truthy(&cond);
                val_release(vm, cond);

                int take = (op == SLE_OP_JZ) ? (!t) : (t);
                if (take) {
                    if (target >= vm->code_size) { fail_msg(vm, SLYZ_ERR_ENTRY, "jump target out of range"); return vm->state; }
                    vm->pc = target;
                }
                break;
            }

            case SLE_OP_CALL: {
                if (vm->pc + 8 > vm->code_size) { fail_msg(vm, SLYZ_ERR_TRUNC, "truncated CALL"); return vm->state; }
                uint32_t target = rd32(code + vm->pc);
                uint32_t argc = rd32(code + vm->pc + 4);
                vm->pc += 8;

                if (target >= vm->code_size) { fail_msg(vm, SLYZ_ERR_ENTRY, "call target out of range"); return vm->state; }
                if (argc > vm->sp) { fail_msg(vm, SLYZ_ERR_STACK, "call argument underflow"); return vm->state; }
                if (vm->depth >= SLYZ_MAX_FRAMES) { fail_msg(vm, SLYZ_ERR_CALL, "call nesting too deep"); return vm->state; }

                vm->frames[vm->depth].base = vm->sp - argc;
                vm->frames[vm->depth].ret_pc = vm->pc;
                vm->depth++;
                vm->pc = target;
                break;
            }

            case SLE_OP_RET: {
                if (vm->depth == 0) { fail_msg(vm, SLYZ_ERR_CALL, "return without call"); return vm->state; }
                if (vm->sp == 0) { fail_msg(vm, SLYZ_ERR_STACK, "stack underflow"); return vm->state; }

                uint32_t base = vm->frames[vm->depth - 1].base;
                uint32_t ret_pc = vm->frames[vm->depth - 1].ret_pc;

                struct SlyzVal rv = vm_pop(vm);
                while (vm->sp > base) {
                    struct SlyzVal dead = vm_pop(vm);
                    val_release(vm, dead);
                }
                vm->depth--;
                if (!vm_push(vm, rv)) return vm->state;
                vm->pc = ret_pc;
                break;
            }

            case SLE_OP_MAKE_ARR: {
                if (vm->pc + 4 > vm->code_size) { fail_msg(vm, SLYZ_ERR_TRUNC, "truncated MAKE_ARR"); return vm->state; }
                uint32_t count = rd32(code + vm->pc);
                vm->pc += 4;
                if (count > vm->sp) { fail_msg(vm, SLYZ_ERR_STACK, "array element underflow"); return vm->state; }

                struct SlyzVal arr = val_new_arr(vm, count);
                if (arr.type != SLYZ_T_ARR) { fail_msg(vm, SLYZ_ERR_OOM, "out of memory building array"); return vm->state; }

                struct SlyzVal *items = (struct SlyzVal *)arr.u.obj->data;
                uint32_t start = vm->sp - count;
                for (uint32_t i = 0; i < count; i++) {
                    items[i] = vm->stack[start + i];   /* 所有权从栈转移到数组 */
                }
                vm->sp = start;
                if (!vm_push(vm, arr)) return vm->state;
                break;
            }

            case SLE_OP_ADD:
            case SLE_OP_SUB:
            case SLE_OP_MUL:
            case SLE_OP_DIV:
            case SLE_OP_MOD:
            case SLE_OP_EQ:
            case SLE_OP_NE:
            case SLE_OP_LT:
            case SLE_OP_LE:
            case SLE_OP_GT:
            case SLE_OP_GE:
            case SLE_OP_AND:
            case SLE_OP_OR: {
                if (vm->sp < 2) { fail_msg(vm, SLYZ_ERR_STACK, "stack underflow"); return vm->state; }
                struct SlyzVal b = vm_pop(vm);
                struct SlyzVal a = vm_pop(vm);
                struct SlyzVal out = val_nil();
                int ok = do_binary(vm, op, a, b, &out);
                val_release(vm, a);
                val_release(vm, b);
                if (!ok) return vm->state;
                if (!vm_push(vm, out)) return vm->state;
                break;
            }

            case SLE_OP_NEG: {
                if (vm->sp == 0) { fail_msg(vm, SLYZ_ERR_STACK, "stack underflow"); return vm->state; }
                struct SlyzVal a = vm_pop(vm);
                struct SlyzVal out = val_int(-val_number(&a));
                val_release(vm, a);
                if (!vm_push(vm, out)) return vm->state;
                break;
            }

            case SLE_OP_NOT: {
                if (vm->sp == 0) { fail_msg(vm, SLYZ_ERR_STACK, "stack underflow"); return vm->state; }
                struct SlyzVal a = vm_pop(vm);
                struct SlyzVal out = val_int(val_truthy(&a) ? 0 : 1);
                val_release(vm, a);
                if (!vm_push(vm, out)) return vm->state;
                break;
            }

            case SLE_OP_TYPEOF: {
                if (vm->sp == 0) { fail_msg(vm, SLYZ_ERR_STACK, "stack underflow"); return vm->state; }
                struct SlyzVal a = vm_pop(vm);
                struct SlyzVal out = val_int((int64_t)a.type);
                val_release(vm, a);
                if (!vm_push(vm, out)) return vm->state;
                break;
            }

            case SLE_OP_IDX_GET: {
                if (vm->sp < 2) { fail_msg(vm, SLYZ_ERR_STACK, "stack underflow"); return vm->state; }
                struct SlyzVal idxv = vm_pop(vm);
                struct SlyzVal obj = vm_pop(vm);
                int64_t idx = val_number(&idxv);
                struct SlyzVal out = val_nil();

                if (obj.type == SLYZ_T_ARR && obj.u.obj) {
                    if (idx >= 0 && (uint32_t)idx < obj.u.obj->len) {
                        out = ((struct SlyzVal *)obj.u.obj->data)[idx];
                        val_retain(out);
                    }
                } else if (obj.type == SLYZ_T_STR && obj.u.obj) {
                    if (idx >= 0 && (uint32_t)idx < obj.u.obj->len) {
                        const char *s = (const char *)obj.u.obj->data;
                        out = val_new_str(vm, s + idx, 1);
                    }
                }

                val_release(vm, idxv);
                val_release(vm, obj);
                if (!vm_push(vm, out)) return vm->state;
                break;
            }

            case SLE_OP_IDX_SET: {
                if (vm->sp < 3) { fail_msg(vm, SLYZ_ERR_STACK, "stack underflow"); return vm->state; }
                struct SlyzVal val = vm_pop(vm);
                struct SlyzVal idxv = vm_pop(vm);
                struct SlyzVal obj = vm_pop(vm);

                int64_t idx = val_number(&idxv);
                if (obj.type == SLYZ_T_ARR && obj.u.obj && idx >= 0) {
                    struct SlyzObj *o = obj.u.obj;
                    if ((uint32_t)idx >= o->cap) {
                        if (!arr_reserve(vm, obj, (uint32_t)idx + 1)) {
                            val_release(vm, val);
                            val_release(vm, idxv);
                            val_release(vm, obj);
                            fail_msg(vm, SLYZ_ERR_OOM, "array growth failed");
                            return vm->state;
                        }
                        o = obj.u.obj;
                    }
                    struct SlyzVal *items = (struct SlyzVal *)o->data;
                    val_release(vm, items[idx]);
                    val_retain(val);
                    items[idx] = val;
                    if ((uint32_t)idx >= o->len) o->len = (uint32_t)idx + 1;
                }

                val_release(vm, val);
                val_release(vm, idxv);
                val_release(vm, obj);
                break;
            }

            case SLE_OP_SYS: {
                if (vm->pc + 4 > vm->code_size) { fail_msg(vm, SLYZ_ERR_TRUNC, "truncated SYS"); return vm->state; }
                uint16_t id = rd16(code + vm->pc);
                uint16_t argc = rd16(code + vm->pc + 2);
                vm->pc += 4;

                if (argc > vm->sp) { fail_msg(vm, SLYZ_ERR_STACK, "syscall argument underflow"); return vm->state; }

                struct SlyzVal *args = &vm->stack[vm->sp - argc];
                struct SlyzVal ret = val_nil();
                int rc = 0;

                if (id >= 100) {
                    rc = bif_exec(vm, id, args, argc, &ret) ? 0 : -1;
                } else if (vm->ops->sys) {
                    rc = vm->ops->sys(vm, id, args, argc, &ret);
                } else {
                    rc = -1;
                    fail_msg(vm, SLYZ_ERR_SYS, "host has no syscall handler");
                }

                if (rc != 0) {
                    if (vm->state == 0) {
                        fail_msg(vm, SLYZ_ERR_SYS, "syscall failed");
                    }
                    return vm->state;
                }

                /* 宿主可能在调用中结束了程序（os.exit）：立即收尾，
                 * 不再把返回值压栈，也不再执行后续指令。 */
                if (vm->state != 0) {
                    for (uint16_t i = 0; i < argc; i++) val_release(vm, args[i]);
                    vm->sp -= argc;
                    return vm->state;
                }

                for (uint16_t i = 0; i < argc; i++) val_release(vm, args[i]);
                vm->sp -= argc;

                if (!vm_push(vm, ret)) return vm->state;

                /* 宿主请求让出：gfx.flush 之后立即返回，
                 * 应用逻辑不会被一次 step 跑完整个生命周期。 */
                if (vm->yield_request) {
                    vm->yield_request = 0;
                    return 1;
                }
                break;
            }

            default:
                fail_msg(vm, SLYZ_ERR_FORMAT, "unknown opcode");
                return vm->state;
        }
    }

    return 1;
}

int slyzvm_run(const uint8_t *image, int size,
               const struct SlyzVmOps *ops, const char *name) {
    struct SlyzVm *vm = slyzvm_create(image, size, ops, name);
    if (!vm) return 0;

    int rc = 1;
    for (;;) {
        int st = slyzvm_step(vm, 65536);
        if (st == 0) { rc = 1; break; }
        if (st < 0) { rc = 0; break; }
    }

    slyzvm_destroy(vm);
    return rc;
}
