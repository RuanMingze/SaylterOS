/* ============================================================
 * slyzc.h - SLYZ v2 编译器内部结构
 * ------------------------------------------------------------
 * 编译流水线：
 *   源码(.slyzcode) -> 词法 -> 语法树 -> 语义解析 -> 字节码
 *                   -> SLYZBIN v2 镜像(.slyz)
 *
 * 词法、语法、代码生成分文件实现，共享本头文件中的 Token、
 * Expr、Stmt、FnDef 与 Compiler 结构。所有诊断都带行号列号，
 * 第一条错误即终止编译，避免错误语法继续生成不可用镜像。
 * ============================================================ */

#ifndef SLYZC_H
#define SLYZC_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "slyz.h"

/* ---------- 词法 ---------- */

enum {
    T_EOF = 0,
    T_IDENT,
    T_INT,
    T_STR,
    T_PUNCT,
    T_KW
};

#define NAME_MAX_LEN 96

typedef struct {
    int      kind;
    int      line;
    int      col;
    int64_t  ival;
    char     text[NAME_MAX_LEN];   /* 标识符、标点、关键字原文 */
    char    *sval;                 /* 字符串字面量解码后的字节串 */
    int      slen;
} Token;

/* ---------- 语法树 ---------- */

typedef struct Expr Expr;
typedef struct Stmt Stmt;

enum {
    EX_INT = 1,
    EX_STR,
    EX_BOOL,
    EX_NIL,
    EX_IDENT,
    EX_BIN,
    EX_UN,
    EX_CALL,
    EX_INDEX,
    EX_ARRAY,
    EX_ASSIGN,
    EX_MEMBER,
    EX_POSTFIX
};

enum {
    ASSIGN_SET = 0,
    ASSIGN_ADD,
    ASSIGN_SUB,
    ASSIGN_MUL,
    ASSIGN_DIV,
    ASSIGN_MOD
};

struct Expr {
    int      kind;
    int      line;
    int      col;

    int64_t  ival;
    char    *sval;
    int      slen;
    int      op;           /* 二元/一元运算符的 token 类型 */

    Expr    *a;
    Expr    *b;
    Expr    *c;

    Expr   **list;
    int      count;

    char    *name;         /* EX_IDENT / EX_MEMBER 的基础名 */
    char    *member;       /* EX_MEMBER 的成员名 */
    int      assign_op;

    /* 语义解析结果 */
    int      resolved;     /* 0 未解析，1 局部，2 全局，3 内建，4 用户函数 */
    int      slot;
    int      call_id;      /* 内建/系统调用号 */
    int      argc;         /* 调用参数个数 */
    int      fn_index;     /* 用户函数下标 */
};

enum {
    ST_LET = 1,
    ST_EXPR,
    ST_IF,
    ST_WHILE,
    ST_FOR,
    ST_RETURN,
    ST_BREAK,
    ST_CONTINUE,
    ST_BLOCK
};

struct Stmt {
    int       kind;
    int       line;
    int       col;

    char     *name;
    Expr     *e;
    Expr     *e2;
    Expr     *e3;
    Stmt     *then_s;
    Stmt     *else_s;
    Stmt    **body;
    int       count;

    /* 语义解析结果：let 绑定到的槽 */
    int       slot;
    int       is_global;
};

/* ---------- 函数 ---------- */

typedef struct {
    char  *name;
    char **params;
    int    param_count;
    Stmt  *body;           /* 一定是 ST_BLOCK */
    int    line;
    int    col;

    int    local_count;    /* 参数 + 局部变量总槽数 */
    uint32_t entry;        /* 代码段内入口偏移 */
    int    compiled;

    /* 前向调用回填 */
    struct CallPatch *patches;
    int    patch_count;
    int    patch_cap;
} FnDef;

typedef struct CallPatch {
    uint32_t at;           /* CALL 指令操作数所在偏移 */
    char    *name;
} CallPatch;

/* ---------- 常量池 ---------- */

typedef struct {
    uint8_t  *data;
    uint32_t  len;
    uint32_t  cap;
    uint32_t *offs;
    int      *lens;
    int       count;
    int       cap_count;
} ConstPool;

/* ---------- 编译单元 ---------- */

typedef struct {
    const char *path;
    const char *source;
    int         source_len;

    Stmt      **program;
    int         program_count;

    FnDef     **funcs;
    int         func_count;

    /* 全局变量 */
    char      **global_names;
    int        *global_slots;
    int         global_count;

    ConstPool  consts;

    /* 输出代码 */
    uint8_t   *code;
    uint32_t   code_len;
    uint32_t   code_cap;

    int        max_stack;      /* 观测到的最大栈深 */
    int        uses_gui;
    uint32_t   entry_offset;   /* 顶层程序体在代码段内的偏移 */

    int        error_count;
} Compiler;

/* ---------- 全局编译状态（单文件编译，进程内一次） ---------- */

extern Compiler g_cc;

void diag_error(int line, int col, const char *fmt, ...);
void diag_warn(int line, int col, const char *fmt, ...);

/* ---------- 词法 ---------- */

void  lex_init(const char *source, int len, const char *path);
Token lex_next(void);
Token lex_peek(void);
void  lex_consume(void);
int   tok_is(const Token *t, const char *punct);
int   tok_is_kw(const Token *t, const char *kw);

/* ---------- 语法 ---------- */

void parse_program(Compiler *cc);

/* ---------- 语义与代码生成 ---------- */

void codegen_program(Compiler *cc);
int  compile_image(Compiler *cc, uint8_t **out, uint32_t *out_size);

/* ---------- 工具 ---------- */

void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t m);
char *xstrdup(const char *s);
char *xstrndup(const char *s, int n);
void  consts_init(ConstPool *p);
uint32_t consts_add(ConstPool *p, const char *s, int len, int *out_index);

/* 语法/语义致命错误：打印后长跳转到编译入口，只报告第一条错误 */
void parse_fail(int line, int col, const char *fmt, ...);

#endif /* SLYZC_H */
