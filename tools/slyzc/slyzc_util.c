/* ============================================================
 * slyzc_util.c - 编译器基础设施
 * ------------------------------------------------------------
 * 内存分配失败、常量池去重与字符串工具。编译器是短命进程，
 * 语法树与符号不做回收，退出时由操作系统统一释放；这里只
 * 保证分配失败时给出明确诊断而不是生成半个镜像。
 * ============================================================ */

#include "slyzc.h"

void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) {
        fprintf(stderr, "slyzc: out of memory (%lu bytes)\n", (unsigned long)n);
        exit(2);
    }
    return p;
}

void *xcalloc(size_t n, size_t m) {
    void *p = calloc(n ? n : 1, m ? m : 1);
    if (!p) {
        fprintf(stderr, "slyzc: out of memory (%lu x %lu bytes)\n",
                (unsigned long)n, (unsigned long)m);
        exit(2);
    }
    return p;
}

char *xstrdup(const char *s) {
    if (!s) return 0;
    size_t n = strlen(s) + 1;
    char *p = (char *)xmalloc(n);
    memcpy(p, s, n);
    return p;
}

char *xstrndup(const char *s, int n) {
    if (!s) return 0;
    if (n < 0) n = 0;
    char *p = (char *)xmalloc((size_t)n + 1);
    memcpy(p, s, (size_t)n);
    p[n] = '\0';
    return p;
}

void consts_init(ConstPool *p) {
    memset(p, 0, sizeof(*p));
}

static void consts_grow_data(ConstPool *p, uint32_t need) {
    if (p->len + need <= p->cap) return;
    uint32_t next = p->cap ? p->cap * 2 : 4096;
    while (next < p->len + need) next *= 2;
    uint8_t *nd = (uint8_t *)xmalloc(next);
    if (p->data) {
        memcpy(nd, p->data, p->len);
        free(p->data);
    }
    p->data = nd;
    p->cap = next;
}

static void consts_grow_index(ConstPool *p) {
    if (p->count < p->cap_count) return;
    int next = p->cap_count ? p->cap_count * 2 : 64;
    uint32_t *no = (uint32_t *)xmalloc(sizeof(uint32_t) * (size_t)next);
    int *nl = (int *)xmalloc(sizeof(int) * (size_t)next);
    for (int i = 0; i < p->count; i++) {
        no[i] = p->offs[i];
        nl[i] = p->lens[i];
    }
    if (p->offs) free(p->offs);
    if (p->lens) free(p->lens);
    p->offs = no;
    p->lens = nl;
    p->cap_count = next;
}

uint32_t consts_add(ConstPool *p, const char *s, int len, int *out_index) {
    if (len < 0) len = 0;

    for (int i = 0; i < p->count; i++) {
        if (p->lens[i] != len) continue;
        if (len > 0 && memcmp(p->data + p->offs[i], s, (size_t)len) != 0) continue;
        if (out_index) *out_index = i;
        return p->offs[i];
    }

    uint32_t off = p->len;
    consts_grow_data(p, (uint32_t)len + 1);
    if (len > 0) memcpy(p->data + p->len, s, (size_t)len);
    p->data[p->len + (uint32_t)len] = 0;
    p->len += (uint32_t)len + 1;

    consts_grow_index(p);
    p->offs[p->count] = off;
    p->lens[p->count] = len;
    if (out_index) *out_index = p->count;
    p->count++;

    return off;
}
