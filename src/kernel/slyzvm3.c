/* ============================================================
 * slyzvm3.c - SLYZ v3 寄存器虚拟机
 * ------------------------------------------------------------
 * 与 v2 栈式解释器并存：加载器按 magic 选择解释器，两套 VM
 * 共享 SlyzVal / SlyzObj 值布局和 SLE_SYS_* 宿主 ABI。
 *
 * 执行模型：
 *   - 固定 8 字节指令，pc 始终指向下一条指令边界；
 *   - 每帧 64 个寄存器，调用时参数复制到被调帧 r0..rN-1；
 *   - 返回时 r0 写回调用者 CALL 指令里给出的基址寄存器；
 *   - 字符串/数组引用计数，所有写寄存器的指令负责转移引用。
 *
 * 该解释器不依赖内核，Windows 端宿主可以链接同一份代码做
 * 语法/语义自测，因此这里只用 ops->alloc/free 申请内存。
 * ============================================================ */

#include <stdint.h>

#include "slyz3.h"

/* ============================================================
 * 基础内存与字符串
 * ============================================================ */

static void v3_memcpy(void *dst, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
}

static void v3_memset(void *dst, uint8_t v, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = v;
}

static int v3_strlen(const char *s) {
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

static int v3_strcmp(const char *a, const char *b) {
    while (a && b && *a && *b && *a == *b) {
        a++;
        b++;
    }
    if (!a || !b) return a ? 1 : (b ? -1 : 0);
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ============================================================
 * 虚拟机结构
 * ============================================================ */

struct Slyz3Vm {
    const Slyz3Ops *ops;

    const uint8_t *image;
    uint32_t image_size;

    const uint8_t *consts;
    uint32_t const_size;

    const uint8_t *code;
    uint32_t code_size;

    const uint8_t *funcs;
    uint32_t func_count;

    uint32_t entry;
    uint32_t globals_count;
    uint32_t regs_per_frame;
    int      uses_gui;

    struct SlyzVal *regs;       /* (SLYZ3_MAX_FRAMES+1) * regs_per_frame */
    struct SlyzVal *globals;

    /* 常量字符串对象：按池内偏移查找 */
    struct SlyzVal *const_vals;
    uint32_t       *const_offs;
    uint32_t        const_count;

    int      depth;
    uint32_t pc;
    uint32_t base[SLYZ3_MAX_FRAMES + 1];     /* 每帧寄存器窗口起点 */
    uint32_t call_ret[SLYZ3_MAX_FRAMES + 1]; /* 本帧发起的调用，返回值写回哪个寄存器 */
    uint32_t frame_pc[SLYZ3_MAX_FRAMES + 1];

    int      state;      /* 0 运行, 1 结束, <0 错误 */
    int      exit_code;
    const char *error;

    uint32_t steps;
    int      yield_request;
    uint32_t rng;
    const char *name;
};

/* ============================================================
 * 值操作
 * ============================================================ */

static struct SlyzVal v_nil(void) {
    struct SlyzVal v;
    v.type = SLYZ_T_NIL;
    v.aux = 0;
    v.u.i = 0;
    return v;
}

static struct SlyzVal v_int(int64_t x) {
    struct SlyzVal v;
    v.type = SLYZ_T_INT;
    v.aux = 0;
    v.u.i = x;
    return v;
}

static void *v3_alloc(struct Slyz3Vm *vm, uint32_t n) {
    if (!vm || !vm->ops || !vm->ops->alloc) return 0;
    return vm->ops->alloc(n ? n : 1);
}

static void v3_free(struct Slyz3Vm *vm, void *p) {
    if (vm && vm->ops && vm->ops->free && p) vm->ops->free(p);
}

static void v_retain(struct SlyzVal v) {
    if ((v.type == SLYZ_T_STR || v.type == SLYZ_T_ARR) && v.u.obj) {
        if (!(v.u.obj->flags & SLYZ_OBJ_STATIC)) {
            if (v.u.obj->refs < 0xFFFFu) v.u.obj->refs++;
        }
    }
}

static void v_release(struct Slyz3Vm *vm, struct SlyzVal v) {
    if ((v.type != SLYZ_T_STR && v.type != SLYZ_T_ARR) || !v.u.obj) return;
    if (v.u.obj->flags & SLYZ_OBJ_STATIC) return;
    if (v.u.obj->refs == 0) return;
    v.u.obj->refs--;
    if (v.u.obj->refs > 0) return;

    struct SlyzObj *o = v.u.obj;
    if (o->kind == SLYZ_T_ARR && o->data) {
        struct SlyzVal *items = (struct SlyzVal *)o->data;
        for (uint32_t i = 0; i < o->len; i++) v_release(vm, items[i]);
        v3_free(vm, o->data);
    } else if (o->data) {
        v3_free(vm, o->data);
    }
    v3_free(vm, o);
}

static struct SlyzVal v_new_str(struct Slyz3Vm *vm, const char *s, uint32_t len) {
    struct SlyzVal v = v_nil();
    if (!vm) return v;

    struct SlyzObj *o = (struct SlyzObj *)v3_alloc(vm, (uint32_t)sizeof(struct SlyzObj));
    if (!o) return v;
    v3_memset(o, 0, (uint32_t)sizeof(struct SlyzObj));

    o->kind = SLYZ_T_STR;
    o->flags = 0;
    o->refs = 1;
    o->len = len;
    o->cap = len;
    o->data = v3_alloc(vm, len + 1);
    if (!o->data) {
        v3_free(vm, o);
        return v;
    }
    if (s && len) v3_memcpy(o->data, s, len);
    ((char *)o->data)[len] = '\0';

    v.type = SLYZ_T_STR;
    v.aux = 0;
    v.u.obj = o;
    return v;
}

static struct SlyzVal v_new_arr(struct Slyz3Vm *vm, uint32_t count) {
    struct SlyzVal v = v_nil();
    if (!vm) return v;

    struct SlyzObj *o = (struct SlyzObj *)v3_alloc(vm, (uint32_t)sizeof(struct SlyzObj));
    if (!o) return v;
    v3_memset(o, 0, (uint32_t)sizeof(struct SlyzObj));

    uint32_t cap = count ? count : 4;
    if (cap > 4096) cap = 4096;
    o->kind = SLYZ_T_ARR;
    o->refs = 1;
    o->len = count;
    o->cap = cap;
    o->data = v3_alloc(vm, cap * (uint32_t)sizeof(struct SlyzVal));
    if (!o->data) {
        v3_free(vm, o);
        return v;
    }
    v3_memset(o->data, 0, cap * (uint32_t)sizeof(struct SlyzVal));

    v.type = SLYZ_T_ARR;
    v.u.obj = o;
    return v;
}

static int v_truthy(const struct SlyzVal *v) {
    if (!v) return 0;
    if (v->type == SLYZ_T_NIL) return 0;
    if (v->type == SLYZ_T_INT) return v->u.i != 0;
    if (v->type == SLYZ_T_STR) return v->u.obj && v->u.obj->len > 0;
    if (v->type == SLYZ_T_ARR) return v->u.obj && v->u.obj->len > 0;
    return 0;
}

static int64_t v_number(const struct SlyzVal *v) {
    if (!v) return 0;
    if (v->type == SLYZ_T_INT) return v->u.i;
    if (v->type == SLYZ_T_STR && v->u.obj) {
        const char *p = (const char *)v->u.obj->data;
        int neg = 0;
        int64_t n = 0;
        int any = 0;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '-') { neg = 1; p++; }
        else if (*p == '+') p++;
        while (*p >= '0' && *p <= '9') {
            n = n * 10 + (*p - '0');
            p++;
            any = 1;
        }
        if (!any) return 0;
        return neg ? -n : n;
    }
    return 0;
}

/* 把值渲染成文本；返回写入长度。数组用 [a, b] 形式。 */
static int v_render(struct Slyz3Vm *vm, const struct SlyzVal *v,
                    char *buf, int cap, int depth) {
    int p = 0;
    if (!buf || cap <= 0) return 0;
    if (!v) { buf[0] = '\0'; return 0; }

    if (v->type == SLYZ_T_NIL) {
        const char *s = "nil";
        while (*s && p < cap - 1) buf[p++] = *s++;
    } else if (v->type == SLYZ_T_INT) {
        char tmp[24];
        int n = 0;
        int64_t x = v->u.i;
        uint64_t u;
        if (x < 0) {
            if (p < cap - 1) buf[p++] = '-';
            u = (uint64_t)(-(x + 1)) + 1u;
        } else {
            u = (uint64_t)x;
        }
        if (u == 0) tmp[n++] = '0';
        while (u > 0) { tmp[n++] = (char)('0' + (u % 10u)); u /= 10u; }
        while (n > 0 && p < cap - 1) buf[p++] = tmp[--n];
    } else if (v->type == SLYZ_T_STR && v->u.obj) {
        const char *s = (const char *)v->u.obj->data;
        uint32_t n = v->u.obj->len;
        for (uint32_t i = 0; i < n && p < cap - 1; i++) buf[p++] = s[i];
    } else if (v->type == SLYZ_T_ARR && v->u.obj) {
        struct SlyzVal *items = (struct SlyzVal *)v->u.obj->data;
        if (p < cap - 1) buf[p++] = '[';
        for (uint32_t i = 0; i < v->u.obj->len && p < cap - 2; i++) {
            if (i > 0) {
                if (p < cap - 2) buf[p++] = ',';
                if (p < cap - 2) buf[p++] = ' ';
            }
            if (depth > 3) {
                const char *e = "...";
                while (*e && p < cap - 1) buf[p++] = *e++;
            } else {
                p += v_render(vm, &items[i], buf + p, cap - p, depth + 1);
            }
        }
        if (p < cap - 1) buf[p++] = ']';
    }
    buf[p] = '\0';
    (void)vm;
    return p;
}

/* ============================================================
 * 寄存器
 * ============================================================ */

static void reg_clear(struct Slyz3Vm *vm, uint32_t idx) {
    struct SlyzVal *r = &vm->regs[idx];
    v_release(vm, *r);
    *r = v_nil();
}

static void reg_write(struct Slyz3Vm *vm, uint32_t idx, struct SlyzVal v) {
    if (idx >= (uint32_t)(SLYZ3_MAX_FRAMES + 1) * vm->regs_per_frame) {
        vm->state = -1;
        vm->error = "register index out of range";
        return;
    }
    v_retain(v);
    v_release(vm, vm->regs[idx]);
    vm->regs[idx] = v;
}

static uint32_t frame_base(const struct Slyz3Vm *vm) {
    return vm->base[vm->depth];
}

/* ============================================================
 * 内置函数
 * ============================================================ */

static struct SlyzVal bif_len(struct Slyz3Vm *vm, struct SlyzVal *args, uint16_t argc) {
    if (argc < 1) return v_int(0);
    struct SlyzVal *v = &args[0];
    if (v->type == SLYZ_T_STR && v->u.obj) return v_int((int64_t)v->u.obj->len);
    if (v->type == SLYZ_T_ARR && v->u.obj) return v_int((int64_t)v->u.obj->len);
    (void)vm;
    return v_int(0);
}

static struct SlyzVal bif_str(struct Slyz3Vm *vm, struct SlyzVal *args, uint16_t argc) {
    char buf[512];
    if (argc < 1) return v_new_str(vm, "", 0);
    int n = v_render(vm, &args[0], buf, (int)sizeof(buf), 0);
    return v_new_str(vm, buf, (uint32_t)n);
}

static struct SlyzVal bif_int(struct Slyz3Vm *vm, struct SlyzVal *args, uint16_t argc) {
    if (argc < 1) return v_int(0);
    return v_int(v_number(&args[0]));
}

static struct SlyzVal bif_substr(struct Slyz3Vm *vm, struct SlyzVal *args, uint16_t argc) {
    if (argc < 3 || args[0].type != SLYZ_T_STR || !args[0].u.obj) {
        return v_new_str(vm, "", 0);
    }
    int64_t start = v_number(&args[1]);
    int64_t len = v_number(&args[2]);
    uint32_t n = args[0].u.obj->len;
    if (start < 0) start = 0;
    if (len < 0) len = 0;
    if ((uint64_t)start > n) start = n;
    if ((uint64_t)(start + len) > n) len = (int64_t)(n - (uint32_t)start);
    return v_new_str(vm, (const char *)args[0].u.obj->data + start, (uint32_t)len);
}

static struct SlyzVal bif_indexof(struct Slyz3Vm *vm, struct SlyzVal *args, uint16_t argc) {
    if (argc < 2 || args[0].type != SLYZ_T_STR || !args[0].u.obj ||
        args[1].type != SLYZ_T_STR || !args[1].u.obj) {
        return v_int(-1);
    }
    const char *h = (const char *)args[0].u.obj->data;
    const char *n = (const char *)args[1].u.obj->data;
    int hl = (int)args[0].u.obj->len;
    int nl = (int)args[1].u.obj->len;
    if (nl == 0) return v_int(0);
    for (int i = 0; i + nl <= hl; i++) {
        int k = 0;
        while (k < nl && h[i + k] == n[k]) k++;
        if (k == nl) return v_int(i);
    }
    (void)vm;
    return v_int(-1);
}

static struct SlyzVal bif_case(struct Slyz3Vm *vm, struct SlyzVal *args,
                               uint16_t argc, int upper) {
    if (argc < 1 || args[0].type != SLYZ_T_STR || !args[0].u.obj) {
        return v_new_str(vm, "", 0);
    }
    uint32_t n = args[0].u.obj->len;
    char *tmp = (char *)v3_alloc(vm, n + 1);
    if (!tmp) return v_nil();
    const char *s = (const char *)args[0].u.obj->data;
    for (uint32_t i = 0; i < n; i++) {
        char c = s[i];
        if (upper && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if (!upper && c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        tmp[i] = c;
    }
    tmp[n] = '\0';
    struct SlyzVal r = v_new_str(vm, tmp, n);
    v3_free(vm, tmp);
    return r;
}

static struct SlyzVal bif_trim(struct Slyz3Vm *vm, struct SlyzVal *args, uint16_t argc) {
    if (argc < 1 || args[0].type != SLYZ_T_STR || !args[0].u.obj) {
        return v_new_str(vm, "", 0);
    }
    const char *s = (const char *)args[0].u.obj->data;
    int n = (int)args[0].u.obj->len;
    int a = 0, b = n;
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' ||
                     s[b - 1] == '\r' || s[b - 1] == '\n')) b--;
    return v_new_str(vm, s + a, (uint32_t)(b - a));
}

static struct SlyzVal bif_replace(struct Slyz3Vm *vm, struct SlyzVal *args, uint16_t argc) {
    if (argc < 3 || args[0].type != SLYZ_T_STR || !args[0].u.obj ||
        args[1].type != SLYZ_T_STR || !args[1].u.obj ||
        args[2].type != SLYZ_T_STR || !args[2].u.obj) {
        return v_new_str(vm, "", 0);
    }
    const char *s = (const char *)args[0].u.obj->data;
    int sl = (int)args[0].u.obj->len;
    const char *from = (const char *)args[1].u.obj->data;
    int fl = (int)args[1].u.obj->len;
    const char *to = (const char *)args[2].u.obj->data;
    int tl = (int)args[2].u.obj->len;
    if (fl <= 0) return v_new_str(vm, s, (uint32_t)sl);

    int count = 0;
    for (int i = 0; i + fl <= sl;) {
        int k = 0;
        while (k < fl && s[i + k] == from[k]) k++;
        if (k == fl) { count++; i += fl; } else i++;
    }

    int out_len = sl + count * (tl - fl);
    if (out_len < 0) out_len = 0;
    if (out_len > 65536) return v_new_str(vm, s, (uint32_t)sl);
    char *tmp = (char *)v3_alloc(vm, (uint32_t)out_len + 1);
    if (!tmp) return v_nil();

    int p = 0;
    for (int i = 0; i < sl;) {
        int k = 0;
        while (k < fl && i + k < sl && s[i + k] == from[k]) k++;
        if (k == fl) {
            for (int j = 0; j < tl; j++) tmp[p++] = to[j];
            i += fl;
        } else {
            tmp[p++] = s[i++];
        }
    }
    tmp[p] = '\0';
    struct SlyzVal r = v_new_str(vm, tmp, (uint32_t)p);
    v3_free(vm, tmp);
    return r;
}

static struct SlyzVal bif_split(struct Slyz3Vm *vm, struct SlyzVal *args, uint16_t argc) {
    if (argc < 2 || args[0].type != SLYZ_T_STR || !args[0].u.obj ||
        args[1].type != SLYZ_T_STR || !args[1].u.obj) {
        return v_new_arr(vm, 0);
    }
    const char *s = (const char *)args[0].u.obj->data;
    int sl = (int)args[0].u.obj->len;
    const char *sep = (const char *)args[1].u.obj->data;
    int pl = (int)args[1].u.obj->len;

    struct SlyzVal arr = v_new_arr(vm, 0);
    if (arr.type != SLYZ_T_ARR) return arr;

    if (pl <= 0) {
        struct SlyzVal item = v_new_str(vm, s, (uint32_t)sl);
        /* 直接追加：复用 S3_APPEND 的数组扩容逻辑在下面实现 */
        struct SlyzObj *o = arr.u.obj;
        ((struct SlyzVal *)o->data)[0] = item;
        o->len = 1;
        return arr;
    }

    int start = 0;
    for (int i = 0; i + pl <= sl;) {
        int k = 0;
        while (k < pl && s[i + k] == sep[k]) k++;
        if (k == pl) {
            struct SlyzVal item = v_new_str(vm, s + start, (uint32_t)(i - start));
            struct SlyzObj *o = arr.u.obj;
            if (o->len < o->cap) {
                ((struct SlyzVal *)o->data)[o->len++] = item;
            } else {
                v_release(vm, item);
            }
            i += pl;
            start = i;
        } else {
            i++;
        }
    }
    struct SlyzVal item = v_new_str(vm, s + start, (uint32_t)(sl - start));
    struct SlyzObj *o = arr.u.obj;
    if (o->len < o->cap) ((struct SlyzVal *)o->data)[o->len++] = item;
    else v_release(vm, item);
    return arr;
}

static struct SlyzVal bif_join(struct Slyz3Vm *vm, struct SlyzVal *args, uint16_t argc) {
    if (argc < 2 || args[0].type != SLYZ_T_ARR || !args[0].u.obj ||
        args[1].type != SLYZ_T_STR || !args[1].u.obj) {
        return v_new_str(vm, "", 0);
    }
    struct SlyzObj *o = args[0].u.obj;
    struct SlyzVal *items = (struct SlyzVal *)o->data;
    const char *sep = (const char *)args[1].u.obj->data;
    int sepl = (int)args[1].u.obj->len;

    int total = 0;
    for (uint32_t i = 0; i < o->len; i++) {
        char tmp[256];
        total += v_render(vm, &items[i], tmp, (int)sizeof(tmp), 0);
        if (i + 1 < o->len) total += sepl;
    }

    char *buf = (char *)v3_alloc(vm, (uint32_t)total + 1);
    if (!buf) return v_nil();
    int p = 0;
    for (uint32_t i = 0; i < o->len; i++) {
        p += v_render(vm, &items[i], buf + p, total + 1 - p, 0);
        if (i + 1 < o->len) {
            for (int k = 0; k < sepl; k++) buf[p++] = sep[k];
        }
    }
    buf[p] = '\0';
    struct SlyzVal r = v_new_str(vm, buf, (uint32_t)p);
    v3_free(vm, buf);
    return r;
}

static struct SlyzVal bif_str_at(struct Slyz3Vm *vm, struct SlyzVal *args, uint16_t argc) {
    if (argc < 2 || args[0].type != SLYZ_T_STR || !args[0].u.obj) {
        return v_new_str(vm, "", 0);
    }
    int64_t idx = v_number(&args[1]);
    uint32_t n = args[0].u.obj->len;
    if (idx < 0 || (uint64_t)idx >= n) return v_new_str(vm, "", 0);
    return v_new_str(vm, (const char *)args[0].u.obj->data + idx, 1);
}

static struct SlyzVal bif_ord(struct Slyz3Vm *vm, struct SlyzVal *args, uint16_t argc) {
    if (argc < 1 || args[0].type != SLYZ_T_STR || !args[0].u.obj ||
        args[0].u.obj->len == 0) {
        return v_int(0);
    }
    /* 解码 UTF-8 首字符 */
    const uint8_t *s = (const uint8_t *)args[0].u.obj->data;
    uint32_t cp = s[0];
    int extra = 0;
    if ((s[0] & 0xE0u) == 0xC0u) { cp = s[0] & 0x1Fu; extra = 1; }
    else if ((s[0] & 0xF0u) == 0xE0u) { cp = s[0] & 0x0Fu; extra = 2; }
    else if ((s[0] & 0xF8u) == 0xF0u) { cp = s[0] & 0x07u; extra = 3; }
    for (int i = 1; i <= extra && (uint32_t)i < args[0].u.obj->len; i++) {
        cp = (cp << 6) | (uint32_t)(s[i] & 0x3Fu);
    }
    return v_int((int64_t)cp);
}

static struct SlyzVal bif_chr(struct Slyz3Vm *vm, struct SlyzVal *args, uint16_t argc) {
    uint32_t cp = argc >= 1 ? (uint32_t)v_number(&args[0]) : 0;
    char b[5];
    int n = 0;
    if (cp < 0x80u) {
        b[n++] = (char)cp;
    } else if (cp < 0x800u) {
        b[n++] = (char)(0xC0u | (cp >> 6));
        b[n++] = (char)(0x80u | (cp & 0x3Fu));
    } else if (cp < 0x10000u) {
        b[n++] = (char)(0xE0u | (cp >> 12));
        b[n++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        b[n++] = (char)(0x80u | (cp & 0x3Fu));
    } else {
        b[n++] = (char)(0xF0u | (cp >> 18));
        b[n++] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
        b[n++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        b[n++] = (char)(0x80u | (cp & 0x3Fu));
    }
    return v_new_str(vm, b, (uint32_t)n);
}

static struct SlyzVal bif_radix(struct Slyz3Vm *vm, struct SlyzVal *args,
                                uint16_t argc, int base) {
    char tmp[80];
    int p = 0;
    int64_t x = argc >= 1 ? v_number(&args[0]) : 0;
    uint64_t u;
    int neg = 0;
    if (x < 0) { neg = 1; u = (uint64_t)(-(x + 1)) + 1u; } else u = (uint64_t)x;

    if (base == 16) { tmp[p++] = '0'; tmp[p++] = 'x'; }
    else { tmp[p++] = '0'; tmp[p++] = 'b'; }
    if (neg) tmp[p++] = '-';

    char digits[64];
    int n = 0;
    if (u == 0) digits[n++] = '0';
    while (u > 0 && n < 64) {
        int d = (int)(u % (uint64_t)base);
        digits[n++] = (char)(d < 10 ? '0' + d : 'A' + (d - 10));
        u /= (uint64_t)base;
    }
    while (n > 0) tmp[p++] = digits[--n];
    return v_new_str(vm, tmp, (uint32_t)p);
}

static struct SlyzVal bif_typeof(struct Slyz3Vm *vm, struct SlyzVal *args, uint16_t argc) {
    (void)vm;
    if (argc < 1) return v_int(SLYZ_T_NIL);
    return v_int(args[0].type);
}

static struct SlyzVal bif_array(struct Slyz3Vm *vm, struct SlyzVal *args, uint16_t argc) {
    struct SlyzVal arr = v_new_arr(vm, argc);
    if (arr.type != SLYZ_T_ARR) return arr;
    struct SlyzObj *o = arr.u.obj;
    for (uint16_t i = 0; i < argc; i++) {
        struct SlyzVal v = args[i];
        v_retain(v);
        ((struct SlyzVal *)o->data)[i] = v;
    }
    o->len = argc;
    return arr;
}

static struct SlyzVal bif_range(struct Slyz3Vm *vm, struct SlyzVal *args, uint16_t argc) {
    int64_t lo = argc >= 1 ? v_number(&args[0]) : 0;
    int64_t hi = argc >= 2 ? v_number(&args[1]) : 0;
    if (hi < lo) hi = lo;
    if (hi - lo > 4096) hi = lo + 4096;
    uint32_t n = (uint32_t)(hi - lo);
    struct SlyzVal arr = v_new_arr(vm, n);
    if (arr.type != SLYZ_T_ARR) return arr;
    struct SlyzVal *items = (struct SlyzVal *)arr.u.obj->data;
    for (uint32_t i = 0; i < n; i++) items[i] = v_int(lo + (int64_t)i);
    arr.u.obj->len = n;
    return arr;
}

static struct SlyzVal bif_bif(struct Slyz3Vm *vm, uint16_t id,
                              struct SlyzVal *args, uint16_t argc) {
    struct SlyzVal nilv = v_nil();

    switch (id) {
        case SLE_BIF_LEN:      return bif_len(vm, args, argc);
        case SLE_BIF_STR:      return bif_str(vm, args, argc);
        case SLE_BIF_INT:      return bif_int(vm, args, argc);
        case SLE_BIF_SUBSTR:   return bif_substr(vm, args, argc);
        case SLE_BIF_INDEXOF:  return bif_indexof(vm, args, argc);
        case SLE_BIF_UPPER:    return bif_case(vm, args, argc, 1);
        case SLE_BIF_LOWER:    return bif_case(vm, args, argc, 0);
        case SLE_BIF_TRIM:     return bif_trim(vm, args, argc);
        case SLE_BIF_REPLACE:  return bif_replace(vm, args, argc);
        case SLE_BIF_SPLIT:    return bif_split(vm, args, argc);
        case SLE_BIF_JOIN:     return bif_join(vm, args, argc);
        case SLE_BIF_STR_AT:   return bif_str_at(vm, args, argc);
        case SLE_BIF_ORD:      return bif_ord(vm, args, argc);
        case SLE_BIF_CHR:      return bif_chr(vm, args, argc);
        case SLE_BIF_HEX:      return bif_radix(vm, args, argc, 16);
        case SLE_BIF_BIN:      return bif_radix(vm, args, argc, 2);
        case SLE_BIF_TYPEOF:   return bif_typeof(vm, args, argc);
        case SLE_BIF_ARRAY:    return bif_array(vm, args, argc);
        case SLE_BIF_RANGE:    return bif_range(vm, args, argc);

        case SLE_BIF_ABS:
            return v_int(argc >= 1 ? (v_number(&args[0]) < 0
                                     ? -v_number(&args[0])
                                     : v_number(&args[0])) : 0);
        case SLE_BIF_MIN: {
            if (argc < 2) return argc ? args[0] : nilv;
            int64_t a = v_number(&args[0]), b = v_number(&args[1]);
            return v_int(a < b ? a : b);
        }
        case SLE_BIF_MAX: {
            if (argc < 2) return argc ? args[0] : nilv;
            int64_t a = v_number(&args[0]), b = v_number(&args[1]);
            return v_int(a > b ? a : b);
        }
        case SLE_BIF_CLAMP: {
            if (argc < 3) return argc ? args[0] : nilv;
            int64_t v = v_number(&args[0]);
            int64_t lo = v_number(&args[1]);
            int64_t hi = v_number(&args[2]);
            if (v < lo) v = lo;
            if (v > hi) v = hi;
            return v_int(v);
        }
        case SLE_BIF_SIGN: {
            int64_t v = argc ? v_number(&args[0]) : 0;
            return v_int(v > 0 ? 1 : (v < 0 ? -1 : 0));
        }
        case SLE_BIF_RANDOM: {
            uint32_t r = vm->rng ? vm->rng : 0x13579BDFu;
            r ^= r << 13; r ^= r >> 17; r ^= r << 5;
            vm->rng = r;
            int64_t lo = argc >= 1 ? v_number(&args[0]) : 0;
            int64_t hi = argc >= 2 ? v_number(&args[1]) : 32767;
            if (hi <= lo) return v_int(lo);
            return v_int(lo + (int64_t)(r % (uint32_t)(hi - lo)));
        }
        case SLE_BIF_PUSH: {
            if (argc < 2 || args[0].type != SLYZ_T_ARR || !args[0].u.obj) {
                return nilv;
            }
            struct SlyzObj *o = args[0].u.obj;
            if (o->len >= o->cap) {
                uint32_t newcap = o->cap ? o->cap * 2 : 4;
                if (newcap > 4096) newcap = 4096;
                if (newcap <= o->len) return args[0];
                struct SlyzVal *items = (struct SlyzVal *)v3_alloc(
                    vm, newcap * (uint32_t)sizeof(struct SlyzVal));
                if (!items) return args[0];
                v3_memset(items, 0, newcap * (uint32_t)sizeof(struct SlyzVal));
                if (o->data && o->len) {
                    v3_memcpy(items, o->data,
                              o->len * (uint32_t)sizeof(struct SlyzVal));
                }
                if (o->data) v3_free(vm, o->data);
                o->data = items;
                o->cap = newcap;
            }
            struct SlyzVal v = args[1];
            v_retain(v);
            ((struct SlyzVal *)o->data)[o->len++] = v;
            return args[0];
        }
        case SLE_BIF_POP: {
            if (argc < 1 || args[0].type != SLYZ_T_ARR || !args[0].u.obj) {
                return nilv;
            }
            struct SlyzObj *o = args[0].u.obj;
            if (o->len == 0) return nilv;
            struct SlyzVal v = ((struct SlyzVal *)o->data)[o->len - 1];
            ((struct SlyzVal *)o->data)[o->len - 1] = v_nil();
            o->len--;
            return v;
        }
        case SLE_BIF_COUNT:
            return bif_len(vm, args, argc);
        default:
            return nilv;
    }
}

/* ============================================================
 * 算术与比较
 * ============================================================ */

static struct SlyzVal v_add(struct Slyz3Vm *vm, struct SlyzVal a, struct SlyzVal b) {
    if (a.type == SLYZ_T_INT && b.type == SLYZ_T_INT) {
        return v_int(a.u.i + b.u.i);
    }

    char ba[512], bb[512];
    int na = v_render(vm, &a, ba, (int)sizeof(ba), 0);
    int nb = v_render(vm, &b, bb, (int)sizeof(bb), 0);
    char *buf = (char *)v3_alloc(vm, (uint32_t)(na + nb) + 1);
    if (!buf) return v_nil();
    if (na) v3_memcpy(buf, ba, (uint32_t)na);
    if (nb) v3_memcpy(buf + na, bb, (uint32_t)nb);
    buf[na + nb] = '\0';
    struct SlyzVal r = v_new_str(vm, buf, (uint32_t)(na + nb));
    v3_free(vm, buf);
    return r;
}

static int v_compare(struct Slyz3Vm *vm, struct SlyzVal a, struct SlyzVal b) {
    if (a.type == SLYZ_T_STR && b.type == SLYZ_T_STR) {
        const char *sa = a.u.obj ? (const char *)a.u.obj->data : "";
        const char *sb = b.u.obj ? (const char *)b.u.obj->data : "";
        return v3_strcmp(sa, sb);
    }
    int64_t x = v_number(&a);
    int64_t y = v_number(&b);
    (void)vm;
    if (x < y) return -1;
    if (x > y) return 1;
    return 0;
}

/* ============================================================
 * 镜像加载
 * ============================================================ */

int slyz3_is_binary(const uint8_t *data, int size) {
    if (!data || size < SLYZ3_HEADER_SIZE) return 0;
    return data[0] == SLYZ3_MAGIC0 && data[1] == SLYZ3_MAGIC1 &&
           data[2] == SLYZ3_MAGIC2 && data[3] == SLYZ3_MAGIC3 &&
           data[4] == SLYZ3_MAGIC4 && data[5] == SLYZ3_MAGIC5 &&
           data[6] == SLYZ3_MAGIC6 && data[7] == SLYZ3_MAGIC7;
}

static uint32_t fnv1a(const uint8_t *data, uint32_t len) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < len; i++) {
        h ^= data[i];
        h *= 16777619u;
    }
    return h;
}

struct Slyz3Vm *slyz3vm_create(const uint8_t *image, int size,
                               const struct Slyz3Ops *ops, const char *name) {
    if (!image || !ops || !ops->alloc || size < SLYZ3_HEADER_SIZE) return 0;
    if (!slyz3_is_binary(image, size)) return 0;
    if (rd16(image + SLYZ3_HDR_VERSION) != SLYZ3_VERSION) return 0;

    uint32_t const_size = rd32(image + SLYZ3_HDR_CONSTSIZE);
    uint32_t code_size = rd32(image + SLYZ3_HDR_CODESIZE);
    uint32_t func_count = rd32(image + SLYZ3_HDR_FUNCS);
    uint32_t regs = rd32(image + SLYZ3_HDR_REGS);
    uint32_t globals = rd32(image + SLYZ3_HDR_GLOBALS);

    if (const_size > SLYZ3_MAX_CONST || code_size > SLYZ3_MAX_CODE) return 0;
    if (func_count > SLYZ3_MAX_FUNCS || globals > SLYZ3_MAX_GLOBALS) return 0;
    if (regs == 0 || regs > SLYZ3_REGS_PER_FRAME) return 0;

    uint32_t need = SLYZ3_HEADER_SIZE + func_count * SLYZ3_FUNC_ENTRY +
                    const_size + code_size;
    if ((uint32_t)size < need) return 0;

    const uint8_t *funcs = image + SLYZ3_HEADER_SIZE;
    const uint8_t *consts = funcs + func_count * SLYZ3_FUNC_ENTRY;
    const uint8_t *code = consts + const_size;

    uint32_t checksum = fnv1a(consts, const_size + code_size);
    if (checksum != rd32(image + SLYZ3_HDR_CHECKSUM)) return 0;

    struct Slyz3Vm *vm = (struct Slyz3Vm *)ops->alloc((uint32_t)sizeof(struct Slyz3Vm));
    if (!vm) return 0;
    v3_memset(vm, 0, (uint32_t)sizeof(struct Slyz3Vm));

    vm->ops = ops;
    vm->image = image;
    vm->image_size = (uint32_t)size;
    vm->consts = consts;
    vm->const_size = const_size;
    vm->code = code;
    vm->code_size = code_size;
    vm->funcs = funcs;
    vm->func_count = func_count;
    vm->entry = rd32(image + SLYZ3_HDR_ENTRY);
    vm->globals_count = globals;
    vm->regs_per_frame = regs;
    vm->uses_gui = (rd16(image + SLYZ3_HDR_FLAGS) & SLYZ3_FLAG_GUI) ? 1 : 0;
    vm->pc = vm->entry;
    vm->depth = 0;
    vm->state = 0;
    vm->rng = 0x2545F491u ^ (uint32_t)(uintptr_t)vm;

    uint32_t reg_slots = (uint32_t)(SLYZ3_MAX_FRAMES + 1) * regs;
    vm->regs = (struct SlyzVal *)ops->alloc(reg_slots * (uint32_t)sizeof(struct SlyzVal));
    vm->globals = (struct SlyzVal *)ops->alloc(
        (globals ? globals : 1) * (uint32_t)sizeof(struct SlyzVal));
    if (!vm->regs || !vm->globals) {
        if (vm->regs) ops->free(vm->regs);
        if (vm->globals) ops->free(vm->globals);
        ops->free(vm);
        return 0;
    }
    v3_memset(vm->regs, 0, reg_slots * (uint32_t)sizeof(struct SlyzVal));
    v3_memset(vm->globals, 0, (globals ? globals : 1) * (uint32_t)sizeof(struct SlyzVal));

    /* 常量池是连续的 NUL 结尾字符串；为每个入口建立静态对象。 */
    uint32_t count = 0;
    for (uint32_t off = 0; off < const_size;) {
        count++;
        while (off < const_size && consts[off]) off++;
        off++;
    }
    vm->const_vals = (struct SlyzVal *)ops->alloc(
        (count ? count : 1) * (uint32_t)sizeof(struct SlyzVal));
    vm->const_offs = (uint32_t *)ops->alloc((count ? count : 1) * sizeof(uint32_t));
    if (!vm->const_vals || !vm->const_offs) {
        slyz3vm_destroy(vm);
        return 0;
    }
    v3_memset(vm->const_vals, 0, (count ? count : 1) * (uint32_t)sizeof(struct SlyzVal));
    vm->const_count = 0;

    for (uint32_t off = 0; off < const_size;) {
        uint32_t start = off;
        while (off < const_size && consts[off]) off++;
        uint32_t len = off - start;
        struct SlyzVal v = v_new_str(vm, (const char *)consts + start, len);
        if (v.type != SLYZ_T_STR) {
            slyz3vm_destroy(vm);
            return 0;
        }
        v.u.obj->flags |= SLYZ_OBJ_STATIC;
        vm->const_vals[vm->const_count] = v;
        vm->const_offs[vm->const_count] = start;
        vm->const_count++;
        off++;
    }

    if (!name) name = "slyz3";
    vm->name = name;
    return vm;
}

void slyz3vm_destroy(struct Slyz3Vm *vm) {
    if (!vm) return;
    const Slyz3Ops *ops = vm->ops;

    if (vm->regs) {
        uint32_t slots = (uint32_t)(SLYZ3_MAX_FRAMES + 1) * vm->regs_per_frame;
        for (uint32_t i = 0; i < slots; i++) v_release(vm, vm->regs[i]);
        ops->free(vm->regs);
    }
    if (vm->globals) {
        for (uint32_t i = 0; i < vm->globals_count; i++) v_release(vm, vm->globals[i]);
        ops->free(vm->globals);
    }
    if (vm->const_vals) {
        for (uint32_t i = 0; i < vm->const_count; i++) {
            struct SlyzVal v = vm->const_vals[i];
            if (v.u.obj) {
                if (v.u.obj->data) ops->free(v.u.obj->data);
                ops->free(v.u.obj);
            }
        }
        ops->free(vm->const_vals);
    }
    if (vm->const_offs) ops->free(vm->const_offs);
    ops->free(vm);
}

/* ============================================================
 * 系统调用与返回
 * ============================================================ */

void slyz3_ret_nil(struct SlyzVal *ret) { if (ret) *ret = v_nil(); }
void slyz3_ret_int(struct SlyzVal *ret, int64_t x) { if (ret) *ret = v_int(x); }

int slyz3_ret_str(struct Slyz3Vm *vm, struct SlyzVal *ret, const char *s, int len) {
    if (!ret) return 0;
    if (len < 0) len = v3_strlen(s);
    struct SlyzVal v = v_new_str(vm, s, (uint32_t)len);
    if (v.type != SLYZ_T_STR) return 0;
    *ret = v;
    return 1;
}

int slyz3_ret_arr(struct Slyz3Vm *vm, struct SlyzVal *ret, int count) {
    if (!ret || count < 0) return 0;
    struct SlyzVal v = v_new_arr(vm, (uint32_t)count);
    if (v.type != SLYZ_T_ARR) return 0;
    v.u.obj->len = 0;
    *ret = v;
    return 1;
}

int slyz3_arr_set(struct Slyz3Vm *vm, struct SlyzVal *arr, int index,
                  struct SlyzVal value) {
    (void)vm;
    if (!arr || arr->type != SLYZ_T_ARR || !arr->u.obj) return 0;
    if (index < 0 || (uint32_t)index >= arr->u.obj->cap) return 0;
    struct SlyzVal *items = (struct SlyzVal *)arr->u.obj->data;
    v_retain(value);
    items[index] = value;
    if ((uint32_t)index >= arr->u.obj->len) arr->u.obj->len = (uint32_t)index + 1;
    return 1;
}

/* ============================================================
 * 解释器
 * ============================================================ */

static int vm_fail(struct Slyz3Vm *vm, const char *msg) {
    vm->state = -1;
    vm->error = msg;
    if (vm->ops && vm->ops->log && msg) vm->ops->log(msg);
    return vm->state;
}

static int call_function(struct Slyz3Vm *vm, uint32_t fn, uint32_t base, uint32_t argc) {
    if (fn >= vm->func_count) return vm_fail(vm, "call: function index out of range");
    if (vm->depth + 1 >= SLYZ3_MAX_FRAMES) return vm_fail(vm, "call: stack overflow");

    const uint8_t *f = vm->funcs + fn * SLYZ3_FUNC_ENTRY;
    uint32_t entry = rd32(f + 0);
    uint32_t want = rd32(f + 4);      /* 参数个数 */
    if (argc != want) return vm_fail(vm, "call: argument count mismatch");
    if (entry >= vm->code_size) return vm_fail(vm, "call: entry out of range");

    uint32_t newdepth = (uint32_t)vm->depth + 1;
    uint32_t newbase = newdepth * vm->regs_per_frame;

    /* 保存调用现场：frame_base 不能被覆盖，返回值目标单独记录 */
    vm->frame_pc[vm->depth] = vm->pc;
    vm->call_ret[vm->depth] = base;

    /* 复制参数到被调帧 r0..argc-1，其余寄存器清空 */
    for (uint32_t i = 0; i < vm->regs_per_frame; i++) {
        if (i < argc) {
            struct SlyzVal v = vm->regs[base + i];
            v_retain(v);
            v_release(vm, vm->regs[newbase + i]);
            vm->regs[newbase + i] = v;
        } else {
            reg_clear(vm, newbase + i);
        }
    }

    vm->depth = (int)newdepth;
    vm->base[newdepth] = newbase;
    vm->pc = entry;
    return 0;
}

int slyz3vm_step(struct Slyz3Vm *vm, uint32_t budget) {
    if (!vm) return -1;
    if (vm->state != 0) return (vm->state == 1) ? 0 : vm->state;

    uint32_t executed = 0;
    while (executed < budget) {
        if (vm->pc + SLYZ3_INSN_SIZE > vm->code_size) {
            return vm_fail(vm, "instruction pointer out of range");
        }

        const uint8_t *ip = vm->code + vm->pc;
        uint8_t op = ip[0];
        uint8_t a = ip[1];
        uint8_t b = ip[2];
        uint8_t c = ip[3];
        uint32_t imm = rd32(ip + 4);
        uint32_t ib = frame_base(vm);

        vm->pc += SLYZ3_INSN_SIZE;
        executed++;
        vm->steps++;

        if (vm->steps > SLYZ3_MAX_STEPS) return vm_fail(vm, "step budget exceeded");
        if ((vm->steps & 0x3FFu) == 0 && vm->ops->tick) vm->ops->tick();

        if (a >= vm->regs_per_frame &&
            op != S3_JMP && op != S3_CALL && op != S3_HALT) {
            return vm_fail(vm, "register index out of range");
        }

        switch (op) {
            case S3_NOP:
                break;

            case S3_MOVE:
                reg_write(vm, ib + a, vm->regs[ib + b]);
                break;

            case S3_LOADI:
                reg_write(vm, ib + a, v_int((int64_t)(int32_t)imm));
                break;

            case S3_LOADNIL:
                reg_write(vm, ib + a, v_nil());
                break;

            case S3_LOADK: {
                struct SlyzVal v = v_nil();
                for (uint32_t i = 0; i < vm->const_count; i++) {
                    if (vm->const_offs[i] == imm) {
                        v = vm->const_vals[i];
                        break;
                    }
                }
                reg_write(vm, ib + a, v);
                break;
            }

            case S3_LOADG:
                if (imm >= vm->globals_count) return vm_fail(vm, "global slot out of range");
                reg_write(vm, ib + a, vm->globals[imm]);
                break;

            case S3_STOREG: {
                if (imm >= vm->globals_count) return vm_fail(vm, "global slot out of range");
                struct SlyzVal v = vm->regs[ib + a];
                v_retain(v);
                v_release(vm, vm->globals[imm]);
                vm->globals[imm] = v;
                break;
            }

            case S3_ADD:
                reg_write(vm, ib + a, v_add(vm, vm->regs[ib + b], vm->regs[ib + c]));
                break;

            case S3_SUB:
            case S3_MUL:
            case S3_DIV:
            case S3_MOD: {
                int64_t x = v_number(&vm->regs[ib + b]);
                int64_t y = v_number(&vm->regs[ib + c]);
                int64_t r = 0;
                if (op == S3_SUB) r = x - y;
                else if (op == S3_MUL) r = x * y;
                else if (op == S3_DIV) {
                    if (y == 0) return vm_fail(vm, "division by zero");
                    r = x / y;
                } else {
                    if (y == 0) return vm_fail(vm, "division by zero");
                    r = x % y;
                }
                reg_write(vm, ib + a, v_int(r));
                break;
            }

            case S3_NEG:
                reg_write(vm, ib + a, v_int(-v_number(&vm->regs[ib + b])));
                break;

            case S3_NOT:
                reg_write(vm, ib + a, v_int(v_truthy(&vm->regs[ib + b]) ? 0 : 1));
                break;

            case S3_EQ:
            case S3_NE:
            case S3_LT:
            case S3_LE:
            case S3_GT:
            case S3_GE: {
                int cmp;
                if (op == S3_EQ || op == S3_NE) {
                    struct SlyzVal x = vm->regs[ib + b];
                    struct SlyzVal y = vm->regs[ib + c];
                    int eq = 0;
                    if (x.type == SLYZ_T_INT && y.type == SLYZ_T_INT) {
                        eq = x.u.i == y.u.i;
                    } else if (x.type == SLYZ_T_STR && y.type == SLYZ_T_STR) {
                        eq = v_compare(vm, x, y) == 0;
                    } else if (x.type == SLYZ_T_NIL && y.type == SLYZ_T_NIL) {
                        eq = 1;
                    } else {
                        eq = 0;
                    }
                    reg_write(vm, ib + a, v_int((op == S3_EQ) ? eq : !eq));
                    break;
                }
                cmp = v_compare(vm, vm->regs[ib + b], vm->regs[ib + c]);
                int r = 0;
                if (op == S3_LT) r = cmp < 0;
                else if (op == S3_LE) r = cmp <= 0;
                else if (op == S3_GT) r = cmp > 0;
                else r = cmp >= 0;
                reg_write(vm, ib + a, v_int(r));
                break;
            }

            case S3_JMP:
                if (imm >= vm->code_size) return vm_fail(vm, "jump target out of range");
                vm->pc = imm;
                break;

            case S3_JZ:
            case S3_JNZ: {
                int t = v_truthy(&vm->regs[ib + a]);
                if ((op == S3_JZ && !t) || (op == S3_JNZ && t)) {
                    if (imm >= vm->code_size) return vm_fail(vm, "jump target out of range");
                    vm->pc = imm;
                }
                break;
            }

            case S3_CALL:
                if ((uint32_t)b + imm > vm->regs_per_frame) {
                    return vm_fail(vm, "call: argument base out of range");
                }
                if (call_function(vm, a, ib + b, imm) != 0) return vm->state;
                break;

            case S3_RET: {
                struct SlyzVal result = vm->regs[ib + a];
                v_retain(result);

                if (vm->depth == 0) {
                    v_release(vm, result);
                    vm->state = 1;
                    return 0;
                }

                uint32_t cur = (uint32_t)vm->depth;
                uint32_t caller_depth = cur - 1;
                uint32_t caller_base = vm->call_ret[caller_depth];
                uint32_t caller_pc = vm->frame_pc[caller_depth];

                /* 先清空当前帧，避免返回后再释放 result 指向的同一对象 */
                for (uint32_t i = 0; i < vm->regs_per_frame; i++) {
                    reg_clear(vm, ib + i);
                }

                vm->depth = (int)caller_depth;
                reg_write(vm, caller_base, result);
                v_release(vm, result);
                vm->pc = caller_pc;
                break;
            }

            case S3_NEWARR: {
                if (b + imm > vm->regs_per_frame) return vm_fail(vm, "array base out of range");
                struct SlyzVal arr = v_new_arr(vm, imm);
                if (arr.type != SLYZ_T_ARR) return vm_fail(vm, "out of memory (array)");
                struct SlyzObj *o = arr.u.obj;
                for (uint32_t i = 0; i < imm; i++) {
                    struct SlyzVal v = vm->regs[ib + b + i];
                    v_retain(v);
                    ((struct SlyzVal *)o->data)[i] = v;
                }
                o->len = imm;
                reg_write(vm, ib + a, arr);
                v_release(vm, arr);
                break;
            }

            case S3_IDXGET: {
                struct SlyzVal arr = vm->regs[ib + b];
                int64_t idx = v_number(&vm->regs[ib + c]);
                struct SlyzVal v = v_nil();
                if (arr.type == SLYZ_T_ARR && arr.u.obj && idx >= 0 &&
                    (uint64_t)idx < arr.u.obj->len) {
                    v = ((struct SlyzVal *)arr.u.obj->data)[idx];
                }
                reg_write(vm, ib + a, v);
                break;
            }

            case S3_IDXSET: {
                struct SlyzVal arr = vm->regs[ib + a];
                int64_t idx = v_number(&vm->regs[ib + b]);
                struct SlyzVal val = vm->regs[ib + c];
                if (arr.type == SLYZ_T_ARR && arr.u.obj && idx >= 0 &&
                    (uint64_t)idx < arr.u.obj->cap) {
                    struct SlyzVal *items = (struct SlyzVal *)arr.u.obj->data;
                    v_retain(val);
                    v_release(vm, items[idx]);
                    items[idx] = val;
                    if ((uint32_t)idx >= arr.u.obj->len) arr.u.obj->len = (uint32_t)idx + 1;
                }
                break;
            }

            case S3_LEN: {
                struct SlyzVal v = vm->regs[ib + b];
                int64_t n = 0;
                if (v.type == SLYZ_T_STR && v.u.obj) n = v.u.obj->len;
                else if (v.type == SLYZ_T_ARR && v.u.obj) n = v.u.obj->len;
                reg_write(vm, ib + a, v_int(n));
                break;
            }

            case S3_TYPEOF:
                reg_write(vm, ib + a, v_int(vm->regs[ib + b].type));
                break;

            case S3_APPEND: {
                struct SlyzVal arr = vm->regs[ib + a];
                if (arr.type == SLYZ_T_ARR && arr.u.obj) {
                    struct SlyzObj *o = arr.u.obj;
                    if (o->len >= o->cap) {
                        uint32_t newcap = o->cap ? o->cap * 2 : 4;
                        if (newcap > 4096) newcap = 4096;
                        if (newcap > o->len) {
                            struct SlyzVal *items = (struct SlyzVal *)v3_alloc(
                                vm, newcap * (uint32_t)sizeof(struct SlyzVal));
                            if (items) {
                                v3_memset(items, 0,
                                          newcap * (uint32_t)sizeof(struct SlyzVal));
                                if (o->data && o->len) {
                                    v3_memcpy(items, o->data,
                                              o->len * (uint32_t)sizeof(struct SlyzVal));
                                }
                                if (o->data) v3_free(vm, o->data);
                                o->data = items;
                                o->cap = newcap;
                            }
                        }
                    }
                    if (o->len < o->cap) {
                        struct SlyzVal v = vm->regs[ib + b];
                        v_retain(v);
                        ((struct SlyzVal *)o->data)[o->len++] = v;
                    }
                }
                break;
            }

            case S3_POPARR: {
                struct SlyzVal arr = vm->regs[ib + b];
                struct SlyzVal v = v_nil();
                if (arr.type == SLYZ_T_ARR && arr.u.obj && arr.u.obj->len > 0) {
                    struct SlyzObj *o = arr.u.obj;
                    v = ((struct SlyzVal *)o->data)[o->len - 1];
                    ((struct SlyzVal *)o->data)[o->len - 1] = v_nil();
                    o->len--;
                }
                reg_write(vm, ib + a, v);
                break;
            }

            case S3_RANGE: {
                int64_t lo = v_number(&vm->regs[ib + b]);
                int64_t hi = v_number(&vm->regs[ib + b + 1]);
                if (hi < lo) hi = lo;
                if (hi - lo > 4096) hi = lo + 4096;
                uint32_t n = (uint32_t)(hi - lo);
                struct SlyzVal arr = v_new_arr(vm, n);
                if (arr.type != SLYZ_T_ARR) return vm_fail(vm, "out of memory (range)");
                struct SlyzVal *items = (struct SlyzVal *)arr.u.obj->data;
                for (uint32_t i = 0; i < n; i++) items[i] = v_int(lo + (int64_t)i);
                arr.u.obj->len = n;
                reg_write(vm, ib + a, arr);
                v_release(vm, arr);
                break;
            }

            case S3_SYS: {
                uint16_t id = (uint16_t)(imm >> 16);
                uint16_t argc = (uint16_t)(imm & 0xFFFFu);
                if (b + argc > vm->regs_per_frame) return vm_fail(vm, "sys args out of range");

                struct SlyzVal ret = v_nil();

                if (id >= 100u) {
                    /* 内置函数：参数从寄存器搬运到临时数组 */
                    struct SlyzVal stack_args[8];
                    struct SlyzVal *args = argc <= 8 ? stack_args
                        : (struct SlyzVal *)v3_alloc(vm, argc * (uint32_t)sizeof(struct SlyzVal));
                    if (!args) return vm_fail(vm, "out of memory (sys args)");
                    for (uint16_t i = 0; i < argc; i++) {
                        args[i] = vm->regs[ib + b + i];
                        v_retain(args[i]);
                    }
                    ret = bif_bif(vm, id, args, argc);
                    /* 内置函数可能直接返回某个参数（push/clamp 等），
                     * 必须在释放参数前先把返回值固定住。 */
                    v_retain(ret);
                    for (uint16_t i = 0; i < argc; i++) v_release(vm, args[i]);
                    if (args != stack_args) v3_free(vm, args);
                } else {
                    struct SlyzVal stack_args[8];
                    struct SlyzVal *args = argc <= 8 ? stack_args
                        : (struct SlyzVal *)v3_alloc(vm, argc * (uint32_t)sizeof(struct SlyzVal));
                    if (!args) return vm_fail(vm, "out of memory (sys args)");
                    for (uint16_t i = 0; i < argc; i++) {
                        args[i] = vm->regs[ib + b + i];
                        v_retain(args[i]);
                    }

                    if (vm->ops->sys) {
                        int rc = vm->ops->sys(vm, id, args, argc, &ret);
                        if (rc < 0) {
                            for (uint16_t i = 0; i < argc; i++) v_release(vm, args[i]);
                            if (args != stack_args) v3_free(vm, args);
                            return vm_fail(vm, "host syscall failed");
                        }
                    }
                    for (uint16_t i = 0; i < argc; i++) v_release(vm, args[i]);
                    if (args != stack_args) v3_free(vm, args);
                }

                reg_write(vm, ib + a, ret);
                v_release(vm, ret);

                if (vm->yield_request) {
                    vm->yield_request = 0;
                    return 1;
                }
                break;
            }

            case S3_BAND:
            case S3_BOR:
            case S3_BXOR:
            case S3_SHL:
            case S3_SHR: {
                int64_t x = v_number(&vm->regs[ib + b]);
                int64_t y = v_number(&vm->regs[ib + c]);
                int64_t r = 0;
                if (op == S3_BAND)      r = x & y;
                else if (op == S3_BOR)  r = x | y;
                else if (op == S3_BXOR) r = x ^ y;
                else if (op == S3_SHL) {
                    /* 与 C 一致：移位量按 63 截断，避免 x86 的隐式取模
                     * 让 1 << 64 变成 1 << 0。 */
                    uint32_t s = (uint32_t)(y & 63);
                    r = (int64_t)((uint64_t)x << s);
                } else {
                    uint32_t s = (uint32_t)(y & 63);
                    r = (int64_t)((uint64_t)x >> s);
                }
                reg_write(vm, ib + a, v_int(r));
                break;
            }

            case S3_HALT:
                vm->state = 1;
                return 0;

            default:
                return vm_fail(vm, "unknown opcode");
        }
    }

    return 1;
}

int slyz3vm_run(const uint8_t *image, int size,
                const struct Slyz3Ops *ops, const char *name) {
    struct Slyz3Vm *vm = slyz3vm_create(image, size, ops, name);
    if (!vm) return 0;

    int rc;
    while ((rc = slyz3vm_step(vm, 200000u)) == 1) {
        if (vm->yield_request) {
            vm->yield_request = 0;
            break;
        }
    }
    int ok = (rc == 0);
    slyz3vm_destroy(vm);
    return ok;
}

int slyz3vm_state(const struct Slyz3Vm *vm) { return vm ? vm->state : -1; }
int slyz3vm_exit_code(const struct Slyz3Vm *vm) { return vm ? vm->exit_code : -1; }
const char *slyz3vm_error(const struct Slyz3Vm *vm) {
    return vm ? (vm->error ? vm->error : "no error") : "no vm";
}
uint32_t slyz3vm_steps(const struct Slyz3Vm *vm) { return vm ? vm->steps : 0; }
int slyz3vm_uses_gui(const struct Slyz3Vm *vm) { return vm ? vm->uses_gui : 0; }

void slyz3vm_request_exit(struct Slyz3Vm *vm, int code) {
    if (!vm) return;
    vm->exit_code = code;
    vm->state = 1;
}

void slyz3vm_request_yield(struct Slyz3Vm *vm) {
    if (vm) vm->yield_request = 1;
}
