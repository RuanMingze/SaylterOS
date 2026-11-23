/* ============================================================
 * slyzc_serve.c - 结构化诊断与常驻检查服务
 * ------------------------------------------------------------
 * 编辑器插件不应该自己复刻一遍语法规则：那必然会和编译器漂移。
 * 这里把编译器的诊断变成两种可消费的形式：
 *
 *   1. `slyzc check --json <file>`
 *      一次性检查，输出一行 JSON，供脚本或简单集成使用。
 *
 *   2. `slyzc serve`
 *      常驻进程，stdin/stdout 上的 JSON Lines 协议：
 *        请求：{"id":1,"path":"a.slyzcode","text":"let x = 1\n"}
 *        响应：{"id":1,"ok":true,"errorCount":0,...}
 *      扩展只需启动一次进程，之后每次编辑发一行请求。
 *
 * 常驻进程不做 AST 回收（编译器原本按"进程短命"设计）。为了不让
 * 长时间编辑把内存吃满，进程会统计累计分配量，超过水位后主动退出，
 * 由扩展按需重启；协议里的 restart 字段用于告知这一点。
 * ============================================================ */

#include "slyzc.h"
#include <stdarg.h>
#include <setjmp.h>

extern jmp_buf g_parse_jmp;

/* ---------------- 诊断收集 ---------------- */

static SlyzDiag *g_diags = 0;
static int       g_diag_count = 0;
static int       g_diag_cap = 0;
static int       g_diag_quiet = 0;

void slyz_diag_reset(void) {
    g_diag_count = 0;
}

int slyz_diag_count(void) {
    return g_diag_count;
}

const SlyzDiag *slyz_diag_at(int index) {
    if (index < 0 || index >= g_diag_count) return 0;
    return &g_diags[index];
}

void slyz_diag_set_quiet(int quiet) {
    g_diag_quiet = quiet ? 1 : 0;
}

int slyz_diag_quiet(void) {
    return g_diag_quiet;
}

static void diag_push(int severity, int line, int col, const char *message) {
    if (g_diag_count == g_diag_cap) {
        int next = g_diag_cap ? g_diag_cap * 2 : 16;
        SlyzDiag *nd = (SlyzDiag *)realloc(g_diags, sizeof(SlyzDiag) * (size_t)next);
        if (!nd) {
            fprintf(stderr, "slyzc: out of memory while recording diagnostics\n");
            exit(2);
        }
        g_diags = nd;
        g_diag_cap = next;
    }
    SlyzDiag *d = &g_diags[g_diag_count++];
    d->line = line;
    d->col = col;
    d->severity = severity;
    snprintf(d->message, sizeof(d->message), "%s", message ? message : "");
}

void slyz_diag_record(int severity, int line, int col, const char *message) {
    if (!g_diag_quiet) {
        fprintf(stderr, "%s:%d:%d: %s: %s\n",
                g_cc.path ? g_cc.path : "<source>",
                line, col, severity ? "warning" : "error",
                message ? message : "");
    }
    diag_push(severity, line, col, message);
}

void slyz_diag_emit(int severity, int line, int col, const char *fmt, ...) {
    char msg[SLYZ_DIAG_MSG_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    slyz_diag_record(severity, line, col, msg);
}

/* ---------------- JSON 输出 ---------------- */

void slyz_write_json_string(FILE *out, const char *s) {
    fputc('"', out);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        switch (c) {
            case '"':  fputs("\\\"", out); break;
            case '\\': fputs("\\\\", out); break;
            case '\n': fputs("\\n", out); break;
            case '\r': fputs("\\r", out); break;
            case '\t': fputs("\\t", out); break;
            case '\b': fputs("\\b", out); break;
            case '\f': fputs("\\f", out); break;
            default:
                if (c < 0x20) fprintf(out, "\\u%04X", (unsigned)c);
                else fputc((int)c, out);
                break;
        }
    }
    fputc('"', out);
}

void slyz_write_diagnostics_json(FILE *out) {
    fputc('[', out);
    for (int i = 0; i < g_diag_count; i++) {
        const SlyzDiag *d = &g_diags[i];
        if (i) fputc(',', out);
        fprintf(out, "{\"line\":%d,\"col\":%d,\"severity\":\"%s\",\"message\":",
                d->line, d->col, d->severity ? "warning" : "error");
        slyz_write_json_string(out, d->message);
        fputc('}', out);
    }
    fputc(']', out);
}

void slyz_write_stats_json(FILE *out) {
    fprintf(out,
            "{\"code\":%u,\"const\":%u,\"globals\":%d,\"functions\":%d,\"stack\":%d}",
            (unsigned)g_cc.code_len, (unsigned)g_cc.consts.len,
            g_cc.global_count, g_cc.func_count, g_cc.max_stack + 32);
}

/* ---------------- 编译入口 ---------------- */

int slyz_compile_source(const char *path, const uint8_t *source, uint32_t size) {
    memset(&g_cc, 0, sizeof(g_cc));
    g_cc.path = path;
    g_cc.source = (const char *)source;
    g_cc.source_len = (int)size;
    consts_init(&g_cc.consts);
    slyz_diag_reset();

    if (setjmp(g_parse_jmp) != 0) return 0;

    lex_init((const char *)source, (int)size, path);
    parse_program(&g_cc);
    if (g_cc.error_count) return 0;

    codegen_program(&g_cc);
    if (g_cc.error_count) return 0;
    return 1;
}

/* ---------------- 极简 JSON 解析 ---------------- */

typedef struct {
    const char *p;
} JsonCur;

static void js_skip_ws(JsonCur *c) {
    while (*c->p == ' ' || *c->p == '\t' || *c->p == '\r' || *c->p == '\n') c->p++;
}

static int js_hex4(const char *p, unsigned *out) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char ch = p[i];
        int d;
        if (ch >= '0' && ch <= '9') d = ch - '0';
        else if (ch >= 'a' && ch <= 'f') d = ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F') d = ch - 'A' + 10;
        else return 0;
        v = (v << 4) | (unsigned)d;
    }
    *out = v;
    return 1;
}

static void utf8_put(char **dst, unsigned cp) {
    char *d = *dst;
    if (cp < 0x80) {
        *d++ = (char)cp;
    } else if (cp < 0x800) {
        *d++ = (char)(0xC0 | (cp >> 6));
        *d++ = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        *d++ = (char)(0xE0 | (cp >> 12));
        *d++ = (char)(0x80 | ((cp >> 6) & 0x3F));
        *d++ = (char)(0x80 | (cp & 0x3F));
    } else {
        *d++ = (char)(0xF0 | (cp >> 18));
        *d++ = (char)(0x80 | ((cp >> 12) & 0x3F));
        *d++ = (char)(0x80 | ((cp >> 6) & 0x3F));
        *d++ = (char)(0x80 | (cp & 0x3F));
    }
    *dst = d;
}

/* 解析一个 JSON 字符串；返回 malloc 出来的 UTF-8 缓冲，失败返回 0。 */
static char *js_parse_string(JsonCur *c) {
    js_skip_ws(c);
    if (*c->p != '"') return 0;
    c->p++;

    /* 输出长度不会超过原文长度（转义只会变短或等长）。 */
    size_t cap = strlen(c->p) + 1;
    char *out = (char *)malloc(cap);
    if (!out) return 0;
    char *d = out;

    while (*c->p && *c->p != '"') {
        unsigned char ch = (unsigned char)*c->p++;
        if (ch != '\\') {
            *d++ = (char)ch;
            continue;
        }
        char esc = *c->p++;
        switch (esc) {
            case '"':  *d++ = '"'; break;
            case '\\': *d++ = '\\'; break;
            case '/':  *d++ = '/'; break;
            case 'b':  *d++ = '\b'; break;
            case 'f':  *d++ = '\f'; break;
            case 'n':  *d++ = '\n'; break;
            case 'r':  *d++ = '\r'; break;
            case 't':  *d++ = '\t'; break;
            case 'u': {
                unsigned cp = 0;
                if (!js_hex4(c->p, &cp)) { free(out); return 0; }
                c->p += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF &&
                    c->p[0] == '\\' && c->p[1] == 'u') {
                    unsigned lo = 0;
                    if (js_hex4(c->p + 2, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        c->p += 6;
                    }
                }
                utf8_put(&d, cp);
                break;
            }
            default:
                free(out);
                return 0;
        }
    }
    if (*c->p != '"') { free(out); return 0; }
    c->p++;
    *d = '\0';
    return out;
}

static int js_skip_number(JsonCur *c) {
    const char *start = c->p;
    if (*c->p == '-') c->p++;
    while ((*c->p >= '0' && *c->p <= '9') || *c->p == '.' ||
           *c->p == 'e' || *c->p == 'E' || *c->p == '+' || *c->p == '-') {
        c->p++;
    }
    return c->p != start;
}

static int js_skip_value(JsonCur *c) {
    js_skip_ws(c);
    char ch = *c->p;
    if (ch == '"') {
        char *s = js_parse_string(c);
        if (!s) return 0;
        free(s);
        return 1;
    }
    if (ch == '{' || ch == '[') {
        char close = (ch == '{') ? '}' : ']';
        c->p++;
        js_skip_ws(c);
        if (*c->p == close) { c->p++; return 1; }
        for (;;) {
            if (ch == '{') {
                char *key = js_parse_string(c);
                if (!key) return 0;
                free(key);
                js_skip_ws(c);
                if (*c->p != ':') return 0;
                c->p++;
            }
            if (!js_skip_value(c)) return 0;
            js_skip_ws(c);
            if (*c->p == ',') { c->p++; continue; }
            if (*c->p == close) { c->p++; return 1; }
            return 0;
        }
    }
    if (ch == 't' && strncmp(c->p, "true", 4) == 0)  { c->p += 4; return 1; }
    if (ch == 'f' && strncmp(c->p, "false", 5) == 0) { c->p += 5; return 1; }
    if (ch == 'n' && strncmp(c->p, "null", 4) == 0)  { c->p += 4; return 1; }
    return js_skip_number(c);
}

typedef struct {
    long  id;
    char *method;
    char *path;
    char *text;
} ServeRequest;

static void serve_request_free(ServeRequest *r) {
    free(r->method);
    free(r->path);
    free(r->text);
    memset(r, 0, sizeof(*r));
}

/* 解析 {"id":N,"path":"...","text":"...","method":"..."}。
 * 未知字段用 js_skip_value 跳过，字段顺序任意。 */
static int js_parse_request(const char *line, ServeRequest *out) {
    JsonCur c;
    c.p = line;
    memset(out, 0, sizeof(*out));

    js_skip_ws(&c);
    if (*c.p != '{') return 0;
    c.p++;
    js_skip_ws(&c);
    if (*c.p == '}') return 1;

    for (;;) {
        char *key = js_parse_string(&c);
        if (!key) return 0;
        js_skip_ws(&c);
        if (*c.p != ':') { free(key); return 0; }
        c.p++;
        js_skip_ws(&c);

        if (strcmp(key, "id") == 0) {
            char *endp = 0;
            out->id = strtol(c.p, &endp, 10);
            if (endp) c.p = endp;
            free(key);
        } else if (strcmp(key, "method") == 0) {
            free(out->method);
            out->method = js_parse_string(&c);
            free(key);
            if (!out->method) return 0;
        } else if (strcmp(key, "path") == 0) {
            free(out->path);
            out->path = js_parse_string(&c);
            free(key);
            if (!out->path) return 0;
        } else if (strcmp(key, "text") == 0) {
            free(out->text);
            out->text = js_parse_string(&c);
            free(key);
            if (!out->text) return 0;
        } else {
            free(key);
            if (!js_skip_value(&c)) return 0;
        }

        js_skip_ws(&c);
        if (*c.p == ',') { c.p++; js_skip_ws(&c); continue; }
        if (*c.p == '}') return 1;
        return 0;
    }
}

/* ---------------- 常驻服务 ---------------- */

/* 超过这个累计分配量就结束进程，让扩展重启。一次 200 行源码的
 * 编译大约分配几十 KB，96 MB 对应数千次检查，足够覆盖长时间编辑。 */
#define SLYZ_SERVE_MEM_LIMIT (96ull * 1024ull * 1024ull)

static int read_line_dyn(FILE *f, char **buf, size_t *cap) {
    size_t len = 0;
    int ch;

    if (*buf == 0) {
        *cap = 8192;
        *buf = (char *)malloc(*cap);
        if (!*buf) return 0;
    }

    while ((ch = fgetc(f)) != EOF) {
        if (len + 2 >= *cap) {
            size_t next = *cap * 2;
            char *nb = (char *)realloc(*buf, next);
            if (!nb) return 0;
            *buf = nb;
            *cap = next;
        }
        (*buf)[len++] = (char)ch;
        if (ch == '\n') break;
    }

    if (len == 0 && ch == EOF) return 0;
    (*buf)[len] = '\0';
    return 1;
}

int slyz_serve_main(void) {
    char *line = 0;
    size_t cap = 0;

    /* 行缓冲：每条响应必须立刻到达扩展，不能等缓冲区填满。 */
    setvbuf(stdout, 0, _IOLBF, 0);

    while (read_line_dyn(stdin, &line, &cap)) {
        ServeRequest req;
        if (!js_parse_request(line, &req)) {
            printf("{\"id\":0,\"ok\":false,\"fatal\":\"invalid request json\"}\n");
            fflush(stdout);
            serve_request_free(&req);
            continue;
        }

        if (req.method) {
            if (strcmp(req.method, "exit") == 0) {
                serve_request_free(&req);
                break;
            }
            if (strcmp(req.method, "ping") == 0) {
                printf("{\"id\":%ld,\"ok\":true,\"pong\":true}\n", req.id);
                fflush(stdout);
                serve_request_free(&req);
                continue;
            }
        }

        if (!req.text) {
            printf("{\"id\":%ld,\"ok\":false,\"fatal\":\"missing text\"}\n", req.id);
            fflush(stdout);
            serve_request_free(&req);
            continue;
        }

        slyz_diag_set_quiet(1);
        int ok = slyz_compile_source(req.path ? req.path : "<editor>",
                                     (const uint8_t *)req.text,
                                     (uint32_t)strlen(req.text));

        printf("{\"id\":%ld,\"ok\":%s,\"errorCount\":%d,\"diagnostics\":",
               req.id, ok ? "true" : "false", g_cc.error_count);
        slyz_write_diagnostics_json(stdout);
        if (ok) {
            fputs(",\"stats\":", stdout);
            slyz_write_stats_json(stdout);
        }
        fputs("}\n", stdout);
        fflush(stdout);

        serve_request_free(&req);

        if (g_alloc_bytes > SLYZ_SERVE_MEM_LIMIT) {
            /* 明确告知后退出：扩展会重启进程，编辑器侧无感。 */
            printf("{\"restart\":true}\n");
            fflush(stdout);
            break;
        }
    }

    free(line);
    return 0;
}
