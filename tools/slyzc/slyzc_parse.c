/* ============================================================
 * slyzc_parse.c - SLYZ 语法分析
 * ------------------------------------------------------------
 * 递归下降，一次性构建完整语法树。语句以换行分隔，分号可选；
 * 表达式在遇到不可接续的运算符时自然结束，因此不需要显式的
 * 语句结束符。
 *
 * 运算符优先级（低到高）：
 *   = += -= *= /= %=       右结合
 *   || or
 *   && and
 *   == != < <= > >=
 *   + -
 *   * / %
 *   一元 - ! not
 *   后缀 调用 f() / 索引 a[i] / 成员 ns.name / 自增 x++ x--
 * ============================================================ */

#include "slyzc.h"
#include <stdarg.h>
#include <setjmp.h>

jmp_buf g_parse_jmp;

void parse_fail(int line, int col, const char *fmt, ...) {
    va_list ap;
    char msg[SLYZ_DIAG_MSG_MAX];
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    slyz_diag_record(0, line, col, msg);
    g_cc.error_count++;
    longjmp(g_parse_jmp, 1);
}

/* ---------- 构造辅助 ---------- */

static Expr *new_expr(int kind, int line, int col) {
    Expr *e = (Expr *)xcalloc(1, sizeof(Expr));
    e->kind = kind;
    e->line = line;
    e->col = col;
    return e;
}

static Stmt *new_stmt(int kind, int line, int col) {
    Stmt *s = (Stmt *)xcalloc(1, sizeof(Stmt));
    s->kind = kind;
    s->line = line;
    s->col = col;
    return s;
}

static void list_push_expr(Expr ***list, int *count, Expr *e) {
    Expr **nl = (Expr **)xmalloc(sizeof(Expr *) * (size_t)(*count + 1));
    for (int i = 0; i < *count; i++) nl[i] = (*list)[i];
    nl[*count] = e;
    if (*list) free(*list);
    *list = nl;
    (*count)++;
}

static void list_push_stmt(Stmt ***list, int *count, Stmt *s) {
    Stmt **nl = (Stmt **)xmalloc(sizeof(Stmt *) * (size_t)(*count + 1));
    for (int i = 0; i < *count; i++) nl[i] = (*list)[i];
    nl[*count] = s;
    if (*list) free(*list);
    *list = nl;
    (*count)++;
}

/* ---------- 前置声明 ---------- */

static Expr *parse_expr(void);
static Stmt *parse_stmt(void);
static Stmt *parse_block(void);

/* ---------- 基本消费 ---------- */

static Token expect_punct(const char *p) {
    Token t = lex_peek();
    if (!tok_is(&t, p)) {
        char got[128];
        if (t.kind == T_EOF) {
            snprintf(got, sizeof(got), "end of file");
        } else if (t.kind == T_STR) {
            snprintf(got, sizeof(got), "string literal");
        } else {
            snprintf(got, sizeof(got), "'%s'", t.text);
        }
        parse_fail(t.line, t.col, "expected '%s' but found %s", p, got);
    }
    lex_consume();
    return t;
}

static Token expect_ident(const char *what) {
    Token t = lex_peek();
    if (t.kind != T_IDENT) {
        parse_fail(t.line, t.col, "expected %s but found '%s'", what,
                   t.kind == T_EOF ? "end of file" : t.text);
    }
    lex_consume();
    return t;
}

static int accept_punct(const char *p) {
    Token t = lex_peek();
    if (tok_is(&t, p)) {
        lex_consume();
        return 1;
    }
    return 0;
}

static int accept_kw(const char *kw) {
    Token t = lex_peek();
    if (tok_is_kw(&t, kw)) {
        lex_consume();
        return 1;
    }
    return 0;
}

static void skip_semicolons(void) {
    while (accept_punct(";")) { }
}

/* ---------- 表达式 ---------- */

static Expr *parse_or(void);

static Expr *parse_primary(void) {
    Token t = lex_peek();

    if (t.kind == T_INT) {
        lex_consume();
        Expr *e = new_expr(EX_INT, t.line, t.col);
        e->ival = t.ival;
        return e;
    }

    if (t.kind == T_STR) {
        lex_consume();
        Expr *e = new_expr(EX_STR, t.line, t.col);
        e->sval = t.sval;
        e->slen = t.slen;
        e->ival = (int64_t)t.slen;
        return e;
    }

    if (t.kind == T_KW) {
        if (strcmp(t.text, "true") == 0) {
            lex_consume();
            Expr *e = new_expr(EX_BOOL, t.line, t.col);
            e->ival = 1;
            return e;
        }
        if (strcmp(t.text, "false") == 0) {
            lex_consume();
            Expr *e = new_expr(EX_BOOL, t.line, t.col);
            e->ival = 0;
            return e;
        }
        if (strcmp(t.text, "nil") == 0) {
            lex_consume();
            return new_expr(EX_NIL, t.line, t.col);
        }
    }

    if (t.kind == T_IDENT) {
        lex_consume();
        Expr *e = new_expr(EX_IDENT, t.line, t.col);
        e->name = xstrdup(t.text);
        return e;
    }

    if (tok_is(&t, "(")) {
        lex_consume();
        Expr *inner = parse_expr();
        expect_punct(")");
        return inner;
    }

    if (tok_is(&t, "[")) {
        lex_consume();
        Expr *arr = new_expr(EX_ARRAY, t.line, t.col);
        if (!accept_punct("]")) {
            for (;;) {
                list_push_expr(&arr->list, &arr->count, parse_expr());
                if (accept_punct(",")) continue;
                expect_punct("]");
                break;
            }
        }
        return arr;
    }

    if (t.kind == T_EOF) {
        parse_fail(t.line, t.col, "unexpected end of file in expression");
    }

    parse_fail(t.line, t.col, "unexpected '%s' in expression", t.text);
    return 0;
}

static Expr *parse_postfix(void) {
    Expr *e = parse_primary();

    for (;;) {
        Token t = lex_peek();

        if (tok_is(&t, "(")) {
            lex_consume();
            Expr *call = new_expr(EX_CALL, t.line, t.col);
            call->a = e;
            if (!accept_punct(")")) {
                for (;;) {
                    list_push_expr(&call->list, &call->count, parse_expr());
                    if (accept_punct(",")) continue;
                    expect_punct(")");
                    break;
                }
            }
            e = call;
            continue;
        }

        if (tok_is(&t, "[")) {
            lex_consume();
            Expr *idx = new_expr(EX_INDEX, t.line, t.col);
            idx->a = e;
            idx->b = parse_expr();
            expect_punct("]");
            e = idx;
            continue;
        }

        if (tok_is(&t, ".")) {
            lex_consume();
            Token name = expect_ident("member name");
            Expr *m = new_expr(EX_MEMBER, t.line, t.col);
            m->a = e;
            m->member = xstrdup(name.text);
            e = m;
            continue;
        }

        if (tok_is(&t, "++") || tok_is(&t, "--")) {
            lex_consume();
            Expr *p = new_expr(EX_POSTFIX, t.line, t.col);
            p->a = e;
            p->op = tok_is(&t, "++") ? 1 : -1;
            e = p;
            continue;
        }

        break;
    }

    return e;
}

static Expr *parse_unary(void) {
    Token t = lex_peek();

    if (tok_is(&t, "-") || tok_is(&t, "!") ||
        (t.kind == T_KW && strcmp(t.text, "not") == 0)) {
        lex_consume();
        Expr *e = new_expr(EX_UN, t.line, t.col);
        if (tok_is(&t, "-") || strcmp(t.text, "-") == 0) e->op = '-';
        else e->op = '!';
        e->a = parse_unary();
        return e;
    }

    return parse_postfix();
}

static Expr *parse_mul(void) {
    Expr *left = parse_unary();

    for (;;) {
        Token t = lex_peek();
        if (!tok_is(&t, "*") && !tok_is(&t, "/") && !tok_is(&t, "%")) break;
        lex_consume();
        Expr *e = new_expr(EX_BIN, t.line, t.col);
        e->op = t.text[0];
        e->a = left;
        e->b = parse_unary();
        left = e;
    }

    return left;
}

static Expr *parse_add(void) {
    Expr *left = parse_mul();

    for (;;) {
        Token t = lex_peek();
        if (!tok_is(&t, "+") && !tok_is(&t, "-")) break;
        lex_consume();
        Expr *e = new_expr(EX_BIN, t.line, t.col);
        e->op = t.text[0];
        e->a = left;
        e->b = parse_mul();
        left = e;
    }

    return left;
}

static Expr *parse_compare(void) {
    Expr *left = parse_add();

    for (;;) {
        Token t = lex_peek();
        int op = 0;
        if (tok_is(&t, "==")) op = 1;
        else if (tok_is(&t, "!=")) op = 2;
        else if (tok_is(&t, "<")) op = 3;
        else if (tok_is(&t, "<=")) op = 4;
        else if (tok_is(&t, ">")) op = 5;
        else if (tok_is(&t, ">=")) op = 6;
        if (!op) break;
        lex_consume();
        Expr *e = new_expr(EX_BIN, t.line, t.col);
        e->op = op;
        e->a = left;
        e->b = parse_add();
        left = e;
    }

    return left;
}

static Expr *parse_and(void) {
    Expr *left = parse_compare();

    for (;;) {
        Token t = lex_peek();
        if (!tok_is(&t, "&&") && !(t.kind == T_KW && strcmp(t.text, "and") == 0)) break;
        lex_consume();
        Expr *e = new_expr(EX_BIN, t.line, t.col);
        e->op = 10;   /* 逻辑与 */
        e->a = left;
        e->b = parse_compare();
        left = e;
    }

    return left;
}

static Expr *parse_or(void) {
    Expr *left = parse_and();

    for (;;) {
        Token t = lex_peek();
        if (!tok_is(&t, "||") && !(t.kind == T_KW && strcmp(t.text, "or") == 0)) break;
        lex_consume();
        Expr *e = new_expr(EX_BIN, t.line, t.col);
        e->op = 11;   /* 逻辑或 */
        e->a = left;
        e->b = parse_and();
        left = e;
    }

    return left;
}

static Expr *parse_assign(void) {
    Expr *left = parse_or();

    Token t = lex_peek();
    int op = -1;
    if (tok_is(&t, "=")) op = ASSIGN_SET;
    else if (tok_is(&t, "+=")) op = ASSIGN_ADD;
    else if (tok_is(&t, "-=")) op = ASSIGN_SUB;
    else if (tok_is(&t, "*=")) op = ASSIGN_MUL;
    else if (tok_is(&t, "/=")) op = ASSIGN_DIV;
    else if (tok_is(&t, "%=")) op = ASSIGN_MOD;

    if (op < 0) return left;

    lex_consume();

    if (left->kind != EX_IDENT && left->kind != EX_INDEX) {
        parse_fail(t.line, t.col, "left side of assignment must be a variable or array element");
    }

    Expr *e = new_expr(EX_ASSIGN, t.line, t.col);
    e->a = left;
    e->b = parse_assign();   /* 右结合 */
    e->assign_op = op;
    return e;
}

static Expr *parse_expr(void) {
    return parse_assign();
}

/* ---------- 语句 ---------- */

static Stmt *parse_block(void) {
    Token open = expect_punct("{");
    Stmt *block = new_stmt(ST_BLOCK, open.line, open.col);

    for (;;) {
        skip_semicolons();
        Token t = lex_peek();
        if (t.kind == T_EOF) {
            parse_fail(open.line, open.col, "unterminated block, missing '}'");
        }
        if (tok_is(&t, "}")) break;
        list_push_stmt(&block->body, &block->count, parse_stmt());
    }

    expect_punct("}");
    return block;
}

static Stmt *parse_stmt(void) {
    skip_semicolons();

    Token t = lex_peek();

    if (tok_is_kw(&t, "let") || tok_is_kw(&t, "var")) {
        lex_consume();
        Token name = expect_ident("variable name");
        Stmt *s = new_stmt(ST_LET, name.line, name.col);
        s->name = xstrdup(name.text);
        if (accept_punct("=")) s->e = parse_expr();
        return s;
    }

    if (tok_is_kw(&t, "fn")) {
        parse_fail(t.line, t.col, "function declarations are only allowed at top level");
    }

    if (tok_is_kw(&t, "if")) {
        lex_consume();
        Stmt *s = new_stmt(ST_IF, t.line, t.col);
        s->e = parse_expr();
        s->then_s = parse_block();
        if (accept_kw("else")) {
            Token look = lex_peek();
            if (look.kind == T_KW && tok_is_kw(&look, "if")) {
                s->else_s = parse_stmt();
            } else {
                s->else_s = parse_block();
            }
        }
        return s;
    }

    if (tok_is_kw(&t, "while")) {
        lex_consume();
        Stmt *s = new_stmt(ST_WHILE, t.line, t.col);
        s->e = parse_expr();
        s->then_s = parse_block();
        return s;
    }

    if (tok_is_kw(&t, "for")) {
        lex_consume();
        Token name = expect_ident("loop variable name");
        if (!accept_kw("in")) {
            Token n = lex_peek();
            parse_fail(n.line, n.col, "expected 'in' after for variable");
        }
        Stmt *s = new_stmt(ST_FOR, t.line, t.col);
        s->name = xstrdup(name.text);
        s->e = parse_expr();
        expect_punct("..");
        s->e2 = parse_expr();
        s->then_s = parse_block();
        return s;
    }

    if (tok_is_kw(&t, "return")) {
        lex_consume();
        Stmt *s = new_stmt(ST_RETURN, t.line, t.col);
        Token n = lex_peek();
        if (!tok_is(&n, "}") && !tok_is(&n, ";") && n.kind != T_EOF) {
            s->e = parse_expr();
        }
        return s;
    }

    if (tok_is_kw(&t, "break")) {
        lex_consume();
        return new_stmt(ST_BREAK, t.line, t.col);
    }

    if (tok_is_kw(&t, "continue")) {
        lex_consume();
        return new_stmt(ST_CONTINUE, t.line, t.col);
    }

    if (tok_is(&t, "{")) {
        return parse_block();
    }

    Stmt *s = new_stmt(ST_EXPR, t.line, t.col);
    s->e = parse_expr();
    return s;
}

/* ---------- 顶层 ---------- */

static FnDef *parse_function(void) {
    Token kw = lex_peek();
    if (!tok_is_kw(&kw, "fn")) {
        parse_fail(kw.line, kw.col, "expected 'fn'");
    }
    lex_consume();

    Token name = expect_ident("function name");
    FnDef *fn = (FnDef *)xcalloc(1, sizeof(FnDef));
    fn->name = xstrdup(name.text);
    fn->line = name.line;
    fn->col = name.col;

    expect_punct("(");
    if (!accept_punct(")")) {
        for (;;) {
            Token p = expect_ident("parameter name");
            char **np = (char **)xmalloc(sizeof(char *) * (size_t)(fn->param_count + 1));
            for (int i = 0; i < fn->param_count; i++) np[i] = fn->params[i];
            np[fn->param_count] = xstrdup(p.text);
            if (fn->params) free(fn->params);
            fn->params = np;
            fn->param_count++;
            if (accept_punct(",")) continue;
            expect_punct(")");
            break;
        }
    }

    fn->body = parse_block();
    return fn;
}

static void push_fn(Compiler *cc, FnDef *fn) {
    FnDef **nf = (FnDef **)xmalloc(sizeof(FnDef *) * (size_t)(cc->func_count + 1));
    for (int i = 0; i < cc->func_count; i++) nf[i] = cc->funcs[i];
    nf[cc->func_count] = fn;
    if (cc->funcs) free(cc->funcs);
    cc->funcs = nf;
    cc->func_count++;
}

void parse_program(Compiler *cc) {
    for (;;) {
        skip_semicolons();
        Token t = lex_peek();
        if (t.kind == T_EOF) break;

        if (tok_is_kw(&t, "fn")) {
            FnDef *fn = parse_function();
            push_fn(cc, fn);
            continue;
        }

        list_push_stmt(&cc->program, &cc->program_count, parse_stmt());
    }
}
