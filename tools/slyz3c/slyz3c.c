/* ============================================================
 * slyz3c.c - SLYZ v3 编译器（寄存器字节码 + 内嵌 screen/style）
 * ------------------------------------------------------------
 * 语言刻意与 v2 分开：顶层声明使用
 *   state / fn / screen / style / on start|click|key|tick
 * 语句使用 let / if / else / while / return / break / continue，
 * 表达式接近现代脚本语言。v3 编译器只产出寄存器字节码，
 * v2 的栈式编译器与解释器原样保留，旧镜像继续可执行。
 *
 * 界面采用 XAML 风格的 screen 块和 CSS 样式表。编译器把两块
 * 原始文本放进常量池，运行时由 slyzui.c 解析、布局和绘制；
 * 事件由编译器生成的 __main 循环通过 ui.present() 取回并分发。
 *
 * 用法：
 *   slyz3c compile <source.slyz3> -o <out.slyz>
 *   slyz3c check   <source.slyz3>
 * ============================================================ */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "slyz.h"
#include "slyz3.h"

/* ============================================================
 * 词法
 * ============================================================ */

enum {
    TK_EOF = 0,
    TK_IDENT,
    TK_INT,
    TK_STR,
    TK_PUNCT,
    TK_KW
};

typedef struct {
    int   kind;
    int   line;
    int   col;
    int64_t ival;
    char  text[96];
    char *sval;
    int   slen;
    int   end_pos;
} Token;

static const char *g_src = 0;
static int g_src_len = 0;
static int g_pos = 0;
static int g_line = 1;
static int g_col = 1;
static const char *g_path = "?";

static Token g_tok;

static void die_at(int line, int col, const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "%s:%d:%d: error: ", g_path, line, col);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    exit(1);
}

static void die(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "%s: error: ", g_path);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    exit(1);
}

static int is_alpha(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int is_digit(int c) { return c >= '0' && c <= '9'; }

static char *xstrdup(const char *s) {
    int n = (int)strlen(s);
    char *p = (char *)malloc((size_t)n + 1);
    if (!p) die("out of memory");
    memcpy(p, s, (size_t)n + 1);
    return p;
}

static int is_alnum(int c) { return is_alpha(c) || is_digit(c); }

static void advance_char(void) {
    if (g_pos >= g_src_len) return;
    if (g_src[g_pos] == '\n') {
        g_line++;
        g_col = 1;
    } else {
        g_col++;
    }
    g_pos++;
}

static void skip_space(void) {
    for (;;) {
        while (g_pos < g_src_len) {
            char c = g_src[g_pos];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                advance_char();
            } else {
                break;
            }
        }
        if (g_pos + 1 < g_src_len && g_src[g_pos] == '/' && g_src[g_pos + 1] == '/') {
            while (g_pos < g_src_len && g_src[g_pos] != '\n') advance_char();
            continue;
        }
        if (g_pos < g_src_len && g_src[g_pos] == '#') {
            while (g_pos < g_src_len && g_src[g_pos] != '\n') advance_char();
            continue;
        }
        if (g_pos + 1 < g_src_len && g_src[g_pos] == '/' && g_src[g_pos + 1] == '*') {
            advance_char();
            advance_char();
            while (g_pos + 1 < g_src_len &&
                   !(g_src[g_pos] == '*' && g_src[g_pos + 1] == '/')) {
                advance_char();
            }
            if (g_pos + 1 < g_src_len) {
                advance_char();
                advance_char();
            }
            continue;
        }
        break;
    }
}

static const char *g_keywords[] = {
    "state", "fn", "screen", "style", "on", "start", "click", "key",
    "tick", "if", "else", "while", "return", "break", "continue",
    "let", "true", "false", "nil", "and", "or", "not", 0
};

static int is_keyword(const char *s) {
    for (int i = 0; g_keywords[i]; i++) {
        if (strcmp(g_keywords[i], s) == 0) return 1;
    }
    return 0;
}

static void lex_next(void) {
    skip_space();
    memset(&g_tok, 0, sizeof(g_tok));
    g_tok.line = g_line;
    g_tok.col = g_col;

    if (g_pos >= g_src_len) {
        g_tok.kind = TK_EOF;
        g_tok.end_pos = g_pos;
        return;
    }

    char c = g_src[g_pos];

    if (is_alpha((unsigned char)c)) {
        int start = g_pos;
        while (g_pos < g_src_len && is_alnum((unsigned char)g_src[g_pos])) {
            advance_char();
        }
        int n = g_pos - start;
        if (n > 95) n = 95;
        memcpy(g_tok.text, g_src + start, (size_t)n);
        g_tok.text[n] = '\0';
        g_tok.kind = is_keyword(g_tok.text) ? TK_KW : TK_IDENT;
        g_tok.end_pos = g_pos;
        return;
    }

    if (is_digit((unsigned char)c)) {
        uint64_t v = 0;
        if (c == '0' && g_pos + 1 < g_src_len &&
            (g_src[g_pos + 1] == 'x' || g_src[g_pos + 1] == 'X')) {
            advance_char();
            advance_char();
            while (g_pos < g_src_len) {
                char h = g_src[g_pos];
                int d = -1;
                if (h >= '0' && h <= '9') d = h - '0';
                else if (h >= 'a' && h <= 'f') d = h - 'a' + 10;
                else if (h >= 'A' && h <= 'F') d = h - 'A' + 10;
                if (d < 0) break;
                v = v * 16u + (uint64_t)d;
                advance_char();
            }
        } else {
            while (g_pos < g_src_len && is_digit((unsigned char)g_src[g_pos])) {
                v = v * 10u + (uint64_t)(g_src[g_pos] - '0');
                advance_char();
            }
        }
        g_tok.kind = TK_INT;
        g_tok.ival = (int64_t)v;
        g_tok.end_pos = g_pos;
        return;
    }

    if (c == '"' || c == '\'') {
        char quote = c;
        advance_char();
        char *buf = (char *)malloc(4096);
        int len = 0;
        if (!buf) die("out of memory");
        while (g_pos < g_src_len && g_src[g_pos] != quote) {
            char ch = g_src[g_pos];
            if (ch == '\\') {
                advance_char();
                if (g_pos >= g_src_len) break;
                char e = g_src[g_pos];
                if (e == 'n') ch = '\n';
                else if (e == 'r') ch = '\r';
                else if (e == 't') ch = '\t';
                else if (e == '0') ch = '\0';
                else if (e == '\\') ch = '\\';
                else if (e == '"') ch = '"';
                else if (e == '\'') ch = '\'';
                else ch = e;
                advance_char();
            } else {
                advance_char();
            }
            if (len < 4095) buf[len++] = ch;
        }
        if (g_pos >= g_src_len) {
            free(buf);
            die_at(g_tok.line, g_tok.col, "unterminated string literal");
        }
        advance_char();
        buf[len] = '\0';
        g_tok.kind = TK_STR;
        g_tok.sval = buf;
        g_tok.slen = len;
        g_tok.end_pos = g_pos;
        return;
    }

    /* 三字符运算符必须先于两字符匹配：<<= / >>= 的头部是 << / >>。 */
    static const char *three[] = { "<<=", ">>=", 0 };
    if (g_pos + 2 < g_src_len) {
        char tri[4] = { g_src[g_pos], g_src[g_pos + 1], g_src[g_pos + 2], 0 };
        for (int i = 0; three[i]; i++) {
            if (strcmp(tri, three[i]) == 0) {
                advance_char();
                advance_char();
                advance_char();
                strcpy(g_tok.text, tri);
                g_tok.kind = TK_PUNCT;
                g_tok.end_pos = g_pos;
                return;
            }
        }
    }

    static const char *two[] = {
        "==", "!=", "<=", ">=", "+=", "-=", "*=", "/=", "%=", "&&", "||",
        "<<", ">>", "&=", "|=", "^=", 0
    };
    if (g_pos + 1 < g_src_len) {
        char pair[3] = { g_src[g_pos], g_src[g_pos + 1], 0 };
        for (int i = 0; two[i]; i++) {
            if (strcmp(pair, two[i]) == 0) {
                advance_char();
                advance_char();
                strcpy(g_tok.text, pair);
                g_tok.kind = TK_PUNCT;
                g_tok.end_pos = g_pos;
                return;
            }
        }
    }

    if (strchr("{}()[],.:;=<>+-*/%!&|^~", c)) {
        g_tok.text[0] = c;
        g_tok.text[1] = '\0';
        g_tok.kind = TK_PUNCT;
        advance_char();
        g_tok.end_pos = g_pos;
        return;
    }

    die_at(g_tok.line, g_tok.col, "unexpected character '%c'", c);
}

static void tok_next(void) {
    if (g_tok.kind == TK_STR && g_tok.sval) {
        free(g_tok.sval);
    }
    lex_next();
}

static int tok_is(const char *s) {
    return (g_tok.kind == TK_PUNCT || g_tok.kind == TK_KW) &&
           strcmp(g_tok.text, s) == 0;
}

static int tok_is_kw(const char *s) {
    return g_tok.kind == TK_KW && strcmp(g_tok.text, s) == 0;
}

static void expect(const char *s) {
    if (!tok_is(s)) {
        die_at(g_tok.line, g_tok.col, "expected '%s', got '%s'", s, g_tok.text);
    }
    tok_next();
}

static char *expect_ident(void) {
    if (g_tok.kind != TK_IDENT) {
        die_at(g_tok.line, g_tok.col, "expected identifier, got '%s'", g_tok.text);
    }
    char *s = xstrdup(g_tok.text);
    if (!s) die("out of memory");
    tok_next();
    return s;
}

/* ============================================================
 * 语法树
 * ============================================================ */

typedef struct Expr Expr;
typedef struct Stmt Stmt;

enum {
    E_INT = 1, E_STR, E_BOOL, E_NIL, E_IDENT, E_MEMBER,
    E_BIN, E_UN, E_CALL, E_INDEX, E_ARRAY, E_ASSIGN
};

enum {
    A_SET = 0, A_ADD, A_SUB, A_MUL, A_DIV, A_MOD,
    A_BAND, A_BOR, A_BXOR, A_SHL, A_SHR
};

struct Expr {
    int kind;
    int line, col;
    int op;
    int64_t ival;
    char *sval;
    int slen;
    char *name;
    char *member;
    Expr *a, *b, *c;
    Expr **list;
    int count;
    int assign_op;
};

enum {
    S_LET = 1, S_EXPR, S_IF, S_WHILE, S_RETURN,
    S_BREAK, S_CONTINUE, S_BLOCK
};

struct Stmt {
    int kind;
    int line, col;
    char *name;
    Expr *e;
    Stmt *then_s;
    Stmt *else_s;
    Stmt **body;
    int count;
};

typedef struct {
    char *name;
    char **params;
    int argc;
    Stmt *body;
    int line, col;
    uint32_t entry;
    int regs;
} FnDef;

typedef struct {
    char *name;
    Expr *init;
    int line, col;
} GlobalDef;

typedef struct {
    int kind;        /* 0 start, 1 click, 2 key, 3 tick */
    char *param;
    Stmt *body;
    int line, col;
    int fn_index;
} HandlerDef;

typedef struct {
    const char *path;
    const char *src;
    int src_len;

    GlobalDef *globals;
    int global_count;

    FnDef *funcs;
    int func_count;

    HandlerDef handlers[4];
    int handler_count;

    char *markup;
    int markup_len;
    char *css;
    int css_len;

    /* 输出 */
    uint8_t *code;
    uint32_t code_len;
    uint32_t code_cap;

    uint8_t *consts;
    uint32_t const_len;
    uint32_t const_cap;

    int uses_gui;
    int main_index;
} Compiler;

static Compiler g_cc;

/* ============================================================
 * 解析
 * ============================================================ */

static Expr *new_expr(int kind, int line, int col) {
    Expr *e = (Expr *)calloc(1, sizeof(Expr));
    if (!e) die("out of memory");
    e->kind = kind;
    e->line = line;
    e->col = col;
    return e;
}

static Stmt *new_stmt(int kind, int line, int col) {
    Stmt *s = (Stmt *)calloc(1, sizeof(Stmt));
    if (!s) die("out of memory");
    s->kind = kind;
    s->line = line;
    s->col = col;
    return s;
}

static Expr *parse_expr(void);

static Expr *parse_primary(void) {
    int line = g_tok.line, col = g_tok.col;

    if (g_tok.kind == TK_INT) {
        Expr *e = new_expr(E_INT, line, col);
        e->ival = g_tok.ival;
        tok_next();
        return e;
    }
    if (g_tok.kind == TK_STR) {
        Expr *e = new_expr(E_STR, line, col);
        e->sval = xstrdup(g_tok.sval);
        e->slen = g_tok.slen;
        tok_next();
        return e;
    }
    if (tok_is_kw("true") || tok_is_kw("false")) {
        Expr *e = new_expr(E_BOOL, line, col);
        e->ival = tok_is_kw("true") ? 1 : 0;
        tok_next();
        return e;
    }
    if (tok_is_kw("nil")) {
        tok_next();
        return new_expr(E_NIL, line, col);
    }
    if (g_tok.kind == TK_IDENT) {
        Expr *e = new_expr(E_IDENT, line, col);
        e->name = xstrdup(g_tok.text);
        tok_next();
        return e;
    }
    if (tok_is("(")) {
        tok_next();
        Expr *e = parse_expr();
        expect(")");
        return e;
    }
    if (tok_is("[")) {
        Expr *e = new_expr(E_ARRAY, line, col);
        tok_next();
        while (!tok_is("]")) {
            Expr *item = parse_expr();
            e->list = (Expr **)realloc(e->list, sizeof(Expr *) * (size_t)(e->count + 1));
            if (!e->list) die("out of memory");
            e->list[e->count++] = item;
            if (tok_is(",")) tok_next();
            else break;
        }
        expect("]");
        return e;
    }

    die_at(line, col, "expected expression, got '%s'", g_tok.text);
    return 0;
}

static Expr *parse_postfix(void) {
    Expr *e = parse_primary();

    for (;;) {
        if (tok_is("(")) {
            Expr *call = new_expr(E_CALL, e->line, e->col);
            call->a = e;
            tok_next();
            while (!tok_is(")")) {
                Expr *arg = parse_expr();
                call->list = (Expr **)realloc(call->list,
                                              sizeof(Expr *) * (size_t)(call->count + 1));
                if (!call->list) die("out of memory");
                call->list[call->count++] = arg;
                if (tok_is(",")) tok_next();
                else break;
            }
            expect(")");
            e = call;
            continue;
        }
        if (tok_is("[")) {
            Expr *idx = new_expr(E_INDEX, e->line, e->col);
            idx->a = e;
            tok_next();
            idx->b = parse_expr();
            expect("]");
            e = idx;
            continue;
        }
        if (tok_is(".")) {
            tok_next();
            /* 成员名允许与关键字同名：audio.state()、time.text() 这类
             * 宿主 API 不该因为语言保留字而不可用。 */
            char *m;
            if (g_tok.kind == TK_IDENT || g_tok.kind == TK_KW) {
                m = xstrdup(g_tok.text);
                tok_next();
            } else {
                die_at(g_tok.line, g_tok.col, "expected member name, got '%s'",
                       g_tok.text);
                m = 0;
            }
            Expr *mem = new_expr(E_MEMBER, e->line, e->col);
            mem->a = e;
            mem->member = m;
            e = mem;
            continue;
        }
        break;
    }
    return e;
}

static Expr *parse_unary(void) {
    if (tok_is("!") || tok_is_kw("not") || tok_is("-") || tok_is("~")) {
        int line = g_tok.line, col = g_tok.col;
        int op = tok_is("-") ? '-' : (tok_is("~") ? '~' : '!');
        tok_next();
        Expr *e = new_expr(E_UN, line, col);
        e->op = op;
        e->a = parse_unary();
        return e;
    }
    return parse_postfix();
}

static Expr *parse_mul(void) {
    Expr *e = parse_unary();
    while (tok_is("*") || tok_is("/") || tok_is("%")) {
        int op = g_tok.text[0];
        int line = g_tok.line, col = g_tok.col;
        tok_next();
        Expr *b = parse_unary();
        Expr *n = new_expr(E_BIN, line, col);
        n->op = op;
        n->a = e;
        n->b = b;
        e = n;
    }
    return e;
}

static Expr *parse_add(void) {
    Expr *e = parse_mul();
    while (tok_is("+") || tok_is("-")) {
        int op = g_tok.text[0];
        int line = g_tok.line, col = g_tok.col;
        tok_next();
        Expr *b = parse_mul();
        Expr *n = new_expr(E_BIN, line, col);
        n->op = op;
        n->a = e;
        n->b = b;
        e = n;
    }
    return e;
}

/* 位运算优先级（与 C 接近）：
 *   ||  -> && -> 比较 -> | -> ^ -> & -> 移位 -> +- -> * / % -> 一元
 * 文件系统代码几乎总是加括号，这里保持自洽即可。 */
static Expr *parse_shift(void) {
    Expr *e = parse_add();
    while (tok_is("<<") || tok_is(">>")) {
        int op = tok_is("<<") ? 10 : 11;
        int line = g_tok.line, col = g_tok.col;
        tok_next();
        Expr *b = parse_add();
        Expr *n = new_expr(E_BIN, line, col);
        n->op = op;
        n->a = e;
        n->b = b;
        e = n;
    }
    return e;
}

static Expr *parse_bitand(void) {
    Expr *e = parse_shift();
    while (tok_is("&")) {
        int line = g_tok.line, col = g_tok.col;
        tok_next();
        Expr *b = parse_shift();
        Expr *n = new_expr(E_BIN, line, col);
        n->op = 9;
        n->a = e;
        n->b = b;
        e = n;
    }
    return e;
}

static Expr *parse_bitxor(void) {
    Expr *e = parse_bitand();
    while (tok_is("^")) {
        int line = g_tok.line, col = g_tok.col;
        tok_next();
        Expr *b = parse_bitand();
        Expr *n = new_expr(E_BIN, line, col);
        n->op = 8;
        n->a = e;
        n->b = b;
        e = n;
    }
    return e;
}

static Expr *parse_bitor(void) {
    Expr *e = parse_bitxor();
    while (tok_is("|")) {
        int line = g_tok.line, col = g_tok.col;
        tok_next();
        Expr *b = parse_bitxor();
        Expr *n = new_expr(E_BIN, line, col);
        n->op = 7;
        n->a = e;
        n->b = b;
        e = n;
    }
    return e;
}

static Expr *parse_cmp(void) {
    Expr *e = parse_bitor();
    while (tok_is("<") || tok_is("<=") || tok_is(">") || tok_is(">=") ||
           tok_is("==") || tok_is("!=")) {
        int op = 0;
        if (tok_is("<")) op = '<';
        else if (tok_is("<=")) op = 1;
        else if (tok_is(">")) op = '>';
        else if (tok_is(">=")) op = 2;
        else if (tok_is("==")) op = 3;
        else op = 4;
        int line = g_tok.line, col = g_tok.col;
        tok_next();
        Expr *b = parse_bitor();
        Expr *n = new_expr(E_BIN, line, col);
        n->op = op;
        n->a = e;
        n->b = b;
        e = n;
    }
    return e;
}

static Expr *parse_and(void) {
    Expr *e = parse_cmp();
    while (tok_is_kw("and") || tok_is("&&")) {
        int line = g_tok.line, col = g_tok.col;
        tok_next();
        Expr *b = parse_cmp();
        Expr *n = new_expr(E_BIN, line, col);
        n->op = 5;
        n->a = e;
        n->b = b;
        e = n;
    }
    return e;
}

static Expr *parse_or(void) {
    Expr *e = parse_and();
    while (tok_is_kw("or") || tok_is("||")) {
        int line = g_tok.line, col = g_tok.col;
        tok_next();
        Expr *b = parse_and();
        Expr *n = new_expr(E_BIN, line, col);
        n->op = 6;
        n->a = e;
        n->b = b;
        e = n;
    }
    return e;
}

static Expr *parse_expr(void) {
    Expr *e = parse_or();
    if (tok_is("=") || tok_is("+=") || tok_is("-=") || tok_is("*=") ||
        tok_is("/=") || tok_is("%=") || tok_is("&=") || tok_is("|=") ||
        tok_is("^=") || tok_is("<<=") || tok_is(">>=")) {
        int op = A_SET;
        if (tok_is("+=")) op = A_ADD;
        else if (tok_is("-=")) op = A_SUB;
        else if (tok_is("*=")) op = A_MUL;
        else if (tok_is("/=")) op = A_DIV;
        else if (tok_is("%=")) op = A_MOD;
        else if (tok_is("&=")) op = A_BAND;
        else if (tok_is("|=")) op = A_BOR;
        else if (tok_is("^=")) op = A_BXOR;
        else if (tok_is("<<=")) op = A_SHL;
        else if (tok_is(">>=")) op = A_SHR;
        int line = g_tok.line, col = g_tok.col;
        tok_next();
        Expr *rhs = parse_expr();
        Expr *n = new_expr(E_ASSIGN, line, col);
        n->assign_op = op;
        n->a = e;
        n->b = rhs;
        return n;
    }
    return e;
}

static Stmt *parse_block(void);

/* 定义在调用表之后：判断名字是否已被内置函数/宿主调用占用。
 * 用户函数若与内置名重名，调用点会被编译成内置调用，表现为
 * “函数返回空值且没有任何报错”，极难排查，因此在定义处直接拒绝。 */
static int is_reserved_call(const char *name);

static Stmt *parse_stmt(void) {
    int line = g_tok.line, col = g_tok.col;

    if (tok_is("{")) {
        return parse_block();
    }
    if (tok_is_kw("let")) {
        tok_next();
        char *name = expect_ident();
        expect("=");
        Stmt *s = new_stmt(S_LET, line, col);
        s->name = name;
        s->e = parse_expr();
        if (tok_is(";")) tok_next();
        return s;
    }
    if (tok_is_kw("if")) {
        tok_next();
        Stmt *s = new_stmt(S_IF, line, col);
        s->e = parse_expr();
        s->then_s = parse_block();
        if (tok_is_kw("else")) {
            tok_next();
            if (tok_is_kw("if")) {
                /* else if 递归成嵌套 if */
                s->else_s = parse_stmt();
            } else {
                s->else_s = parse_block();
            }
        }
        return s;
    }
    if (tok_is_kw("while")) {
        tok_next();
        Stmt *s = new_stmt(S_WHILE, line, col);
        s->e = parse_expr();
        s->then_s = parse_block();
        return s;
    }
    if (tok_is_kw("return")) {
        tok_next();
        Stmt *s = new_stmt(S_RETURN, line, col);
        if (!tok_is("}") && !tok_is(";") && g_tok.kind != TK_EOF) {
            s->e = parse_expr();
        }
        if (tok_is(";")) tok_next();
        return s;
    }
    if (tok_is_kw("break")) {
        tok_next();
        if (tok_is(";")) tok_next();
        return new_stmt(S_BREAK, line, col);
    }
    if (tok_is_kw("continue")) {
        tok_next();
        if (tok_is(";")) tok_next();
        return new_stmt(S_CONTINUE, line, col);
    }

    Stmt *s = new_stmt(S_EXPR, line, col);
    s->e = parse_expr();
    if (tok_is(";")) tok_next();
    return s;
}

static Stmt *parse_block(void) {
    Stmt *b = new_stmt(S_BLOCK, g_tok.line, g_tok.col);
    expect("{");
    while (!tok_is("}")) {
        if (g_tok.kind == TK_EOF) {
            die_at(b->line, b->col, "unterminated block");
        }
        Stmt *s = parse_stmt();
        b->body = (Stmt **)realloc(b->body, sizeof(Stmt *) * (size_t)(b->count + 1));
        if (!b->body) die("out of memory");
        b->body[b->count++] = s;
    }
    expect("}");
    return b;
}

/* 捕获当前 '{' 与匹配 '}' 之间的原始文本。
 * 调用时当前 token 必须是 '{'；返回后位置停在 '}' 之后，
 * 调用方需要再执行 tok_next() 装载后续 token。 */
static char *capture_raw(int *out_len, int start_line, int start_col) {
    int start = g_tok.end_pos;
    int i = start;
    int depth = 1;
    int state = 0;   /* 0 普通, 1 字符串, 2 行注释, 3 块注释 */

    while (i < g_src_len) {
        char c = g_src[i];
        char n = (i + 1 < g_src_len) ? g_src[i + 1] : '\0';

        if (state == 1) {
            if (c == '\\') { i += 2; continue; }
            if (c == '"' || c == '\'') state = 0;
            i++;
            continue;
        }
        if (state == 2) {
            if (c == '\n') state = 0;
            i++;
            continue;
        }
        if (state == 3) {
            if (c == '*' && n == '/') { state = 0; i += 2; continue; }
            i++;
            continue;
        }

        if (c == '"' || c == '\'') { state = 1; i++; continue; }
        if (c == '/' && n == '/') { state = 2; i += 2; continue; }
        if (c == '/' && n == '*') { state = 3; i += 2; continue; }
        if (c == '{') { depth++; i++; continue; }
        if (c == '}') {
            depth--;
            if (depth == 0) {
                int len = i - start;
                char *out = (char *)malloc((size_t)len + 1);
                if (!out) die("out of memory");
                memcpy(out, g_src + start, (size_t)len);
                out[len] = '\0';
                *out_len = len;

                /* 位置停在 '}' 之后，并按整个源文件重算行列 */
                g_pos = i + 1;
                g_line = 1;
                g_col = 1;
                for (int k = 0; k < g_pos; k++) {
                    if (g_src[k] == '\n') { g_line++; g_col = 1; }
                    else g_col++;
                }
                return out;
            }
            i++;
            continue;
        }
        i++;
    }

    die_at(start_line, start_col, "unterminated screen/style block");
    return 0;
}

static void parse_program(void) {
    for (;;) {
        if (g_tok.kind == TK_EOF) break;

        if (tok_is_kw("state")) {
            int line = g_tok.line, col = g_tok.col;
            tok_next();
            char *name = expect_ident();
            expect("=");
            Expr *init = parse_expr();
            if (tok_is(";")) tok_next();

            g_cc.globals = (GlobalDef *)realloc(g_cc.globals,
                sizeof(GlobalDef) * (size_t)(g_cc.global_count + 1));
            if (!g_cc.globals) die("out of memory");
            GlobalDef *gd = &g_cc.globals[g_cc.global_count++];
            gd->name = name;
            gd->init = init;
            gd->line = line;
            gd->col = col;
            continue;
        }

        if (tok_is_kw("fn")) {
            int line = g_tok.line, col = g_tok.col;
            tok_next();
            char *name = expect_ident();
            if (is_reserved_call(name)) {
                die_at(line, col,
                       "function '%s' shadows a built-in call; rename it", name);
            }
            expect("(");
            char **params = 0;
            int argc = 0;
            while (!tok_is(")")) {
                char *p = expect_ident();
                params = (char **)realloc(params, sizeof(char *) * (size_t)(argc + 1));
                if (!params) die("out of memory");
                params[argc++] = p;
                if (tok_is(",")) tok_next();
                else break;
            }
            expect(")");
            Stmt *body = parse_block();

            g_cc.funcs = (FnDef *)realloc(g_cc.funcs,
                sizeof(FnDef) * (size_t)(g_cc.func_count + 1));
            if (!g_cc.funcs) die("out of memory");
            FnDef *f = &g_cc.funcs[g_cc.func_count++];
            memset(f, 0, sizeof(*f));
            f->name = name;
            f->params = params;
            f->argc = argc;
            f->body = body;
            f->line = line;
            f->col = col;
            continue;
        }

        if (tok_is_kw("screen")) {
            tok_next();
            char *name = expect_ident();
            (void)name;
            if (!tok_is("{")) {
                die_at(g_tok.line, g_tok.col, "expected '{' after screen name");
            }
            int brace_line = g_tok.line, brace_col = g_tok.col;
            int len = 0;
            char *raw = capture_raw(&len, brace_line, brace_col);
            tok_next();
            free(g_cc.markup);
            g_cc.markup = raw;
            g_cc.markup_len = len;
            g_cc.uses_gui = 1;
            continue;
        }

        if (tok_is_kw("style")) {
            tok_next();
            if (!tok_is("{")) {
                die_at(g_tok.line, g_tok.col, "expected '{' after style");
            }
            int brace_line = g_tok.line, brace_col = g_tok.col;
            int len = 0;
            char *raw = capture_raw(&len, brace_line, brace_col);
            tok_next();
            if (g_cc.css) {
                int old = g_cc.css_len;
                char *merged = (char *)malloc((size_t)(old + len + 2));
                if (!merged) die("out of memory");
                memcpy(merged, g_cc.css, (size_t)old);
                merged[old] = '\n';
                memcpy(merged + old + 1, raw, (size_t)len);
                merged[old + 1 + len] = '\0';
                free(g_cc.css);
                free(raw);
                g_cc.css = merged;
                g_cc.css_len = old + 1 + len;
            } else {
                g_cc.css = raw;
                g_cc.css_len = len;
            }
            g_cc.uses_gui = 1;
            continue;
        }

        if (tok_is_kw("on")) {
            int line = g_tok.line, col = g_tok.col;
            tok_next();
            HandlerDef h;
            memset(&h, 0, sizeof(h));
            h.line = line;
            h.col = col;

            if (tok_is_kw("start")) {
                h.kind = 0;
                tok_next();
                h.body = parse_block();
            } else if (tok_is_kw("click") || tok_is_kw("key")) {
                h.kind = tok_is_kw("click") ? 1 : 2;
                tok_next();
                expect("(");
                h.param = expect_ident();
                expect(")");
                h.body = parse_block();
            } else if (tok_is_kw("tick")) {
                h.kind = 3;
                tok_next();
                h.body = parse_block();
            } else {
                die_at(g_tok.line, g_tok.col,
                       "expected start, click, key or tick after 'on'");
            }

            for (int i = 0; i < g_cc.handler_count; i++) {
                if (g_cc.handlers[i].kind == h.kind) {
                    die_at(line, col, "duplicate handler for the same event");
                }
            }
            g_cc.handlers[g_cc.handler_count++] = h;
            /* 只有屏幕事件才需要图形宿主；`on start` 是普通入口，
             * 终端程序（文件系统工具等）也能使用。 */
            if (h.kind != 0) g_cc.uses_gui = 1;
            continue;
        }

        die_at(g_tok.line, g_tok.col,
               "top-level expects state, fn, screen, style or on");
    }
}

/* ============================================================
 * 代码生成
 * ============================================================ */

typedef struct {
    char *name;
    int reg;
} Local;

typedef struct {
    int start;
    int *breaks;
    int nbreaks, capbreaks;
    int *conts;
    int nconts, capconts;
} LoopPatch;

typedef struct {
    int next_reg;
    int max_reg;
    Local locals[128];
    int nlocals;
    LoopPatch loops[32];
    int nloops;
} Gen;

static Gen g_gen;

static void emit(uint8_t op, uint8_t a, uint8_t b, uint8_t c, uint32_t imm) {
    if (g_cc.code_len + SLYZ3_INSN_SIZE > g_cc.code_cap) {
        uint32_t next = g_cc.code_cap ? g_cc.code_cap * 2 : 4096;
        uint8_t *nb = (uint8_t *)realloc(g_cc.code, next);
        if (!nb) die("out of memory");
        g_cc.code = nb;
        g_cc.code_cap = next;
    }
    uint8_t *p = g_cc.code + g_cc.code_len;
    p[0] = op;
    p[1] = a;
    p[2] = b;
    p[3] = c;
    p[4] = (uint8_t)(imm & 0xFF);
    p[5] = (uint8_t)((imm >> 8) & 0xFF);
    p[6] = (uint8_t)((imm >> 16) & 0xFF);
    p[7] = (uint8_t)((imm >> 24) & 0xFF);
    g_cc.code_len += SLYZ3_INSN_SIZE;
}

static uint32_t emit_jump(uint8_t op, uint8_t cond, uint32_t imm) {
    uint32_t at = g_cc.code_len;
    emit(op, cond, 0, 0, imm);
    return at;
}

static void patch_jump(uint32_t at, uint32_t target) {
    uint8_t *p = g_cc.code + at;
    p[4] = (uint8_t)(target & 0xFF);
    p[5] = (uint8_t)((target >> 8) & 0xFF);
    p[6] = (uint8_t)((target >> 16) & 0xFF);
    p[7] = (uint8_t)((target >> 24) & 0xFF);
}

static uint32_t const_add(const char *s, int len) {
    if (len < 0) len = (int)strlen(s);

    /* 复用完全相同的常量，减少池体积 */
    for (uint32_t off = 0; off < g_cc.const_len;) {
        int n = 0;
        while (off + (uint32_t)n < g_cc.const_len && g_cc.consts[off + (uint32_t)n]) n++;
        if (n == len && (len == 0 || memcmp(g_cc.consts + off, s, (size_t)len) == 0)) {
            return off;
        }
        off += (uint32_t)n + 1;
    }

    uint32_t need = g_cc.const_len + (uint32_t)len + 1;
    if (need > g_cc.const_cap) {
        uint32_t next = g_cc.const_cap ? g_cc.const_cap * 2 : 1024;
        while (next < need) next *= 2;
        uint8_t *nb = (uint8_t *)realloc(g_cc.consts, next);
        if (!nb) die("out of memory");
        g_cc.consts = nb;
        g_cc.const_cap = next;
    }
    uint32_t off = g_cc.const_len;
    if (len) memcpy(g_cc.consts + off, s, (size_t)len);
    g_cc.consts[off + (uint32_t)len] = 0;
    g_cc.const_len += (uint32_t)len + 1;
    return off;
}

static int alloc_reg(void) {
    if (g_gen.next_reg >= SLYZ3_REGS_PER_FRAME) {
        die("function needs more than %d registers; split it into smaller functions",
            SLYZ3_REGS_PER_FRAME);
    }
    int r = g_gen.next_reg++;
    if (g_gen.next_reg > g_gen.max_reg) g_gen.max_reg = g_gen.next_reg;
    return r;
}

static void free_to(int mark) {
    for (int r = mark; r < g_gen.next_reg; r++) {
        emit(S3_LOADNIL, (uint8_t)r, 0, 0, 0);
    }
    if (mark < g_gen.next_reg) g_gen.next_reg = mark;
}

static int find_local(const char *name) {
    for (int i = g_gen.nlocals - 1; i >= 0; i--) {
        if (strcmp(g_gen.locals[i].name, name) == 0) return g_gen.locals[i].reg;
    }
    return -1;
}

static int find_global(const char *name) {
    for (int i = 0; i < g_cc.global_count; i++) {
        if (strcmp(g_cc.globals[i].name, name) == 0) return i;
    }
    return -1;
}

static int find_func(const char *name) {
    for (int i = 0; i < g_cc.func_count; i++) {
        if (strcmp(g_cc.funcs[i].name, name) == 0) return i;
    }
    return -1;
}

/* ---------------- 系统调用与内置函数表 ---------------- */

typedef struct {
    const char *name;
    int id;
    int argc;
} CallDef;

static const CallDef g_calls[] = {
    { "print", SLE_SYS_PRINT, 1 },
    { "println", SLE_SYS_PRINTLN, 1 },
    { "eprint", SLE_SYS_EPRINT, 1 },

    { "fs.read", SLE_SYS_FS_READ, 1 },
    { "fs.write", SLE_SYS_FS_WRITE, 2 },
    { "fs.exists", SLE_SYS_FS_EXISTS, 1 },
    { "fs.list", SLE_SYS_FS_LIST, 1 },
    { "fs.remove", SLE_SYS_FS_REMOVE, 1 },
    { "fs.mkdir", SLE_SYS_FS_MKDIR, 1 },
    { "fs.entries", SLE_SYS_FS_ENTRIES, 1 },
    { "fs.rmdir", SLE_SYS_FS_RMDIR, 1 },
    { "fs.copy", SLE_SYS_FS_COPY, 2 },
    { "fs.move", SLE_SYS_FS_MOVE, 2 },

    { "cfg.get", SLE_SYS_CFG_GET, 1 },
    { "cfg.set", SLE_SYS_CFG_SET, 2 },
    { "cfg.keys", SLE_SYS_CFG_KEYS, 0 },
    { "ui.theme", SLE_SYS_THEME_GET, 1 },

    { "part.list", SLE_SYS_PART_LIST, 0 },
    { "part.add", SLE_SYS_PART_ADD, 3 },
    { "part.delete", SLE_SYS_PART_DELETE, 1 },
    { "part.format", SLE_SYS_PART_FORMAT, 2 },
    { "part.mount", SLE_SYS_PART_MOUNT, 1 },
    { "part.unmount", SLE_SYS_PART_UNMOUNT, 1 },

    { "time.now", SLE_SYS_TIME_NOW, 0 },
    { "time.ms", SLE_SYS_TIME_MS, 0 },
    { "time.sleep", SLE_SYS_SLEEP, 1 },
    { "time.text", SLE_SYS_TIME_TEXT, 0 },
    { "time.date", SLE_SYS_DATE_TEXT, 0 },

    { "os.exec", SLE_SYS_EXEC, 1 },
    { "os.exit", SLE_SYS_EXIT, 1 },
    { "os.beep", SLE_SYS_BEEP, 2 },
    { "os.open", SLE_SYS_OPEN_UI, 1 },
    { "os.info", SLE_SYS_INFO, 1 },
    { "os.proc", SLE_SYS_PROC, 2 },
    { "os.proc_name", SLE_SYS_PROC_NAME, 1 },

    { "fault.trigger", SLE_SYS_FAULT_TRIGGER, 1 },
    { "fault.dump", SLE_SYS_FAULT_DUMP, 0 },
    { "fault.leak", SLE_SYS_FAULT_LEAK, 1 },
    { "fault.leak_free", SLE_SYS_FAULT_LEAK_FREE, 0 },
    { "fault.set", SLE_SYS_FAULT_SET, 2 },
    { "fault.get", SLE_SYS_FAULT_GET, 1 },

    { "win.open", SLE_SYS_WIN_OPEN, 3 },
    { "win.close", SLE_SYS_WIN_CLOSE, 1 },
    { "win.width", SLE_SYS_WIN_SIZE, 1 },
    { "win.height", SLE_SYS_WIN_HEIGHT, 1 },
    { "win.title", SLE_SYS_WIN_TITLE, 2 },

    { "gfx.clear", SLE_SYS_GFX_CLEAR, 1 },
    { "gfx.rect", SLE_SYS_GFX_RECT, 5 },
    { "gfx.frame", SLE_SYS_GFX_FRAME, 6 },
    { "gfx.text", SLE_SYS_GFX_TEXT, 4 },
    { "gfx.text_width", SLE_SYS_GFX_TWIDTH, 1 },
    { "gfx.line", SLE_SYS_GFX_LINE, 5 },
    { "gfx.circle", SLE_SYS_GFX_CIRCLE, 4 },
    { "gfx.flush", SLE_SYS_GFX_FLUSH, 0 },

    { "event.poll", SLE_SYS_EVT_POLL, 0 },
    { "event.wait", SLE_SYS_EVT_WAIT, 1 },

    { "media.request", SLE_SYS_MEDIA_REQ, 0 },
    { "audio.play", SLE_SYS_AUDIO_PLAY, 1 },
    { "audio.stop", SLE_SYS_AUDIO_STOP, 0 },
    { "audio.pause", SLE_SYS_AUDIO_PAUSE, 1 },
    { "audio.state", SLE_SYS_AUDIO_STATE, 0 },
    { "audio.position", SLE_SYS_AUDIO_POS, 0 },
    { "audio.duration", SLE_SYS_AUDIO_DUR, 0 },
    { "audio.set_volume", SLE_SYS_AUDIO_VOL, 1 },
    { "audio.volume", SLE_SYS_AUDIO_GETVOL, 0 },

    { "img.load", SLE_SYS_IMG_LOAD, 1 },
    { "img.width", SLE_SYS_IMG_WIDTH, 1 },
    { "img.height", SLE_SYS_IMG_HEIGHT, 1 },
    { "img.draw", SLE_SYS_IMG_DRAW, 3 },
    { "img.draw_scaled", SLE_SYS_IMG_DRAW_SCALED, 5 },
    { "img.draw_circle", SLE_SYS_IMG_DRAW_CIRCLE, 4 },
    { "img.free", SLE_SYS_IMG_FREE, 1 },

    { "ui.markup", SLE_SYS_UI_MARKUP, 1 },
    { "ui.style", SLE_SYS_UI_STYLE, 1 },
    { "ui.present", SLE_SYS_UI_PRESENT, 0 },
    { "ui.set", SLE_SYS_UI_SET, 2 },
    { "ui.get", SLE_SYS_UI_GET, 1 },
    { "input", SLE_SYS_INPUT, 1 },

    { "len", SLE_BIF_LEN, 1 },
    { "str", SLE_BIF_STR, 1 },
    { "int", SLE_BIF_INT, 1 },
    { "substr", SLE_BIF_SUBSTR, 3 },
    { "index_of", SLE_BIF_INDEXOF, 2 },
    { "upper", SLE_BIF_UPPER, 1 },
    { "lower", SLE_BIF_LOWER, 1 },
    { "trim", SLE_BIF_TRIM, 1 },
    { "replace", SLE_BIF_REPLACE, 3 },
    { "split", SLE_BIF_SPLIT, 2 },
    { "join", SLE_BIF_JOIN, 2 },
    { "str_at", SLE_BIF_STR_AT, 2 },
    { "ord", SLE_BIF_ORD, 1 },
    { "chr", SLE_BIF_CHR, 1 },
    { "hex", SLE_BIF_HEX, 1 },
    { "bin", SLE_BIF_BIN, 1 },
    { "typeof", SLE_BIF_TYPEOF, 1 },
    { "array", SLE_BIF_ARRAY, -1 },
    { "push", SLE_BIF_PUSH, 2 },
    { "pop", SLE_BIF_POP, 1 },
    { "range", SLE_BIF_RANGE, 2 },
    { "random", SLE_BIF_RANDOM, 2 },
    { "abs", SLE_BIF_ABS, 1 },
    { "min", SLE_BIF_MIN, 2 },
    { "max", SLE_BIF_MAX, 2 },
    { "clamp", SLE_BIF_CLAMP, 3 },
    { "sign", SLE_BIF_SIGN, 1 },
    { "count", SLE_BIF_COUNT, 1 },

    { 0, 0, 0 }
};

static const CallDef *find_call(const char *name) {
    for (int i = 0; g_calls[i].name; i++) {
        if (strcmp(g_calls[i].name, name) == 0) return &g_calls[i];
    }
    return 0;
}

static int is_reserved_call(const char *name) {
    return find_call(name) != 0;
}

/* ---------------- 表达式生成 ---------------- */

static int gen_expr(Expr *e);

static int gen_call(Expr *e, Expr *callee);

/* 复合赋值运算符 → 对应的二元运算操作码。
 * /=、%=、移位与位运算都在这里统一映射，避免三处赋值路径各写一份。 */
static int binop_from_assign(int aop) {
    switch (aop) {
        case A_ADD:  return S3_ADD;
        case A_SUB:  return S3_SUB;
        case A_MUL:  return S3_MUL;
        case A_DIV:  return S3_DIV;
        case A_MOD:  return S3_MOD;
        case A_BAND: return S3_BAND;
        case A_BOR:  return S3_BOR;
        case A_BXOR: return S3_BXOR;
        case A_SHL:  return S3_SHL;
        case A_SHR:  return S3_SHR;
        default:     return S3_MOVE;
    }
}

/* 把一个临时寄存器里的结果收拢到 mark 位置，并释放其余临时值。
 * 用于"值已经在临时寄存器里、但表达式结果还要继续使用"的场景。 */
static int keep_result(int r, int mark) {
    if (r < mark) return r;
    if (r != mark) emit(S3_MOVE, (uint8_t)mark, (uint8_t)r, 0, 0);
    free_to(mark + 1);
    return mark;
}

static int gen_assign(Expr *e) {
    int mark = g_gen.next_reg;

    if (e->a->kind == E_IDENT) {
        int val = gen_expr(e->b);
        int target = find_local(e->a->name);
        if (target >= 0) {
            if (e->assign_op != A_SET) {
                int tmp = alloc_reg();
                emit((uint8_t)binop_from_assign(e->assign_op),
                     (uint8_t)tmp, (uint8_t)target, (uint8_t)val, 0);
                emit(S3_MOVE, (uint8_t)target, (uint8_t)tmp, 0, 0);
                free_to(mark);
                return target;
            }
            emit(S3_MOVE, (uint8_t)target, (uint8_t)val, 0, 0);
            free_to(mark);
            return target;
        }

        int gslot = find_global(e->a->name);
        if (gslot >= 0) {
            if (e->assign_op != A_SET) {
                int cur = alloc_reg();
                emit(S3_LOADG, (uint8_t)cur, 0, 0, (uint32_t)gslot);
                int tmp = alloc_reg();
                emit((uint8_t)binop_from_assign(e->assign_op),
                     (uint8_t)tmp, (uint8_t)cur, (uint8_t)val, 0);
                emit(S3_STOREG, (uint8_t)tmp, 0, 0, (uint32_t)gslot);
                return keep_result(tmp, mark);
            }
            emit(S3_STOREG, (uint8_t)val, 0, 0, (uint32_t)gslot);
            return keep_result(val, mark);
        }

        die_at(e->line, e->col, "assignment to unknown variable '%s'", e->a->name);
    }

    if (e->a->kind == E_INDEX) {
        int arr = gen_expr(e->a->a);
        int idx = gen_expr(e->a->b);
        int val = gen_expr(e->b);
        if (e->assign_op != A_SET) {
            int cur = alloc_reg();
            emit(S3_IDXGET, (uint8_t)cur, (uint8_t)arr, (uint8_t)idx, 0);
            int tmp = alloc_reg();
            emit((uint8_t)binop_from_assign(e->assign_op),
                 (uint8_t)tmp, (uint8_t)cur, (uint8_t)val, 0);
            emit(S3_IDXSET, (uint8_t)arr, (uint8_t)idx, (uint8_t)tmp, 0);
            return keep_result(tmp, mark);
        }
        emit(S3_IDXSET, (uint8_t)arr, (uint8_t)idx, (uint8_t)val, 0);
        return keep_result(val, mark);
    }

    die_at(e->line, e->col, "left side of assignment is not a variable");
    return 0;
}

static int gen_bin(Expr *e) {
    int op = e->op;

    if (op == 5 || op == 6) {   /* and / or，短路求值 */
        int rd = alloc_reg();   /* 结果槽位固定在 mark */
        int ra = gen_expr(e->a);
        uint32_t j1;
        if (op == 5) j1 = emit_jump(S3_JZ, (uint8_t)ra, 0);
        else j1 = emit_jump(S3_JNZ, (uint8_t)ra, 0);

        int rb = gen_expr(e->b);
        uint32_t j2;
        if (op == 5) j2 = emit_jump(S3_JZ, (uint8_t)rb, 0);
        else j2 = emit_jump(S3_JNZ, (uint8_t)rb, 0);

        emit(S3_LOADI, (uint8_t)rd, 0, 0, (op == 5) ? 1u : 0u);
        uint32_t jend = emit_jump(S3_JMP, 0, 0);

        patch_jump(j1, g_cc.code_len);
        patch_jump(j2, g_cc.code_len);
        emit(S3_LOADI, (uint8_t)rd, 0, 0, (op == 5) ? 0u : 1u);
        patch_jump(jend, g_cc.code_len);

        free_to(rd + 1);
        return rd;
    }

    int ra = gen_expr(e->a);
    int rb = gen_expr(e->b);
    int rd = alloc_reg();
    uint8_t o = S3_ADD;
    if (op == '+') o = S3_ADD;
    else if (op == '-') o = S3_SUB;
    else if (op == '*') o = S3_MUL;
    else if (op == '/') o = S3_DIV;
    else if (op == '%') o = S3_MOD;
    else if (op == '<') o = S3_LT;
    else if (op == 1) o = S3_LE;
    else if (op == '>') o = S3_GT;
    else if (op == 2) o = S3_GE;
    else if (op == 3) o = S3_EQ;
    else if (op == 4) o = S3_NE;
    else if (op == 9) o = S3_BAND;
    else if (op == 7) o = S3_BOR;
    else if (op == 8) o = S3_BXOR;
    else if (op == 10) o = S3_SHL;
    else if (op == 11) o = S3_SHR;
    emit(o, (uint8_t)rd, (uint8_t)ra, (uint8_t)rb, 0);
    return rd;
}

static int gen_expr(Expr *e) {
    int mark = g_gen.next_reg;

    switch (e->kind) {
        case E_INT: {
            int r = alloc_reg();
            emit(S3_LOADI, (uint8_t)r, 0, 0, (uint32_t)(int32_t)e->ival);
            return r;
        }
        case E_STR: {
            int r = alloc_reg();
            uint32_t off = const_add(e->sval, e->slen);
            emit(S3_LOADK, (uint8_t)r, 0, 0, off);
            return r;
        }
        case E_BOOL: {
            int r = alloc_reg();
            emit(S3_LOADI, (uint8_t)r, 0, 0, e->ival ? 1u : 0u);
            return r;
        }
        case E_NIL: {
            int r = alloc_reg();
            emit(S3_LOADNIL, (uint8_t)r, 0, 0, 0);
            return r;
        }
        case E_IDENT: {
            int lr = find_local(e->name);
            if (lr >= 0) {
                int r = alloc_reg();
                emit(S3_MOVE, (uint8_t)r, (uint8_t)lr, 0, 0);
                return r;
            }
            int g = find_global(e->name);
            if (g >= 0) {
                int r = alloc_reg();
                emit(S3_LOADG, (uint8_t)r, 0, 0, (uint32_t)g);
                return r;
            }
            die_at(e->line, e->col, "unknown variable '%s'", e->name);
            return 0;
        }
        case E_MEMBER: {
            if (strcmp(e->member, "length") == 0) {
                int base = gen_expr(e->a);
                int r = alloc_reg();
                emit(S3_LEN, (uint8_t)r, (uint8_t)base, 0, 0);
                return keep_result(r, mark);
            }
            die_at(e->line, e->col, "unknown property '%s'", e->member);
            return 0;
        }
        case E_UN: {
            int a = gen_expr(e->a);
            int r = alloc_reg();
            if (e->op == '~') {
                /* 位取反 = 与 -1 异或；VM 没有单独的位 NOT 指令。 */
                int m = alloc_reg();
                emit(S3_LOADI, (uint8_t)m, 0, 0, 0xFFFFFFFFu);
                emit(S3_BXOR, (uint8_t)r, (uint8_t)a, (uint8_t)m, 0);
            } else {
                emit(e->op == '-' ? S3_NEG : S3_NOT, (uint8_t)r, (uint8_t)a, 0, 0);
            }
            return r;
        }
        case E_BIN:
            return gen_bin(e);

        case E_CALL:
            return gen_call(e, e->a);

        case E_INDEX: {
            int arr = gen_expr(e->a);
            int idx = gen_expr(e->b);
            int r = alloc_reg();
            emit(S3_IDXGET, (uint8_t)r, (uint8_t)arr, (uint8_t)idx, 0);
            return r;
        }
        case E_ARRAY: {
            int base = g_gen.next_reg;
            /* 先为所有元素预留连续寄存器，再逐个求值；
             * 复杂表达式产生的临时寄存器因此不会覆盖已放好的元素。 */
            for (int i = 0; i < e->count; i++) alloc_reg();
            for (int i = 0; i < e->count; i++) {
                int r = gen_expr(e->list[i]);
                emit(S3_MOVE, (uint8_t)(base + i), (uint8_t)r, 0, 0);
            }
            /* NEWARR 的源和目标可以是同一个基址：VM 先复制元素，
             * 再写目标寄存器，数组已经持有元素的引用。 */
            emit(S3_NEWARR, (uint8_t)base, (uint8_t)base, 0, (uint32_t)e->count);
            free_to(base + 1);
            return base;
        }
        case E_ASSIGN:
            return gen_assign(e);

        default:
            die_at(e->line, e->col, "internal: unknown expression kind");
            return 0;
    }
}

static int is_namespace(const char *name) {
    static const char *ns[] = {
        "fs", "cfg", "time", "os", "fault", "win", "gfx", "event",
        "media", "audio", "img", "ui", 0
    };
    for (int i = 0; ns[i]; i++) {
        if (strcmp(ns[i], name) == 0) return 1;
    }
    return 0;
}

static int gen_call(Expr *e, Expr *callee) {
    /* 数组方法只对非命名空间的接收者生效：
     * fault.set / ui.set 与数组 set 同名，必须靠命名空间区分。 */
    int array_receiver = 1;
    if (callee->kind == E_MEMBER && callee->a->kind == E_IDENT &&
        is_namespace(callee->a->name)) {
        array_receiver = 0;
    }

    if (callee->kind == E_MEMBER && array_receiver) {
        const char *m = callee->member;
        if (strcmp(m, "at") == 0 && e->count == 1) {
            int arr = gen_expr(callee->a);
            int idx = gen_expr(e->list[0]);
            int r = alloc_reg();
            emit(S3_IDXGET, (uint8_t)r, (uint8_t)arr, (uint8_t)idx, 0);
            return r;
        }
        if (strcmp(m, "set") == 0 && e->count == 2) {
            int arr = gen_expr(callee->a);
            int idx = gen_expr(e->list[0]);
            int val = gen_expr(e->list[1]);
            emit(S3_IDXSET, (uint8_t)arr, (uint8_t)idx, (uint8_t)val, 0);
            return val;
        }
        if (strcmp(m, "push") == 0 && e->count == 1) {
            int arr = gen_expr(callee->a);
            int val = gen_expr(e->list[0]);
            emit(S3_APPEND, (uint8_t)arr, (uint8_t)val, 0, 0);
            return arr;
        }
        if (strcmp(m, "pop") == 0 && e->count == 0) {
            int arr = gen_expr(callee->a);
            int r = alloc_reg();
            emit(S3_POPARR, (uint8_t)r, (uint8_t)arr, 0, 0);
            return r;
        }
    }

    char full[160];
    full[0] = '\0';

    if (callee->kind == E_IDENT) {
        snprintf(full, sizeof(full), "%s", callee->name);
    } else if (callee->kind == E_MEMBER && callee->a->kind == E_IDENT) {
        snprintf(full, sizeof(full), "%s.%s", callee->a->name, callee->member);
    }

    if (full[0]) {
        const CallDef *cd = find_call(full);
        if (cd) {
            if (cd->argc >= 0 && cd->argc != e->count) {
                die_at(e->line, e->col, "'%s' expects %d argument(s), got %d",
                       full, cd->argc, e->count);
            }
            int base = g_gen.next_reg;
            for (int i = 0; i < e->count; i++) alloc_reg();
            /* 无参调用也要占住返回值寄存器：S3_SYS 把结果写回 base，
             * 没有分配时会被后续表达式复用覆盖（time.ms() 等）。 */
            if (e->count == 0) alloc_reg();
            for (int i = 0; i < e->count; i++) {
                int r = gen_expr(e->list[i]);
                emit(S3_MOVE, (uint8_t)(base + i), (uint8_t)r, 0, 0);
            }
            /* 结果直接写回 base：VM 在调用宿主前已经把参数复制到
             * 独立数组，因此 rd 与参数区重叠是安全的。 */
            uint32_t imm = ((uint32_t)cd->id << 16) | (uint32_t)e->count;
            emit(S3_SYS, (uint8_t)base, (uint8_t)base, 0, imm);
            free_to(base + 1);
            /* 窗口、绘图、事件、图片与媒体接口需要图形宿主；
             * 终端工具只用到 print/input/fs/cfg/time/os/fault，
             * 不会被误判为 GUI 程序。 */
            if (cd->id == SLE_SYS_UI_MARKUP || cd->id == SLE_SYS_UI_STYLE ||
                cd->id == SLE_SYS_UI_PRESENT || cd->id == SLE_SYS_UI_SET ||
                cd->id == SLE_SYS_UI_GET ||
                (cd->id >= (int)SLE_SYS_WIN_OPEN && cd->id <= (int)SLE_SYS_WIN_TITLE) ||
                (cd->id >= (int)SLE_SYS_GFX_CLEAR && cd->id <= (int)SLE_SYS_GFX_FLUSH) ||
                (cd->id >= (int)SLE_SYS_EVT_POLL && cd->id <= (int)SLE_SYS_EVT_WAIT) ||
                (cd->id >= (int)SLE_SYS_IMG_LOAD && cd->id <= (int)SLE_SYS_IMG_FREE) ||
                cd->id == SLE_SYS_MEDIA_REQ || cd->id == SLE_SYS_OPEN_UI) {
                g_cc.uses_gui = 1;
            }
            return base;
        }

        int fn = find_func(full);
        if (fn >= 0) {
            if (g_cc.funcs[fn].argc != e->count) {
                die_at(e->line, e->col, "'%s' expects %d argument(s), got %d",
                       full, g_cc.funcs[fn].argc, e->count);
            }
            int base = g_gen.next_reg;
            for (int i = 0; i < e->count; i++) alloc_reg();
            if (e->count == 0) alloc_reg();
            for (int i = 0; i < e->count; i++) {
                int r = gen_expr(e->list[i]);
                emit(S3_MOVE, (uint8_t)(base + i), (uint8_t)r, 0, 0);
            }
            emit(S3_CALL, (uint8_t)fn, (uint8_t)base, 0, (uint32_t)e->count);
            free_to(base + 1);
            return base;
        }
    }

    die_at(e->line, e->col, "unknown function '%s'", full[0] ? full : "(expression)");
    return 0;
}

/* ---------------- 语句生成 ---------------- */

static void gen_stmt(Stmt *s);

static void gen_block(Stmt *b) {
    for (int i = 0; i < b->count; i++) gen_stmt(b->body[i]);
}

static void loop_add_break(int at) {
    LoopPatch *l = &g_gen.loops[g_gen.nloops - 1];
    if (l->nbreaks >= l->capbreaks) {
        l->capbreaks = l->capbreaks ? l->capbreaks * 2 : 8;
        l->breaks = (int *)realloc(l->breaks, sizeof(int) * (size_t)l->capbreaks);
        if (!l->breaks) die("out of memory");
    }
    l->breaks[l->nbreaks++] = at;
}

static void loop_add_continue(int at) {
    LoopPatch *l = &g_gen.loops[g_gen.nloops - 1];
    if (l->nconts >= l->capconts) {
        l->capconts = l->capconts ? l->capconts * 2 : 8;
        l->conts = (int *)realloc(l->conts, sizeof(int) * (size_t)l->capconts);
        if (!l->conts) die("out of memory");
    }
    l->conts[l->nconts++] = at;
}

static void gen_stmt(Stmt *s) {
    int mark = g_gen.next_reg;

    switch (s->kind) {
        case S_LET: {
            int lr = alloc_reg();
            g_gen.locals[g_gen.nlocals].name = s->name;
            g_gen.locals[g_gen.nlocals].reg = lr;
            g_gen.nlocals++;
            int v = gen_expr(s->e);
            if (v != lr) emit(S3_MOVE, (uint8_t)lr, (uint8_t)v, 0, 0);
            free_to(lr + 1);
            break;
        }

        case S_EXPR:
            gen_expr(s->e);
            free_to(mark);
            break;

        case S_IF: {
            int c = gen_expr(s->e);
            uint32_t jfalse = emit_jump(S3_JZ, (uint8_t)c, 0);
            free_to(mark);

            gen_block(s->then_s);

            if (s->else_s) {
                uint32_t jend = emit_jump(S3_JMP, 0, 0);
                patch_jump(jfalse, g_cc.code_len);
                gen_stmt(s->else_s);
                patch_jump(jend, g_cc.code_len);
            } else {
                patch_jump(jfalse, g_cc.code_len);
            }
            break;
        }

        case S_WHILE: {
            if (g_gen.nloops >= 32) {
                die_at(s->line, s->col, "loop nesting too deep");
            }
            int loop_start = (int)g_cc.code_len;
            LoopPatch *lp = &g_gen.loops[g_gen.nloops++];
            memset(lp, 0, sizeof(*lp));
            lp->start = loop_start;

            int c = gen_expr(s->e);
            uint32_t jend = emit_jump(S3_JZ, (uint8_t)c, 0);
            free_to(mark);

            gen_block(s->then_s);

            emit(S3_JMP, 0, 0, 0, (uint32_t)loop_start);
            patch_jump(jend, g_cc.code_len);

            for (int i = 0; i < lp->nbreaks; i++) patch_jump((uint32_t)lp->breaks[i], g_cc.code_len);
            for (int i = 0; i < lp->nconts; i++) patch_jump((uint32_t)lp->conts[i], (uint32_t)loop_start);

            free(lp->breaks);
            free(lp->conts);
            g_gen.nloops--;
            break;
        }

        case S_RETURN: {
            int r;
            if (s->e) {
                r = gen_expr(s->e);
            } else {
                r = alloc_reg();
                emit(S3_LOADNIL, (uint8_t)r, 0, 0, 0);
            }
            emit(S3_RET, (uint8_t)r, 0, 0, 0);
            break;
        }

        case S_BREAK:
            if (g_gen.nloops == 0) die_at(s->line, s->col, "break outside loop");
            loop_add_break((int)emit_jump(S3_JMP, 0, 0));
            break;

        case S_CONTINUE:
            if (g_gen.nloops == 0) die_at(s->line, s->col, "continue outside loop");
            loop_add_continue((int)emit_jump(S3_JMP, 0, 0));
            break;

        case S_BLOCK:
            gen_block(s);
            break;

        default:
            die_at(s->line, s->col, "internal: unknown statement");
    }
}

/* ---------------- 函数与主程序 ---------------- */

static void build_functions(void) {
    /* 用户函数已经在 g_cc.funcs 里；把事件处理器编译成普通函数，
     * 这样 __main 只需要 CALL，不需要宿主回调函数指针。 */
    static const char *names[4] = {
        "__on_start", "__on_click", "__on_key", "__on_tick"
    };

    for (int i = 0; i < g_cc.handler_count; i++) {
        HandlerDef *h = &g_cc.handlers[i];
        int idx = g_cc.func_count;
        g_cc.funcs = (FnDef *)realloc(g_cc.funcs,
            sizeof(FnDef) * (size_t)(g_cc.func_count + 1));
        if (!g_cc.funcs) die("out of memory");

        FnDef *f = &g_cc.funcs[idx];
        memset(f, 0, sizeof(*f));
        f->name = xstrdup(names[h->kind]);
        f->argc = (h->kind == 1 || h->kind == 2) ? 1 : 0;
        if (f->argc) {
            f->params = (char **)malloc(sizeof(char *));
            if (!f->params) die("out of memory");
            f->params[0] = xstrdup(h->param ? h->param : "arg");
        }
        f->body = h->body;
        f->line = h->line;
        f->col = h->col;
        h->fn_index = idx;
        g_cc.func_count++;
    }

    int idx = g_cc.func_count;
    g_cc.funcs = (FnDef *)realloc(g_cc.funcs,
        sizeof(FnDef) * (size_t)(g_cc.func_count + 1));
    if (!g_cc.funcs) die("out of memory");
    FnDef *m = &g_cc.funcs[idx];
    memset(m, 0, sizeof(*m));
    m->name = xstrdup("__main");
    m->argc = 0;
    m->body = 0;
    g_cc.main_index = idx;
    g_cc.func_count++;

    for (int i = 0; i < g_cc.func_count; i++) {
        for (int j = i + 1; j < g_cc.func_count; j++) {
            if (strcmp(g_cc.funcs[i].name, g_cc.funcs[j].name) == 0) {
                die("duplicate function name '%s'", g_cc.funcs[i].name);
            }
        }
    }
}

static void gen_reset(void) {
    memset(&g_gen, 0, sizeof(g_gen));
}

static void compile_function(FnDef *f) {
    gen_reset();
    g_gen.next_reg = f->argc;
    g_gen.max_reg = f->argc;

    for (int i = 0; i < f->argc; i++) {
        g_gen.locals[g_gen.nlocals].name = f->params[i];
        g_gen.locals[g_gen.nlocals].reg = i;
        g_gen.nlocals++;
    }

    f->entry = g_cc.code_len;
    gen_block(f->body);

    /* 没有显式 return 的函数在末尾返回 nil */
    int r = alloc_reg();
    emit(S3_LOADNIL, (uint8_t)r, 0, 0, 0);
    emit(S3_RET, (uint8_t)r, 0, 0, 0);

    f->regs = g_gen.max_reg;
}

static void emit_const_string_sys(uint16_t id, const char *text, int len) {
    int r = alloc_reg();
    uint32_t off = const_add(text, len);
    emit(S3_LOADK, (uint8_t)r, 0, 0, off);
    uint32_t imm = ((uint32_t)id << 16) | 1u;
    emit(S3_SYS, (uint8_t)r, (uint8_t)r, 0, imm);
    free_to(r + 1);
}

static void compile_main(void) {
    FnDef *m = &g_cc.funcs[g_cc.main_index];
    gen_reset();

    m->entry = g_cc.code_len;

    if (g_cc.markup) {
        emit_const_string_sys(SLE_SYS_UI_MARKUP, g_cc.markup, g_cc.markup_len);
    }
    if (g_cc.css) {
        emit_const_string_sys(SLE_SYS_UI_STYLE, g_cc.css, g_cc.css_len);
    }

    for (int i = 0; i < g_cc.global_count; i++) {
        int r = gen_expr(g_cc.globals[i].init);
        emit(S3_STOREG, (uint8_t)r, 0, 0, (uint32_t)i);
        free_to(0);
    }

    int start_fn = -1, click_fn = -1, key_fn = -1, tick_fn = -1;
    for (int i = 0; i < g_cc.handler_count; i++) {
        if (g_cc.handlers[i].kind == 0) start_fn = g_cc.handlers[i].fn_index;
        else if (g_cc.handlers[i].kind == 1) click_fn = g_cc.handlers[i].fn_index;
        else if (g_cc.handlers[i].kind == 2) key_fn = g_cc.handlers[i].fn_index;
        else tick_fn = g_cc.handlers[i].fn_index;
    }

    if (start_fn >= 0) {
        int base = g_gen.next_reg;
        alloc_reg();
        emit(S3_CALL, (uint8_t)start_fn, (uint8_t)base, 0, 0);
        free_to(base);
    }

    if (!g_cc.uses_gui) {
        emit(S3_HALT, 0, 0, 0, 0);
        m->regs = g_gen.max_reg;
        return;
    }

    int running = alloc_reg();
    emit(S3_LOADI, (uint8_t)running, 0, 0, 1u);

    uint32_t loop_start = g_cc.code_len;

    int ev = alloc_reg();
    emit(S3_SYS, (uint8_t)ev, (uint8_t)ev, 0,
         ((uint32_t)SLE_SYS_UI_PRESENT << 16) | 0u);

    uint32_t jnil = emit_jump(S3_JZ, (uint8_t)ev, 0);

    int idx0 = alloc_reg();
    emit(S3_LOADI, (uint8_t)idx0, 0, 0, 0);
    int typ = alloc_reg();
    emit(S3_IDXGET, (uint8_t)typ, (uint8_t)ev, (uint8_t)idx0, 0);

    if (click_fn >= 0) {
        int c3 = alloc_reg();
        emit(S3_LOADI, (uint8_t)c3, 0, 0, 3u);
        int eq = alloc_reg();
        emit(S3_EQ, (uint8_t)eq, (uint8_t)typ, (uint8_t)c3, 0);
        uint32_t skip = emit_jump(S3_JZ, (uint8_t)eq, 0);

        int idx1 = alloc_reg();
        emit(S3_LOADI, (uint8_t)idx1, 0, 0, 1u);
        int arg = alloc_reg();
        emit(S3_IDXGET, (uint8_t)arg, (uint8_t)ev, (uint8_t)idx1, 0);
        emit(S3_CALL, (uint8_t)click_fn, (uint8_t)arg, 0, 1u);
        patch_jump(skip, g_cc.code_len);
    }

    if (key_fn >= 0) {
        int c1 = alloc_reg();
        emit(S3_LOADI, (uint8_t)c1, 0, 0, 1u);
        int eq = alloc_reg();
        emit(S3_EQ, (uint8_t)eq, (uint8_t)typ, (uint8_t)c1, 0);
        uint32_t skip = emit_jump(S3_JZ, (uint8_t)eq, 0);

        int idx1 = alloc_reg();
        emit(S3_LOADI, (uint8_t)idx1, 0, 0, 1u);
        int arg = alloc_reg();
        emit(S3_IDXGET, (uint8_t)arg, (uint8_t)ev, (uint8_t)idx1, 0);
        emit(S3_CALL, (uint8_t)key_fn, (uint8_t)arg, 0, 1u);
        patch_jump(skip, g_cc.code_len);
    }

    {
        int c5 = alloc_reg();
        emit(S3_LOADI, (uint8_t)c5, 0, 0, 5u);
        int eq = alloc_reg();
        emit(S3_EQ, (uint8_t)eq, (uint8_t)typ, (uint8_t)c5, 0);
        uint32_t skip = emit_jump(S3_JZ, (uint8_t)eq, 0);
        emit(S3_LOADI, (uint8_t)running, 0, 0, 0u);
        patch_jump(skip, g_cc.code_len);
    }

    patch_jump(jnil, g_cc.code_len);

    if (tick_fn >= 0) {
        int base = g_gen.next_reg;
        alloc_reg();
        emit(S3_CALL, (uint8_t)tick_fn, (uint8_t)base, 0, 0);
        free_to(base);
    }

    emit(S3_JNZ, (uint8_t)running, 0, 0, loop_start);
    emit(S3_HALT, 0, 0, 0, 0);

    m->regs = g_gen.max_reg;
}

/* ---------------- 镜像输出 ---------------- */

static uint32_t fnv1a(const uint8_t *data, uint32_t len) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < len; i++) {
        h ^= data[i];
        h *= 16777619u;
    }
    return h;
}

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void write_image(const char *out_path) {
    uint32_t fn = (uint32_t)g_cc.func_count;
    uint32_t *name_off = (uint32_t *)calloc(fn ? fn : 1, sizeof(uint32_t));
    if (!name_off) die("out of memory");

    for (uint32_t i = 0; i < fn; i++) {
        name_off[i] = const_add(g_cc.funcs[i].name, -1);
    }

    uint32_t total = SLYZ3_HEADER_SIZE + fn * SLYZ3_FUNC_ENTRY +
                     g_cc.const_len + g_cc.code_len;
    if (g_cc.const_len > SLYZ3_MAX_CONST) die("constant pool exceeds %d bytes", SLYZ3_MAX_CONST);
    if (g_cc.code_len > SLYZ3_MAX_CODE) die("code exceeds %d bytes", SLYZ3_MAX_CODE);

    uint8_t *img = (uint8_t *)calloc(total ? total : 1, 1);
    if (!img) die("out of memory");

    memcpy(img, "SLYZ3BIN", 8);
    put16(img + SLYZ3_HDR_VERSION, SLYZ3_VERSION);
    put16(img + SLYZ3_HDR_FLAGS, g_cc.uses_gui ? SLYZ3_FLAG_GUI : 0);
    put32(img + SLYZ3_HDR_CONSTSIZE, g_cc.const_len);
    put32(img + SLYZ3_HDR_CODESIZE, g_cc.code_len);
    put32(img + SLYZ3_HDR_ENTRY, g_cc.funcs[g_cc.main_index].entry);
    put32(img + SLYZ3_HDR_GLOBALS, (uint32_t)g_cc.global_count);
    put32(img + SLYZ3_HDR_FUNCS, fn);
    put32(img + SLYZ3_HDR_REGS, SLYZ3_REGS_PER_FRAME);
    put32(img + SLYZ3_HDR_SRCSIZE, (uint32_t)g_cc.src_len);
    put32(img + SLYZ3_HDR_RESERVED, 0);

    uint8_t *fp = img + SLYZ3_HEADER_SIZE;
    for (uint32_t i = 0; i < fn; i++) {
        put32(fp + 0, g_cc.funcs[i].entry);
        put32(fp + 4, (uint32_t)g_cc.funcs[i].argc);
        put32(fp + 8, (uint32_t)(g_cc.funcs[i].regs > 0 ? g_cc.funcs[i].regs : 1));
        put32(fp + 12, name_off[i]);
        fp += SLYZ3_FUNC_ENTRY;
    }

    memcpy(fp, g_cc.consts, g_cc.const_len);
    fp += g_cc.const_len;
    memcpy(fp, g_cc.code, g_cc.code_len);

    uint32_t sum = fnv1a(img + SLYZ3_HEADER_SIZE + fn * SLYZ3_FUNC_ENTRY,
                         g_cc.const_len + g_cc.code_len);
    put32(img + SLYZ3_HDR_CHECKSUM, sum);

    FILE *f = fopen(out_path, "wb");
    if (!f) die("cannot open output '%s'", out_path);
    if (fwrite(img, 1, total, f) != total) {
        fclose(f);
        die("cannot write output '%s'", out_path);
    }
    fclose(f);
    free(img);
    free(name_off);

    printf("slyz3c: wrote %s (%u bytes, code %u, const %u, globals %d, functions %u)\n",
           out_path, total, g_cc.code_len, g_cc.const_len,
           g_cc.global_count, fn);
}

/* ============================================================
 * 裸机 AOT 后端（--baremetal）
 * ------------------------------------------------------------
 * 输出 NASM 汇编（bits 64 / org <load>），由 nasm -f bin 生成
 * 扁平镜像，再交给 mkslys 打包成 .slys。
 *
 * 约定：
 *   - 所有值都是 64 位整数；字符串是 .rodata 里的零结尾字节串，
 *     表达式结果是它的地址。
 *   - 函数调用遵循 Microsoft x64 ABI（rcx rdx r8 r9，第 5 个起走栈），
 *     返回值在 rax。内核与 .slys 模块都由 MinGW gcc 构建，使用同一套
 *     约定，因此 C 侧可以按普通函数指针直接调用模块入口。
 *   - 模块自带极小的运行时（内存分配、磁盘转发、字符串/输出），
 *     不链接 libc，也不依赖内核里的 Slyz VM。
 *   - 内核通过 rdi 传入 SlysFsApi*，模块把它保存在 __api，
 *     之后 disk.* 内建通过函数指针转发（见 src/include/slysfs.h）。
 * ============================================================ */

static int      g_bm;                 /* 1 = 裸机 AOT 模式 */
static uint32_t g_bm_org = 0x03000000u;
static char     g_bm_entry[96];       /* 导出入口函数名（空 = 直接执行） */
static uint32_t g_bm_heap = 2u * 1024u * 1024u;
static FILE    *g_asm;
static int      g_bm_label;

typedef struct {
    const char *name;
    int slot;
} BmLocal;

static BmLocal g_bm_locals[192];
static int     g_bm_nlocals;
static int     g_bm_slot;

typedef struct {
    int id;
    char *text;
    int len;
} BmString;

static BmString *g_bm_strs;
static int       g_bm_nstrs;

static const char *g_bm_cur_fn = "";
static int g_bm_loop_break[32];
static int g_bm_loop_cont[32];
static int g_bm_nloops;

static int bm_label(void) { return g_bm_label++; }

static void aemit(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_asm, fmt, ap);
    va_end(ap);
}

static int bm_string_add(const char *s, int len) {
    if (len < 0) len = (int)strlen(s);
    for (int i = 0; i < g_bm_nstrs; i++) {
        if (g_bm_strs[i].len == len &&
            (len == 0 || memcmp(g_bm_strs[i].text, s, (size_t)len) == 0)) {
            return g_bm_strs[i].id;
        }
    }
    g_bm_strs = (BmString *)realloc(g_bm_strs, sizeof(BmString) * (size_t)(g_bm_nstrs + 1));
    if (!g_bm_strs) die("out of memory");
    BmString *b = &g_bm_strs[g_bm_nstrs++];
    b->id = g_bm_label++;
    b->len = len;
    b->text = (char *)malloc((size_t)len + 1);
    if (!b->text) die("out of memory");
    if (len) memcpy(b->text, s, (size_t)len);
    b->text[len] = '\0';
    return b->id;
}

static int bm_find_local(const char *name) {
    for (int i = g_bm_nlocals - 1; i >= 0; i--) {
        if (strcmp(g_bm_locals[i].name, name) == 0) return g_bm_locals[i].slot;
    }
    return -1;
}

static int bm_find_global(const char *name) {
    for (int i = 0; i < g_cc.global_count; i++) {
        if (strcmp(g_cc.globals[i].name, name) == 0) return i;
    }
    return -1;
}

/* ---------------- 裸机内建函数表 ---------------- */

typedef struct {
    const char *name;
    const char *sym;
    int argc;
} BmCall;

static const BmCall g_bm_calls[] = {
    { "mem.alloc",  "__bm_alloc",   1 },
    { "mem.free",   "__bm_free",    1 },
    { "mem.zero",   "__bm_zero",    2 },
    { "mem.copy",   "__bm_copy",    3 },

    { "disk.read",  "__bm_dread",   2 },
    { "disk.write", "__bm_dwrite",  2 },
    { "disk.sectors","__bm_sectors", 0 },
    { "disk.flush", "__bm_flush",   0 },

    { "load8",      "__bm_load8",   2 },
    { "load16",     "__bm_load16",  2 },
    { "load32",     "__bm_load32",  2 },
    { "load64",     "__bm_load64",  2 },
    { "store8",     "__bm_store8",  3 },
    { "store16",    "__bm_store16", 3 },
    { "store32",    "__bm_store32", 3 },
    { "store64",    "__bm_store64", 3 },

    { "puts",       "__bm_puts",    1 },
    { "putdec",     "__bm_putdec",  1 },
    { "puthex",     "__bm_puthex",  1 },
    { "strlen",     "__bm_strlen",  1 },
    { "strcmp",     "__bm_strcmp",  2 },
    { "halt",       "__bm_halt",    0 },

    { 0, 0, 0 }
};

static const BmCall *bm_find_call(const char *name) {
    for (int i = 0; g_bm_calls[i].name; i++) {
        if (strcmp(g_bm_calls[i].name, name) == 0) return &g_bm_calls[i];
    }
    return 0;
}

/* ---------------- 表达式 ---------------- */

static void bm_expr(Expr *e);

static void bm_store_target(Expr *lhs) {
    int slot = bm_find_local(lhs->name);
    if (slot >= 0) {
        aemit("    mov [rbp-%d], rax\n", 8 * (slot + 1));
        return;
    }
    if (bm_find_global(lhs->name) >= 0) {
        aemit("    mov [rel __g_%s], rax\n", lhs->name);
        return;
    }
    die_at(lhs->line, lhs->col, "assignment to unknown variable '%s'", lhs->name);
}

static void bm_load_target(Expr *lhs) {
    int slot = bm_find_local(lhs->name);
    if (slot >= 0) {
        aemit("    mov rax, [rbp-%d]\n", 8 * (slot + 1));
        return;
    }
    if (bm_find_global(lhs->name) >= 0) {
        aemit("    mov rax, [rel __g_%s]\n", lhs->name);
        return;
    }
    die_at(lhs->line, lhs->col, "unknown variable '%s'", lhs->name);
}

/* rax = lhs, rcx = rhs，把结果留在 rax。*/
static void bm_binop(int op) {
    switch (op) {
        case '+': aemit("    add rax, rcx\n"); break;
        case '-': aemit("    sub rax, rcx\n"); break;
        case '*': aemit("    imul rax, rcx\n"); break;
        case '/': {
            int lz = bm_label(), ld = bm_label();
            aemit("    test rcx, rcx\n    jz __L%d\n", lz);
            aemit("    cqo\n    idiv rcx\n");
            aemit("    jmp __L%d\n", ld);
            aemit("__L%d:\n    xor eax, eax\n__L%d:\n", lz, ld);
            break;
        }
        case '%': {
            int lz = bm_label(), ld = bm_label();
            aemit("    test rcx, rcx\n    jz __L%d\n", lz);
            aemit("    cqo\n    idiv rcx\n    mov rax, rdx\n");
            aemit("    jmp __L%d\n", ld);
            aemit("__L%d:\n    xor eax, eax\n__L%d:\n", lz, ld);
            break;
        }
        case 7: aemit("    or rax, rcx\n"); break;
        case 8: aemit("    xor rax, rcx\n"); break;
        case 9: aemit("    and rax, rcx\n"); break;
        case 10: aemit("    shl rax, cl\n"); break;
        case 11: aemit("    shr rax, cl\n"); break;
        case '<':  aemit("    cmp rax, rcx\n    setl al\n    movzx rax, al\n"); break;
        case 1:    aemit("    cmp rax, rcx\n    setle al\n    movzx rax, al\n"); break;
        case '>':  aemit("    cmp rax, rcx\n    setg al\n    movzx rax, al\n"); break;
        case 2:    aemit("    cmp rax, rcx\n    setge al\n    movzx rax, al\n"); break;
        case 3:    aemit("    cmp rax, rcx\n    sete al\n    movzx rax, al\n"); break;
        case 4:    aemit("    cmp rax, rcx\n    setne al\n    movzx rax, al\n"); break;
        default:   die("internal: unknown binary operator %d", op);
    }
}

static void bm_bin(Expr *e) {
    int op = e->op;

    if (op == 5 || op == 6) {   /* and / or，短路求值 */
        int lshort = bm_label(), lend = bm_label();
        bm_expr(e->a);
        aemit("    test rax, rax\n");
        aemit("    j%s __L%d\n", (op == 5) ? "z" : "nz", lshort);
        bm_expr(e->b);
        aemit("    test rax, rax\n");
        aemit("    j%s __L%d\n", (op == 5) ? "z" : "nz", lshort);
        aemit("    mov rax, %d\n    jmp __L%d\n", (op == 5) ? 1 : 0, lend);
        aemit("__L%d:\n    mov rax, %d\n", lshort, (op == 5) ? 0 : 1);
        aemit("__L%d:\n", lend);
        return;
    }

    bm_expr(e->a);
    aemit("    push rax\n");
    bm_expr(e->b);
    aemit("    mov rcx, rax\n    pop rax\n");
    bm_binop(op);
}

/* 复合赋值：assign_op 是 A_* 编号，转换成二元运算编号。 */
static int bm_binop_from_assign(int aop) {
    switch (aop) {
        case A_ADD:  return '+';
        case A_SUB:  return '-';
        case A_MUL:  return '*';
        case A_DIV:  return '/';
        case A_MOD:  return '%';
        case A_BAND: return 9;
        case A_BOR:  return 7;
        case A_BXOR: return 8;
        case A_SHL:  return 10;
        case A_SHR:  return 11;
        default:     return '+';
    }
}

static void bm_assign(Expr *e) {
    if (e->a->kind != E_IDENT) {
        die_at(e->line, e->col,
               "baremetal mode only supports assignment to variables");
    }
    if (e->assign_op == A_SET) {
        bm_expr(e->b);
        bm_store_target(e->a);
        return;
    }
    bm_load_target(e->a);
    aemit("    push rax\n");
    bm_expr(e->b);
    aemit("    mov rcx, rax\n    pop rax\n");
    bm_binop(bm_binop_from_assign(e->assign_op));
    bm_store_target(e->a);
}

/* 参数传递遵循 Microsoft x64 ABI：
 *   rcx, rdx, r8, r9 依次是前四个整数参数，第 5 个起放在
 *   [rsp+32]、[rsp+40]…（调用者预留 32 字节 shadow space）。
 * 内核与 .slys 模块都由 MinGW gcc 构建，使用同一套 ABI，
 * 因此 C 侧的 SlysFsCall 函数指针可以直接调用模块入口。
 *
 * 返回值为本次调用压掉的栈字节数，调用方在 call 之后补 add rsp。 */
static int bm_push_args(Expr **args, int count) {
    if (count > 6) {
        die("baremetal functions support at most 6 arguments");
    }

    int stack_args = (count > 4) ? count - 4 : 0;
    int frame = 32 + 8 * stack_args;
    int pad = ((frame / 8) & 1) ? 8 : 0;

    aemit("    sub rsp, %d\n", frame + pad);
    for (int i = 0; i < count; i++) {
        bm_expr(args[i]);
        aemit("    mov [rsp+%d], rax\n", 8 * i);
    }

    static const char *regs[4] = { "rcx", "rdx", "r8", "r9" };
    for (int i = 0; i < count && i < 4; i++) {
        aemit("    mov %s, [rsp+%d]\n", regs[i], 8 * i);
    }
    return frame + pad;
}

static void bm_call(Expr *e, Expr *callee) {
    char full[160];
    full[0] = '\0';

    if (callee->kind == E_IDENT) {
        snprintf(full, sizeof(full), "%s", callee->name);
    } else if (callee->kind == E_MEMBER && callee->a->kind == E_IDENT) {
        snprintf(full, sizeof(full), "%s.%s", callee->a->name, callee->member);
    }

    if (full[0]) {
        const BmCall *bc = bm_find_call(full);
        if (bc) {
            if (bc->argc != e->count) {
                die_at(e->line, e->col, "'%s' expects %d argument(s), got %d",
                       full, bc->argc, e->count);
            }
            int pad = bm_push_args(e->list, e->count);
            aemit("    call %s\n", bc->sym);
            if (pad) aemit("    add rsp, %d\n", pad);
            return;
        }

        int fn = find_func(full);
        if (fn >= 0) {
            if (g_cc.funcs[fn].argc != e->count) {
                die_at(e->line, e->col, "'%s' expects %d argument(s), got %d",
                       full, g_cc.funcs[fn].argc, e->count);
            }
            int pad = bm_push_args(e->list, e->count);
            aemit("    call __fn_%s\n", full);
            if (pad) aemit("    add rsp, %d\n", pad);
            return;
        }
    }

    die_at(e->line, e->col, "unknown function '%s' in baremetal mode",
           full[0] ? full : "(expression)");
}

static void bm_expr(Expr *e) {
    switch (e->kind) {
        case E_INT:
            aemit("    mov rax, %lld\n", (long long)e->ival);
            break;
        case E_STR: {
            int id = bm_string_add(e->sval, e->slen);
            aemit("    lea rax, [rel __str_%d]\n", id);
            break;
        }
        case E_BOOL:
            aemit("    mov rax, %d\n", e->ival ? 1 : 0);
            break;
        case E_NIL:
            aemit("    xor eax, eax\n");
            break;
        case E_IDENT: {
            int slot = bm_find_local(e->name);
            if (slot >= 0) {
                aemit("    mov rax, [rbp-%d]\n", 8 * (slot + 1));
            } else if (bm_find_global(e->name) >= 0) {
                aemit("    mov rax, [rel __g_%s]\n", e->name);
            } else {
                die_at(e->line, e->col, "unknown variable '%s'", e->name);
            }
            break;
        }
        case E_MEMBER:
            if (strcmp(e->member, "length") == 0) {
                bm_expr(e->a);
                aemit("    call __bm_strlen\n");
            } else {
                die_at(e->line, e->col,
                       "property '%s' is not available in baremetal mode", e->member);
            }
            break;
        case E_UN:
            bm_expr(e->a);
            if (e->op == '-') aemit("    neg rax\n");
            else if (e->op == '~') aemit("    not rax\n");
            else aemit("    test rax, rax\n    setz al\n    movzx rax, al\n");
            break;
        case E_BIN:
            bm_bin(e);
            break;
        case E_CALL:
            bm_call(e, e->a);
            break;
        case E_ASSIGN:
            bm_assign(e);
            break;
        case E_INDEX:
            die_at(e->line, e->col,
                   "array indexing is not available in baremetal mode; use load8/store8");
            break;
        case E_ARRAY:
            die_at(e->line, e->col,
                   "array literals are not available in baremetal mode; use mem.alloc");
            break;
        default:
            die_at(e->line, e->col, "internal: unknown expression kind");
    }
}

/* ---------------- 语句 ---------------- */

static void bm_stmt(Stmt *s);
static void bm_block(Stmt *b);

static void bm_alloc_local(const char *name, int line, int col) {
    if (g_bm_slot >= 120) {
        die_at(line, col, "too many local variables in one function (max 120)");
    }
    g_bm_locals[g_bm_nlocals].name = name;
    g_bm_locals[g_bm_nlocals].slot = g_bm_slot++;
    g_bm_nlocals++;
}

static void bm_block(Stmt *b) {
    int save_slot = g_bm_slot;
    int save_n = g_bm_nlocals;
    for (int i = 0; i < b->count; i++) bm_stmt(b->body[i]);
    g_bm_slot = save_slot;
    g_bm_nlocals = save_n;
}

static void bm_stmt(Stmt *s) {
    switch (s->kind) {
        case S_LET:
            bm_alloc_local(s->name, s->line, s->col);
            bm_expr(s->e);
            aemit("    mov [rbp-%d], rax\n", 8 * (g_bm_locals[g_bm_nlocals - 1].slot + 1));
            break;

        case S_EXPR:
            bm_expr(s->e);
            break;

        case S_IF: {
            int lelse = bm_label();
            int lend = bm_label();
            bm_expr(s->e);
            aemit("    test rax, rax\n    jz __L%d\n", lelse);
            bm_block(s->then_s);
            aemit("    jmp __L%d\n", lend);
            aemit("__L%d:\n", lelse);
            if (s->else_s) bm_stmt(s->else_s);
            aemit("__L%d:\n", lend);
            break;
        }

        case S_WHILE: {
            if (g_bm_nloops >= 32) die_at(s->line, s->col, "loop nesting too deep");
            int lstart = bm_label();
            int lend = bm_label();
            g_bm_loop_break[g_bm_nloops] = lend;
            g_bm_loop_cont[g_bm_nloops] = lstart;
            g_bm_nloops++;
            aemit("__L%d:\n", lstart);
            bm_expr(s->e);
            aemit("    test rax, rax\n    jz __L%d\n", lend);
            bm_block(s->then_s);
            aemit("    jmp __L%d\n", lstart);
            aemit("__L%d:\n", lend);
            g_bm_nloops--;
            break;
        }

        case S_RETURN:
            if (s->e) bm_expr(s->e);
            else aemit("    xor eax, eax\n");
            aemit("    jmp __fn_%s__ret\n", g_bm_cur_fn);
            break;

        case S_BREAK:
            if (g_bm_nloops == 0) die_at(s->line, s->col, "break outside loop");
            aemit("    jmp __L%d\n", g_bm_loop_break[g_bm_nloops - 1]);
            break;

        case S_CONTINUE:
            if (g_bm_nloops == 0) die_at(s->line, s->col, "continue outside loop");
            aemit("    jmp __L%d\n", g_bm_loop_cont[g_bm_nloops - 1]);
            break;

        case S_BLOCK:
            bm_block(s);
            break;

        default:
            die_at(s->line, s->col, "internal: unknown statement");
    }
}

/* ---------------- 函数与模块 ---------------- */

static void bm_function(FnDef *f) {
    if (f->argc > 6) {
        die_at(f->line, f->col, "baremetal functions support at most 6 arguments");
    }

    g_bm_cur_fn = f->name;
    g_bm_slot = 0;
    g_bm_nlocals = 0;
    g_bm_nloops = 0;

    static const char *regs[4] = { "rcx", "rdx", "r8", "r9" };
    int exported = (g_bm_entry[0] && strcmp(f->name, g_bm_entry) == 0);

    aemit("\n__fn_%s:\n", f->name);
    aemit("    push rbp\n    mov rbp, rsp\n    sub rsp, 1024\n");
    if (exported) {
        /* 导出函数直接由内核 C 代码调用，必须遵守 MS x64 的非易失
         * 寄存器约定：rbx/rsi/rdi/r12-r15 由被调者保存。 */
        aemit("    push rbx\n    push rsi\n    push rdi\n");
        aemit("    push r12\n    push r13\n    push r14\n    push r15\n");
        aemit("    sub rsp, 8\n");
    }
    for (int i = 0; i < f->argc; i++) {
        if (i < 4) {
            aemit("    mov [rbp-%d], %s\n", 8 * (i + 1), regs[i]);
        } else {
            /* 第 5 个起的参数在调用者栈帧：[rbp+48] 是第一个栈参数，
             * 之后每 8 字节一个（rbp+8 是返回地址，rbp+16..47 是 shadow space）。 */
            aemit("    mov rax, [rbp+%d]\n", 48 + 8 * (i - 4));
            aemit("    mov [rbp-%d], rax\n", 8 * (i + 1));
        }
        bm_alloc_local(f->params[i], f->line, f->col);
    }

    bm_block(f->body);

    aemit("    xor eax, eax\n");
    aemit("__fn_%s__ret:\n", f->name);
    if (exported) {
        aemit("    add rsp, 8\n");
        aemit("    pop r15\n    pop r14\n    pop r13\n    pop r12\n");
        aemit("    pop rdi\n    pop rsi\n    pop rbx\n");
    }
    aemit("    mov rsp, rbp\n    pop rbp\n    ret\n");
}

static void bm_emit_runtime(void) {
    /* 自带运行时：不引用任何内核符号，只通过 __api 里的函数指针
     * 访问磁盘与日志。约定与生成代码一致——Microsoft x64 ABI：
     * 参数依次在 rcx / rdx / r8 / r9，返回值在 rax。
     * SlysFsApi 的布局见 src/include/slysfs.h。 */
    aemit(
        "\n; ---------- baremetal runtime (Microsoft x64 ABI) ----------\n"
        "__api: dq 0\n"
        "__heap_used: dq 0\n"
        "__scratch: times 32 db 0\n"
        "\n"
        "__bm_alloc:                 ; rcx = bytes -> rax = ptr\n"
        "    mov rax, [rel __heap_used]\n"
        "    add rax, 15\n"
        "    and rax, -16\n"
        "    lea rdx, [rax + rcx]\n"
        "    cmp rdx, %u\n"
        "    ja .oom\n"
        "    mov [rel __heap_used], rdx\n"
        "    lea rdx, [rel __bm_heap]\n"
        "    add rax, rdx\n"
        "    ret\n"
        ".oom:\n"
        "    xor eax, eax\n"
        "    ret\n"
        "\n"
        "__bm_free:\n"
        "    xor eax, eax\n"
        "    ret\n"
        "\n"
        "__bm_zero:                  ; rcx = ptr, rdx = len\n"
        "    mov rdi, rcx\n"
        "    mov rcx, rdx\n"
        "    xor eax, eax\n"
        "    rep stosb\n"
        "    ret\n"
        "\n"
        "__bm_copy:                  ; rcx = dst, rdx = src, r8 = len\n"
        "    mov rdi, rcx\n"
        "    mov rsi, rdx\n"
        "    mov rcx, r8\n"
        "    rep movsb\n"
        "    ret\n"
        "\n"
        "__bm_dread:                 ; rcx = lba, rdx = buf\n"
        "    mov rax, [rel __api]\n"
        "    test rax, rax\n"
        "    jz .fail\n"
        "    mov rax, [rax + 8]\n"
        "    test rax, rax\n"
        "    jz .fail\n"
        "    jmp rax\n"
        ".fail:\n"
        "    xor eax, eax\n"
        "    ret\n"
        "\n"
        "__bm_dwrite:                ; rcx = lba, rdx = buf\n"
        "    mov rax, [rel __api]\n"
        "    test rax, rax\n"
        "    jz .fail\n"
        "    mov rax, [rax + 16]\n"
        "    test rax, rax\n"
        "    jz .fail\n"
        "    jmp rax\n"
        ".fail:\n"
        "    xor eax, eax\n"
        "    ret\n"
        "\n"
        "__bm_sectors:\n"
        "    mov rax, [rel __api]\n"
        "    test rax, rax\n"
        "    jz .fail\n"
        "    mov rax, [rax + 24]\n"
        "    test rax, rax\n"
        "    jz .fail\n"
        "    jmp rax\n"
        ".fail:\n"
        "    xor eax, eax\n"
        "    ret\n"
        "\n"
        "__bm_flush:\n"
        "    mov rax, [rel __api]\n"
        "    test rax, rax\n"
        "    jz .fail\n"
        "    mov rax, [rax + 40]\n"
        "    test rax, rax\n"
        "    jz .fail\n"
        "    jmp rax\n"
        ".fail:\n"
        "    xor eax, eax\n"
        "    ret\n"
        "\n"
        "__bm_puts:                  ; rcx = zero-terminated string\n"
        "    mov rax, [rel __api]\n"
        "    test rax, rax\n"
        "    jz .done\n"
        "    mov rax, [rax + 32]\n"
        "    test rax, rax\n"
        "    jz .done\n"
        "    jmp rax\n"
        ".done:\n"
        "    ret\n"
        "\n"
        "__bm_load8:                 ; rcx = ptr, rdx = offset\n"
        "    movzx eax, byte [rcx + rdx]\n"
        "    ret\n"
        "__bm_load16:\n"
        "    movzx eax, word [rcx + rdx]\n"
        "    ret\n"
        "__bm_load32:\n"
        "    mov eax, dword [rcx + rdx]\n"
        "    ret\n"
        "__bm_load64:\n"
        "    mov rax, qword [rcx + rdx]\n"
        "    ret\n"
        "\n"
        "__bm_store8:                ; rcx = ptr, rdx = offset, r8 = value\n"
        "    mov [rcx + rdx], r8b\n"
        "    xor eax, eax\n"
        "    ret\n"
        "__bm_store16:\n"
        "    mov [rcx + rdx], r8w\n"
        "    xor eax, eax\n"
        "    ret\n"
        "__bm_store32:\n"
        "    mov [rcx + rdx], r8d\n"
        "    xor eax, eax\n"
        "    ret\n"
        "__bm_store64:\n"
        "    mov [rcx + rdx], r8\n"
        "    xor eax, eax\n"
        "    ret\n"
        "\n"
        "__bm_strlen:                ; rcx = str -> rax\n"
        "    xor eax, eax\n"
        ".loop:\n"
        "    cmp byte [rcx + rax], 0\n"
        "    je .done\n"
        "    inc rax\n"
        "    jmp .loop\n"
        ".done:\n"
        "    ret\n"
        "\n"
        "__bm_strcmp:                ; rcx, rdx -> rax (<0/0/>0)\n"
        "    xor r8d, r8d\n"
        ".loop:\n"
        "    mov al, [rcx + r8]\n"
        "    mov r9b, [rdx + r8]\n"
        "    cmp al, r9b\n"
        "    jne .diff\n"
        "    test al, al\n"
        "    jz .eq\n"
        "    inc r8\n"
        "    jmp .loop\n"
        ".diff:\n"
        "    movzx eax, al\n"
        "    movzx r9d, r9b\n"
        "    sub eax, r9d\n"
        "    ret\n"
        ".eq:\n"
        "    xor eax, eax\n"
        "    ret\n"
        "\n"
        "__bm_putdec:                ; rcx = signed value\n"
        "    lea rsi, [rel __scratch]\n"
        "    add rsi, 31\n"
        "    mov byte [rsi], 0\n"
        "    mov rax, rcx\n"
        "    xor r9d, r9d\n"
        "    test rax, rax\n"
        "    jns .pos\n"
        "    neg rax\n"
        "    mov r9d, 1\n"
        ".pos:\n"
        "    mov ecx, 10\n"
        ".loop:\n"
        "    xor edx, edx\n"
        "    div rcx\n"
        "    add dl, '0'\n"
        "    dec rsi\n"
        "    mov [rsi], dl\n"
        "    test rax, rax\n"
        "    jnz .loop\n"
        "    test r9d, r9d\n"
        "    jz .out\n"
        "    dec rsi\n"
        "    mov byte [rsi], '-'\n"
        ".out:\n"
        "    mov rcx, rsi\n"
        "    jmp __bm_puts\n"
        "\n"
        "__bm_puthex:                ; rcx = value\n"
        "    lea rsi, [rel __scratch]\n"
        "    add rsi, 31\n"
        "    mov byte [rsi], 0\n"
        "    mov rax, rcx\n"
        "    mov r8d, 16\n"
        ".loop:\n"
        "    mov rdx, rax\n"
        "    and rdx, 15\n"
        "    cmp dl, 10\n"
        "    jb .num\n"
        "    add dl, 'A' - 10\n"
        "    jmp .put\n"
        ".num:\n"
        "    add dl, '0'\n"
        ".put:\n"
        "    dec rsi\n"
        "    mov [rsi], dl\n"
        "    shr rax, 4\n"
        "    dec r8d\n"
        "    jnz .loop\n"
        "    mov rcx, rsi\n"
        "    jmp __bm_puts\n"
        "\n"
        "__bm_halt:\n"
        "    cli\n"
        ".h:\n"
        "    hlt\n"
        "    jmp .h\n",
        (unsigned)g_bm_heap);
}

static void bm_emit_program(const char *out_path) {
    g_asm = fopen(out_path, "w");
    if (!g_asm) die("cannot open output '%s'", out_path);

    aemit("; generated by slyz3c --baremetal\n");
    aemit("bits 64\ndefault rel\n");
    aemit("org 0x%X\n\n", g_bm_org);
    aemit("section .text\n\n");

    /* _start：保存内核 API 表，初始化全局，然后返回导出入口地址
     * （或直接执行 on start / main）。 */
    aemit("_start:\n");
    aemit("    push rbp\n    mov rbp, rsp\n    sub rsp, 1024\n");
    aemit("    mov [rel __api], rcx\n");
    aemit("    push rbx\n    push rsi\n    push rdi\n");
    aemit("    push r12\n    push r13\n    push r14\n    push r15\n");
    aemit("    sub rsp, 8\n");
    aemit("    call __init_globals\n");

    int start_fn = -1;
    for (int i = 0; i < g_cc.handler_count; i++) {
        if (g_cc.handlers[i].kind == 0) start_fn = g_cc.handlers[i].fn_index;
    }

    if (g_bm_entry[0]) {
        /* 模块可以同时声明 on start 做初始化（分配缓冲、挂载默认卷）；
         * _start 先执行它，再把导出函数地址交给内核。 */
        if (start_fn >= 0) {
            aemit("    call __fn_%s\n", g_cc.funcs[start_fn].name);
        }
        aemit("    lea rax, [rel __fn_%s]\n", g_bm_entry);
    } else {
        if (start_fn >= 0) {
            aemit("    call __fn_%s\n", g_cc.funcs[start_fn].name);
        } else {
            int main_fn = find_func("main");
            if (main_fn >= 0) aemit("    call __fn_main\n");
        }
        aemit("    xor eax, eax\n");
    }
    aemit("    add rsp, 8\n");
    aemit("    pop r15\n    pop r14\n    pop r13\n    pop r12\n");
    aemit("    pop rdi\n    pop rsi\n    pop rbx\n");
    aemit("    mov rsp, rbp\n    pop rbp\n    ret\n");

    /* 全局初始化函数 */
    aemit("\n__init_globals:\n");
    aemit("    push rbp\n    mov rbp, rsp\n    sub rsp, 1024\n");
    g_bm_slot = 0;
    g_bm_nlocals = 0;
    for (int i = 0; i < g_cc.global_count; i++) {
        bm_expr(g_cc.globals[i].init);
        aemit("    mov [rel __g_%s], rax\n", g_cc.globals[i].name);
    }
    aemit("    mov rsp, rbp\n    pop rbp\n    ret\n");

    /* 用户函数 */
    for (int i = 0; i < g_cc.func_count; i++) {
        if (i == g_cc.main_index) continue;
        bm_function(&g_cc.funcs[i]);
    }

    bm_emit_runtime();

    /* 字符串常量 */
    aemit("\nsection .rodata\n");
    for (int i = 0; i < g_bm_nstrs; i++) {
        aemit("__str_%d: db ", g_bm_strs[i].id);
        if (g_bm_strs[i].len == 0) {
            aemit("0\n");
            continue;
        }
        for (int k = 0; k < g_bm_strs[i].len; k++) {
            unsigned ch = (unsigned char)g_bm_strs[i].text[k];
            aemit("%u,", ch);
        }
        aemit("0\n");
    }

    /* 全局变量 */
    aemit("\nsection .data\n");
    for (int i = 0; i < g_cc.global_count; i++) {
        aemit("__g_%s: dq 0\n", g_cc.globals[i].name);
    }

    /* 模块私有堆：loader 按 bss_size 清零，镜像本身不携带这 2MB。 */
    aemit("\nsection .bss\n__bm_heap: resb %u\n", (unsigned)g_bm_heap);

    fclose(g_asm);
    g_asm = 0;

    printf("slyz3c: wrote %s (baremetal, org=0x%X, heap=%u, entry=%s)\n",
           out_path, (unsigned)g_bm_org, (unsigned)g_bm_heap,
           g_bm_entry[0] ? g_bm_entry : "(direct)");
}

/* ---------------- 入口 ---------------- */

static char *read_file(const char *path, int *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) die("cannot open '%s'", path);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); die("cannot stat '%s'", path); }
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); die("out of memory"); }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    *out_len = (int)got;
    return buf;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr,
            "SLYZ v3 compiler\n"
            "usage: slyz3c compile <source.slyz3> -o <output.slyz> [options]\n"
            "       slyz3c check   <source.slyz3> [options]\n"
            "options:\n"
            "  --baremetal        emit NASM x86-64 assembly instead of SLYZ3BIN\n"
            "  --org <addr>       load address for baremetal output (default 0x03000000)\n"
            "  --entry <name>     export <name> as the module entry (returns its address)\n"
            "  --heap <bytes>     baremetal heap size (default 2 MiB)\n");
        return 2;
    }

    int check_only = (strcmp(argv[1], "check") == 0);
    if (!check_only && strcmp(argv[1], "compile") != 0) {
        fprintf(stderr, "slyz3c: unknown command '%s'\n", argv[1]);
        return 2;
    }

    const char *out_path = 0;
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            out_path = argv[++i];
        } else if (strcmp(argv[i], "--baremetal") == 0) {
            g_bm = 1;
        } else if (strcmp(argv[i], "--org") == 0 && i + 1 < argc) {
            g_bm_org = (uint32_t)strtoul(argv[++i], 0, 0);
        } else if (strcmp(argv[i], "--entry") == 0 && i + 1 < argc) {
            snprintf(g_bm_entry, sizeof(g_bm_entry), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--heap") == 0 && i + 1 < argc) {
            g_bm_heap = (uint32_t)strtoul(argv[++i], 0, 0);
        } else {
            fprintf(stderr, "slyz3c: unknown option '%s'\n", argv[i]);
            return 2;
        }
    }
    if (!check_only && !out_path) {
        fprintf(stderr, "slyz3c: compile needs -o <output>\n");
        return 2;
    }
    if (g_bm && g_bm_heap < 4096u) {
        fprintf(stderr, "slyz3c: --heap must be at least 4096 bytes\n");
        return 2;
    }

    memset(&g_cc, 0, sizeof(g_cc));
    g_path = argv[2];
    g_cc.path = argv[2];

    int len = 0;
    char *src = read_file(argv[2], &len);
    g_cc.src = src;
    g_cc.src_len = len;

    g_src = src;
    g_src_len = len;
    g_pos = 0;
    g_line = 1;
    g_col = 1;
    lex_next();

    parse_program();

    if (g_bm) {
        /* 裸机模块没有 GUI 运行时：screen/style/click/tick/key 全部拒绝，
         * 只接受 state / fn / on start。 */
        if (g_cc.markup || g_cc.css) {
            die("screen/style blocks are not supported in baremetal mode");
        }
        for (int i = 0; i < g_cc.handler_count; i++) {
            if (g_cc.handlers[i].kind != 0) {
                die_at(g_cc.handlers[i].line, g_cc.handlers[i].col,
                       "only 'on start' is supported in baremetal mode");
            }
        }
        if (g_bm_entry[0] && find_func(g_bm_entry) < 0) {
            die("--entry '%s' is not defined in the source", g_bm_entry);
        }

        build_functions();

        if (check_only) {
            printf("slyz3c: %s OK (baremetal, %d globals, %d functions)\n",
                   argv[2], g_cc.global_count, g_cc.func_count);
            return 0;
        }

        bm_emit_program(out_path);
        return 0;
    }

    build_functions();

    /* 先编译普通函数与事件处理器，再编译主函数：
     * 主函数需要知道 uses_gui（来自函数体里的 ui.* 调用）。 */
    for (int i = 0; i < g_cc.func_count; i++) {
        if (i != g_cc.main_index) compile_function(&g_cc.funcs[i]);
    }
    compile_main();

    if (check_only) {
        printf("slyz3c: %s OK (%u bytes code, %u bytes const, %d globals, %d functions)\n",
               argv[2], g_cc.code_len, g_cc.const_len,
               g_cc.global_count, g_cc.func_count);
        return 0;
    }

    write_image(out_path);
    return 0;
}
